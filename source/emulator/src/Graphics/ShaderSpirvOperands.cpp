#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvTemplates.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Log.h"

#include <cctype>
#include <cstring>
#include <string>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {

namespace {

// These aliases distinguish an explicit numeric operand from a template's
// implicit per-invocation EXEC test, including in the paired bank rewriter.
String8 PackedMaskName(const String8& name)
{
	return name == "exec_lo" || name == "exec_hi" ? "packed_" + name : name;
}

String8 ReplaceMaskToken(const String8& source, const String8& from, const String8& to)
{
	std::string text(source.c_str());
	const std::string find(from.c_str());
	const std::string replacement(to.c_str());
	for (size_t pos = 0; (pos = text.find(find, pos)) != std::string::npos;)
	{
		const size_t end = pos + find.size();
		if (end == text.size() || (!std::isalnum(static_cast<unsigned char>(text[end])) && text[end] != '_'))
		{
			text.replace(pos, find.size(), replacement);
			pos += replacement.size();
		} else
		{
			pos = end;
		}
	}
	return String8(text.c_str());
}

} // namespace

bool Spirv::NativeWave32() const
{
	return !UsesComputeWaveBanks() &&
	       ((GetCsInputInfo() != nullptr && GetCsInputInfo()->wave_layout.guest_wave_size == 32u) ||
	        (GetVsInputInfo() != nullptr && GetVsInputInfo()->native_wave.guest_wave_size == 32u) ||
	        (GetPsInputInfo() != nullptr && GetPsInputInfo()->native_wave.guest_wave_size == 32u));
}

bool Spirv::EmitNativeMaskBit(const ShaderOperand& mask, const String8& result, String8* output) const
{
	if (UsesComputeWaveBanks() || output == nullptr || (mask.size != 2 && mask.size != 1)) { return false; }
	const auto lo = operand_numeric_variable_to_str(mask, mask.size == 1 ? -1 : 0);
	if (lo.type != SpirvType::Uint) { return false; }
	const bool pair = mask.size == 2 && !NativeWave32();
	const auto hi = pair ? operand_numeric_variable_to_str(mask, 1) : SpirvValue {};
	if (pair && hi.type != SpirvType::Uint) { return false; }
	*output += String8(R"(
%<r>_lane = OpLoad %uint %gl_SubgroupInvocationID
%<r>_shift = OpBitwiseAnd %uint %<r>_lane %uint_31
%<r>_lo = OpLoad %uint %<lo>
<high>
%<r>_shifted = OpShiftRightLogical %uint %<r>_word %<r>_shift
%<r> = OpBitwiseAnd %uint %<r>_shifted %uint_1
)")
	               .ReplaceStr("<high>", pair ? String8("%<r>_hi = OpLoad %uint %<hi>\n"
	                                                   "%<r>_upper = OpUGreaterThanEqual %bool %<r>_lane %uint_32\n"
	                                                   "%<r>_word = OpSelect %uint %<r>_upper %<r>_hi %<r>_lo")
	                                           : String8("%<r>_word = OpCopyObject %uint %<r>_lo"))
	               .ReplaceStr("<lo>", lo.value).ReplaceStr("<hi>", hi.value)
	               .ReplaceStr("<r>", result);
	return true;
}

String8 Spirv::NativeExecRefresh(const String8& tag) const
{
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	String8 source;
	EXIT_IF(!EmitNativeMaskBit(exec, "mask_exec_" + tag, &source));
	source += String8(R"(
OpStore %exec_lane_lo %mask_exec_<tag>
OpStore %exec_lane_hi %uint_0
%mask_exec_any_<tag> = OpBitwiseOr %uint %mask_exec_<tag>_lo <high>
%mask_exec_empty_<tag> = OpIEqual %bool %mask_exec_any_<tag> %uint_0
%mask_exec_z_<tag> = OpSelect %uint %mask_exec_empty_<tag> %uint_1 %uint_0
OpStore %execz %mask_exec_z_<tag>
)").ReplaceStr("<high>", NativeWave32() ? "%uint_0" : "%mask_exec_<tag>_hi").ReplaceStr("<tag>", tag);
	return source.ReplaceStr("%packed_exec_", "%exec_");
}

