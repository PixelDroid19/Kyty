#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvTemplates.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/VulkanVertexInputFormat.h"
#include "Emulator/Log.h"

#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool FragmentTapSelection(const ShaderCode& code, uint32_t* pc, int* first_register)
{
	if (pc == nullptr || first_register == nullptr || code.GetType() != ShaderType::Pixel)
	{
		return false;
	}
	const char* selector = std::getenv("KYTY_FS_TAP");
	if (selector == nullptr || selector[0] == '\0')
	{
		return false;
	}
	char*          id_end = nullptr;
	const uint64_t id     = std::strtoull(selector, &id_end, 16);
	if (id_end == selector || *id_end != ':')
	{
		return false;
	}
	char*          pc_end = nullptr;
	const uint64_t parsed_pc = std::strtoull(id_end + 1, &pc_end, 0);
	if (pc_end == id_end + 1 || *pc_end != '\0' || parsed_pc > UINT32_MAX)
	{
		return false;
	}
	const uint64_t code_id = (static_cast<uint64_t>(code.GetHash0()) << 32u) | code.GetCrc32();
	if (id != code_id)
	{
		return false;
	}
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.pc == parsed_pc && inst.dst.type == ShaderOperandType::Vgpr && inst.dst.size > 0)
		{
			static bool logged = false;
			if (!logged)
			{
				logged = true;
				KYTY_LOG_DEBUG( "KYTY_FS_TAP_SELECTED id=0x%016" PRIx64 " pc=%u vgpr=%d\n", code_id,
				             static_cast<uint32_t>(parsed_pc), inst.dst.register_id);
			}
			*pc             = static_cast<uint32_t>(parsed_pc);
			*first_register = inst.dst.register_id;
			return true;
		}
	}
	return false;
}

static int ResolveVertexParameterCount(const ShaderCode& code, const ShaderVertexInputInfo* input_info)
{
	int count = input_info != nullptr ? input_info->export_count : 0;
	for (const auto& inst: code.GetInstructions())
	{
		int required = 0;
		switch (inst.format)
		{
			case ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3: required = 1; break;
			case ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3: required = 2; break;
			case ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3: required = 3; break;
			case ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3: required = 4; break;
			case ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3: required = 5; break;
			case ShaderInstructionFormat::Param5Vsrc0Vsrc1Vsrc2Vsrc3: required = 6; break;
			case ShaderInstructionFormat::Param6Vsrc0Vsrc1Vsrc2Vsrc3: required = 7; break;
			case ShaderInstructionFormat::Param7Vsrc0Vsrc1Vsrc2Vsrc3: required = 8; break;
			default: break;
		}
		if (required > count)
		{
			count = required;
		}
	}
	return count;
}

static bool ShaderCodeHasDiscardTail(const ShaderCode& code)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = 1; index + 1 < instructions.Size(); ++index)
	{
		const auto& previous = instructions.At(index - 1);
		const auto& current  = instructions.At(index);
		const auto& next     = instructions.At(index + 1);
		if (previous.type == ShaderInstructionType::SMovB64 && previous.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
		    previous.dst.type == ShaderOperandType::ExecLo && previous.src[0].type == ShaderOperandType::IntegerInlineConstant &&
		    previous.src[0].constant.i == 0 && current.type == ShaderInstructionType::Exp && ShaderIsNullMrtDoneFormat(current.format) &&
		    next.type == ShaderInstructionType::SEndpgm)
		{
			return true;
		}
	}
	return false;
}

void Spirv::GenerateSource()
{
	m_source.Clear();

	switch (m_code.GetType())
	{
		case ShaderType::Pixel: m_bind = (m_ps_input_info != nullptr ? &m_ps_input_info->bind : nullptr); break;
		case ShaderType::Vertex: m_bind = (m_vs_input_info != nullptr ? &m_vs_input_info->bind : nullptr); break;
		case ShaderType::Compute: m_bind = (m_cs_input_info != nullptr ? &m_cs_input_info->bind : nullptr); break;
		default: m_bind = nullptr; break;
	}

	if (m_vs_input_info != nullptr)
	{
		if (m_vs_input_info->fetch_embedded || m_vs_input_info->fetch_inline)
		{
			DetectFetch();
		}
	}
	if (m_code.GetType() == ShaderType::Pixel && m_ps_input_info != nullptr)
	{
		if (!ResolvePixelInterpolationModes())
		{
			const uint64_t code_id = (static_cast<uint64_t>(m_code.GetHash0()) << 32u) | m_code.GetCrc32();
			KYTY_LOG_DEBUG(
			             "SHADER_INTERPOLATION_REJECT_ID id=0x%016" PRIx64 " input_num=%u ena=0x%08" PRIx32
			             " addr=0x%08" PRIx32 "\n",
			             code_id, m_ps_input_info->input_num, m_ps_input_info->system_input_enable,
			             m_ps_input_info->system_input_address);
			if (true) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: true condition ignored (continuing)\n"); }
		}
	}

	WriteHeader();
	WriteDebug();
	WriteAnnotations();
	WriteTypes();
	WriteConstants();
	WriteGlobalVariables();
	WriteMainProlog();
	WriteLocalVariables();
	WriteInstructions();
	WriteMainEpilog();
	WriteFunctions();
}

static bool spirv_uses_dpp(const ShaderCode& code)
{
	for (const auto& inst: code.GetInstructions())
	{
		for (int source = 0; source < inst.src_num; source++)
		{
			if (inst.src[source].dpp)
			{
				return true;
			}
		}
	}
	return false;
}

static bool spirv_uses_buffer_descriptor_addressing(const ShaderCode& code)
{
	if (!Config::IsNextGen())
	{
		return false;
	}
	return code.HasAnyOf({ShaderInstructionType::BufferLoadUbyte, ShaderInstructionType::BufferLoadDword,
	                      ShaderInstructionType::BufferLoadDwordx2, ShaderInstructionType::BufferLoadDwordx3,
	                      ShaderInstructionType::BufferLoadDwordx4, ShaderInstructionType::BufferLoadFormatX,
	                      ShaderInstructionType::BufferLoadFormatXy, ShaderInstructionType::BufferLoadFormatXyz,
	                      ShaderInstructionType::BufferLoadFormatXyzw, ShaderInstructionType::BufferStoreDword,
	                      ShaderInstructionType::BufferStoreDwordx2, ShaderInstructionType::BufferStoreDwordx3,
	                      ShaderInstructionType::BufferStoreDwordx4, ShaderInstructionType::BufferStoreFormatX,
	                      ShaderInstructionType::BufferStoreFormatXy, ShaderInstructionType::BufferStoreFormatXyzw,
	                      ShaderInstructionType::BufferAtomicAdd,
	                      ShaderInstructionType::TBufferLoadFormatX, ShaderInstructionType::TBufferLoadFormatXy,
	                      ShaderInstructionType::TBufferLoadFormatXyzw});
}

static bool spirv_uses_mbcnt(const ShaderCode& code)
{
	return code.GetType() == ShaderType::Pixel &&
	       code.HasAnyOf({ShaderInstructionType::VMbcntLoU32B32, ShaderInstructionType::VMbcntHiU32B32});
}

static bool spirv_uses_buffer_atomics(const ShaderCode& code)
{
	return Config::IsNextGen() && code.HasAnyOf({ShaderInstructionType::BufferAtomicAdd});
}

// FP64 (double) is used when the shader has f64 ALU or 64-bit float compares.
static bool spirv_uses_f64(const ShaderCode& code)
{
	return code.HasAnyOf({ShaderInstructionType::VCmpFF64, ShaderInstructionType::VCmpLtF64, ShaderInstructionType::VCmpEqF64,
	                      ShaderInstructionType::VCmpLeF64, ShaderInstructionType::VCmpGtF64, ShaderInstructionType::VCmpLgF64,
	                      ShaderInstructionType::VCmpGeF64, ShaderInstructionType::VCmpTruF64, ShaderInstructionType::VCmpNgeF64,
	                      ShaderInstructionType::VCmpNlgF64, ShaderInstructionType::VCmpNgtF64, ShaderInstructionType::VCmpNleF64,
	                      ShaderInstructionType::VCmpNeqF64, ShaderInstructionType::VCmpNltF64, ShaderInstructionType::VCmpOF64,
	                      ShaderInstructionType::VCmpUF64, ShaderInstructionType::VCvtF64F32, ShaderInstructionType::VCvtF32F64,
	                      ShaderInstructionType::VCvtI32F64, ShaderInstructionType::VCvtU32F64, ShaderInstructionType::VCvtF64I32,
	                      ShaderInstructionType::VCvtF64U32, ShaderInstructionType::VAddF64, ShaderInstructionType::VSubF64,
	                      ShaderInstructionType::VMulF64, ShaderInstructionType::VSqrtF64, ShaderInstructionType::VMinF64,
	                      ShaderInstructionType::VMaxF64, ShaderInstructionType::VFmaF64});
}

// FP16 (half) is used when the shader has f16 ALU or 16-bit float compares.
static bool spirv_uses_f16(const ShaderCode& code)
{
	return code.HasAnyOf({ShaderInstructionType::VCmpFF16, ShaderInstructionType::VCmpLtF16, ShaderInstructionType::VCmpEqF16,
	                      ShaderInstructionType::VCmpLeF16, ShaderInstructionType::VCmpGtF16, ShaderInstructionType::VCmpLgF16,
	                      ShaderInstructionType::VCmpGeF16, ShaderInstructionType::VCmpTruF16, ShaderInstructionType::VCmpNgeF16,
	                      ShaderInstructionType::VCmpNlgF16, ShaderInstructionType::VCmpNgtF16, ShaderInstructionType::VCmpNleF16,
	                      ShaderInstructionType::VCmpNeqF16, ShaderInstructionType::VCmpNltF16, ShaderInstructionType::VCmpOF16,
	                      ShaderInstructionType::VCmpUF16, ShaderInstructionType::VAddF16, ShaderInstructionType::VSubF16,
	                      ShaderInstructionType::VMulF16, ShaderInstructionType::VMinF16, ShaderInstructionType::VMaxF16,
	                      ShaderInstructionType::VMadF16, ShaderInstructionType::VFmaF16, ShaderInstructionType::VCvtF16F32,
	                      ShaderInstructionType::VCvtF32F16, ShaderInstructionType::VTruncF16, ShaderInstructionType::VCeilF16,
	                      ShaderInstructionType::VFloorF16, ShaderInstructionType::VRndneF16, ShaderInstructionType::VSqrtF16,
	                      ShaderInstructionType::VRcpF16, ShaderInstructionType::VRsqF16, ShaderInstructionType::VLogF16,
	                      ShaderInstructionType::VExpF16, ShaderInstructionType::VSinF16, ShaderInstructionType::VCosF16});
}

// 16-bit integer ALU (i16/u16) without native Int16 capability, operated in
// 32-bit registers with truncation/extension.
static bool spirv_uses_i16(const ShaderCode& code)
{
	const auto& insts = code.GetInstructions();
	for (uint32_t i = 0; i < insts.Size(); i++)
	{
		const auto type = static_cast<int>(insts.At(i).type);
		if ((type >= static_cast<int>(ShaderInstructionType::VCmpLtI16) && type <= static_cast<int>(ShaderInstructionType::VCmpGeI16)) ||
		    (type >= static_cast<int>(ShaderInstructionType::VCmpLtU16) && type <= static_cast<int>(ShaderInstructionType::VCmpGeU16)))
		{
			return true;
		}
	}
	return code.HasAnyOf({ShaderInstructionType::VAddI16, ShaderInstructionType::VSubI16, ShaderInstructionType::VMulI16,
	                      ShaderInstructionType::VMadI16, ShaderInstructionType::VCvtI16F16, ShaderInstructionType::VCvtU16F16,
	                      ShaderInstructionType::VCvtF16I16, ShaderInstructionType::VCvtF16U16});
}

static bool spirv_uses_readfirstlane(const ShaderCode& code)
{
	return code.HasAnyOf({ShaderInstructionType::VReadfirstlaneB32});
}

static bool spirv_uses_lane_exchange(const ShaderCode& code)
{
	return UsesNativeLaneExchange(code);
}

