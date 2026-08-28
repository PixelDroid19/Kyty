#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvTemplates.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Log.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static PixelInterpolationMode pixel_interpolation_mode(const ShaderPixelInputInfo& info, int source_register)
{
	if (source_register < 0)
	{
		return PixelInterpolationMode::Unsupported;
	}

	constexpr PixelSystemInputField k_fields[] = {
	    {1u << 0u, 2u, PixelInterpolationMode::Unsupported},
	    {1u << 1u, 2u, PixelInterpolationMode::PerspectiveCenter},
	    {1u << 2u, 2u, PixelInterpolationMode::PerspectiveCentroid},
	    {1u << 3u, 3u, PixelInterpolationMode::Unsupported},
	    {1u << 4u, 2u, PixelInterpolationMode::Unsupported},
	    {1u << 5u, 2u, PixelInterpolationMode::LinearCenter},
	    {1u << 6u, 2u, PixelInterpolationMode::LinearCentroid},
	};

	uint32_t first_register = 0;
	for (const auto& field: k_fields)
	{
		if ((info.system_input_address & field.bit) == 0)
		{
			continue;
		}
		const uint32_t register_id = static_cast<uint32_t>(source_register);
		if (register_id >= first_register && register_id < first_register + field.width)
		{
			return ((info.system_input_enable & field.bit) != 0 ? field.mode : PixelInterpolationMode::Unsupported);
		}
		first_register += field.width;
	}

	return PixelInterpolationMode::Unsupported;
}

bool operand_is_constant(ShaderOperand op)
{
	return (op.type == ShaderOperandType::LiteralConstant || op.type == ShaderOperandType::IntegerInlineConstant ||
	        op.type == ShaderOperandType::FloatInlineConstant);
}

bool operand_is_variable(ShaderOperand op)
{
	return (op.type == ShaderOperandType::Vgpr || op.type == ShaderOperandType::VccLo || op.type == ShaderOperandType::VccHi ||
	        op.type == ShaderOperandType::Sgpr || op.type == ShaderOperandType::ExecLo || op.type == ShaderOperandType::ExecHi ||
	        op.type == ShaderOperandType::ExecZ || op.type == ShaderOperandType::Scc || op.type == ShaderOperandType::M0);
}

bool operand_covers_vgpr(ShaderOperand op, int reg)
{
	if (op.type != ShaderOperandType::Vgpr || reg < 0)
	{
		return false;
	}

	const int size = (op.size > 0 ? op.size : 1);
	return reg >= op.register_id && reg < op.register_id + size;
}

bool instruction_writes_vgpr(const ShaderInstruction& inst, int reg)
{
	// V_READFIRSTLANE and V_READLANE encode their scalar destination in the
	// VDST field even though the field shares the vector operand encoding. They
	// write an SGPR, never the VGPR with the same numeric index. Counting either
	// instruction as a vector write corrupts scalar-spill lifetime tracking when
	// the destination SGPR number aliases the spill VGPR.
	if (inst.type == ShaderInstructionType::VReadfirstlaneB32 || inst.type == ShaderInstructionType::VReadlaneB32)
	{
		return false;
	}
	// MIMG stores encode their source data in the same VDATA field used as a
	// destination by loads and samples.  They consume the VGPR and do not start
	// a new register lifetime.
	if (IsStorageImageInstruction(inst))
	{
		return false;
	}
	return operand_covers_vgpr(inst.dst, reg) || operand_covers_vgpr(inst.dst2, reg);
}


PixelInterpolationMode Spirv::GetPixelInterpolationMode(uint32_t input) const
{
	EXIT_IF(input >= 32u);
	return m_pixel_interpolation[input];
}

static bool pixel_interpolation_rejected(const char* reason, const ShaderPixelInputInfo& info, uint32_t instruction_index,
                                         const ShaderInstruction& instruction, int coordinate_source)
{
	KYTY_LOG_DEBUG(
	        "SHADER_INTERPOLATION_REJECT reason=%s index=%u p2_source=%d input=%u input_num=%u ena=0x%08x addr=0x%08x "
	        "coordinate_source=%d\n",
	        reason, instruction_index, instruction.src[0].register_id, instruction.src[1].constant.u, info.input_num,
	        info.system_input_enable, info.system_input_address, coordinate_source);
	return false;
}