String8 Spirv::NativeQuadUniform(const String8& value, const String8& type, const String8& result) const
{
	EXIT_IF(GetHostShaderType() != ShaderType::Pixel);
	// Non-quad collectives can return undefined values in Vulkan helpers.
	// Transfer the real members' uniform result back to every lane of the quad.
	// Constant broadcast indices remain valid with pre-SPIR-V-1.5 toolchains.
	return String8(R"(
%<r>_helper = OpLoad %bool %gl_HelperInvocation
%<r>_real = OpLogicalNot %bool %<r>_helper
%<r>_real0 = OpGroupNonUniformQuadBroadcast %bool %uint_3 %<r>_real %uint_0
%<r>_real1 = OpGroupNonUniformQuadBroadcast %bool %uint_3 %<r>_real %uint_1
%<r>_real2 = OpGroupNonUniformQuadBroadcast %bool %uint_3 %<r>_real %uint_2
%<r>_value0 = OpGroupNonUniformQuadBroadcast %<type> %uint_3 %<v> %uint_0
%<r>_value1 = OpGroupNonUniformQuadBroadcast %<type> %uint_3 %<v> %uint_1
%<r>_value2 = OpGroupNonUniformQuadBroadcast %<type> %uint_3 %<v> %uint_2
%<r>_value3 = OpGroupNonUniformQuadBroadcast %<type> %uint_3 %<v> %uint_3
%<r>_pick23 = OpSelect %<type> %<r>_real2 %<r>_value2 %<r>_value3
%<r>_pick123 = OpSelect %<type> %<r>_real1 %<r>_value1 %<r>_pick23
%<r> = OpSelect %<type> %<r>_real0 %<r>_value0 %<r>_pick123
)").ReplaceStr("<v>", value).ReplaceStr("<type>", type).ReplaceStr("<r>", result);
}

String8 Spirv::NativeMaskBallot(const String8& predicate, const String8& result) const
{
	if (GetHostShaderType() != ShaderType::Pixel)
	{
		return "%" + result + " = OpGroupNonUniformBallot %v4uint %uint_3 %" + predicate + "\n";
	}
	// General subgroup operations may exclude Vulkan helpers. Gather their
	// bits inside the quad first, so every participating real fragment carries
	// its quad's complete guest mask into the wave reduction.
	return String8(R"(
%<r>_lane = OpLoad %uint %gl_SubgroupInvocationID
%<r>_shift = OpBitwiseAnd %uint %<r>_lane %uint_31
%<r>_bit = OpShiftLeftLogical %uint %uint_1 %<r>_shift
%<r>_local = OpSelect %uint %<p> %<r>_bit %uint_0
%<r>_q0 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %<r>_local %uint_0
%<r>_q1 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %<r>_local %uint_1
%<r>_q2 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %<r>_local %uint_2
%<r>_q3 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %<r>_local %uint_3
%<r>_q01 = OpBitwiseOr %uint %<r>_q0 %<r>_q1
%<r>_q23 = OpBitwiseOr %uint %<r>_q2 %<r>_q3
%<r>_quad = OpBitwiseOr %uint %<r>_q01 %<r>_q23
%<r>_upper = OpUGreaterThanEqual %bool %<r>_lane %uint_32
%<r>_low_part = OpSelect %uint %<r>_upper %uint_0 %<r>_quad
%<r>_high_part = OpSelect %uint %<r>_upper %<r>_quad %uint_0
%<r>_low = OpGroupNonUniformBitwiseOr %uint %uint_3 Reduce %<r>_low_part
%<r>_high = OpGroupNonUniformBitwiseOr %uint %uint_3 Reduce %<r>_high_part
%<r>_raw = OpCompositeConstruct %v4uint %<r>_low %<r>_high %uint_0 %uint_0
)").ReplaceStr("<p>", predicate).ReplaceStr("<r>", result) + NativeQuadUniform(result + "_raw", "v4uint", result);
}