static bool spirv_uses_wave_branch_vote(const ShaderCode& code)
{
	if (code.HasAnyOf({ShaderInstructionType::SCbranchExecz}))
	{
		return true;
	}
	for (uint32_t index = 0; index < code.GetInstructions().Size(); index++)
	{
		const auto type = code.GetInstructions().At(index).type;
		if ((type == ShaderInstructionType::SCbranchVccz || type == ShaderInstructionType::SCbranchVccnz) &&
		    !ShaderVccBranchIsWaveUniform(code, index))
		{
			return true;
		}
	}
	return false;
}

static bool spirv_uses_subgroup_invocation(const ShaderCode& code)
{
	return spirv_uses_dpp(code) || spirv_uses_buffer_descriptor_addressing(code) || spirv_uses_readfirstlane(code) ||
	       spirv_uses_lane_exchange(code) || spirv_uses_mbcnt(code);
}

void Spirv::WriteHeader()
{
	static const char* header = R"(
                ; Header
                OpCapability Shader
                OpCapability ImageQuery
				<Capabilities>
                <Extensions>
                <Imports>
                OpMemoryModel Logical GLSL450
                OpEntryPoint <Type> %main "main" <Variables>
                <ExecutionModes>
)";

	String8 header_str;

	Core::StringList8 vars;
	Core::StringList8 capabilities;
	Core::StringList8 extensions;
	Core::StringList8 imports;
	Core::StringList8 execution_modes;

	imports.Add("%GLSL_std_450 = OpExtInstImport \"GLSL.std.450\"");

	if (Config::SpirvDebugPrintfEnabled())
	{
		extensions.Add("OpExtension \"SPV_KHR_non_semantic_info\"");
		imports.Add("%NonSemantic_DebugPrintf = OpExtInstImport \"NonSemantic.DebugPrintf\"");
	}

	if (spirv_uses_subgroup_invocation(m_code) || spirv_uses_wave_branch_vote(m_code))
	{
		capabilities.Add("OpCapability GroupNonUniform");
		if (spirv_uses_dpp(m_code) || spirv_uses_lane_exchange(m_code))
		{
			capabilities.Add("OpCapability GroupNonUniformShuffle");
		}
		if (spirv_uses_readfirstlane(m_code))
		{
			capabilities.Add("OpCapability GroupNonUniformBallot");
		}
		if (spirv_uses_wave_branch_vote(m_code))
		{
			capabilities.Add("OpCapability GroupNonUniformVote");
		}
		if (spirv_uses_mbcnt(m_code))
		{
			capabilities.Add("OpCapability GroupNonUniformArithmetic");
		}
		if (spirv_uses_subgroup_invocation(m_code))
		{
			vars.Add("%gl_SubgroupInvocationID");
		}
	}

	if (spirv_uses_f64(m_code))
	{
		capabilities.Add("OpCapability Float64");
		// The f64 lowering uses a 64-bit integer bitcast as its transport type.
		// Keep Int64 paired with Float64 so validators see the capability required
		// by the generated OpBitcast instructions.
		capabilities.Add("OpCapability Int64");
	}
	if (spirv_uses_f16(m_code))
	{
		capabilities.Add("OpCapability Float16");
	}
	if (spirv_uses_i16(m_code))
	{
		capabilities.Add("OpCapability Int16");
	}

	if (m_bind != nullptr)
	{
		if (UsesFormatlessStorageImages(m_bind))
		{
			capabilities.Add("OpCapability StorageImageReadWithoutFormat");
			capabilities.Add("OpCapability StorageImageWriteWithoutFormat");
		}
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			vars.Add("%buf");
			if (spirv_uses_buffer_atomics(m_code))
			{
				vars.Add("%buf_uint");
			}
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			vars.Add("%textures2D_S");
			// U-named descriptors are consumed by the uint image_load/sample
			// paths, which run for mixed and uint-only shaders alike.
			if (UsesMixedSampledImageNumericTypes(m_bind) || UsesUnsignedIntegerImages(m_bind))
			{
				vars.Add("%textures2D_U");
			}
		}
		if (m_bind->textures2D.textures2d_array_sampled_num > 0)
		{
			vars.Add("%textures2DA_S");
			if (UsesMixedSampledImageNumericTypes(m_bind) || UsesUnsignedIntegerImages(m_bind))
			{
				vars.Add("%textures2DA_U");
			}
		}
		if (m_bind->textures2D.textures3d_sampled_num > 0)
		{
			vars.Add("%textures3D_S");
			if (UsesMixedSampledImageNumericTypes(m_bind) || UsesUnsignedIntegerImages(m_bind))
			{
				vars.Add("%textures3D_U");
			}
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			vars.Add("%textures2D_L");
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			vars.Add("%samplers");
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			vars.Add("%gds");
		}
		if (m_bind->push_constant_size > 0)
		{
			vars.Add("%vsharp");
		}
	}

	switch (m_code.GetType())
	{
		case ShaderType::Pixel:
			// Location 0 always uses %outColor (legacy name). Additional RTs that
			// have a non-zero target_output_mode are declared as %outColorN.
			vars.Add("%outColor");
			if (m_ps_input_info != nullptr)
			{
				for (int rt = 1; rt < 8; rt++)
				{
					if (m_ps_input_info->target_output_mode[rt] != 0)
					{
						vars.Add(String8::FromPrintf("%%outColor%d", rt));
					}
				}
				for (uint32_t i = 0; i < m_ps_input_info->input_num; i++)
				{
					if (ShaderPixelCanonicalInterpolator(*m_ps_input_info, i) == i)
					{
						ShaderPixelInterpolator interpolator {};
						if (!ShaderDecodePixelInterpolator(m_ps_input_info->interpolator_settings[i], &interpolator)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderDecodePixelInterpolator(m_ps_input_info->interpolator_settings[i], &interpolator) condition ignored (continuing)\n"); }
						if (interpolator.source == ShaderPixelInterpolatorSource::Parameter)
						{
							vars.Add(String8::FromPrintf("%%attr%d", i));
						}
					}
				}
				if (m_ps_input_info->ps_pos_xy)
				{
					vars.Add("%gl_FragCoord");
				}
				// Guest EarlyZThenLateZ may reject occluded fragments early, but
				// depth is committed after shader kill. Vulkan EarlyFragmentTests
				// alone commits depth before OpKill, so use late tests for shaders
				// that can discard.
				if (m_ps_input_info->ps_early_z && !m_ps_input_info->ps_pixel_kill_enable && !ShaderCodeHasDiscardTail(m_code))
				{
					execution_modes.Add("OpExecutionMode %main EarlyFragmentTests\n");
				}
			}
			header_str = String8(header).ReplaceStr("<Type>", "Fragment");
			execution_modes.Add("OpExecutionMode %main OriginUpperLeft\n");
			// TODO() do we need PixelCenterInteger mode?
			break;
		case ShaderType::Vertex:
			if (m_vs_input_info != nullptr)
			{
				for (int i = 0; i < m_vs_input_info->resources_num; i++)
				{
					vars.Add(String8::FromPrintf("%%attr%d", i));
				}
			}
			for (int i = 0; i < ResolveVertexParameterCount(m_code, m_vs_input_info); i++)
			{
				vars.Add(String8::FromPrintf("%%param%d", i));
			}
			vars.Add("%gl_VertexIndex");
			vars.Add("%gl_InstanceIndex");
			vars.Add("%outPerVertex");
			// vars.Add("%param0");
			header_str = String8(header).ReplaceStr("<Type>", "Vertex");
			break;
		case ShaderType::Compute:
			if (m_cs_input_info != nullptr)
			{
				execution_modes.Add(String8::FromPrintf("OpExecutionMode %%main LocalSize %u %u %u", m_cs_input_info->threads_num[0],
				                                        m_cs_input_info->threads_num[1], m_cs_input_info->threads_num[2]));
				if (m_cs_input_info->lds_dwords > 0)
				{
					vars.Add("%lds");
				}
			}
			vars.Add("%gl_LocalInvocationID");
			vars.Add("%gl_WorkGroupID");
			header_str = String8(header).ReplaceStr("<Type>", "GLCompute");
			break;
		default: KYTY_LOG_DEBUG("WARNING: unknown shader type (continuing)\n"); return;
	}

	m_source += header_str.ReplaceStr("<Variables>", vars.Concat(' '))
	                .ReplaceStr("<Capabilities>", capabilities.Concat("\n" + String8(' ', 15)))
	                .ReplaceStr("<ExecutionModes>", execution_modes.Concat("\n" + String8(' ', 15)))
	                .ReplaceStr("<Imports>", imports.Concat("\n" + String8(' ', 15)))
	                .ReplaceStr("<Extensions>", extensions.Concat("\n" + String8(' ', 15)));
}

void Spirv::WriteDebug()
{
	if (Config::SpirvDebugPrintfEnabled())
	{
		int index = 0;
		for (const auto& p: m_code.GetDebugPrintfs())
		{
			m_source += String8::FromPrintf("%%printf_str_%d = OpString \"%s\"", index, p.format.C_Str());
			index++;
		}
	}
}