bool Spirv::ResolvePixelInterpolationModes()
{
	EXIT_IF(m_ps_input_info == nullptr);

	for (auto& mode: m_pixel_interpolation)
	{
		mode = PixelInterpolationMode::Unused;
	}

	const auto& instructions = m_code.GetInstructions();
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (inst.type != ShaderInstructionType::VInterpP2F32)
		{
			continue;
		}
		if (!operand_is_variable(inst.src[0]) || inst.src[0].type != ShaderOperandType::Vgpr || !operand_is_constant(inst.src[1]))
		{
			return pixel_interpolation_rejected("p2_operands", *m_ps_input_info, index, inst, -1);
		}

		const uint32_t input = inst.src[1].constant.u;
		if (input >= m_ps_input_info->input_num)
		{
			return pixel_interpolation_rejected("input_index", *m_ps_input_info, index, inst, -1);
		}
		const uint32_t canonical_input = ShaderPixelCanonicalInterpolator(*m_ps_input_info, input);

		ShaderPixelInterpolator interpolator {};
		if (!ShaderDecodePixelInterpolator(m_ps_input_info->interpolator_settings[canonical_input], &interpolator))
		{
			return pixel_interpolation_rejected("interpolator", *m_ps_input_info, index, inst, -1);
		}
		if (interpolator.source == ShaderPixelInterpolatorSource::Default || interpolator.flat)
		{
			continue;
		}
		constexpr uint32_t kInterpolationSystemInputs = 0x7fu;
		if ((m_ps_input_info->system_input_enable & kInterpolationSystemInputs) == 0u &&
		    (m_ps_input_info->system_input_address & kInterpolationSystemInputs) == 0u)
		{
			// With no explicit barycentric system VGPRs, VINTRP uses the
			// rasterizer's ordinary perspective interpolation. SPIR-V's default
			// Smooth decoration is the direct representation of that path.
			continue;
		}

		// P2 reads the J coordinate from VSrc. Its fixed-function interpolation
		// group determines the qualifier of the Vulkan varying; P1 is only the
		// hardware intermediate for that same group.
		const int coordinate_source = inst.src[0].register_id;
		const auto mode = pixel_interpolation_mode(*m_ps_input_info, coordinate_source);
		if (mode == PixelInterpolationMode::Unsupported)
		{
			return pixel_interpolation_rejected("p2_coordinate", *m_ps_input_info, index, inst, coordinate_source);
		}

		auto& resolved_mode = m_pixel_interpolation[canonical_input];
		if (resolved_mode == PixelInterpolationMode::Unused)
		{
			resolved_mode = mode;
		} else if (resolved_mode != mode)
		{
			return pixel_interpolation_rejected("mixed_modes", *m_ps_input_info, index, inst, coordinate_source);
		}
	}

	return true;
}