String8 Spirv::ResolveMaskAccesses(const ShaderInstruction& inst, uint32_t index, const String8& source) const
{
	String8 result = source;
	if (!UsesComputeWaveBanks())
	{
		const auto name = Core::EnumName8(inst.type);
		const auto tag = String8::FromPrintf("%u", index);
		if (!name.StartsWith("S"))
		{
			// Only implicit EXEC loads have these names. Numeric loads and EXECZ
			// use packed_exec_* until this adapter (or the paired adapter) finishes.
			result = ReplaceMaskToken(result, "OpLoad %uint %exec_lo", "OpLoad %uint %exec_lane_lo");
			result = ReplaceMaskToken(result, "OpLoad %uint %exec_hi", "OpLoad %uint %exec_lane_hi");
		}
		if (inst.type == ShaderInstructionType::VCndmaskB32)
		{
			String8 bit;
			EXIT_IF(!EmitNativeMaskBit(inst.src[2], "t22_" + tag, &bit));
			const auto pointer = inst.src[2].size == 1 ? operand_variable_to_str(inst.src[2]) : operand_variable_to_str(inst.src[2], 0);
			const auto old = "%t22_" + tag + " = OpLoad %uint %" + pointer.value;
			const auto lane_old = ReplaceMaskToken(ReplaceMaskToken(old, "%exec_lo", "%exec_lane_lo"), "%exec_hi", "%exec_lane_hi");
			EXIT_IF(!result.ContainsStr(lane_old));
			result = result.ReplaceStr(lane_old, bit);
		}

		ShaderOperand mask {};
		const bool cmpx = name.StartsWith("VCmpx");
		if (cmpx)
		{
			mask.type = ShaderOperandType::ExecLo;
			mask.size = 2;
		} else if (name.StartsWith("VCmp"))
		{
			mask = inst.dst;
		} else if (inst.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1 ||
		           inst.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2 ||
		           inst.format == ShaderInstructionFormat::Vdst2Sdst2Vsrc0Vsrc1Vsrc2Pair)
		{
			mask = inst.dst2;
		}
		if (mask.type != ShaderOperandType::Unknown)
		{
			const auto lo = mask.size == 1 ? operand_variable_to_str(mask) : operand_variable_to_str(mask, 0);
			const auto hi = mask.size == 2 ? operand_variable_to_str(mask, 1) : SpirvValue {};
			const std::string store = "OpStore %" + std::string(lo.value.c_str()) + " %";
			std::string text(result.c_str());
			const auto begin = text.find(store);
			if (begin != std::string::npos)
			{
				const auto value_begin = begin + store.size() - 1;
				const auto end = text.find_first_of(" \t\r\n", value_begin);
				const auto value = String8(text.substr(value_begin, end - value_begin).c_str());
				// The collective stays outside software-EXEC control flow. Capture
				// incoming EXEC before the first store, including EXEC destinations.
				const auto packed = String8(R"(
%mask_out_exec_<tag> = OpLoad %uint %exec_lane_lo
%mask_out_live_<tag> = OpINotEqual %bool %mask_out_exec_<tag> %uint_0
%mask_out_pred_<tag> = OpINotEqual %bool <value> %uint_0
%mask_out_active_<tag> = OpLogicalAnd %bool %mask_out_live_<tag> %mask_out_pred_<tag>
<ballot>
%mask_out_lo_<tag> = OpCompositeExtract %uint %mask_out_ballot_<tag> 0
%mask_out_hi_<tag> = OpCompositeExtract %uint %mask_out_ballot_<tag> 1
OpStore %<lo> %mask_out_lo_<tag>
<store_hi>
)").ReplaceStr("<ballot>", NativeMaskBallot("mask_out_active_" + tag, "mask_out_ballot_" + tag))
				   .ReplaceStr("<store_hi>", !hi.value.IsEmpty() && !NativeWave32() ? "OpStore %<hi> %mask_out_hi_<tag>" : "")
				   .ReplaceStr("<lo>", lo.value).ReplaceStr("<hi>", hi.value).ReplaceStr("<value>", value).ReplaceStr("<tag>", tag);
				text.replace(begin, end - begin, packed.c_str());
				result = String8(text.c_str());
				if (!hi.value.IsEmpty())
				{
					result = ReplaceMaskToken(result, "OpStore %" + hi.value + " %uint_0", "");
				}
			} else
			{
				// The always-true CMPX is a no-op on EXEC. Every other mask
				// producer must expose its result store to this adapter.
				EXIT_IF(inst.type != ShaderInstructionType::VCmpxTruF32);
			}
		}
		const bool exec_branch = inst.type == ShaderInstructionType::SCbranchExecz || inst.type == ShaderInstructionType::SCbranchExecnz;
		const bool vcc_branch = inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz;
		if (exec_branch || vcc_branch)
		{
			const String8 mask_name = exec_branch ? "exec" : "vcc";
			const auto load = "OpLoad %uint %" + mask_name + "_lo";
			const auto pre = String8("%branch_mask_lo_<tag> = OpLoad %uint %<mask>_lo\n"
			                         "%branch_mask_hi_<tag> = OpLoad %uint %<mask>_hi\n")
			                     .ReplaceStr("<mask>", mask_name).ReplaceStr("<tag>", tag);
			result = pre + result.ReplaceStr(load, String8("OpBitwiseOr %uint %branch_mask_lo_<tag> <high>")
			                                          .ReplaceStr("<high>", NativeWave32() ? "%uint_0" : "%branch_mask_hi_<tag>")
			                                          .ReplaceStr("<tag>", tag));
			// Packed masks are wave-uniform, including in helpers. Re-voting the
			// already uniform decision could make a helper branch on an undefined
			// subgroup result and leave its real quad peers behind.
			result = ReplaceMaskToken(result, "OpGroupNonUniformAny %bool %uint_3 %cc_lane_b_" + tag,
			                          "OpCopyObject %bool %cc_lane_b_" + tag);
		}
		if (cmpx || result.ContainsStr("OpStore %exec_lo ") || result.ContainsStr("OpStore %exec_hi "))
		{
			result += NativeExecRefresh(tag);
		}
	}
	return result.ReplaceStr("%packed_exec_", "%exec_");
}

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
		case ShaderOperandType::VccHi:
			if (op.size == 1 && shift == 0)
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