void Spirv::WriteAnnotations()
{
	static const char* pixel_annotations   = R"(
               ; Annotations
               OpDecorate %outColor Location 0
               <Variables>
)";
	static const char* vertex_annotations  = R"(
               ; Annotations
               OpDecorate %gl_VertexIndex BuiltIn VertexIndex
               OpDecorate %gl_InstanceIndex BuiltIn InstanceIndex
               OpMemberDecorate %gl_PerVertex 0 BuiltIn Position
               OpMemberDecorate %gl_PerVertex 1 BuiltIn PointSize
               OpMemberDecorate %gl_PerVertex 2 BuiltIn ClipDistance
               OpMemberDecorate %gl_PerVertex 3 BuiltIn CullDistance
               OpDecorate %gl_PerVertex Block
               ; OpDecorate %param0 Location 0
               <Variables>
)";
	static const char* compute_annotations = R"(
               ; Annotations
               OpDecorate %gl_LocalInvocationID BuiltIn LocalInvocationId
               OpDecorate %gl_WorkGroupID BuiltIn WorkgroupId
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
               <Variables>
)";

	Core::StringList8 vars;
	if (spirv_uses_subgroup_invocation(m_code))
	{
		vars.Add("OpDecorate %gl_SubgroupInvocationID BuiltIn SubgroupLocalInvocationId");
		if (m_code.GetType() == ShaderType::Pixel)
		{
			vars.Add("OpDecorate %gl_SubgroupInvocationID Flat");
		}
	}

	switch (m_code.GetType())
	{
		case ShaderType::Pixel:
			if (m_ps_input_info != nullptr)
			{
				for (int rt = 1; rt < 8; rt++)
				{
					if (m_ps_input_info->target_output_mode[rt] != 0)
					{
						vars.Add(String8::FromPrintf("OpDecorate %%outColor%d Location %d", rt, rt));
					}
				}
				for (uint32_t i = 0; i < m_ps_input_info->input_num; i++)
				{
					if (ShaderPixelCanonicalInterpolator(*m_ps_input_info, i) != i)
					{
						continue;
					}

					ShaderPixelInterpolator interpolator {};
					if (!ShaderDecodePixelInterpolator(m_ps_input_info->interpolator_settings[i], &interpolator)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderDecodePixelInterpolator(m_ps_input_info->interpolator_settings[i], &interpolator) condition ignored (continuing)\n"); }
					if (interpolator.source == ShaderPixelInterpolatorSource::Default)
					{
						continue;
					}

					if (interpolator.flat)
					{
						vars.Add(String8::FromPrintf("OpDecorate %%attr%u Flat", i));
					} else {
						switch (GetPixelInterpolationMode(i))
						{
							case PixelInterpolationMode::PerspectiveCenter:
							case PixelInterpolationMode::Unused: break;
							case PixelInterpolationMode::PerspectiveCentroid:
								vars.Add(String8::FromPrintf("OpDecorate %%attr%u Centroid", i));
								break;
							case PixelInterpolationMode::LinearCenter:
								vars.Add(String8::FromPrintf("OpDecorate %%attr%u NoPerspective", i));
								break;
							case PixelInterpolationMode::LinearCentroid:
								vars.Add(String8::FromPrintf("OpDecorate %%attr%u NoPerspective", i));
								vars.Add(String8::FromPrintf("OpDecorate %%attr%u Centroid", i));
								break;
							case PixelInterpolationMode::Unsupported: EXIT_IF(true); break;
						}
					}
					vars.Add(String8::FromPrintf("OpDecorate %%attr%u Location %u", i, interpolator.location));
				}
				if (m_ps_input_info->ps_pos_xy)
				{
					vars.Add("OpDecorate %gl_FragCoord BuiltIn FragCoord");
				}
			}
			m_source += String8(pixel_annotations).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Vertex:
			if (m_vs_input_info != nullptr)
			{
				for (int i = 0; i < m_vs_input_info->resources_num; i++)
				{
					vars.Add(String8::FromPrintf("OpDecorate %%attr%d Location %d", i, i));
				}
			}
			for (int i = 0; i < ResolveVertexParameterCount(m_code, m_vs_input_info); i++)
			{
				vars.Add(String8::FromPrintf("OpDecorate %%param%d Location %d", i, i));
			}
			m_source += String8(vertex_annotations).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Compute:
			m_source += String8(compute_annotations).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		default: KYTY_LOG_DEBUG("WARNING: unknown shader type (continuing)\n"); return;
	}

	static const char* storage_buffers_annotations = R"(
       OpDecorate %buffers_runtimearr_float ArrayStride 4
       OpMemberDecorate %BufferObject 0 Offset 0
       OpDecorate %BufferObject Block
       OpDecorate %buf DescriptorSet <DescriptorSet>
       OpDecorate %buf Binding <BindingIndex>
)";
	static const char* storage_buffer_uint_annotations = R"(
       OpDecorate %buffers_runtimearr_uint ArrayStride 4
       OpMemberDecorate %BufferObjectUint 0 Offset 0
       OpDecorate %BufferObjectUint Block
       OpDecorate %buf_uint DescriptorSet <DescriptorSet>
       OpDecorate %buf_uint Binding <BindingIndex>
)";

	static const char* textures_annotations_s = R"(
       OpDecorate %textures2D_S DescriptorSet <DescriptorSet>
       OpDecorate %textures2D_S Binding <BindingIndex>
)";
	static const char* textures_annotations_s_3d = R"(
       OpDecorate %textures3D_S DescriptorSet <DescriptorSet>
       OpDecorate %textures3D_S Binding <BindingIndex>
)";
	static const char* textures_annotations_s_array = R"(
       OpDecorate %textures2DA_S DescriptorSet <DescriptorSet>
       OpDecorate %textures2DA_S Binding <BindingIndex>
)";
	static const char* textures_annotations_u = R"(
       OpDecorate %textures2D_U DescriptorSet <DescriptorSet>
       OpDecorate %textures2D_U Binding <BindingIndex>
)";
	static const char* textures_annotations_u_3d = R"(
       OpDecorate %textures3D_U DescriptorSet <DescriptorSet>
       OpDecorate %textures3D_U Binding <BindingIndex>
)";
	static const char* textures_annotations_u_array = R"(
       OpDecorate %textures2DA_U DescriptorSet <DescriptorSet>
       OpDecorate %textures2DA_U Binding <BindingIndex>
)";

	static const char* textures_annotations_l = R"(
       OpDecorate %textures2D_L DescriptorSet <DescriptorSet>
       OpDecorate %textures2D_L Binding <BindingIndex>
)";

	static const char* samplers_annotations = R"(
       OpDecorate %samplers DescriptorSet <DescriptorSet>
       OpDecorate %samplers Binding <BindingIndex>
)";
	static const char* gds_annotations      = R"(
               OpDecorate %gds_runtimearr_uint ArrayStride 4
               OpMemberDecorate %GDS 0 Coherent
               OpMemberDecorate %GDS 0 Offset 0
               OpDecorate %GDS Block
               OpDecorate %gds DescriptorSet <DescriptorSet>
               OpDecorate %gds Binding <BindingIndex>
)";

	static const char* vsharp_annotations = R"(
       OpDecorate %vsharp_arr_uint_uint_4 ArrayStride 4
       OpDecorate %vsharp_arr__arr_uint_uint_4_uint_<buffers_num> ArrayStride 16
	   OpMemberDecorate %BufferResource 0 Offset <Offset>
       OpDecorate %BufferResource Block
)";
	static const char* vsharp_uniform_annotations = R"(
	   OpDecorate %vsharp_arr_v4uint_uint_<buffers_num> ArrayStride 16
		OpMemberDecorate %BufferResource 0 Offset 0
		OpDecorate %BufferResource Block
       OpDecorate %vsharp DescriptorSet <DescriptorSet>
       OpDecorate %vsharp Binding <BindingIndex>
)";

	if (m_bind != nullptr)
	{
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			m_source += String8(storage_buffers_annotations)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->storage_buffers.binding_index));
			if (spirv_uses_buffer_atomics(m_code))
			{
				m_source += String8(storage_buffer_uint_annotations)
				                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
				                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->storage_buffers.binding_index));
			}
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			m_source += String8(textures_annotations_s)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_sampled_index));
			if (UsesMixedSampledImageNumericTypes(m_bind) || UsesUnsignedIntegerImages(m_bind))
			{
				m_source += String8(textures_annotations_u)
				                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
				                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_sampled_uint_index));
			}
		}
		if (m_bind->textures2D.textures2d_array_sampled_num > 0)
		{
			m_source += String8(textures_annotations_s_array)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_sampled_array_index));
			if (UsesMixedSampledImageNumericTypes(m_bind) || UsesUnsignedIntegerImages(m_bind))
			{
				m_source += String8(textures_annotations_u_array)
				                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
				                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_sampled_array_uint_index));
			}
		}
		if (m_bind->textures2D.textures3d_sampled_num > 0)
		{
			m_source += String8(textures_annotations_s_3d)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_sampled_3d_index));
			if (UsesMixedSampledImageNumericTypes(m_bind) || UsesUnsignedIntegerImages(m_bind))
			{
				m_source += String8(textures_annotations_u_3d)
				                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
				                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_sampled_3d_uint_index));
			}
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			m_source += String8(textures_annotations_l)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_storage_index));
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			m_source += String8(samplers_annotations)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->samplers.binding_index));
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			m_source += String8(gds_annotations)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->gds_pointers.binding_index));
		}
		if (m_bind->push_constant_size > 0)
		{
			if (m_bind->vsharp_uniform_buffer)
			{
				m_source += String8(vsharp_uniform_annotations)
				                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->push_constant_size / 16))
				                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
				                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->vsharp_binding_index));
			} else
			{
				m_source += String8(vsharp_annotations)
				                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->push_constant_size / 16))
				                .ReplaceStr("<Offset>", String8::FromPrintf("%u", m_bind->push_constant_offset));
			}
		}
	}
}

void Spirv::WriteTypes()
{
	static const char* types = R"(
                               ; Types
                         %void = OpTypeVoid
                        %float = OpTypeFloat 32
                          %int = OpTypeInt 32 1
                         %uint = OpTypeInt 32 0
                         %bool = OpTypeBool
                      %v2float = OpTypeVector %float 2
                      %v3float = OpTypeVector %float 3
                      %v4float = OpTypeVector %float 4
                       %v2uint = OpTypeVector %uint 2
                       %v3uint = OpTypeVector %uint 3
                       %v4uint = OpTypeVector %uint 4
                        %v2int = OpTypeVector %int 2
						%v3int = OpTypeVector %int 3
                 %undef_v2uint = OpUndef %v2uint
               %_ptr_Input_int = OpTypePointer Input %int
              %_ptr_Input_uint = OpTypePointer Input %uint
            %_ptr_Input_v2uint = OpTypePointer Input %v2uint
            %_ptr_Input_v4uint = OpTypePointer Input %v4uint
             %_ptr_Input_float = OpTypePointer Input %float
           %_ptr_Input_v2float = OpTypePointer Input %v2float
           %_ptr_Input_v3float = OpTypePointer Input %v3float
           %_ptr_Input_v4float = OpTypePointer Input %v4float
            %_ptr_Input_v3uint = OpTypePointer Input %v3uint
          %_ptr_Output_v4float = OpTypePointer Output %v4float
          %_ptr_Function_float = OpTypePointer Function %float
           %_ptr_Function_bool = OpTypePointer Function %bool
            %_ptr_Function_int = OpTypePointer Function %int
           %_ptr_Function_uint = OpTypePointer Function %uint
        %_ptr_Function_v2float = OpTypePointer Function %v2float
        %_ptr_Function_v3float = OpTypePointer Function %v3float
        %_ptr_Function_v4float = OpTypePointer Function %v4float
           %_ptr_Uniform_float = OpTypePointer Uniform %float
     %_ptr_StorageBuffer_float = OpTypePointer StorageBuffer %float
      %_ptr_StorageBuffer_uint = OpTypePointer StorageBuffer %uint
                     %ResTypeI = OpTypeStruct %int %int
                     %ResTypeU = OpTypeStruct %uint %uint
                %function_void = OpTypeFunction %void
              %function_fetch1 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float
              %function_fetch2 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_v2float
              %function_fetch3 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_v3float
              %function_fetch4 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_v4float
               %function_u_u = OpTypeFunction %uint %uint %uint
             %function_u_u_u = OpTypeFunction %uint %uint %uint %uint
            %function_u2_u_u_u = OpTypeFunction %v2uint %uint %uint %uint
               %function_b_f_f = OpTypeFunction %bool %float %float
                 %function_b_i = OpTypeFunction %bool %int
                 %function_i_i = OpTypeFunction %int %int %int
               %function_shift = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint
%function_tbuffer_load_store_format_xyzw = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
    %function_buffer_load_store_float1 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
		%function_buffer_raw_address = OpTypeFunction %uint %uint %uint %uint %uint %uint
   %function_buffer_load_store_ubyte = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
    %function_buffer_load_store_float2 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
     %function_buffer_load_store_float4 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
 %function_tbuffer_load_store_format_x = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
%function_tbuffer_load_store_format_xy = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
          %function_sbuffer_load_dword = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
        %function_sbuffer_load_dword_2 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
        %function_sbuffer_load_dword_4 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
        %function_sbuffer_load_dword_8 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
       %function_sbuffer_load_dword_16 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
)";

	static const char* pixel_types = R"(
)";

	static const char* vertex_types = R"(
            %array_length = OpConstant %uint 1
        %int_per_vertex_0 = OpConstant %int 0
       %_arr_float_uint_1 = OpTypeArray %float %array_length
            %gl_PerVertex = OpTypeStruct %v4float %float %_arr_float_uint_1 %_arr_float_uint_1
%_ptr_Output_gl_PerVertex = OpTypePointer Output %gl_PerVertex
)";

static const char* compute_types = R"(
)";

	// Optional scalar types must be emitted only when the shader uses them.
	// Keeping unused OpTypeFloat 64/OpTypeFloat 16 declarations in every module
	// forces capabilities that ordinary shaders do not declare (and validators
	// correctly reject the resulting module).
	String8 optional_types;
	if (spirv_uses_f64(m_code))
	{
		optional_types += "\n %double = OpTypeFloat 64\n";
	}
	if (spirv_uses_f16(m_code))
	{
		optional_types += "\n  %half = OpTypeFloat 16\n";
	}
	if (spirv_uses_i16(m_code))
	{
		optional_types += "\n %short = OpTypeInt 16 1\n %ushort = OpTypeInt 16 0\n";
	}
	if (spirv_uses_f64(m_code))
	{
		optional_types += "\n %ulong = OpTypeInt 64 0\n %slong = OpTypeInt 64 1\n";
	}
	m_source += optional_types;
	m_source += types;

	switch (m_code.GetType())
	{
		case ShaderType::Vertex: m_source += vertex_types; break;
		case ShaderType::Pixel: m_source += pixel_types; break;
		case ShaderType::Compute: m_source += compute_types; break;
		default: KYTY_LOG_DEBUG("WARNING: unknown shader type (continuing)\n"); return;
	}

	if (m_code.GetType() == ShaderType::Compute && m_cs_input_info != nullptr && m_cs_input_info->lds_dwords > 0)
	{
		m_source += String8(R"(
                  %lds_length = OpConstant %uint <lds_dwords>
              %lds_array_uint = OpTypeArray %uint %lds_length
          %_ptr_Workgroup_uint = OpTypePointer Workgroup %uint
%_ptr_Workgroup_lds_array_uint = OpTypePointer Workgroup %lds_array_uint
)")
		                .ReplaceStr("<lds_dwords>", String8::FromPrintf("%u", m_cs_input_info->lds_dwords));
	}