String8 packed_half_shadow_to_str(ShaderOperand op)
{
	if (op.type != ShaderOperandType::Vgpr || op.size != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: op.type != ShaderOperandType::Vgpr || op.size != 1 condition ignored (continuing)\n"); }
	return String8::FromPrintf("v%d_packed_half", op.register_id);
}

SpirvValue operand_variable_to_str(ShaderOperand op)
{
	SpirvValue ret;

	EXIT_IF(op.size != 1);

	switch (op.type)
	{
		case ShaderOperandType::Vgpr:
			ret.value = String8::FromPrintf("v%d", op.register_id);
			ret.type  = SpirvType::Float;
			break;
		case ShaderOperandType::Sgpr:
			ret.value = String8::FromPrintf("s%d", op.register_id);
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::VccLo:
			ret.value = "vcc_lo";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::VccHi:
			ret.value = "vcc_hi";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::ExecLo:
			ret.value = "exec_lo";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::ExecHi:
			ret.value = "exec_hi";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::ExecZ:
			ret.value = "execz";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::Scc:
			ret.value = "scc";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::M0:
			ret.value = "m0";
			ret.type  = SpirvType::Uint;
			break;
		default: break;
	}

	return ret;
}

SpirvValue operand_variable_to_str(ShaderOperand op, int shift)
{
	SpirvValue ret;

	EXIT_IF(op.size <= shift || shift < 0);

	switch (op.type)
	{
		case ShaderOperandType::Vgpr:
			ret.value = String8::FromPrintf("v%d", op.register_id + shift);
			ret.type  = SpirvType::Float;
			break;
		case ShaderOperandType::Sgpr:
			ret.value = String8::FromPrintf("s%d", op.register_id + shift);
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::VccLo:
			if (shift == 0)
			{
				ret.value = "vcc_lo";
				ret.type  = SpirvType::Uint;
			} else if (shift == 1)
			{
				ret.value = "vcc_hi";
				ret.type  = SpirvType::Uint;
			}
			break;
		case ShaderOperandType::ExecLo:
			if (shift == 0)
			{
				ret.value = "exec_lo";
				ret.type  = SpirvType::Uint;
			} else if (shift == 1)
			{
				ret.value = "exec_hi";
				ret.type  = SpirvType::Uint;
			}
			break;
		case ShaderOperandType::ExecZ:
			if (shift == 0)
			{
				ret.value = "execz";
				ret.type  = SpirvType::Uint;
			}
			break;
		case ShaderOperandType::Scc:
			if (shift == 0)
			{
				ret.value = "scc";
				ret.type  = SpirvType::Uint;
			}
			break;
		case ShaderOperandType::M0:
			if (shift == 0)
			{
				ret.value = "m0";
				ret.type  = SpirvType::Uint;
			}
			break;
		default: break;
	}

	return ret;
}

SpirvValue buffer_index_variable_to_str(const ShaderInstruction& inst)
{
	if (inst.format == ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen)
	{
		return operand_variable_to_str(inst.src[0]);
	}

	if (inst.format != ShaderInstructionFormat::Vdata1Vaddr2SvSoffsOffenIdxen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.format != ShaderInstructionFormat::Vdata1Vaddr2SvSoffsOffenIdxen condition ignored (continuing)\n"); }
	return operand_variable_to_str(inst.src[0], 1);
}

SpirvValue mimg_address_to_str(const ShaderInstruction& inst, int address)
{
	EXIT_IF(address < 0);
	if (inst.mimg_address_num != 0)
	{
		EXIT_IF(address >= inst.mimg_address_num);
		return operand_variable_to_str(inst.mimg_address[address]);
	}
	return operand_variable_to_str(inst.src[0], address);
}


bool operand_is_exec(ShaderOperand op)
{
	switch (op.type)
	{
		case ShaderOperandType::ExecLo:
		case ShaderOperandType::ExecHi:
		case ShaderOperandType::ExecZ: return true;
		default: break;
	}
	return false;
}

// SDWA SEL (GCN/RDNA): zero-extend BYTE_n / WORD_n from a uint register value.
// sel 6 (DWORD) is a no-op. Returns SPIR-V that writes <result_id> from <input_id>.
static String8 sdwa_swizzle_uint(const String8& input_id, const String8& result_id, const String8& index, uint8_t sel)
{
	if (sel == 6u)
	{
		return {};
	}
	if (sel > 6u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sel > 6u condition ignored (continuing)\n"); }

	// offset,count for OpBitFieldUExtract
	uint32_t offset = 0;
	uint32_t count  = 32;
	switch (sel)
	{
		case 0:
			offset = 0;
			count  = 8;
			break; // BYTE_0
		case 1:
			offset = 8;
			count  = 8;
			break; // BYTE_1
		case 2:
			offset = 16;
			count  = 8;
			break; // BYTE_2
		case 3:
			offset = 24;
			count  = 8;
			break; // BYTE_3
		case 4:
			offset = 0;
			count  = 16;
			break; // WORD_0
		case 5:
			offset = 16;
			count  = 16;
			break; // WORD_1
		default: break;
	}

	return String8("%<result_id> = OpBitFieldUExtract %uint %<input_id> %uint_<off> %uint_<cnt>\n")
	    .ReplaceStr("<result_id>", result_id)
	    .ReplaceStr("<input_id>", input_id)
	    .ReplaceStr("<off>", String8::FromPrintf("%u", offset))
	    .ReplaceStr("<cnt>", String8::FromPrintf("%u", count))
	    .ReplaceStr("<index>", index);
}

bool operand_load_int(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load)
{
	EXIT_IF(load == nullptr);

	if (op.negate || op.absolute) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: op.negate || op.absolute condition ignored (continuing)\n"); }

	if (operand_is_constant(op))
	{
		String8 id = spirv->GetConstant(op);

		*load = String8("%<result_id> = OpBitcast %int %<id>")
		            .ReplaceStr("<index>", index)
		            .ReplaceStr("<id>", id)
		            .ReplaceStr("<result_id>", result_id);
	} else if (operand_is_variable(op))
	{
		auto value = operand_variable_to_str(op);

		if (value.type == SpirvType::Float)
		{
			*load = (String8("%t<result_id> = OpLoad %float %<id>\n") + String8(' ', 10) +
			         String8("%<result_id> = OpBitcast %int %t<result_id>\n"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", result_id);
		} else if (value.type == SpirvType::Uint)
		{
			*load = (String8("%t<result_id> = OpLoad %uint %<id>\n") + String8(' ', 10) +
			         String8("%<result_id> = OpBitcast %int %t<result_id>\n"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", result_id);
		}
	} else
	{
		return false;
	}
	return true;
}

bool operand_load_uint(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load, int shift)
{
	EXIT_IF(load == nullptr);
	if (op.type == ShaderOperandType::Null)
	{
		*load = String8("%<result_id> = OpCopyObject %uint %uint_0").ReplaceStr("<result_id>", result_id);
		return true;
	}
	const bool scalar_special = op.type == ShaderOperandType::VccZ || op.type == ShaderOperandType::ExecZ ||
	                            op.type == ShaderOperandType::Scc || op.type == ShaderOperandType::M0;
	if (op.size == 2 && shift == 1 && scalar_special)
	{
		*load = String8("%<result_id> = OpCopyObject %uint %uint_0").ReplaceStr("<result_id>", result_id);
		return true;
	}
	if (op.type == ShaderOperandType::VccZ)
	{
		*load = String8(R"(%vccz_lo_<index> = OpLoad %uint %vcc_lo
%vccz_hi_<index> = OpLoad %uint %vcc_hi
%vccz_or_<index> = OpBitwiseOr %uint %vccz_lo_<index> %vccz_hi_<index>
%vccz_bool_<index> = OpIEqual %bool %vccz_or_<index> %uint_0
%<result_id> = OpSelect %uint %vccz_bool_<index> %uint_1 %uint_0)")
		            .ReplaceStr("<index>", index)
		            .ReplaceStr("<result_id>", result_id);
		return true;
	}

	if (op.negate || op.absolute) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: op.negate || op.absolute condition ignored (continuing)\n"); }

	const bool    need_swizzle = (op.swizzle != 6u);
	const String8 raw_id       = need_swizzle ? ("raw" + result_id) : result_id;

	if (operand_is_constant(op))
	{
		if (op.size == 2)
		{
			if (shift < 0 || shift >= 2) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: shift < 0 || shift >= 2 condition ignored (continuing)\n"); }

			if (shift == 0)
			{
				String8 id = spirv->GetConstant(op);
				*load      = String8("%<result_id> = OpBitcast %uint %<id>")
				                 .ReplaceStr("<index>", index)
				                 .ReplaceStr("<id>", id)
				                 .ReplaceStr("<result_id>", raw_id);
			} else
			{
				if (op.type == ShaderOperandType::IntegerInlineConstant && op.constant.i < 0)
				{
					*load = String8("%<result_id> = OpBitcast %uint %uint_0xffffffff")
					            .ReplaceStr("<index>", index)
					            .ReplaceStr("<result_id>", raw_id);
				} else
				{
					*load =
					    String8("%<result_id> = OpBitcast %uint %uint_0").ReplaceStr("<index>", index).ReplaceStr("<result_id>", raw_id);
				}
			}
		} else
		{
			String8 id = spirv->GetConstant(op);
			*load      = String8("%<result_id> = OpBitcast %uint %<id>")
			                 .ReplaceStr("<index>", index)
			                 .ReplaceStr("<id>", id)
			                 .ReplaceStr("<result_id>", raw_id);
		}
	} else if (operand_is_variable(op))
	{
		auto value = (shift >= 0 ? operand_variable_to_str(op, shift) : operand_variable_to_str(op));

		if (value.type == SpirvType::Float)
		{
			*load = (String8("%t<result_id> = OpLoad %float %<id>\n") + String8(' ', 10) +
			         String8("%<result_id> = OpBitcast %uint %t<result_id>\n"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", raw_id);
		} else if (value.type == SpirvType::Uint)
		{
			*load = (String8("%<result_id> = OpLoad %uint %<id>"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", raw_id);
		} else
		{
			return false;
		}
	} else
	{
		return false;
	}

	if (need_swizzle)
	{
		*load += String8(' ', 10) + sdwa_swizzle_uint(raw_id, result_id, index, op.swizzle);
	}
	return true;
}

bool operand_load_float(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load)
{
	EXIT_IF(load == nullptr);
	if (op.type == ShaderOperandType::VccZ)
	{
		String8 uint_load;
		if (!operand_load_uint(spirv, op, "vccz_u_" + result_id, index, &uint_load))
		{
			return false;
		}
		*load = uint_load + String8("\n%<result_id> = OpBitcast %float %vccz_u_<result_id>").ReplaceStr("<result_id>", result_id);
		return true;
	}

	String8    l;
	const bool need_swizzle = (op.swizzle != 6u);

	// SDWA BYTE/WORD selects operate on the raw 32-bit register image, then
	// the extracted uint is bitcast back to float for VGPR storage.
	if (need_swizzle)
	{
		String8 uint_load;
		if (!operand_load_uint(spirv, op, "su_" + result_id, index, &uint_load))
		{
			return false;
		}
		if (op.negate && op.absolute)
		{
			l     = uint_load + String8(' ', 10) +
			        String8("%swf_<index> = OpBitcast %float %su_<result_id>\n").ReplaceStr("<result_id>", result_id) + String8(' ', 10) +
			        String8("%abs_<index> = OpExtInst %float %GLSL_std_450 FAbs %swf_<index>\n") + String8(' ', 10) +
			        String8("%<result> = OpFNegate %float %abs_<index>\n");
			*load = l.ReplaceStr("<index>", index).ReplaceStr("<result>", result_id);
			return true;
		}
		if (op.absolute)
		{
			l     = uint_load + String8(' ', 10) +
			        String8("%swf_<index> = OpBitcast %float %su_<result_id>\n").ReplaceStr("<result_id>", result_id) + String8(' ', 10) +
			        String8("%<result> = OpExtInst %float %GLSL_std_450 FAbs %swf_<index>\n");
			*load = l.ReplaceStr("<index>", index).ReplaceStr("<result>", result_id);
			return true;
		}
		if (op.negate)
		{
			l     = uint_load + String8(' ', 10) +
			        String8("%swf_<index> = OpBitcast %float %su_<result_id>\n").ReplaceStr("<result_id>", result_id) + String8(' ', 10) +
			        String8("%<result> = OpFNegate %float %swf_<index>\n");
			*load = l.ReplaceStr("<index>", index).ReplaceStr("<result>", result_id);
			return true;
		}
		l = uint_load + String8(' ', 10) + String8("%<result> = OpBitcast %float %su_<result_id>\n").ReplaceStr("<result_id>", result_id);
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result>", result_id);
		return true;
	}

	if (operand_is_constant(op))
	{
		String8 id = spirv->GetConstant(op);

		const char* operation = (op.type == ShaderOperandType::FloatInlineConstant ? "OpCopyObject" : "OpBitcast");
		l = String8("%<result_id> = <operation> %float %<id>").ReplaceStr("<operation>", operation).ReplaceStr("<id>", id);
	} else if (operand_is_variable(op))
	{
		auto value = operand_variable_to_str(op);

		if (value.type == SpirvType::Float)
		{
			l = String8("%<result_id> = OpLoad %float %<id>\n").ReplaceStr("<id>", value.value);
		} else if (value.type == SpirvType::Uint)
		{
			l = (String8("%t<result_id> = OpLoad %uint %<id>\n") + String8(' ', 10) +
			     String8("%<result_id> = OpBitcast %float %t<result_id>\n"))
			        .ReplaceStr("<id>", value.value);
		} else
		{
			return false;
		}
	} else
	{
		return false;
	}

	if (op.negate && op.absolute)
	{
		l += String8(' ', 10) + String8("%abs_<index> = OpExtInst %float %GLSL_std_450 FAbs %<result_id>\n") + String8(' ', 10) +
		     String8("%<result> = OpFNegate %float %abs_<index>\n");

		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", "a" + result_id).ReplaceStr("<result>", result_id);

		return true;
	}

	if (op.absolute)
	{
		l += String8(' ', 10) + String8("%<result> = OpExtInst %float %GLSL_std_450 FAbs %<result_id>\n");
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", "a" + result_id).ReplaceStr("<result>", result_id);
	} else if (op.negate)
	{
		l += String8(' ', 10) + String8("%<result> = OpFNegate %float %<result_id>\n");
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", "n" + result_id).ReplaceStr("<result>", result_id);
	} else
	{
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", result_id);
	}

	return true;
}

String8 get_scc_check(SccCheck scc_check, int dst_num)
{
	EXIT_IF(dst_num < 1 || dst_num > 2);

	if (dst_num == 1)
	{
		switch (scc_check)
		{
			case SccCheck::NonZero: return SCC_NZ_1; break;
			case SccCheck::OverflowAdd: return SCC_OVERFLOW_ADD_1; break;
			case SccCheck::OverflowSub: return SCC_OVERFLOW_SUB_1; break;
			case SccCheck::CarryOut: return SCC_CARRY_1; break;
			default: break;
		}
	} else if (dst_num == 2)
	{
		switch (scc_check)
		{
			case SccCheck::NonZero: return SCC_NZ_2; break;
			case SccCheck::ExecNonZero: return SCC_EXEC_NZ_2; break;
			case SccCheck::OverflowAdd: KYTY_NOT_IMPLEMENTED; break;
			case SccCheck::OverflowSub: KYTY_NOT_IMPLEMENTED; break;
			case SccCheck::CarryOut: KYTY_NOT_IMPLEMENTED; break;
			default: break;
		}
	}
	return "";
}

// MUBUF/MTBUF soffset is stored into %temp_int_* (signed int pointers).
// GetConstant() returns %uint_N for LiteralConstant, which fails OpStore into
// %_ptr_Function_int. Always materialize the offset as an Int constant id.
// FindConstants must register the Int twin for every LiteralConstant.

void Spirv::AddConstantUint(uint32_t u)
{
	ShaderConstant c {};
	c.u = u;
	AddConstant(SpirvType::Uint, c);
}

void Spirv::AddConstantInt(int i)
{
	ShaderConstant c {};
	c.i = i;
	AddConstant(SpirvType::Int, c);
}

void Spirv::AddConstantFloat(float f)
{
	ShaderConstant c {};
	c.f = f;
	AddConstant(SpirvType::Float, c);
}

void Spirv::AddConstant(ShaderOperand op)
{
	SpirvType type = SpirvType::Unknown;

	if (op.type == ShaderOperandType::LiteralConstant)
	{
		type = SpirvType::Uint;
	}
	if (op.type == ShaderOperandType::IntegerInlineConstant)
	{
		type = SpirvType::Int;
	}
	if (op.type == ShaderOperandType::FloatInlineConstant)
	{
		type = SpirvType::Float;
	}

	if (type == SpirvType::Unknown) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: type == SpirvType::Unknown condition ignored (continuing)\n"); }

	AddConstant(type, op.constant);
}

void Spirv::AddConstant(SpirvType type, ShaderConstant constant)
{
	for (const auto& c: m_constants)
	{
		if (c.type == type && c.constant.u == constant.u)
		{
			return;
		}
	}

	Constant c {};
	c.type     = type;
	c.constant = constant;
	c.type_str = Core::EnumName(type).ToLower().C_Str();

	if (type == SpirvType::Uint)
	{
		c.value_str = constant.u < 256 ? String8::FromPrintf("%u", constant.u) : String8::FromPrintf("0x%08" PRIx32, constant.u);
		c.literal_str = c.value_str;
	}
	if (type == SpirvType::Int)
	{
		c.value_str = String8::FromPrintf("%d", constant.i);
		c.literal_str = c.value_str;
	}
	if (type == SpirvType::Float)
	{
		c.value_str = String8::FromPrintf("%f", constant.f);
		c.literal_str = String8::FromPrintf("%.9g", static_cast<double>(constant.f));
	}

	c.id = String8::FromPrintf("%s_%s", c.type_str.c_str(), c.value_str.ReplaceChar('.', '_').ReplaceChar('-', 'm').c_str());

	m_constants.Add(c);
}

void Spirv::AddVariable(ShaderOperandType type, int register_id, int size)
{
	ShaderOperand op;
	op.type        = type;
	op.register_id = register_id;
	op.size        = size;
	AddVariable(op);
}

void Spirv::AddVariable(ShaderOperand op)
{
	if (op.type == ShaderOperandType::VccZ)
	{
		AddVariable(ShaderOperandType::VccLo, 0, 2);
		return;
	}
	if (operand_is_variable(op))
	{
		EXIT_IF(op.size == 0);
		const bool scalar_special = op.type == ShaderOperandType::ExecZ || op.type == ShaderOperandType::Scc ||
		                            op.type == ShaderOperandType::M0;
		const int variable_count = scalar_special ? 1 : op.size;

		for (int i = 0; i < variable_count; i++)
		{
			Variable v;
			v.op.type        = op.type;
			v.op.register_id = op.register_id + i;
			v.op.size        = 1;

			if (op.type == ShaderOperandType::VccLo && op.size == 2 && i == 1)
			{
				v.op.type        = ShaderOperandType::VccHi;
				v.op.register_id = 0;
			}

			if (op.type == ShaderOperandType::ExecLo && op.size == 2 && i == 1)
			{
				v.op.type        = ShaderOperandType::ExecHi;
				v.op.register_id = 0;
			}

			if (!m_variables.Contains(v, [](auto v1, auto v2) { return v1.op == v2.op; }))
			{
				m_variables.Add(v);
			}
		}
	}
}

String8 Spirv::GetConstantUint(uint32_t u) const
{
	for (const auto& c: m_constants)
	{
		if (c.type == SpirvType::Uint && c.constant.u == u)
		{
			return c.id;
		}
	}

	return "unknown_uint_constant";
}

String8 Spirv::GetConstantInt(int i) const
{
	for (const auto& c: m_constants)
	{
		if (c.type == SpirvType::Int && c.constant.i == i)
		{
			return c.id;
		}
	}

	return "unknown_int_constant";
}

String8 Spirv::GetConstantFloat(float f) const
{
	for (const auto& c: m_constants)
	{
		if (c.type == SpirvType::Float && c.constant.f == f)
		{
			return c.id;
		}
	}

	return "unknown_float_constant";
}

String8 Spirv::GetConstant(ShaderOperand op) const
{
	SpirvType type = SpirvType::Unknown;

	if (op.type == ShaderOperandType::LiteralConstant)
	{
		type = SpirvType::Uint;
	}
	if (op.type == ShaderOperandType::IntegerInlineConstant)
	{
		type = SpirvType::Int;
	}
	if (op.type == ShaderOperandType::FloatInlineConstant)
	{
		type = SpirvType::Float;
	}

	for (const auto& c: m_constants)
	{
		if (c.type == type && c.constant.u == op.constant.u)
		{
			return c.id;
		}
	}

	return "unknown_operand_constant";
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