SpirvValue operand_numeric_variable_to_str(ShaderOperand op, int shift)
{
	auto value = shift >= 0 ? operand_variable_to_str(op, shift) : operand_variable_to_str(op);
	value.value = PackedMaskName(value.value);
	return value;
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

// DPP transforms the source before the ALU operation for every operand type:
// the lane permutation is bit-level and does not depend on float or integer
// interpretation. Only the full-mask quad-permutation subset is admitted.
static bool operand_dpp_supported(const ShaderOperand& op)
{
	return op.type == ShaderOperandType::Vgpr && op.dpp_ctrl <= 0xffu && op.dpp_row_mask == 0xfu &&
	       op.dpp_bank_mask == 0xfu && !op.dpp_fetch_inactive && op.swizzle == 6u;
}

// Row-class DPP controls (ISA table 91) the neutral-region tier lowers to an
// OpGroupNonUniformShuffle. The guest wave is wider than the host subgroup;
// lanes past SubgroupSize are the ghost lanes the proven region zeroed, and an
// in-bound shift reads them as zero. A shift leaving the row with bound_ctrl=0
// does not write on hardware, which the caller's ALU reproduces only for
// or/xor against the destination; the tier analyzer admits just that shape.
// A wave32 program has no ghost lanes, so out-of-row DPP sources only need the
// architectural bound_ctrl behaviour: bound_ctrl=1 writes the fetched-or-zero
// value, while bound_ctrl=0 must leave the destination untouched. The lowering
// substitutes zero for out-of-range sources, which preserves the destination
// only through a zero-identity op whose other operand is the destination.
static bool fragment_row_dpp_wave32_keeps_dst(const ShaderCode& code, uint32_t index, const ShaderOperand& dpp_source)
{
	if (dpp_source.dpp_bound_ctrl)
	{
		return true;
	}
	if (index >= code.GetInstructions().Size())
	{
		return false;
	}
	const auto& inst = code.GetInstructions().At(index);
	if (inst.type != ShaderInstructionType::VOrB32 && inst.type != ShaderInstructionType::VXorB32 &&
	    inst.type != ShaderInstructionType::VAddI32)
	{
		return false;
	}
	if (inst.dst.type != ShaderOperandType::Vgpr || inst.src_num != 2)
	{
		return false;
	}
	for (int source = 0; source < inst.src_num; ++source)
	{
		const auto& src = inst.src[source];
		if (src.dpp)
		{
			continue;
		}
		if (src.type == ShaderOperandType::Vgpr && src.register_id == inst.dst.register_id)
		{
			return true;
		}
	}
	return false;
}

static bool fragment_row_dpp_supported(const Spirv& spirv, const ShaderOperand& op, const String8& index)
{
	if (op.type != ShaderOperandType::Vgpr || op.dpp_row_mask != 0xfu || op.dpp_bank_mask != 0xfu || op.swizzle != 6u)
	{
		return false;
	}
	const uint32_t control = op.dpp_ctrl;
	const bool     row     = (control >= 0x101u && control <= 0x10fu) || (control >= 0x111u && control <= 0x11fu) ||
	                         (control >= 0x121u && control <= 0x12fu) || control == 0x140u || control == 0x141u;
	if (!row || (op.dpp_bound_ctrl && control < 0x121u))
	{
		return false;
	}
	for (const char* c = index.c_str(); *c != '\0'; ++c)
	{
		if (*c < '0' || *c > '9')
		{
			return false;
		}
	}
	const uint32_t inst_index = index.ToUint32();
	if (ShaderFragmentWaveInsideRegion(spirv.GetCode(), inst_index))
	{
		return true;
	}
	const auto* pixel = spirv.GetPsInputInfo();
	return pixel != nullptr && pixel->required_subgroup_size == 32u &&
	       fragment_row_dpp_wave32_keeps_dst(spirv.GetCode(), inst_index, op);
}

// Emits the row-class lane permutation for an already-loaded operand inside a
// proven neutral region (full EXEC). The clamp keeps the shuffle index inside
// the subgroup; the select substitutes the neutral zero for ghost lanes and
// for out-of-row shifts, which the keep-old ALU shape turns back into dst.
// With exec_guard (a wave32 op outside a proven region, where EXEC may be
// partial) FI=0 also zeroes sources fetched from EXEC-inactive lanes.
static bool operand_dpp_row_permute_uint(Spirv* spirv, const ShaderOperand& op, const String8& result_id,
                                         const String8& input_id, bool input_is_uint, bool exec_guard,
                                         String8* text)
{
	String8 head = input_is_uint ? String8("\n        %dpp_bits_<result> = OpCopyObject %uint %<in>\n")
	                             : String8("\n        %dpp_bits_<result> = OpBitcast %uint %<in>\n");
	static const char* permutation = R"(      %dpp_lane_<result> = OpLoad %uint %gl_SubgroupInvocationID
      %dpp_ssize_<result> = OpLoad %uint %gl_SubgroupSize
      %dpp_last_<result> = OpISub %uint %dpp_ssize_<result> %uint_1
     %dpp_local_<result> = OpBitwiseAnd %uint %dpp_lane_<result> %<lane_15>
<target>
     %dpp_oob_<result> = OpULessThan %bool %dpp_target_<result> %dpp_ssize_<result>
       %dpp_ok_<result> = OpLogicalAnd %bool %dpp_bounds_<result> %dpp_oob_<result>
  %dpp_clamped_<result> = OpExtInst %uint %GLSL_std_450 UMin %dpp_target_<result> %dpp_last_<result>
<exec_guard>    %dpp_fetch_<result> = OpGroupNonUniformShuffle %uint %uint_3 %dpp_bits_<result> %dpp_clamped_<result>
    %dpp_value_<result> = OpSelect %uint <ok> %dpp_fetch_<result> %<zero>
)";
	// FI=0 substitutes zero when the fetched source lane is EXEC-inactive.
	// %exec_lo holds this lane's bit; shuffling it by the source lane index
	// yields the source lane's bit, mirroring %wave_dpp_source_exec in the
	// banked compute path.
	static const char* exec_guard_text =
	    R"( %dpp_exlo_<result> = OpLoad %uint %exec_lo
 %dpp_srclive_<result> = OpGroupNonUniformShuffle %uint %uint_3 %dpp_exlo_<result> %dpp_clamped_<result>
    %dpp_srcon_<result> = OpINotEqual %bool %dpp_srclive_<result> %<zero>
      %dpp_okg_<result> = OpLogicalAnd %bool %dpp_ok_<result> %dpp_srcon_<result>
)";
	const uint32_t control = op.dpp_ctrl;
	String8          target;
	if (control == 0x140u || control == 0x141u)
	{
		target = String8("    %dpp_target_<result> = OpBitwiseXor %uint %dpp_lane_<result> %<mirror>\n"
		                 "    %dpp_bounds_<result> = OpCopyObject %bool %true\n")
		             .ReplaceStr("<mirror>", spirv->GetConstantUint(control == 0x140u ? 15u : 7u));
	} else
	{
		const bool left   = control <= 0x10fu;
		const bool rotate = control >= 0x121u;
		target = String8(
		             "     %dpp_base_<result> = OpBitwiseAnd %uint %dpp_lane_<result> %<row_mask>\n"
		             "   %dpp_offset_<result> = <operation> %uint %dpp_local_<result> %<step>\n"
		             "  %dpp_wrapped_<result> = OpBitwiseAnd %uint %dpp_offset_<result> %<lane_15>\n"
		             "   %dpp_target_<result> = OpBitwiseOr %uint %dpp_base_<result> %dpp_wrapped_<result>\n")
		             .ReplaceStr("<operation>", left ? "OpIAdd" : "OpISub");
		target += rotate     ? "    %dpp_bounds_<result> = OpCopyObject %bool %true\n"
		          : left     ? "    %dpp_bounds_<result> = OpULessThan %bool %dpp_offset_<result> %<lane_16>\n"
		                     : "    %dpp_bounds_<result> = OpUGreaterThanEqual %bool %dpp_local_<result> %<step>\n";
	}
	*text = (head + String8(permutation))
	            .ReplaceStr("<target>", target)
	            .ReplaceStr("<exec_guard>", exec_guard ? String8(exec_guard_text) : String8())
	            .ReplaceStr("<ok>", exec_guard ? String8("%dpp_okg_<result>") : String8("%dpp_ok_<result>"))
	            .ReplaceStr("<result>", result_id)
	            .ReplaceStr("<in>", input_id)
	            .ReplaceStr("<row_mask>", spirv->GetConstantUint(0xfffffff0u))
	            .ReplaceStr("<lane_15>", spirv->GetConstantUint(15u))
	            .ReplaceStr("<lane_16>", spirv->GetConstantUint(16u))
	            .ReplaceStr("<step>", spirv->GetConstantUint(control & 15u))
	            .ReplaceStr("<zero>", spirv->GetConstantUint(0u));
	return true;
}