static const char* storage_buffers_types = R"(
                               %buffers_runtimearr_float = OpTypeRuntimeArray %float
                                           %BufferObject = OpTypeStruct %buffers_runtimearr_float
                         %buffers_num_uint_<buffers_num> = OpConstant %uint <buffers_num>
                   %_arr_BufferObject_uint_<buffers_num> = OpTypeArray %BufferObject %buffers_num_uint_<buffers_num>
%_ptr_StorageBuffer__arr_BufferObject_uint_<buffers_num> = OpTypePointer StorageBuffer %_arr_BufferObject_uint_<buffers_num>
)";
	static const char* storage_buffer_uint_types = R"(
                               %buffers_runtimearr_uint = OpTypeRuntimeArray %uint
                                       %BufferObjectUint = OpTypeStruct %buffers_runtimearr_uint
               %_arr_BufferObjectUint_uint_<buffers_num> = OpTypeArray %BufferObjectUint %buffers_num_uint_<buffers_num>
%_ptr_StorageBuffer__arr_BufferObjectUint_uint_<buffers_num> = OpTypePointer StorageBuffer %_arr_BufferObjectUint_uint_<buffers_num>
)";

static const char* textures_sampled_types = R"(
                                             %ImageS = OpTypeImage %<image_scalar> <image_dimension> 0 <arrayed> 0 1 Unknown
                    %textures2D_S_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_ImageS_uint_<buffers_num> = OpTypeArray %ImageS %textures2D_S_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageS_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageS_uint_<buffers_num>
                        %_ptr_UniformConstant_ImageS = OpTypePointer UniformConstant %ImageS
                                       %SampledImage = OpTypeSampledImage %ImageS
)";

static const char* textures_sampled_types_3d = R"(
                                             %ImageS3D = OpTypeImage %<image_scalar> 3D 0 0 0 1 Unknown
                    %textures3D_S_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_ImageS3D_uint_<buffers_num> = OpTypeArray %ImageS3D %textures3D_S_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageS3D_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageS3D_uint_<buffers_num>
                        %_ptr_UniformConstant_ImageS3D = OpTypePointer UniformConstant %ImageS3D
                                       %SampledImage3D = OpTypeSampledImage %ImageS3D
)";

static const char* textures_sampled_types_array = R"(
                                             %ImageSA = OpTypeImage %<image_scalar> 2D 0 1 0 1 Unknown
                   %textures2DA_S_uint_<buffers_num> = OpConstant %uint <buffers_num>
                    %_arr_ImageSA_uint_<buffers_num> = OpTypeArray %ImageSA %textures2DA_S_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageSA_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageSA_uint_<buffers_num>
                       %_ptr_UniformConstant_ImageSA = OpTypePointer UniformConstant %ImageSA
                                      %SampledImageA = OpTypeSampledImage %ImageSA
)";

	static const char* textures_sampled_uint_types = R"(
                                             %ImageU = OpTypeImage %uint 2D 0 0 0 1 Unknown
                    %textures2D_U_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_ImageU_uint_<buffers_num> = OpTypeArray %ImageU %textures2D_U_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageU_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageU_uint_<buffers_num>
                        %_ptr_UniformConstant_ImageU = OpTypePointer UniformConstant %ImageU
                                      %SampledImageU = OpTypeSampledImage %ImageU
)";

	static const char* textures_sampled_uint_types_3d = R"(
                                             %ImageU3D = OpTypeImage %uint 3D 0 0 0 1 Unknown
                    %textures3D_U_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_ImageU3D_uint_<buffers_num> = OpTypeArray %ImageU3D %textures3D_U_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageU3D_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageU3D_uint_<buffers_num>
                        %_ptr_UniformConstant_ImageU3D = OpTypePointer UniformConstant %ImageU3D
                                      %SampledImageU3D = OpTypeSampledImage %ImageU3D
)";

	static const char* textures_sampled_uint_types_array = R"(
                                             %ImageUA = OpTypeImage %uint 2D 0 1 0 1 Unknown
                   %textures2DA_U_uint_<buffers_num> = OpConstant %uint <buffers_num>
                    %_arr_ImageUA_uint_<buffers_num> = OpTypeArray %ImageUA %textures2DA_U_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageUA_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageUA_uint_<buffers_num>
                       %_ptr_UniformConstant_ImageUA = OpTypePointer UniformConstant %ImageUA
                                      %SampledImageUA = OpTypeSampledImage %ImageUA
)";

static const char* textures_loaded_types = R"(
                                             %ImageL = OpTypeImage %<image_scalar> <image_dimension> 0 <arrayed> 0 2 <image_format>
                    %textures2D_L_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_ImageL_uint_<buffers_num> = OpTypeArray %ImageL %textures2D_L_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageL_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageL_uint_<buffers_num>
                        %_ptr_UniformConstant_ImageL = OpTypePointer UniformConstant %ImageL
)";

	static const char* samplers_types = R"(
                                             %Sampler = OpTypeSampler
                         %samplers_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_Sampler_uint_<buffers_num> = OpTypeArray %Sampler %samplers_uint_<buffers_num>
%_ptr_UniformConstant__arr_Sampler_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_Sampler_uint_<buffers_num>
                        %_ptr_UniformConstant_Sampler = OpTypePointer UniformConstant %Sampler
)";

	static const char* gds_types = R"(
            %gds_runtimearr_uint = OpTypeRuntimeArray %uint
                    %GDS = OpTypeStruct %gds_runtimearr_uint
            %_ptr_StorageBuffer_GDS = OpTypePointer StorageBuffer %GDS
)";

	static const char* vsharp_types = R"(
         %vsharp_buffers_num_uint_<buffers_num> = OpConstant %uint <buffers_num>
                             %vsharp_num_uint_4 = OpConstant %uint 4
                        %vsharp_arr_uint_uint_4 = OpTypeArray %uint %vsharp_num_uint_4
%vsharp_arr__arr_uint_uint_4_uint_<buffers_num> = OpTypeArray %vsharp_arr_uint_uint_4 %vsharp_buffers_num_uint_<buffers_num>
                                %BufferResource = OpTypeStruct %vsharp_arr__arr_uint_uint_4_uint_<buffers_num>
              %_ptr_PushConstant_BufferResource = OpTypePointer PushConstant %BufferResource
                        %_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
)";
	static const char* vsharp_uniform_types = R"(
	         %vsharp_buffers_num_uint_<buffers_num> = OpConstant %uint <buffers_num>
	     %vsharp_arr_v4uint_uint_<buffers_num> = OpTypeArray %v4uint %vsharp_buffers_num_uint_<buffers_num>
	                                %BufferResource = OpTypeStruct %vsharp_arr_v4uint_uint_<buffers_num>
	                    %_ptr_Uniform_BufferResource = OpTypePointer Uniform %BufferResource
	                              %_ptr_Uniform_uint = OpTypePointer Uniform %uint
)";

	if (m_bind != nullptr)
	{
		const char* storage_arrayed = UsesArrayed2dImages(m_bind, ShaderTextureUsage::ReadWrite) ? "1" : "0";
		const bool uint_images = UsesUnsignedIntegerImages(m_bind);
		const bool mixed_sampled_image_types = UsesMixedSampledImageNumericTypes(m_bind);
		const char* image_scalar = uint_images ? "uint" : "float";
		const char* image_format = uint_images ? "R32ui" : "Unknown";
		const char* image_dimension = "2D";
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			m_source +=
			    String8(storage_buffers_types).ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->storage_buffers.buffers_num));
			if (spirv_uses_buffer_atomics(m_code))
			{
				m_source += String8(storage_buffer_uint_types)
				                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->storage_buffers.buffers_num));
			}
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			m_source += String8(textures_sampled_types)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures2d_sampled_num))
			                .ReplaceStr("<image_scalar>", image_scalar)
			                .ReplaceStr("<image_dimension>", image_dimension)
			                .ReplaceStr("<arrayed>", "0");
			if (mixed_sampled_image_types || uint_images)
			{
				m_source += String8(textures_sampled_uint_types)
				                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures2d_sampled_num));
			}
		}
		if (m_bind->textures2D.textures2d_array_sampled_num > 0)
		{
			m_source += String8(textures_sampled_types_array)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures2d_array_sampled_num))
			                .ReplaceStr("<image_scalar>", image_scalar);
			if (mixed_sampled_image_types || uint_images)
			{
				m_source += String8(textures_sampled_uint_types_array)
				                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures2d_array_sampled_num));
			}
		}
		if (m_bind->textures2D.textures3d_sampled_num > 0)
		{
			m_source += String8(textures_sampled_types_3d)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures3d_sampled_num))
			                .ReplaceStr("<image_scalar>", image_scalar);
			if (mixed_sampled_image_types || uint_images)
			{
				m_source += String8(textures_sampled_uint_types_3d)
				                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures3d_sampled_num));
			}
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			m_source += String8(textures_loaded_types)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures2d_storage_num))
			                .ReplaceStr("<image_scalar>", image_scalar)
			                .ReplaceStr("<image_format>", image_format)
			                .ReplaceStr("<image_dimension>", image_dimension)
			                .ReplaceStr("<arrayed>", storage_arrayed);
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			m_source += String8(samplers_types).ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->samplers.samplers_num));
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			m_source += String8(gds_types);
		}
		if (m_bind->push_constant_size > 0)
		{
			m_source += String8(m_bind->vsharp_uniform_buffer ? vsharp_uniform_types : vsharp_types)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->push_constant_size / 16));
		}
	}

}

void Spirv::WriteConstants()
{
	FindConstants();

	static const char* comment = R"(
    ; Constants
         %true = OpConstantTrue %bool
        %false = OpConstantFalse %bool
    %float_2pi = OpConstant %float 6.283185307179586476925286766559
)";

	m_source += comment;

	for (const auto& c: m_constants)
	{
		m_source += String8::FromPrintf("%%%s = OpConstant %%%s %s\n", c.id.c_str(), c.type_str.c_str(), c.literal_str.c_str());
	}
}

void Spirv::WriteGlobalVariables()
{
	static const char* pixel_variables   = R"(
              ;Variables
   %outColor = OpVariable %_ptr_Output_v4float Output
               <Variables>
)";
	static const char* vertex_variables  = R"(
              ;Variables
    %gl_VertexIndex = OpVariable %_ptr_Input_int Input
  %gl_InstanceIndex = OpVariable %_ptr_Input_int Input
      %outPerVertex = OpVariable %_ptr_Output_gl_PerVertex Output
            ; %param0 = OpVariable %_ptr_Output_v4float Output
               <Variables>
)";
	static const char* compute_variables = R"(
              ;Variables
%gl_LocalInvocationID = OpVariable %_ptr_Input_v3uint Input
      %gl_WorkGroupID = OpVariable %_ptr_Input_v3uint Input
               <Variables>
)";

	Core::StringList8 vars;
	if (spirv_uses_subgroup_invocation(m_code))
	{
		vars.Add("%gl_SubgroupInvocationID = OpVariable %_ptr_Input_uint Input");
	}

	if (m_code.GetType() == ShaderType::Pixel && m_ps_input_info != nullptr)
	{
		for (int rt = 1; rt < 8; rt++)
		{
			if (m_ps_input_info->target_output_mode[rt] != 0)
			{
				vars.Add(String8::FromPrintf("%%outColor%d = OpVariable %%_ptr_Output_v4float Output", rt));
			}
		}
	}

	if (m_bind != nullptr)
	{
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			vars.Add(String8::FromPrintf("%%buf = OpVariable %%_ptr_StorageBuffer__arr_BufferObject_uint_%d StorageBuffer",
			                             m_bind->storage_buffers.buffers_num));
			if (spirv_uses_buffer_atomics(m_code))
			{
				vars.Add(String8::FromPrintf("%%buf_uint = OpVariable %%_ptr_StorageBuffer__arr_BufferObjectUint_uint_%d StorageBuffer",
				                             m_bind->storage_buffers.buffers_num));
			}
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			vars.Add(String8::FromPrintf("%%textures2D_S = OpVariable %%_ptr_UniformConstant__arr_ImageS_uint_%d UniformConstant",
			                             m_bind->textures2D.textures2d_sampled_num));
			if (UsesMixedSampledImageNumericTypes(m_bind) || UsesUnsignedIntegerImages(m_bind))
			{
				vars.Add(String8::FromPrintf("%%textures2D_U = OpVariable %%_ptr_UniformConstant__arr_ImageU_uint_%d UniformConstant",
				                             m_bind->textures2D.textures2d_sampled_num));
			}
		}
		if (m_bind->textures2D.textures2d_array_sampled_num > 0)
		{
			vars.Add(String8::FromPrintf("%%textures2DA_S = OpVariable %%_ptr_UniformConstant__arr_ImageSA_uint_%d UniformConstant",
			                             m_bind->textures2D.textures2d_array_sampled_num));
			if (UsesMixedSampledImageNumericTypes(m_bind) || UsesUnsignedIntegerImages(m_bind))
			{
				vars.Add(String8::FromPrintf("%%textures2DA_U = OpVariable %%_ptr_UniformConstant__arr_ImageUA_uint_%d UniformConstant",
				                             m_bind->textures2D.textures2d_array_sampled_num));
			}
		}
		if (m_bind->textures2D.textures3d_sampled_num > 0)
		{
			vars.Add(String8::FromPrintf("%%textures3D_S = OpVariable %%_ptr_UniformConstant__arr_ImageS3D_uint_%d UniformConstant",
			                             m_bind->textures2D.textures3d_sampled_num));
			if (UsesMixedSampledImageNumericTypes(m_bind) || UsesUnsignedIntegerImages(m_bind))
			{
				vars.Add(String8::FromPrintf("%%textures3D_U = OpVariable %%_ptr_UniformConstant__arr_ImageU3D_uint_%d UniformConstant",
				                             m_bind->textures2D.textures3d_sampled_num));
			}
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			vars.Add(String8::FromPrintf("%%textures2D_L = OpVariable %%_ptr_UniformConstant__arr_ImageL_uint_%d UniformConstant",
			                             m_bind->textures2D.textures2d_storage_num));
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			vars.Add(String8::FromPrintf("%%samplers = OpVariable %%_ptr_UniformConstant__arr_Sampler_uint_%d UniformConstant",
			                             m_bind->samplers.samplers_num));
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			vars.Add("%gds = OpVariable %_ptr_StorageBuffer_GDS StorageBuffer");
		}
		if (m_bind->push_constant_size > 0)
		{
			vars.Add(m_bind->vsharp_uniform_buffer ? "%vsharp = OpVariable %_ptr_Uniform_BufferResource Uniform"
			                                          : "%vsharp = OpVariable %_ptr_PushConstant_BufferResource PushConstant");
		}
	}

	if (m_code.GetType() == ShaderType::Compute && m_cs_input_info != nullptr && m_cs_input_info->lds_dwords > 0)
	{
		vars.Add("%lds = OpVariable %_ptr_Workgroup_lds_array_uint Workgroup");
	}

	switch (m_code.GetType())
	{
		case ShaderType::Pixel:
			if (m_ps_input_info != nullptr)
			{
				for (uint32_t i = 0; i < m_ps_input_info->input_num; i++)
				{
					if (ShaderPixelCanonicalInterpolator(*m_ps_input_info, i) == i)
					{
						ShaderPixelInterpolator interpolator {};
						if (!ShaderDecodePixelInterpolator(m_ps_input_info->interpolator_settings[i], &interpolator)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderDecodePixelInterpolator(m_ps_input_info->interpolator_settings[i], &interpolator) condition ignored (continuing)\n"); }
						if (interpolator.source == ShaderPixelInterpolatorSource::Parameter)
						{
							vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v4float Input", i));
						}
					}
				}
				if (m_ps_input_info->ps_pos_xy)
				{
					vars.Add("%gl_FragCoord = OpVariable %_ptr_Input_v4float Input");
				}
			}
			m_source += String8(pixel_variables).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Vertex:
			if (m_vs_input_info != nullptr)
			{
				for (int i = 0; i < m_vs_input_info->resources_num; i++)
				{
					const auto format = VulkanResolveGen5VertexInputFormat(m_vs_input_info->resources[i].Format());
					const bool uint_input = format.numeric_class == VulkanVertexInputNumericClass::Uint;
					const int width = m_vs_input_info->resources_dst[i].registers_num;
					if (width < 1 || width > 4)
					{
						KYTY_LOG_DEBUG("WARNING: invalid registers_num %d in shader (continuing)\n", width);
						continue;
					}
					const String8 type = width == 1 ? String8(uint_input ? "uint" : "float")
					                                : String8::FromPrintf("v%d%s", width, uint_input ? "uint" : "float");
					vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_%s Input", i, type.c_str()));
				}
			}
			for (int i = 0; i < ResolveVertexParameterCount(m_code, m_vs_input_info); i++)
			{
				vars.Add(String8::FromPrintf("%%param%d = OpVariable %%_ptr_Output_v4float Output", i));
			}
			m_source += String8(vertex_variables).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Compute:
			if (m_cs_input_info != nullptr)
			{
				vars.Add(String8::FromPrintf("%%gl_WorkGroupSize = OpConstantComposite %%v3uint %%uint_%u %%uint_%u %%uint_%u",
				                             m_cs_input_info->threads_num[0], m_cs_input_info->threads_num[1],
				                             m_cs_input_info->threads_num[2]));
			}
			m_source += String8(compute_variables).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		default: KYTY_LOG_DEBUG("WARNING: unknown shader type (continuing)\n"); return;
	}
}