// Emits the lane permutation for an already-loaded operand. Produces
// %dpp_value_<result> as %uint read from the %<in> source id.
static bool operand_dpp_permute_uint(Spirv* spirv, const ShaderOperand& op, const String8& result_id,
                                     const String8& input_id, bool input_is_uint, const String8& index, String8* text)
{
	if (!operand_dpp_supported(op))
	{
		if (spirv->GetHostShaderType() != ShaderType::Pixel || !fragment_row_dpp_supported(*spirv, op, index))
		{
			return false;
		}
		// Outside a proven region EXEC may be partial; FI=0 must then zero
		// sources fetched from EXEC-inactive lanes. Inside a proven region
		// EXEC is full and the extra shuffle would be a no-op.
		const bool exec_guard = !op.dpp_fetch_inactive &&
		                        !ShaderFragmentWaveInsideRegion(spirv->GetCode(), index.ToUint32());
		return operand_dpp_row_permute_uint(spirv, op, result_id, input_id, input_is_uint, exec_guard, text);
	}
	const auto control = op.dpp_ctrl;
	const auto ctrl    = spirv->GetConstantUint(control);

	String8 head = input_is_uint ? String8("\n        %dpp_bits_<result> = OpCopyObject %uint %<in>\n")
	                             : String8("\n        %dpp_bits_<result> = OpBitcast %uint %<in>\n");
	static const char* permutation = R"(        %dpp_lane_<result> = OpLoad %uint %gl_SubgroupInvocationID
       %dpp_local_<result> = OpBitwiseAnd %uint %dpp_lane_<result> %<three>
       %dpp_shift_<result> = OpShiftLeftLogical %uint %dpp_local_<result> %<one>
       %dpp_table_<result> = OpShiftRightLogical %uint %<ctrl> %dpp_shift_<result>
      %dpp_select_<result> = OpBitwiseAnd %uint %dpp_table_<result> %<three>
<exchange>
)";
	if (!spirv->UsesComputeWaveBanks())
	{
		// FI=0 filters the source image before routing, not the destination.
		// QuadBroadcast still supplies native fragment helper participation.
		head = head.ReplaceStr("%dpp_bits_<result>", "%dpp_unfiltered_<result>");
		head += R"(%dpp_source_exec_<result> = OpLoad %uint %exec_lo
%dpp_source_on_<result> = OpINotEqual %bool %dpp_source_exec_<result> %uint_0
%dpp_bits_<result> = OpSelect %uint %dpp_source_on_<result> %dpp_unfiltered_<result> %uint_0
)";
	}
	// Fragment quad operations keep helper invocations participating. A general
	// shuffle may treat them as inactive, losing values needed at primitive edges.
	// Constant broadcast indices also work with pre-SPIR-V-1.5 toolchains.
	String8 exchange;
	if (spirv->GetHostShaderType() == ShaderType::Pixel)
	{
		uint32_t selected_lanes = 0;
		for (uint32_t lane = 0; lane < 4; ++lane)
		{
			selected_lanes |= 1u << ((control >> (lane * 2u)) & 3u);
		}
		String8 selected;
		for (uint32_t lane = 0; lane < 4; ++lane)
		{
			if ((selected_lanes & (1u << lane)) == 0u) { continue; }
			const auto suffix = String8::FromPrintf("%u", lane);
			const auto value = "%dpp_quad" + suffix + "_<result>";
			exchange += value + " = OpGroupNonUniformQuadBroadcast %uint %uint_3 %dpp_bits_<result> %" +
			            spirv->GetConstantUint(lane) + "\n";
			if (selected.IsEmpty())
			{
				selected = value;
			} else
			{
				const auto condition = "%dpp_is" + suffix + "_<result>";
				const auto pick = "%dpp_pick" + suffix + "_<result>";
				exchange += condition + " = OpIEqual %bool %dpp_select_<result> %" + spirv->GetConstantUint(lane) + "\n";
				exchange += pick + " = OpSelect %uint " + condition + " " + value + " " + selected + "\n";
				selected = pick;
			}
		}
		exchange += "%dpp_value_<result> = OpCopyObject %uint " + selected + "\n";
	} else
	{
		exchange = R"(
        %dpp_base_<result> = OpBitwiseAnd %uint %dpp_lane_<result> %<quad_mask>
      %dpp_target_<result> = OpBitwiseOr %uint %dpp_base_<result> %dpp_select_<result>
       %dpp_value_<result> = OpGroupNonUniformShuffle %uint %uint_3 %dpp_bits_<result> %dpp_target_<result>
)";
		if (spirv->UsesComputeWaveBanks())
		{
			// FI=0 substitutes zero for an inactive source before ALU modifiers.
			// The bank rewriter resolves this mask alias to the architectural
			// word, independently of its ordinary per-destination EXEC loads.
			exchange = exchange.ReplaceStr("%dpp_value_<result> =", "%dpp_fetched_<result> =");
			exchange += R"(
    %dpp_exec_word_<result> = OpLoad %uint %wave_dpp_source_exec
     %dpp_exec_bit_<result> = OpShiftLeftLogical %uint %<one> %dpp_target_<result>
    %dpp_exec_mask_<result> = OpBitwiseAnd %uint %dpp_exec_word_<result> %dpp_exec_bit_<result>
  %dpp_source_live_<result> = OpINotEqual %bool %dpp_exec_mask_<result> %<zero>
       %dpp_value_<result> = OpSelect %uint %dpp_source_live_<result> %dpp_fetched_<result> %<zero>
)";
		}
	}
	*text = (head + String8(permutation))
	                     .ReplaceStr("<exchange>", exchange)
	                     .ReplaceStr("<result>", result_id)
	                     .ReplaceStr("<in>", input_id)
	                     .ReplaceStr("<ctrl>", ctrl)
	                     .ReplaceStr("<quad_mask>", spirv->GetConstantUint(0xfffffffcu))
	                     .ReplaceStr("<zero>", spirv->GetConstantUint(0u))
	                     .ReplaceStr("<one>", spirv->GetConstantUint(1u))
	                     .ReplaceStr("<two>", spirv->GetConstantUint(2u))
	                     .ReplaceStr("<three>", spirv->GetConstantUint(3u));
	return true;
}