void Spirv::WriteMainProlog()
{
	static const char* text = R"(
                   ; Function main
                   ; Prolog
       %main       = OpFunction %void None %function_void
       %main_label = OpLabel
)";

	m_source += text;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Spirv::WriteLocalVariables()
{
	FindVariables();

	Vector<int> packed_half_regs;
	for (const auto& inst: m_code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::VCvtPkrtzF16F32 && inst.dst.type == ShaderOperandType::Vgpr && inst.dst.size == 1)
		{
			bool exists = false;
			for (auto reg: packed_half_regs)
			{
				if (reg == inst.dst.register_id)
				{
					exists = true;
					break;
				}
			}
			if (!exists)
			{
				packed_half_regs.Add(inst.dst.register_id);
			}
		}
	}

	static const char* comment = R"(
    ; Registers
)";

	m_source += comment;

	for (const auto& c: m_variables)
	{
		auto value = operand_variable_to_str(c.op);
		m_source += String8::FromPrintf("%%%s = OpVariable %%_ptr_Function_%s Function\n", value.value.c_str(),
		                                Core::EnumName(value.type).ToLower().C_Str());
	}
	for (auto reg: packed_half_regs)
	{
		m_source += String8::FromPrintf("%%v%d_packed_half = OpVariable %%_ptr_Function_uint Function\n", reg);
	}
	std::set<std::pair<int, int>> scalar_spill_slots;
	for (const auto& inst: m_code.GetInstructions())
	{
		int register_id = 0;
		int lane        = 0;
		if (IsStaticScalarSpillWrite(inst, &register_id, &lane))
		{
			scalar_spill_slots.emplace(register_id, lane);
		}
	}
	for (const auto& slot: scalar_spill_slots)
	{
		m_source += String8::FromPrintf("%%%s = OpVariable %%_ptr_Function_float Function\n",
		                                ScalarSpillSlotName(slot.first, slot.second).c_str());
	}
	uint32_t tap_pc = 0;
	int      tap_register = 0;
	const bool fragment_tap = FragmentTapSelection(m_code, &tap_pc, &tap_register);
	if (fragment_tap)
	{
		for (uint32_t component = 0; component < 4u; component++)
		{
			m_source += String8::FromPrintf("%%fs_tap_%u = OpVariable %%_ptr_Function_float Function\n", component);
		}
	}

	static const char* common_vars = R"(
             %temp_float = OpVariable %_ptr_Function_float Function
           %temp_v2float = OpVariable %_ptr_Function_v2float Function
           %temp_v3float = OpVariable %_ptr_Function_v3float Function
	       %temp_v4float = OpVariable %_ptr_Function_v4float Function
           %temp_int_0 = OpVariable %_ptr_Function_int Function
           %temp_int_1 = OpVariable %_ptr_Function_int Function
           %temp_int_2 = OpVariable %_ptr_Function_int Function
           %temp_int_3 = OpVariable %_ptr_Function_int Function
           %temp_int_4 = OpVariable %_ptr_Function_int Function
           %temp_int_5 = OpVariable %_ptr_Function_int Function
           %temp_uint_0 = OpVariable %_ptr_Function_uint Function
           %temp_uint_1 = OpVariable %_ptr_Function_uint Function
           %temp_uint_2 = OpVariable %_ptr_Function_uint Function
           %temp_uint_3 = OpVariable %_ptr_Function_uint Function
           %temp_uint_4 = OpVariable %_ptr_Function_uint Function
           %temp_uint_5 = OpVariable %_ptr_Function_uint Function
)";

	m_source += common_vars;
	if (fragment_tap)
	{
		for (uint32_t component = 0; component < 4u; component++)
		{
			m_source += String8::FromPrintf("               OpStore %%fs_tap_%u %%float_0_000000\n", component);
		}
	}
	for (auto reg: packed_half_regs)
	{
		m_source += String8::FromPrintf("               OpStore %%v%d_packed_half %%uint_0\n", reg);
	}

	if (m_code.GetType() == ShaderType::Vertex)
	{
		static const char* text = R"(
       %vertex_index_int = OpLoad %int %gl_VertexIndex
           %vertex_index = OpBitcast %float %vertex_index_int
                           OpStore %<v> %vertex_index
       %instance_index_int = OpLoad %int %gl_InstanceIndex
           %instance_index = OpBitcast %float %instance_index_int
                           OpStore %<i> %instance_index
)";
		if (m_vs_input_info != nullptr && m_vs_input_info->gs_prolog)
		{
			m_source += String8(text).ReplaceStr("<v>", "v5").ReplaceStr("<i>", "v8");

			// [7:0] - num_vertices, [15:8] - num_primitives
			static const char* init_s3 = R"(
	               OpStore %s3 %uint_1
				)";

			m_source += init_s3;
		} else
		{
			m_source += String8(text).ReplaceStr("<v>", "v0").ReplaceStr("<i>", "v3");
		}
	}

	if (m_code.GetType() == ShaderType::Pixel)
	{
		if (m_ps_input_info != nullptr && m_ps_input_info->ps_pos_xy)
		{
			static const char* native_text = R"(
         %FragCoord_px = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_0
         %FragCoord_x = OpLoad %float %FragCoord_px
               OpStore %v2 %FragCoord_x
         %FragCoord_py = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_1
         %FragCoord_y = OpLoad %float %FragCoord_py
               OpStore %v3 %FragCoord_y
)";
			static const char* scaled_text = R"(
         %FragCoord_px = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_0
         %FragCoord_x = OpLoad %float %FragCoord_px
         %FragCoord_scale_x = OpFDiv %float %<x_numerator> %<x_denominator>
         %FragCoord_guest_x = OpFMul %float %FragCoord_x %FragCoord_scale_x
               OpStore %v2 %FragCoord_guest_x
         %FragCoord_py = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_1
         %FragCoord_y = OpLoad %float %FragCoord_py
         %FragCoord_scale_y = OpFDiv %float %<y_numerator> %<y_denominator>
         %FragCoord_guest_y = OpFMul %float %FragCoord_y %FragCoord_scale_y
               OpStore %v3 %FragCoord_guest_y
)";
			const auto&        scale       = m_ps_input_info->host_to_guest_scale;
			if (scale.IsIdentity())
			{
				m_source += native_text;
			} else
			{
				m_source += String8(scaled_text)
				                .ReplaceStr("<x_numerator>", GetConstantFloat(static_cast<float>(scale.x_guest_numerator)))
				                .ReplaceStr("<x_denominator>", GetConstantFloat(static_cast<float>(scale.x_host_denominator)))
				                .ReplaceStr("<y_numerator>", GetConstantFloat(static_cast<float>(scale.y_guest_numerator)))
				                .ReplaceStr("<y_denominator>", GetConstantFloat(static_cast<float>(scale.y_host_denominator)));
			}
		}
	}

	if (m_code.GetType() == ShaderType::Compute)
	{
		static const char* text_thread_id = R"(
		%LocalInvocationID_114_<i> = OpAccessChain %_ptr_Input_uint %gl_LocalInvocationID %uint_<i>
        %LocalInvocationID_115_<i> = OpLoad %uint %LocalInvocationID_114_<i>
        %LocalInvocationID_116_<i> = OpBitcast %float %LocalInvocationID_115_<i>
               OpStore %v<i> %LocalInvocationID_116_<i>
)";

		static const char* text_group_id = R"(
        %WorkGroupID_120_<i> = OpAccessChain %_ptr_Input_uint %gl_WorkGroupID %uint_<i>
        %WorkGroupID_121_<i> = OpLoad %uint %WorkGroupID_120_<i>
               OpStore %<WorkGroupReg> %WorkGroupID_121_<i>
)";
		if (m_cs_input_info != nullptr)
		{
			for (int i = 0; i < m_cs_input_info->thread_ids_num; i++)
			{
				m_source += String8(text_thread_id).ReplaceStr("<i>", String8::FromPrintf("%d", i));
			}

			int reg = 0;
			for (int i = 0; i < 3; i++)
			{
				if (m_cs_input_info->group_id[i])
				{
					m_source += String8(text_group_id)
					                .ReplaceStr("<WorkGroupReg>", String8::FromPrintf("s%u", m_cs_input_info->workgroup_register + reg))
					                .ReplaceStr("<i>", String8::FromPrintf("%d", i));
					reg++;
				}
			}
		}
	}

	if (m_bind != nullptr)
	{
		static const char* text = R"(
		 %vsharp_<reg>_<buffer>_<field> = OpAccessChain %<vsharp_uint_ptr> %vsharp %int_0 %int_<buffer> %int_<field>
         %vsharp_value_<reg>_<buffer>_<field> = OpLoad %uint %vsharp_<reg>_<buffer>_<field>
	               OpStore %<reg> %vsharp_value_<reg>_<buffer>_<field>
		)";

		int buffer_index = 0;

		int shift_regs = (m_vs_input_info != nullptr && m_vs_input_info->gs_prolog ? 8 : 0);

		for (auto& m: m_extended_mapping)
		{
			m[0] = m[1] = 0;
		}

		for (int i = 0; i < m_bind->storage_buffers.buffers_num; i++)
		{
			int  start_reg = m_bind->storage_buffers.start_register[i];
			bool extended  = m_bind->storage_buffers.extended[i];

			EXIT_IF(buffer_index + i >= static_cast<int>(m_bind->push_constant_size) / 16);
			if (m_bind->storage_buffers.dynamic_sload[i])
			{
				// The S_LOAD instruction below supplies these four fields. Writing
				// them here would erase the descriptor's instruction-local lifetime.
				continue;
			}

			String8 buffer = String8::FromPrintf("%d", buffer_index + i);
			for (int f = 0; f < 4; f++)
			{
				if (extended)
				{
					EXIT_IF(start_reg < 16);
					if (shift_regs != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: shift_regs != 0 condition ignored (continuing)\n"); }
					if (start_reg - 16 + f >= m_extended_mapping.Size()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_reg - 16 + f >= m_extended_mapping.Size() condition ignored (continuing)\n"); }
					m_extended_mapping[start_reg - 16 + f][0] = buffer_index + i;
					m_extended_mapping[start_reg - 16 + f][1] = f;
				} else
				{
					String8 reg   = String8::FromPrintf("s%d", start_reg + f + shift_regs);
					String8 field = String8::FromPrintf("%d", f);
					m_source += String8(text)
					                .ReplaceStr("<vsharp_uint_ptr>", m_bind->vsharp_uniform_buffer ? "_ptr_Uniform_uint" :
					                                                       "_ptr_PushConstant_uint")
					                .ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
				}
			}
		}

		buffer_index += m_bind->storage_buffers.buffers_num;

		for (int i = 0; i < m_bind->textures2D.textures_num; i++)
		{
			int  start_reg = m_bind->textures2D.desc[i].start_register;
			bool extended  = m_bind->textures2D.desc[i].extended;
			if (m_bind->textures2D.desc[i].dynamic_sload)
			{
				continue;
			}

			for (int ti = 0; ti < 2; ti++)
			{
				EXIT_IF(buffer_index + i * 2 + ti >= static_cast<int>(m_bind->push_constant_size) / 16);

				String8 buffer = String8::FromPrintf("%d", buffer_index + i * 2 + ti);
				for (int f = 0; f < 4; f++)
				{
					if (extended)
					{
						EXIT_IF(start_reg < 16);
						if (shift_regs != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: shift_regs != 0 condition ignored (continuing)\n"); }
						if (start_reg - 16 + 4 * ti + f >= m_extended_mapping.Size()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_reg - 16 + 4 * ti + f >= m_extended_mapping.Size() condition ignored (continuing)\n"); }
						m_extended_mapping[start_reg - 16 + 4 * ti + f][0] = buffer_index + i * 2 + ti;
						m_extended_mapping[start_reg - 16 + 4 * ti + f][1] = f;
					} else
					{
						String8 reg   = String8::FromPrintf("s%d", start_reg + 4 * ti + f + shift_regs);
						String8 field = String8::FromPrintf("%d", f);
						m_source += String8(text)
						                .ReplaceStr("<vsharp_uint_ptr>", m_bind->vsharp_uniform_buffer ? "_ptr_Uniform_uint" :
						                                                       "_ptr_PushConstant_uint")
						                .ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
					}
				}
			}
		}

		buffer_index += m_bind->textures2D.textures_num * 2;

		for (int i = 0; i < m_bind->samplers.samplers_num; i++)
		{
			int  start_reg = m_bind->samplers.start_register[i];
			bool extended  = m_bind->samplers.extended[i];
			if (m_bind->samplers.dynamic_sload[i])
			{
				continue;
			}

			EXIT_IF(buffer_index + i >= static_cast<int>(m_bind->push_constant_size) / 16);

			String8 buffer = String8::FromPrintf("%d", buffer_index + i);
			for (int f = 0; f < 4; f++)
			{
				if (extended)
				{
					EXIT_IF(start_reg < 16);
					if (shift_regs != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: shift_regs != 0 condition ignored (continuing)\n"); }
					if (start_reg - 16 + f >= m_extended_mapping.Size()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_reg - 16 + f >= m_extended_mapping.Size() condition ignored (continuing)\n"); }
					m_extended_mapping[start_reg - 16 + f][0] = buffer_index + i;
					m_extended_mapping[start_reg - 16 + f][1] = f;
				} else
				{
					String8 reg   = String8::FromPrintf("s%d", start_reg + f + shift_regs);
					String8 field = String8::FromPrintf("%d", f);
					m_source += String8(text)
					                .ReplaceStr("<vsharp_uint_ptr>", m_bind->vsharp_uniform_buffer ? "_ptr_Uniform_uint" :
					                                                       "_ptr_PushConstant_uint")
					                .ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
				}
			}
		}

		buffer_index += m_bind->samplers.samplers_num;

		for (int i = 0; i < m_bind->gds_pointers.pointers_num; i++)
		{
			int  start_reg = m_bind->gds_pointers.start_register[i];
			bool extended  = m_bind->gds_pointers.extended[i];

			EXIT_IF(buffer_index + i / 4 >= static_cast<int>(m_bind->push_constant_size) / 16);

			if (extended)
			{
				EXIT_IF(start_reg < 16);
				if (shift_regs != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: shift_regs != 0 condition ignored (continuing)\n"); }
				if (start_reg - 16 >= m_extended_mapping.Size()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_reg - 16 >= m_extended_mapping.Size() condition ignored (continuing)\n"); }
				m_extended_mapping[start_reg - 16][0] = buffer_index + i / 4;
				m_extended_mapping[start_reg - 16][1] = i % 4;
			} else
			{
				String8 buffer = String8::FromPrintf("%d", buffer_index + i / 4);
				String8 reg    = String8::FromPrintf("s%d", start_reg + shift_regs);
				String8 field  = String8::FromPrintf("%d", i % 4);
				m_source += String8(text)
				                .ReplaceStr("<vsharp_uint_ptr>", m_bind->vsharp_uniform_buffer ? "_ptr_Uniform_uint" :
				                                                       "_ptr_PushConstant_uint")
				                .ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
			}
		}

		buffer_index += (m_bind->gds_pointers.pointers_num > 0 ? (m_bind->gds_pointers.pointers_num - 1) / 4 + 1 : 0);

		auto is_descriptor_register = [this](int reg)
		{
			for (int i = 0; i < m_bind->storage_buffers.buffers_num; ++i)
			{
				if (!m_bind->storage_buffers.extended[i] && reg >= m_bind->storage_buffers.start_register[i] &&
				    reg < m_bind->storage_buffers.start_register[i] + 4)
				{
					return true;
				}
			}
			for (int i = 0; i < m_bind->textures2D.textures_num; ++i)
			{
				if (!m_bind->textures2D.desc[i].extended && reg >= m_bind->textures2D.desc[i].start_register &&
				    reg < m_bind->textures2D.desc[i].start_register + 8)
				{
					return true;
				}
			}
			for (int i = 0; i < m_bind->samplers.samplers_num; ++i)
			{
				if (!m_bind->samplers.extended[i] && reg >= m_bind->samplers.start_register[i] &&
				    reg < m_bind->samplers.start_register[i] + 4)
				{
					return true;
				}
			}
			return false;
		};

		for (int i = 0; i < m_bind->direct_sgprs.sgprs_num; i++)
		{
			int start_reg = m_bind->direct_sgprs.start_register[i];
			if (is_descriptor_register(start_reg))
			{
				continue;
			}

			EXIT_IF(buffer_index + i / 4 >= static_cast<int>(m_bind->push_constant_size) / 16);

			String8 buffer = String8::FromPrintf("%d", buffer_index + i / 4);
			String8 reg    = String8::FromPrintf("s%d", start_reg + shift_regs);
			String8 field  = String8::FromPrintf("%d", i % 4);
			m_source += String8(text)
			                .ReplaceStr("<vsharp_uint_ptr>", m_bind->vsharp_uniform_buffer ? "_ptr_Uniform_uint" : "_ptr_PushConstant_uint")
			                .ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
		}

		/* buffer_index += (m_bind->direct_sgprs.sgprs_num > 0 ? (m_bind->direct_sgprs.sgprs_num - 1) / 4 + 1 : 0); */

		if (m_bind->extended.used)
		{
			// TODO() load pointer

			KYTY_LOG_DEBUG("Extended mapping: ");
			for (auto& m: m_extended_mapping)
			{
				KYTY_LOG_DEBUG("{%d, %d} ", m[0], m[1]);
			}
			KYTY_LOG_DEBUG("\n");
		}
	}

	static const char* common_init = R"(
               OpStore %exec_lo %uint_1
               OpStore %exec_hi %uint_0
               OpStore %execz %uint_0
               OpStore %scc %uint_0
	)";

	m_source += common_init;
	m_source += "\n";
}


void Spirv::DetectFetch()
{
	EXIT_IF(m_vs_input_info == nullptr);
	EXIT_IF(!m_vs_input_info->fetch_embedded);

	if (!m_vs_input_info->gs_prolog) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !m_vs_input_info->gs_prolog condition ignored (continuing)\n"); }
	if (m_vs_input_info->fetch_inline) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_vs_input_info->fetch_inline condition ignored (continuing)\n"); }

	enum class Type
	{
		Unknown,
		Attrib,
		Buffer,
		Index
	};

	struct VgprInfo
	{
		Type type = Type::Unknown;
	};

	struct SgprInfo
	{
		Type type      = Type::Unknown;
		int  attrib_id = 0;
	};

	auto is_sgpr = [](const ShaderOperand& op)
	{ return op.type == ShaderOperandType::Sgpr || op.type == ShaderOperandType::VccLo || op.type == ShaderOperandType::VccHi; };
	auto sgpr_reg = [](const ShaderOperand& op)
	{ return (op.type == ShaderOperandType::VccLo ? 106 : (op.type == ShaderOperandType::VccHi ? 107 : op.register_id)); };
	auto is_vgpr  = [](const ShaderOperand& op) { return op.type == ShaderOperandType::Vgpr; };
	auto vgpr_reg = [](const ShaderOperand& op) { return op.register_id; };

	int shift_regs = 8;
	int attrib_reg = m_vs_input_info->fetch_attrib_reg + shift_regs;
	int buffer_reg = m_vs_input_info->fetch_buffer_reg + shift_regs;

	Vector<ShaderControlFlowBlock> blocks;

	blocks.Add(m_code.ReadBlock(0));

	for (const auto& label: m_code.GetLabels())
	{
		blocks.Add(m_code.ReadBlock(label.GetDst()));
	}

	for (const auto& label: m_code.GetIndirectLabels())
	{
		blocks.Add(m_code.ReadBlock(label.GetDst()));
	}

	Vector<std::pair<ShaderInstruction, int>> load_instructions;

	for (const auto& block: blocks)
	{
		auto code = m_code.ReadIntructions(block);

		Core::Array<SgprInfo, 108> sgprs;
		Core::Array<VgprInfo, 256> vgprs;

		for (const auto& inst: code)
		{

			switch (inst.type)
			{
				case ShaderInstructionType::SLoadDword:
				case ShaderInstructionType::SLoadDwordx2:
				case ShaderInstructionType::SLoadDwordx4:
				case ShaderInstructionType::SLoadDwordx8:
				case ShaderInstructionType::SLoadDwordx16:
					if (is_sgpr(inst.src[0]) && sgpr_reg(inst.src[0]) == attrib_reg)
					{
						if (!operand_is_constant(inst.src[1])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[1]) condition ignored (continuing)\n"); }
						if (inst.src[1].constant.i < 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.src[1].constant.i < 0 condition ignored (continuing)\n"); }
						int register_id = sgpr_reg(inst.dst);
						int index       = inst.src[1].constant.i / 4;
						for (int i = 0; i < inst.dst.size; i++)
						{
							sgprs[register_id + i].type      = Type::Attrib;
							sgprs[register_id + i].attrib_id = i + index;
						}
					}
					if (is_sgpr(inst.src[0]) && sgpr_reg(inst.src[0]) == buffer_reg)
					{
						if (operand_is_constant(inst.src[1])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_constant(inst.src[1]) condition ignored (continuing)\n"); }
						if (is_sgpr(inst.src[1]) && sgprs[sgpr_reg(inst.src[1])].type != Type::Attrib) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: is_sgpr(inst.src[1]) && sgprs[sgpr_reg(inst.src[1])].type != Type::Attrib condition ignored (continuing)\n"); }
						int register_id = sgpr_reg(inst.dst);
						for (int i = 0; i < inst.dst.size; i++)
						{
							sgprs[register_id + i].type      = Type::Buffer;
							sgprs[register_id + i].attrib_id = sgprs[sgpr_reg(inst.src[1])].attrib_id;
						}
					}
					break;

				case ShaderInstructionType::VCndmaskB32:
					if (is_vgpr(inst.src[0]) && vgpr_reg(inst.src[0]) == 8 && is_vgpr(inst.src[1]) && vgpr_reg(inst.src[1]) == 5)
					{
						vgprs[vgpr_reg(inst.dst)].type = Type::Index;
					}
					break;

				case ShaderInstructionType::SBfeU32:
				case ShaderInstructionType::SAndB32:
				case ShaderInstructionType::SLshlB32:
					if (is_sgpr(inst.src[0]) && sgprs[sgpr_reg(inst.src[0])].type == Type::Attrib && operand_is_constant(inst.src[1]))
					{
						sgprs[sgpr_reg(inst.dst)] = sgprs[sgpr_reg(inst.src[0])];
					}
					break;

				case ShaderInstructionType::BufferLoadFormatX:
				case ShaderInstructionType::BufferLoadFormatXy:
				case ShaderInstructionType::BufferLoadFormatXyz:
				case ShaderInstructionType::BufferLoadFormatXyzw:
				{
					if (!(vgprs[vgpr_reg(inst.src[0])].type == Type::Index &&
					                       sgprs[sgpr_reg(inst.src[1])].type == Type::Buffer &&
					                       sgprs[sgpr_reg(inst.src[2])].type == Type::Attrib)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !(vgprs[vgpr_reg(inst.src[0])].type == Type::Index && condition ignored (continuing)\n"); }

					const int semantic = sgprs[sgpr_reg(inst.src[1])].attrib_id;
					int       resource = -1;
					for (int i = 0; i < m_vs_input_info->resources_num; i++)
					{
						if (m_vs_input_info->resources_dst[i].semantic == semantic)
						{
							resource = i;
							break;
						}
					}
					if (resource < 0)
					{
						KYTY_LOG_DEBUG("WARNING: vertex fetch semantic missing input resource (continuing)\n");
					}

					load_instructions.Add({inst, resource});

					break;
				}
				default: break;
			}
		}
	}

	for (auto& inst: m_code.GetInstructions())
	{
		if (auto index = load_instructions.Find(inst.pc, [](auto i, auto pc) { return i.first.pc == pc; });
		    load_instructions.IndexValid(index))
		{
			const auto& p = load_instructions.At(index);

			KYTY_LOG_DEBUG("load vertex: pc = 0x%08" PRIx32 ", size = %d, attrib_id = %d\n", p.first.pc, p.first.dst.size, p.second);

			EXIT_IF(inst.type != p.first.type);
			EXIT_IF(inst.format != p.first.format);

			switch (inst.type)
			{
				case ShaderInstructionType::BufferLoadFormatX: inst.type = ShaderInstructionType::FetchX; break;
				case ShaderInstructionType::BufferLoadFormatXy: inst.type = ShaderInstructionType::FetchXy; break;
				case ShaderInstructionType::BufferLoadFormatXyz: inst.type = ShaderInstructionType::FetchXyz; break;
				case ShaderInstructionType::BufferLoadFormatXyzw: inst.type = ShaderInstructionType::FetchXyzw; break;
				default: break;
			}

			inst.src[2].type       = ShaderOperandType::IntegerInlineConstant;
			inst.src[2].size       = 0;
			inst.src[2].constant.i = p.second;
		}
	}
}

void Spirv::WriteInstructions()
{
	ModifyCode();

	const bool  uses_arrayed_2d_sampled_images = UsesArrayed2dImages(m_bind, ShaderTextureUsage::ReadOnly);
	const bool  uses_arrayed_2d_storage_images = UsesArrayed2dImages(m_bind, ShaderTextureUsage::ReadWrite);
	const bool  uses_uint_images                = UsesUnsignedIntegerImages(m_bind);
	int         index        = -1;
	const auto& instructions = m_code.GetInstructions();
	bool        need_debug   = (Config::SpirvDebugPrintfEnabled() && !m_code.GetDebugPrintfs().IsEmpty());
	for (const auto& inst: instructions)
	{
		index++;
		if (uses_arrayed_2d_sampled_images && IsSampledImageInstruction(inst) &&
		                     !SupportsArrayed2dImageInstruction(inst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: uses_arrayed_2d_sampled_images && IsSampledImageInstruction(inst) && condition ignored (continuing)\n"); }
		if (uses_arrayed_2d_storage_images && IsStorageImageInstruction(inst) &&
		                     !SupportsArrayed2dImageInstruction(inst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: uses_arrayed_2d_storage_images && IsStorageImageInstruction(inst) && condition ignored (continuing)\n"); }
		// Mixed 2D/3D sampled descriptors carry an explicit runtime tag. Image
		// operations that have not yet needed a 3D coordinate keep their 2D
		// path; ImageSampleL below consumes the tagged 3D path used by the
		// captured material shader.
		if (uses_uint_images && IsImageInstruction(inst) && !SupportsArrayed2dImageInstruction(inst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: uses_uint_images && IsImageInstruction(inst) && !SupportsArrayed2dImageInstruction(inst) condition ignored (continuing)\n"); }

		WriteLabel(index);

		String8 src = ShaderCode::DbgInstructionToStr(inst);
		String8 dst;
		String8 dst_debug;

		bool ok = false;

		const auto* func = RecompFunc(inst.type, inst.format);

		if (func != nullptr)
		{
			EXIT_IF(func->type != inst.type);
			EXIT_IF(func->format != inst.format);
			ok = func->func(index, m_code, &dst, this, func->param, func->scc_check);
		}

		if (!ok)
		{
			const int sampled_2d    = m_bind == nullptr ? 0 : m_bind->textures2D.textures2d_sampled_num;
			const int sampled_array = m_bind == nullptr ? 0 : m_bind->textures2D.textures2d_array_sampled_num;
			const int sampled_3d    = m_bind == nullptr ? 0 : m_bind->textures2D.textures3d_sampled_num;
			EXIT("shader emitter missing: stage=%u instruction=%u format=0x%016" PRIx64 " pc=0x%08" PRIx32
			     " sampled=%d/%d/%d\n",
			     static_cast<unsigned>(m_code.GetType()), static_cast<unsigned>(inst.type), static_cast<uint64_t>(inst.format), inst.pc,
			     sampled_2d, sampled_array, sampled_3d);
		}
		if (IsImageInstruction(inst))
		{
			dst = GuardImageDestinationStores(dst, inst, static_cast<uint32_t>(index));
		}

		m_source += String8::FromPrintf("; %s\n", src.c_str());
		m_source += String8::FromPrintf("%s\n", dst.c_str());

		uint32_t tap_pc = 0;
		int      tap_register = 0;
		if (FragmentTapSelection(m_code, &tap_pc, &tap_register) && inst.pc == tap_pc)
		{
			for (uint32_t component = 0; component < 4u; component++)
			{
				m_source += String8::FromPrintf("%%fs_tap_value_%u = OpLoad %%float %%v%d\n", component,
				                                tap_register + static_cast<int>(component));
				m_source += String8::FromPrintf("               OpStore %%fs_tap_%u %%fs_tap_value_%u\n", component, component);
			}
		}

		if (need_debug && Recompile_Inject_Debug(index, m_code, &dst_debug, this, nullptr, SccCheck::None))
		{
			m_source += String8::FromPrintf("%s\n", dst_debug.c_str());
		}
	}
}

void Spirv::WriteMainEpilog()
{
	static const char* text = R"(
                   ; Epilog
                   OpFunctionEnd
)";

	m_source += text;
}

void Spirv::WriteFunctions()
{
	if (spirv_uses_buffer_descriptor_addressing(m_code))
	{
		m_source += BUFFER_RAW_ADDRESS;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::BufferLoadUbyte}))
	{
		m_source += BUFFER_LOAD_UBYTE;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VSadU32}))
	{
		m_source += FUNC_ABS_DIFF;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SWqmB64}))
	{
		m_source += FUNC_WQM;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SAddcU32, ShaderInstructionType::VAddCoCiU32}))
	{
		m_source += FUNC_ADDC;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SLshl4AddU32}))
	{
		m_source += FUNC_LSHL_ADD;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::ImageStoreMip}))
	{
		m_source += FUNC_MIPMAP;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VCmpOF32, ShaderInstructionType::VCmpUF32}))
	{
		m_source += FUNC_ORDERED;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VMulHiI32, ShaderInstructionType::VMulLoI32, ShaderInstructionType::VMulLoU32, ShaderInstructionType::VMulHiU32,
	                     ShaderInstructionType::VMadU32U24, ShaderInstructionType::VMulU32U24, ShaderInstructionType::SMulHiU32}))
	{
		m_source += FUNC_MUL_EXTENDED;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SLshrB64, ShaderInstructionType::SBfeU64}))
	{
		m_source += FUNC_SHIFT_RIGHT;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SLshlB64, ShaderInstructionType::SBfeU64}))
	{
		m_source += FUNC_SHIFT_LEFT;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SSwappcB64, ShaderInstructionType::FetchX, ShaderInstructionType::FetchXy,
	                     ShaderInstructionType::FetchXyz, ShaderInstructionType::FetchXyzw}))
	{
		m_source += FUNC_FETCH_1;
		m_source += FUNC_FETCH_2;
		m_source += FUNC_FETCH_3;
		m_source += FUNC_FETCH_4;
	}

	const bool needs_buffer_load_helpers = m_code.HasAnyOf({
	    ShaderInstructionType::BufferLoadDword, ShaderInstructionType::BufferLoadDwordx2, ShaderInstructionType::BufferLoadDwordx3,
	    ShaderInstructionType::BufferLoadDwordx4, ShaderInstructionType::BufferLoadFormatX, ShaderInstructionType::BufferLoadFormatXy,
	    ShaderInstructionType::BufferLoadFormatXyz, ShaderInstructionType::BufferLoadFormatXyzw, ShaderInstructionType::TBufferLoadFormatX,
	    ShaderInstructionType::TBufferLoadFormatXy, ShaderInstructionType::TBufferLoadFormatXyzw});
	const bool needs_tbuffer_load_helpers = m_code.HasAnyOf({
	    ShaderInstructionType::BufferLoadFormatX, ShaderInstructionType::BufferLoadFormatXy, ShaderInstructionType::BufferLoadFormatXyz,
	    ShaderInstructionType::BufferLoadFormatXyzw, ShaderInstructionType::TBufferLoadFormatX, ShaderInstructionType::TBufferLoadFormatXy,
	    ShaderInstructionType::TBufferLoadFormatXyzw});
	const bool needs_buffer_store_helpers = m_code.HasAnyOf({
	    ShaderInstructionType::BufferStoreDword, ShaderInstructionType::BufferStoreDwordx2, ShaderInstructionType::BufferStoreDwordx3,
	    ShaderInstructionType::BufferStoreDwordx4, ShaderInstructionType::BufferStoreFormatX, ShaderInstructionType::BufferStoreFormatXy,
	    ShaderInstructionType::BufferStoreFormatXyzw});
	const bool needs_tbuffer_store_helpers = m_code.HasAnyOf({ShaderInstructionType::BufferStoreFormatX,
	                                                          ShaderInstructionType::BufferStoreFormatXy,
	                                                          ShaderInstructionType::BufferStoreFormatXyzw});

	// The scalar format predicate is a dependency of the typed buffer helper
	// families only. Raw dword operations use the buffer helpers directly and
	// must not emit function bodies that reference an omitted typed predicate.
	if (needs_tbuffer_load_helpers || needs_tbuffer_store_helpers)
	{
		m_source += TBUFFER_FORMAT_SCALAR32;
	}

	if (needs_buffer_load_helpers)
	{
		m_source += BUFFER_LOAD_FLOAT1;
		m_source += BUFFER_LOAD_FLOAT4;
	}
	if (needs_tbuffer_load_helpers)
	{
		m_source += TBUFFER_LOAD_FORMAT_X;
		m_source += TBUFFER_LOAD_FORMAT_XY;
		m_source += TBUFFER_LOAD_FORMAT_XYZW;
	}

	if (needs_buffer_store_helpers)
	{
		m_source += BUFFER_STORE_FLOAT1;
		m_source += BUFFER_STORE_FLOAT2;
		m_source += BUFFER_STORE_FLOAT4;
	}
	if (needs_tbuffer_store_helpers)
	{
		m_source += TBUFFER_STORE_FORMAT_X;
		m_source += TBUFFER_STORE_FORMAT_XY;
		m_source += TBUFFER_STORE_FORMAT_XYZW;
	}

	if (m_bind != nullptr && m_bind->storage_buffers.buffers_num > 0 &&
	    m_code.HasAnyOf({ShaderInstructionType::SBufferLoadDword, ShaderInstructionType::SBufferLoadDwordx2,
	                     ShaderInstructionType::SBufferLoadDwordx4, ShaderInstructionType::SBufferLoadDwordx8,
	                     ShaderInstructionType::SBufferLoadDwordx16}))
	{
		m_source += SBUFFER_LOAD_DWORD;
		m_source += SBUFFER_LOAD_DWORD_2;
		m_source += SBUFFER_LOAD_DWORD_4;
		m_source += SBUFFER_LOAD_DWORD_8;
		m_source += SBUFFER_LOAD_DWORD_16;
	}
}