bool operand_load_int(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load)
{
	EXIT_IF(load == nullptr);

	if (op.dpp)
	{
		if (!operand_dpp_supported(op) && !fragment_row_dpp_supported(*spirv, op, index))
		{
			return false;
		}
		const auto permute_op = op;
		op.dpp              = false;
		String8 source;
		String8 permuted;
		if (!operand_load_int(spirv, op, "dpp_input_" + result_id, index, &source) ||
		    !operand_dpp_permute_uint(spirv, permute_op, result_id, "dpp_input_" + result_id, false, index, &permuted))
		{
			return false;
		}
		*load = source + permuted +
		        String8("                 %<result> = OpBitcast %int %dpp_value_<result>\n").ReplaceStr("<result>", result_id);
		return true;
	}

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
		auto value = operand_numeric_variable_to_str(op);

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
	if (op.dpp)
	{
		if (!operand_dpp_supported(op) && !fragment_row_dpp_supported(*spirv, op, index))
		{
			return false;
		}
		const auto permute_op = op;
		op.dpp              = false;
		String8 source;
		String8 permuted;
		if (!operand_load_uint(spirv, op, "dpp_input_" + result_id, index, &source, shift) ||
		    !operand_dpp_permute_uint(spirv, permute_op, result_id, "dpp_input_" + result_id, true, index, &permuted))
		{
			return false;
		}
		*load = source + permuted +
		        String8("                 %<result> = OpCopyObject %uint %dpp_value_<result>\n").ReplaceStr("<result>", result_id);
		return true;
	}
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
		*load = String8(R"(%vccz_lo_<result_id> = OpLoad %uint %vcc_lo
%vccz_hi_<result_id> = OpLoad %uint %vcc_hi
%vccz_or_<result_id> = OpBitwiseOr %uint %vccz_lo_<result_id> <high>
%vccz_bool_<result_id> = OpIEqual %bool %vccz_or_<result_id> %uint_0
%<result_id> = OpSelect %uint %vccz_bool_<result_id> %uint_1 %uint_0)")
		            .ReplaceStr("<high>", spirv->NativeWave32() ? "%uint_0" : "%vccz_hi_<result_id>")
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
		auto value = operand_numeric_variable_to_str(op, shift);

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
	if (op.dpp)
	{
		// DPP transforms source zero before the ALU operation, not only V_MOV.
		// Keep the full-mask quad-permutation subset shared by all consumers.
		if (!operand_dpp_supported(op) && !fragment_row_dpp_supported(*spirv, op, index))
		{
			return false;
		}
		const auto permute_op = op;
		op.dpp = false;
		op.negate = false;
		op.absolute = false;
		String8 source;
		String8 permuted;
		if (!operand_load_float(spirv, op, "dpp_input_" + result_id, index, &source) ||
		    !operand_dpp_permute_uint(spirv, permute_op, result_id, "dpp_input_" + result_id, false, index, &permuted))
		{
			return false;
		}
		String8 modifiers = "%dpp_float_<result> = OpBitcast %float %dpp_value_<result>\n";
		String8 value = "%dpp_float_<result>";
		if (permute_op.absolute)
		{
			modifiers += "%dpp_abs_<result> = OpExtInst %float %GLSL_std_450 FAbs " + value + "\n";
			value = "%dpp_abs_<result>";
		}
		modifiers += "%<result> = " + String8(permute_op.negate ? "OpFNegate" : "OpCopyObject") + " %float " + value + "\n";
		*load = source + permuted + modifiers.ReplaceStr("<result>", result_id);
		return true;
	}
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
		auto value = operand_numeric_variable_to_str(op);

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