void Spirv::FindConstants()
{
	m_constants.Clear();
	AddConstantFloat(0.0f);
	AddConstantFloat(0.5f);
	AddConstantFloat(1.0f);
	AddConstantFloat(2.0f);
	AddConstantFloat(3.0f);
	AddConstantFloat(4.0f);
	AddConstantFloat(5.0f);
	AddConstantFloat(0.0625f);
	AddConstantUint(0x1fffffffu);
	AddConstantUint(0x20000000u);
	AddConstantUint(0x40000000u);
	AddConstantUint(0x7fffffffu);
	AddConstantUint(0x80000000u);
	for (int i = 0; i <= 32; i++)
	{
		AddConstantInt(i);
		AddConstantUint(i);
	}
	if (m_bind != nullptr)
	{
		const uint32_t binding_dwords = m_bind->push_constant_size / 4u;
		const uint32_t binding_vec4s  = (binding_dwords + 3u) / 4u;
		for (uint32_t i = 33u; i <= binding_vec4s; ++i)
		{
			AddConstantInt(static_cast<int>(i));
			AddConstantUint(i);
		}
	}
	for (const auto& inst: m_code.GetInstructions())
	{
		if (inst.buffer_imm_offset != 0)
		{
			AddConstantUint(inst.buffer_imm_offset);
		}
		if (inst.type == ShaderInstructionType::SBarrier)
		{
			AddConstantUint(SPIRV_WORKGROUP_MEMORY_ACQ_REL);
		}
		if (inst.type == ShaderInstructionType::BufferAtomicAdd)
		{
			AddConstantUint(SPIRV_DEVICE_MEMORY_ACQ_REL);
		}
		for (int i = 0; i < inst.src_num; i++)
		{
			if (inst.src[i].dpp)
			{
				AddConstantUint(inst.src[i].dpp_ctrl);
				AddConstantUint(0xfffffffcu);
			}
			if (operand_is_constant(inst.src[i]))
			{
				AddConstant(inst.src[i]);
				// MUBUF/MTBUF soffset is OpStore'd into %temp_int_* (signed).
				// LiteralConstant only registers as Uint via AddConstant; also
				// emit the Int twin so GetConstantInt succeeds at recompile.
				// Offsets such as 56/80/136 appear as folded 12-bit+imm values.
				if (inst.src[i].type == ShaderOperandType::LiteralConstant)
				{
					AddConstantInt(static_cast<int>(inst.src[i].constant.u));
				}
			}
		}
		// SMEM dual-offset path adds SGPR soffset + signed imm in SPIR-V.
		if (inst.smem_imm_offset != 0)
		{
			AddConstantUint(static_cast<uint32_t>(inst.smem_imm_offset));
		}
	}
	// Attribute-table dwords materialized by SLoad from fetch_attrib_reg.
	if (m_vs_input_info != nullptr)
	{
		for (int i = 0; i < m_vs_input_info->fetch_attrib_data_num; i++)
		{
			AddConstantUint(m_vs_input_info->fetch_attrib_data[i]);
		}
	}
	if (m_vs_input_info != nullptr || m_ps_input_info != nullptr || m_cs_input_info != nullptr)
	{
		AddConstantInt(12);
		AddConstantInt(16);
		AddConstantInt(31);
		AddConstantInt(36);
		AddConstantInt(39);
		AddConstantInt(75);
		AddConstantInt(76);
		AddConstantInt(77);
		AddConstantInt(92);
		AddConstantInt(95);
		AddConstantInt(119);
		AddConstantUint(24);
		AddConstantUint(1);
		AddConstantUint(3);
		AddConstantUint(31);
		AddConstantUint(32);
		AddConstantUint(63);
		AddConstantUint(64);
		AddConstantUint(72);
		AddConstantUint(127);
		AddConstantUint(103);
		AddConstantUint(112);
		AddConstantUint(126);
		AddConstantUint(143);
		AddConstantUint(255);
		AddConstantUint(0x00008000);
		AddConstantUint(0x000000ff);
		AddConstantUint(0x007fffff);
		AddConstantUint(0x00800000);
		AddConstantUint(0x00000200);
		AddConstantUint(0x00007c00);
		AddConstantUint(0x00007bff);
		AddConstantUint(0x0000ffff);
		AddConstantUint(0x3fff);
		AddConstantUint(0xffffff);
		AddConstantUint(0xffffffff);
		AddConstantUint(0x0000000f);
		AddConstantUint(0x000000f0);
		AddConstantUint(0x00000f00);
		AddConstantUint(0x0000f000);
		AddConstantUint(0x000f0000);
		AddConstantUint(0x00f00000);
		AddConstantUint(0x0f000000);
		AddConstantUint(0xf0000000);
	}
	if (m_ps_input_info != nullptr && m_ps_input_info->ps_pos_xy && !m_ps_input_info->host_to_guest_scale.IsIdentity())
	{
		const auto& scale = m_ps_input_info->host_to_guest_scale;
		AddConstantFloat(static_cast<float>(scale.x_guest_numerator));
		AddConstantFloat(static_cast<float>(scale.x_host_denominator));
		AddConstantFloat(static_cast<float>(scale.y_guest_numerator));
		AddConstantFloat(static_cast<float>(scale.y_host_denominator));
	}
	if (m_cs_input_info != nullptr)
	{
		AddConstantUint(m_cs_input_info->threads_num[0]);
		AddConstantUint(m_cs_input_info->threads_num[1]);
		AddConstantUint(m_cs_input_info->threads_num[2]);
	}
}

void Spirv::FindVariables()
{
	m_variables.Clear();

	AddVariable(ShaderOperandType::Vgpr, 0, 1);
	AddVariable(ShaderOperandType::ExecLo, 0, 2);
	AddVariable(ShaderOperandType::ExecZ, 0, 1);
	AddVariable(ShaderOperandType::Scc, 0, 1);

	for (const auto& inst: m_code.GetInstructions())
	{
		AddVariable(inst.dst);
		AddVariable(inst.dst2);
		for (int i = 0; i < inst.src_num; i++)
		{
			AddVariable(inst.src[i]);
		}
		for (int address = 0; address < inst.mimg_address_num; ++address)
		{
			AddVariable(inst.mimg_address[address]);
		}
	}

	if (m_vs_input_info != nullptr)
	{
		if (m_vs_input_info->gs_prolog)
		{
			AddVariable(ShaderOperandType::Vgpr, 5, 1);
			AddVariable(ShaderOperandType::Vgpr, 8, 1);
		} else
		{
			AddVariable(ShaderOperandType::Vgpr, 3, 1);
		}
		for (int i = 0; i < m_vs_input_info->resources_num; i++)
		{
			AddVariable(ShaderOperandType::Vgpr, m_vs_input_info->resources_dst[i].register_start,
			            m_vs_input_info->resources_dst[i].registers_num);
		}
	}

	if (m_ps_input_info != nullptr)
	{
		if (m_ps_input_info->ps_pos_xy)
		{
			if (!m_ps_input_info->host_to_guest_scale.IsValid()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !m_ps_input_info->host_to_guest_scale.IsValid() condition ignored (continuing)\n"); }
			AddVariable(ShaderOperandType::Vgpr, 2, 1);
			AddVariable(ShaderOperandType::Vgpr, 3, 1);
		}
	}

	if (m_cs_input_info != nullptr)
	{
		AddVariable(ShaderOperandType::Vgpr, 0, 3);
		AddVariable(ShaderOperandType::Sgpr, m_cs_input_info->workgroup_register, 3);
	}

	if (m_bind != nullptr)
	{
		int shift_regs = (m_vs_input_info != nullptr && m_vs_input_info->gs_prolog ? 8 : 0);

		for (int i = 0; i < m_bind->storage_buffers.buffers_num; i++)
		{
			int storage_start = m_bind->storage_buffers.start_register[i] + shift_regs;
			AddVariable(ShaderOperandType::Sgpr, storage_start, 4);
		}
		for (int i = 0; i < m_bind->textures2D.textures_num; i++)
		{
			int storage_start = m_bind->textures2D.desc[i].start_register + shift_regs;
			AddVariable(ShaderOperandType::Sgpr, storage_start, 8);
		}
		for (int i = 0; i < m_bind->samplers.samplers_num; i++)
		{
			int storage_start = m_bind->samplers.start_register[i] + shift_regs;
			AddVariable(ShaderOperandType::Sgpr, storage_start, 8);
		}
		for (int i = 0; i < m_bind->direct_sgprs.sgprs_num; i++)
		{
			int direct_start = m_bind->direct_sgprs.start_register[i] + shift_regs;
			AddVariable(ShaderOperandType::Sgpr, direct_start, 1);
		}
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
