#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/RenderResolutionShaderUsageCache.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "ShaderSpirvInternal.h"
#include "ShaderSpirvToolchain.h"
#include "ShaderStorageAnalysis.h"
#include "ShaderDebugInternal.h"
#include "ShaderLogInternal.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/VulkanVertexInputFormat.h"
#include "Emulator/Profiler.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

// #define SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
// #include "spirv_cross/spirv_glsl.hpp"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static RenderResolutionShaderUsageCache g_shader_resolution_usage_cache(512);

bool ShaderSamplerDepthComparisonEligible(const ShaderTextureResources& textures, const ShaderSamplerResources& samplers,
                                          int sampler_index)
{
	if (sampler_index < 0 || sampler_index >= samplers.samplers_num ||
	    samplers.operations[sampler_index] != State::ImageSampleOperation::DepthReference ||
	    samplers.samplers[sampler_index].ForceUnormCoords())
	{
		return false;
	}

	const auto eligible = [](const ShaderTextureDescriptor& descriptor)
	{
		return descriptor.usage == ShaderTextureUsage::ReadOnly &&
		       descriptor.sample_operation == State::ImageSampleOperation::DepthReference &&
		       ShaderResolvedSampledTextureShape(descriptor) == ShaderGen5SampledTextureShape::TwoDimensional;
	};

	bool matched = false;
	for (int i = 0; i < textures.textures_num; ++i)
	{
		const auto& descriptor = textures.desc[i];
		if (descriptor.usage != ShaderTextureUsage::ReadOnly || descriptor.slot != samplers.slots[sampler_index])
		{
			continue;
		}
		matched = true;
		if (!eligible(descriptor))
		{
			return false;
		}
	}
	if (matched)
	{
		return true;
	}

	// Dynamic/legacy metadata may not preserve pair slots. Without an exact
	// match, require every sampled descriptor in the bind to be compatible.
	for (int i = 0; i < textures.textures_num; ++i)
	{
		if (textures.desc[i].usage == ShaderTextureUsage::ReadOnly && !eligible(textures.desc[i]))
		{
			return false;
		}
		matched = matched || textures.desc[i].usage == ShaderTextureUsage::ReadOnly;
	}
	return matched;
}

ShaderSampledImageViewDecision ResolveDepthReferenceImageView(State::ImageSampleOperation operation,
                                                              ShaderGen5SampledTextureShape shape, bool floating_point,
                                                              ShaderSampledImageViewKind resolved_view)
{
	if (operation == State::ImageSampleOperation::Regular)
	{
		const bool compatible =
		    (shape == ShaderGen5SampledTextureShape::TwoDimensional &&
		     (resolved_view == ShaderSampledImageViewKind::Color2D || resolved_view == ShaderSampledImageViewKind::Depth2D)) ||
		    (shape == ShaderGen5SampledTextureShape::TwoDimensionalArray &&
		     (resolved_view == ShaderSampledImageViewKind::Color2DArray || resolved_view == ShaderSampledImageViewKind::Depth2DArray)) ||
		    (shape == ShaderGen5SampledTextureShape::ThreeDimensional && resolved_view == ShaderSampledImageViewKind::Color3D);
		return {compatible, resolved_view};
	}
	if (operation == State::ImageSampleOperation::Mixed)
	{
		const bool compatible =
		    floating_point &&
		    ((shape == ShaderGen5SampledTextureShape::TwoDimensional &&
		      (resolved_view == ShaderSampledImageViewKind::Color2D || resolved_view == ShaderSampledImageViewKind::Depth2D)) ||
		     (shape == ShaderGen5SampledTextureShape::TwoDimensionalArray &&
		      (resolved_view == ShaderSampledImageViewKind::Color2DArray || resolved_view == ShaderSampledImageViewKind::Depth2DArray)));
		return {compatible, resolved_view};
	}
	if (!floating_point)
	{
		return {false, resolved_view};
	}
	const bool compatible =
	    (shape == ShaderGen5SampledTextureShape::TwoDimensional && resolved_view == ShaderSampledImageViewKind::Depth2D) ||
	    (shape == ShaderGen5SampledTextureShape::TwoDimensionalArray && resolved_view == ShaderSampledImageViewKind::Depth2DArray);
	return {compatible, resolved_view};
}


void RecordShaderInputAnalysis(uint64_t elapsed_ns)
{
	DebugStatsRecordShaderIrParse(DebugStatsShaderParseKind::InputAnalysis, elapsed_ns);
}

void RecordShaderPipelineMissParse(uint64_t elapsed_ns)
{
	DebugStatsRecordShaderIrParse(DebugStatsShaderParseKind::PipelineMiss, elapsed_ns);
}

static bool ShaderIsVccCompare(ShaderInstructionType type)
{
	const auto value = static_cast<uint32_t>(type);
	return (value >= static_cast<uint32_t>(ShaderInstructionType::VCmpEqF32) &&
	        value <= static_cast<uint32_t>(ShaderInstructionType::VCmpTU32)) ||
	       (value >= static_cast<uint32_t>(ShaderInstructionType::VCmpxEqF32) &&
	        value <= static_cast<uint32_t>(ShaderInstructionType::VCmpxUF32));
}

static bool ShaderInstructionTypeChangesExec(ShaderInstructionType type)
{
	const auto value = static_cast<uint32_t>(type);
	if (value >= static_cast<uint32_t>(ShaderInstructionType::VCmpxEqF32) &&
	    value <= static_cast<uint32_t>(ShaderInstructionType::VCmpxUF32))
	{
		return true;
	}
	switch (type)
	{
		case ShaderInstructionType::SAndSaveexecB64:
		case ShaderInstructionType::SAndn1SaveexecB64:
		case ShaderInstructionType::SAndn2SaveexecB64:
		case ShaderInstructionType::SNandSaveexecB64:
		case ShaderInstructionType::SNorSaveexecB64:
		case ShaderInstructionType::SOrSaveexecB64:
		case ShaderInstructionType::SOrn2SaveexecB64:
		case ShaderInstructionType::SXnorSaveexecB64:
		case ShaderInstructionType::SXorSaveexecB64:
		case ShaderInstructionType::VCmpxEqI16:
		case ShaderInstructionType::VCmpxEqU16:
		case ShaderInstructionType::VCmpxGeI16:
		case ShaderInstructionType::VCmpxGeU16:
		case ShaderInstructionType::VCmpxGtI16:
		case ShaderInstructionType::VCmpxGtU16:
		case ShaderInstructionType::VCmpxLeI16:
		case ShaderInstructionType::VCmpxLeU16:
		case ShaderInstructionType::VCmpxNeI16:
		case ShaderInstructionType::VCmpxNeU16:
		case ShaderInstructionType::VCmpxLtI16:
		case ShaderInstructionType::VCmpxLtU16: return true;
		default: return false;
	}
}

static bool ShaderIsWaveScalarOperand(const ShaderOperand& operand)
{
	if (operand.dpp)
	{
		return false;
	}

	switch (operand.type)
	{
		case ShaderOperandType::LiteralConstant:
		case ShaderOperandType::IntegerInlineConstant:
		case ShaderOperandType::FloatInlineConstant:
		case ShaderOperandType::Sgpr:
		case ShaderOperandType::Scc:
		case ShaderOperandType::M0: return true;
		default: return false;
	}
}

static bool ShaderInstructionWritesVcc(const ShaderInstruction& inst)
{
	return inst.dst.type == ShaderOperandType::VccLo || inst.dst.type == ShaderOperandType::VccHi ||
	       inst.dst2.type == ShaderOperandType::VccLo || inst.dst2.type == ShaderOperandType::VccHi;
}

static bool ShaderInstructionWritesVgpr(const ShaderInstruction& inst, int register_id)
{
	const auto covers = [register_id](const ShaderOperand& dst)
	{
		if (dst.type != ShaderOperandType::Vgpr || register_id < dst.register_id)
		{
			return false;
		}
		const int size = std::max(dst.size, 1);
		return register_id - dst.register_id < size;
	};
	return covers(inst.dst) || covers(inst.dst2);
}

static bool ShaderInstructionIsPureLaneAlu(const ShaderInstruction& inst)
{
	if (inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size > 1 || inst.src_num <= 0 || inst.src_num > 2)
	{
		return false;
	}

	for (int source = 0; source < inst.src_num; ++source)
	{
		if (inst.src[source].dpp)
		{
			return false;
		}
	}

	if (inst.src_num == 1)
	{
		switch (inst.type)
		{
			case ShaderInstructionType::VCeilF32:
			case ShaderInstructionType::VCosF32:
			case ShaderInstructionType::VCvtF32F16:
			case ShaderInstructionType::VCvtF32I32:
			case ShaderInstructionType::VCvtF32U32:
			case ShaderInstructionType::VCvtF32Ubyte0:
			case ShaderInstructionType::VCvtF32Ubyte1:
			case ShaderInstructionType::VCvtF32Ubyte2:
			case ShaderInstructionType::VCvtF32Ubyte3:
			case ShaderInstructionType::VCvtFlrI32F32:
			case ShaderInstructionType::VCvtI32F32:
			case ShaderInstructionType::VCvtU32F32:
			case ShaderInstructionType::VExpF32:
			case ShaderInstructionType::VFloorF32:
			case ShaderInstructionType::VFractF32:
			case ShaderInstructionType::VLogF32:
			case ShaderInstructionType::VMovB32:
			case ShaderInstructionType::VNotB32:
			case ShaderInstructionType::VRcpF32:
			case ShaderInstructionType::VRndneF32:
			case ShaderInstructionType::VRsqF32:
			case ShaderInstructionType::VSinF32:
			case ShaderInstructionType::VSqrtF32:
			case ShaderInstructionType::VTruncF32: return true;
			default: return false;
		}
	}

	switch (inst.type)
	{
		case ShaderInstructionType::VAddF32:
		case ShaderInstructionType::VAddI32:
		case ShaderInstructionType::VAndB32:
		case ShaderInstructionType::VAndOrB32:
		case ShaderInstructionType::VAshrI32:
		case ShaderInstructionType::VAshrrevI32:
		case ShaderInstructionType::VBcntU32B32:
		case ShaderInstructionType::VBfeI32:
		case ShaderInstructionType::VBfeU32:
		case ShaderInstructionType::VBfiB32:
		case ShaderInstructionType::VBfmB32:
		case ShaderInstructionType::VBfrevB32:
		case ShaderInstructionType::VLshlB32:
		case ShaderInstructionType::VLshlrevB32:
		case ShaderInstructionType::VLshrB32:
		case ShaderInstructionType::VLshrrevB32:
		case ShaderInstructionType::VMaxF32:
		case ShaderInstructionType::VMaxI32:
		case ShaderInstructionType::VMaxU32:
		case ShaderInstructionType::VMinF32:
		case ShaderInstructionType::VMinI32:
		case ShaderInstructionType::VMinU32:
		case ShaderInstructionType::VMulF32:
		case ShaderInstructionType::VMulHiI32:
		case ShaderInstructionType::VMulHiU32:
		case ShaderInstructionType::VMulLoI32:
		case ShaderInstructionType::VMulLoU32:
		case ShaderInstructionType::VMulU32U24:
		case ShaderInstructionType::VOrB32:
		case ShaderInstructionType::VSubF32:
		case ShaderInstructionType::VSubI32:
		case ShaderInstructionType::VSubrevF32:
		case ShaderInstructionType::VSubrevI32:
		case ShaderInstructionType::VXnorB32:
		case ShaderInstructionType::VXorB32: return true;
		default: return false;
	}
}

static bool ShaderInstructionWritesExec(const ShaderInstruction& inst)
{
	return inst.dst.type == ShaderOperandType::ExecLo || inst.dst.type == ShaderOperandType::ExecHi ||
	       inst.dst.type == ShaderOperandType::ExecZ || inst.dst2.type == ShaderOperandType::ExecLo ||
	       inst.dst2.type == ShaderOperandType::ExecHi || inst.dst2.type == ShaderOperandType::ExecZ ||
	       ShaderInstructionTypeChangesExec(inst.type);
}

static bool ShaderInstructionIsNoopExecWrite(const ShaderInstruction& inst)
{
	return inst.type == ShaderInstructionType::SWqmB64 && inst.dst.type == ShaderOperandType::ExecLo && inst.src_num >= 1 &&
	       inst.src[0].type == ShaderOperandType::ExecLo;
}

static bool ShaderInstructionIsControlFlowBoundary(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::SBranch:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz:
		case ShaderInstructionType::SSetpcB64:
		case ShaderInstructionType::SSwappcB64:
		case ShaderInstructionType::SEndpgm: return true;
		default: return false;
	}
}

static bool ShaderOperandWritesVccWord(const ShaderOperand& dst, ShaderOperandType word)
{
	if (word == ShaderOperandType::VccLo)
	{
		return dst.type == ShaderOperandType::VccLo;
	}
	if (word == ShaderOperandType::VccHi)
	{
		return dst.type == ShaderOperandType::VccHi || (dst.type == ShaderOperandType::VccLo && dst.size >= 2);
	}
	return word == ShaderOperandType::VccZ && dst.type == ShaderOperandType::VccZ;
}

static bool ShaderInstructionWritesVccWord(const ShaderInstruction& inst, ShaderOperandType word)
{
	return ShaderOperandWritesVccWord(inst.dst, word) || ShaderOperandWritesVccWord(inst.dst2, word);
}

static bool ShaderExecRemainsInitial(const ShaderCode& code, uint32_t use_index)
{
	if (use_index > code.GetInstructions().Size())
	{
		return false;
	}
	for (uint32_t index = 0; index < use_index; ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		if (ShaderInstructionWritesExec(inst) && !ShaderInstructionIsNoopExecWrite(inst))
		{
			return false;
		}
	}
	return true;
}

static bool ShaderRangeHasIncomingEdge(const ShaderCode& code, uint32_t definition_index, uint32_t use_index)
{
	if (definition_index >= use_index || use_index >= code.GetInstructions().Size())
	{
		return true;
	}

	const uint32_t definition_pc = code.GetInstructions().At(definition_index).pc;
	const uint32_t use_pc        = code.GetInstructions().At(use_index).pc;
	const auto     enters_range  = [definition_pc, use_pc](const ShaderLabel& label)
	{
		return !label.IsDisabled() && label.GetDst() > definition_pc && label.GetDst() <= use_pc;
	};
	for (const auto& label: code.GetLabels())
	{
		if (enters_range(label))
		{
			return true;
		}
	}
	for (const auto& label: code.GetIndirectLabels())
	{
		if (enters_range(label))
		{
			return true;
		}
	}
	return false;
}

static bool ShaderOperandIsWaveUniform(const ShaderCode& code, const ShaderOperand& operand, uint32_t use_index, uint32_t depth);

static bool ShaderVccWordIsWaveUniform(const ShaderCode& code, ShaderOperandType word, uint32_t use_index, uint32_t depth)
{
	if (depth >= 32 || use_index > code.GetInstructions().Size() ||
	    (word != ShaderOperandType::VccLo && word != ShaderOperandType::VccHi && word != ShaderOperandType::VccZ))
	{
		return false;
	}

	for (int index = static_cast<int>(use_index) - 1; index >= 0; --index)
	{
		const auto& definition = code.GetInstructions().At(static_cast<uint32_t>(index));
		if (ShaderInstructionIsControlFlowBoundary(definition))
		{
			return false;
		}
		if (!ShaderInstructionWritesVccWord(definition, word))
		{
			continue;
		}
		// Only the scalar select chain observed in the material shader is admitted.
		// Vector compares and unknown VCC writers remain lane-dependent.
		if (definition.type != ShaderInstructionType::SCselectB32 || definition.src_num != 2 || definition.dst.size > 1)
		{
			return false;
		}
		if (ShaderRangeHasIncomingEdge(code, static_cast<uint32_t>(index), use_index))
		{
			return false;
		}
		for (int source = 0; source < definition.src_num; ++source)
		{
			if (!ShaderOperandIsWaveUniform(code, definition.src[source], static_cast<uint32_t>(index), depth + 1))
			{
				return false;
			}
		}
		return true;
	}

	return false;
}

static bool ShaderOperandIsWaveUniform(const ShaderCode& code, const ShaderOperand& operand, uint32_t use_index,
	                                   uint32_t depth)
{
	if (operand.dpp || depth >= 32 || use_index > code.GetInstructions().Size())
	{
		return false;
	}
	if (operand.type == ShaderOperandType::VccLo || operand.type == ShaderOperandType::VccHi ||
	    operand.type == ShaderOperandType::VccZ)
	{
		return ShaderVccWordIsWaveUniform(code, operand.type, use_index, depth + 1);
	}
	if (ShaderIsWaveScalarOperand(operand))
	{
		return true;
	}
	if (operand.type != ShaderOperandType::Vgpr)
	{
		return false;
	}

	for (int index = static_cast<int>(use_index) - 1; index >= 0; --index)
	{
		const auto& definition = code.GetInstructions().At(static_cast<uint32_t>(index));
		if (ShaderInstructionIsControlFlowBoundary(definition))
		{
			return false;
		}
		if (!ShaderInstructionWritesVgpr(definition, operand.register_id))
		{
			continue;
		}

		if (!ShaderInstructionIsPureLaneAlu(definition))
		{
			return false;
		}
		if (ShaderRangeHasIncomingEdge(code, static_cast<uint32_t>(index), use_index))
		{
			return false;
		}

		for (uint32_t between = static_cast<uint32_t>(index + 1); between < use_index; ++between)
		{
			if (ShaderInstructionWritesExec(code.GetInstructions().At(between)))
			{
				return false;
			}
		}

		for (int source = 0; source < definition.src_num; ++source)
		{
			if (!ShaderOperandIsWaveUniform(code, definition.src[source], static_cast<uint32_t>(index), depth + 1))
			{
				return false;
			}
		}
		return true;
	}

	return false;
}

bool ShaderReadfirstlaneCanUseUniformCopy(const ShaderCode& code, uint32_t instruction_index)
{
	if (instruction_index >= code.GetInstructions().Size())
	{
		return false;
	}

	const auto& inst = code.GetInstructions().At(instruction_index);
	return inst.type == ShaderInstructionType::VReadfirstlaneB32 && inst.src_num >= 1 &&
	       ShaderExecRemainsInitial(code, instruction_index) &&
	       ShaderOperandIsWaveUniform(code, inst.src[0], instruction_index, 0);
}

bool ShaderVccBranchIsWaveUniform(const ShaderCode& code, uint32_t instruction_index)
{
	if (instruction_index >= code.GetInstructions().Size())
	{
		return false;
	}

	const auto branch_type = code.GetInstructions().At(instruction_index).type;
	if (branch_type != ShaderInstructionType::SCbranchVccz && branch_type != ShaderInstructionType::SCbranchVccnz)
	{
		return false;
	}

	int compare_index = -1;
	for (int index = static_cast<int>(instruction_index) - 1; index >= 0; --index)
	{
		const auto& inst = code.GetInstructions().At(static_cast<uint32_t>(index));
		if (ShaderInstructionIsControlFlowBoundary(inst))
		{
			return false;
		}
		if (ShaderInstructionWritesExec(inst))
		{
			return false;
		}
		if (ShaderInstructionWritesVcc(inst))
		{
			if (!ShaderIsVccCompare(inst.type))
			{
				return false;
			}
			compare_index = index;
			break;
		}
	}

	if (compare_index < 0)
	{
		return false;
	}

	const auto& compare = code.GetInstructions().At(static_cast<uint32_t>(compare_index));
	if (ShaderInstructionTypeChangesExec(compare.type) || compare.src_num < 2)
	{
		return false;
	}

	for (uint32_t index = static_cast<uint32_t>(compare_index + 1); index < instruction_index; ++index)
	{
		if (ShaderInstructionWritesExec(code.GetInstructions().At(index)) ||
		    ShaderInstructionWritesVcc(code.GetInstructions().At(index)))
		{
			return false;
		}
	}

	for (int source = 0; source < compare.src_num; source++)
	{
		if (!ShaderOperandIsWaveUniform(code, compare.src[source], static_cast<uint32_t>(compare_index), 0))
		{
			return false;
		}
	}
	return true;
}

static bool ShaderPixelUsesSubgroupSemantics(const ShaderCode& code)
{
	if (UsesNativeLaneExchange(code) || code.HasAnyOf({ShaderInstructionType::VMbcntLoU32B32,
	                   ShaderInstructionType::VMbcntHiU32B32, ShaderInstructionType::SCbranchExecz}))
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

	for (const auto& inst: code.GetInstructions())
	{
		for (uint32_t source = 0; source < static_cast<uint32_t>(inst.src_num); source++)
		{
			if (inst.src[source].dpp)
			{
				return true;
			}
		}
	}
	return false;
}

static uint32_t ShaderPixelRequiredSubgroupSize(const ShaderCode& code, bool wave32)
{
	return ShaderPixelUsesSubgroupSemantics(code) ? (wave32 ? 32u : 64u) : 0u;
}











static Vector<uint64_t>*                               g_disabled_shaders = nullptr;
static Vector<ShaderDebugPrintfCmds>*                  g_debug_printfs    = nullptr;
static std::unordered_map<uint64_t, ShaderMappedData>* g_shader_map       = nullptr;
static std::mutex                                      g_shader_map_mutex;
static std::shared_mutex                               g_shader_lifetime_mutex;
struct VertexOffsetCacheEntry
{
	uint32_t hash0  = 0;
	uint32_t crc32  = 0;
	int32_t  offset = -1;
};
static std::unordered_map<uint64_t, VertexOffsetCacheEntry>* g_vertex_offset_sgpr_map = nullptr;
static std::mutex                                      g_vertex_offset_sgpr_mutex;
static std::unordered_map<uint64_t, uint64_t>*         g_shader_continuations  = nullptr;
static std::unordered_map<uint64_t, std::shared_ptr<ShaderCode>>* g_vs_isa_cache = nullptr;
static std::mutex                                      g_vs_isa_cache_mutex;

static std::shared_ptr<ShaderCode> GetCachedParsedVsIsa(uint64_t shader_addr, uint32_t hash0, uint32_t crc32);

static bool NggCapturedBufferQuad(const HW::UserSgprInfo& user_sgpr, int user_sgpr_num, int start)
{
	const int captured = std::min(std::min(std::max(user_sgpr_num, 0), static_cast<int>(user_sgpr.count)),
	                              HW::UserSgprInfo::SGPRS_MAX);
	if (start < 0 || start + 4 > captured)
	{
		return false;
	}
	bool typed = true;
	for (int i = 0; i < 4; ++i)
	{
		const auto type = user_sgpr.type[start + i];
		if (type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region)
		{
			typed = false;
		}
	}
	const uint32_t word0 = user_sgpr.value[start];
	const uint32_t word1 = user_sgpr.value[start + 1];
	const uint32_t word2 = user_sgpr.value[start + 2];
	const uint32_t word3 = user_sgpr.value[start + 3];
	const bool     untyped_buffer = ((word0 | word1 | word2 | word3) != 0u) && (((word3 >> 28u) & 0xfu) < 8u);
	return typed || untyped_buffer;
}

// AGC metadata lists NGG constant V# sharps at the shader scalar register
// (s8+). Hardware user-data slot 0 is that register minus 8. Rebase only when
// the ISA actually SBUFFERs that scalar and the captured user-data quad looks
// like a buffer — same contract as the measured NGG constant-sharp rebase.
static void RebaseNggConstantSharps(ShaderUserData* user_data, const ShaderCode* code, const HW::UserSgprInfo& user_sgpr,
                                    int user_sgpr_num)
{
	if (user_data == nullptr || code == nullptr || user_data->sharp_resource_offset[3] == nullptr)
	{
		return;
	}
	constexpr int kNggScalarBase = 8;
	bool          used[108]      = {};
	for (const auto& inst: code->GetInstructions())
	{
		if (!ShaderInstructionIsScalarBufferLoad(inst) || inst.src_num == 0 || inst.src[0].type != ShaderOperandType::Sgpr)
		{
			continue;
		}
		const int reg = inst.src[0].register_id;
		if (reg >= 0 && reg < 108)
		{
			used[reg] = true;
		}
	}
	for (uint16_t slot = 0; slot < user_data->sharp_resource_count[3]; ++slot)
	{
		auto& sharp = user_data->sharp_resource_offset[3][slot];
		if (sharp.offset_dw == 0x7fff || sharp.size != 1)
		{
			continue;
		}
		const int scalar = static_cast<int>(sharp.offset_dw);
		const int captured = std::min(std::min(std::max(user_sgpr_num, 0), static_cast<int>(user_sgpr.count)),
		                              HW::UserSgprInfo::SGPRS_MAX);
		// Metadata that already names a captured user-data slot (offset <
		// captured) is left alone. Only scalar registers past that window
		// are NGG-encoded (s8 + slot) and need the minus-eight rebase.
		if (scalar < captured || scalar < kNggScalarBase || scalar >= 108 || !used[scalar])
		{
			continue;
		}
		const int hardware_slot = scalar - kNggScalarBase;
		if (!NggCapturedBufferQuad(user_sgpr, user_sgpr_num, hardware_slot))
		{
			continue;
		}
		sharp.offset_dw = static_cast<uint16_t>(hardware_slot);
	}
}

void ShaderInit()
{
	EXIT_IF(g_shader_map != nullptr);

	g_shader_map             = new std::unordered_map<uint64_t, ShaderMappedData>();
	g_vertex_offset_sgpr_map = new std::unordered_map<uint64_t, VertexOffsetCacheEntry>();
	g_shader_continuations   = new std::unordered_map<uint64_t, uint64_t>();
	g_vs_isa_cache           = new std::unordered_map<uint64_t, std::shared_ptr<ShaderCode>>();
}

void ShaderMapUserData(uint64_t addr, const ShaderMappedData& data)
{
	EXIT_IF(g_shader_map == nullptr);

	std::unique_lock lifetime_lock(g_shader_lifetime_mutex);
	std::scoped_lock lock(g_shader_map_mutex);
	// A new mapping at the same guest address is a new shader generation. Any
	// relation using its previous owned range as front or back must not survive
	// address reuse.
	uint32_t previous_size = 0;
	if (const auto previous = g_shader_map->find(addr); previous != g_shader_map->end())
	{
		previous_size = previous->second.code_size_bytes;
	}
	const auto belonged_to_previous = [addr, previous_size](uint64_t shader_addr)
	{
		return shader_addr == addr ||
		       (previous_size != 0u && shader_addr >= addr && shader_addr - addr < static_cast<uint64_t>(previous_size));
	};
	for (auto continuation = g_shader_continuations->begin(); continuation != g_shader_continuations->end();)
	{
		if (belonged_to_previous(continuation->first) || belonged_to_previous(continuation->second))
		{
			continuation = g_shader_continuations->erase(continuation);
		} else
		{
			++continuation;
		}
	}
	{
		std::scoped_lock cache_lock(g_vs_isa_cache_mutex, g_vertex_offset_sgpr_mutex);
		if (g_vs_isa_cache != nullptr)
		{
			for (auto cached = g_vs_isa_cache->begin(); cached != g_vs_isa_cache->end();)
			{
				if (belonged_to_previous(cached->first))
				{
					cached = g_vs_isa_cache->erase(cached);
				} else
				{
					++cached;
				}
			}
		}
		if (g_vertex_offset_sgpr_map != nullptr)
		{
			for (auto cached = g_vertex_offset_sgpr_map->begin(); cached != g_vertex_offset_sgpr_map->end();)
			{
				if (belonged_to_previous(cached->first))
				{
					cached = g_vertex_offset_sgpr_map->erase(cached);
				} else
				{
					++cached;
				}
			}
		}
	}
	g_shader_map->insert_or_assign(addr, data);
}

static bool ShaderHasMappedCodeRangeLocked(uint64_t addr)
{
	for (const auto& [base, mapped]: *g_shader_map)
	{
		if (mapped.code_size_bytes != 0u && addr >= base && addr - base < mapped.code_size_bytes)
		{
			return true;
		}
	}
	return false;
}

bool ShaderRegisterContinuation(uint64_t front_code_addr, uint64_t back_code_addr)
{
	EXIT_IF(g_shader_continuations == nullptr);
	if (front_code_addr == 0 || back_code_addr == 0 || front_code_addr == back_code_addr)
	{
		return false;
	}
	std::unique_lock lifetime_lock(g_shader_lifetime_mutex);
	std::scoped_lock lock(g_shader_map_mutex);
	if (!ShaderHasMappedCodeRangeLocked(front_code_addr) || !ShaderHasMappedCodeRangeLocked(back_code_addr))
	{
		return false;
	}
	g_shader_continuations->insert_or_assign(front_code_addr, back_code_addr);
	return true;
}

uint64_t ShaderLookupContinuation(uint64_t front_code_addr)
{
	EXIT_IF(g_shader_continuations == nullptr);
	if (front_code_addr == 0)
	{
		return 0;
	}
	std::scoped_lock lock(g_shader_map_mutex);
	if (auto it = g_shader_continuations->find(front_code_addr); it != g_shader_continuations->end())
	{
		return it->second;
	}
	return 0;
}

enum class ShaderContinuationMode : uint8_t
{
	None,
	AllowTerminator,
	Append,
};

static void ShaderParseMappedLocked(uint64_t shader_addr, ShaderCode* code, ShaderContinuationMode continuation_mode);
static void ShaderParseMapped(uint64_t shader_addr, ShaderCode* code,
	                          ShaderContinuationMode continuation_mode = ShaderContinuationMode::None);

bool ShaderHasTerminalSetpc(const ShaderCode& code)
{
	return !code.GetInstructions().IsEmpty() &&
	       code.GetInstructions().At(code.GetInstructions().Size() - 1u).type == ShaderInstructionType::SSetpcB64;
}

// Linearize a Gen5 fused front→back chain: append the back half after the
// front's instructions and rewrite terminal s_setpc into a static branch so
// the SPIR-V CFG reaches position/param exports in the back half.
static void ShaderAppendContinuation(ShaderCode* code, uint64_t back_code_addr)
{
	EXIT_IF(code == nullptr || back_code_addr == 0);
	if (!ShaderHasTerminalSetpc(*code))
	{
		return;
	}
	const uint32_t front_terminal_index = code->GetInstructions().Size() - 1u;

	const auto* back_src = reinterpret_cast<const uint32_t*>(back_code_addr);
	if (back_src == nullptr)
	{
		return;
	}

	ShaderCode back;
	back.SetType(code->GetType());
	ShaderParseMappedLocked(back_code_addr, &back, ShaderContinuationMode::None);
	if (back.GetInstructions().IsEmpty())
	{
		return;
	}

	uint32_t front_max_pc = 0;
	for (const auto& inst: code->GetInstructions())
	{
		if (inst.pc >= front_max_pc)
		{
			front_max_pc = inst.pc;
		}
	}
	// Place the back half after the front's last instruction dword so PCs stay
	// unique. Relative branches inside the back half remain valid because every
	// back PC is shifted by the same constant.
	const uint32_t pc_offset       = front_max_pc + 16u;
	const uint32_t back_entry_pc   = back.GetInstructions().At(0).pc + pc_offset;

	for (auto& inst: back.GetInstructions())
	{
		inst.pc += pc_offset;
		code->GetInstructions().Add(inst);
	}
	for (const auto& label: back.GetLabels())
	{
		code->GetLabels().Add(ShaderLabel(label.GetDst() + pc_offset, label.GetSrc() + pc_offset));
	}
	for (const auto& label: back.GetIndirectLabels())
	{
		code->GetIndirectLabels().Add(ShaderLabel(label.GetDst() + pc_offset, label.GetSrc() + pc_offset));
	}

	// Only the effective final transfer belongs to the registered back half.
	// Earlier setpc instructions may belong to other control-flow paths.
	uint32_t terminal_index = 0;
	for (auto& inst: code->GetInstructions())
	{
		if (terminal_index++ != front_terminal_index)
		{
			continue;
		}
		const int32_t rel = static_cast<int32_t>(back_entry_pc) - static_cast<int32_t>(inst.pc) - 4;
		inst.type                    = ShaderInstructionType::SBranch;
		inst.format                  = ShaderInstructionFormat::Label;
		inst.src_num                 = 1;
		inst.src[0]                  = {};
		inst.src[0].type             = ShaderOperandType::IntegerInlineConstant;
		inst.src[0].size             = 0;
		inst.src[0].constant.i       = rel;
		inst.dst                     = {};
		code->GetLabels().Add(ShaderLabel(back_entry_pc, inst.pc));
		break;
	}

	static uint32_t logs = 0;
	if (logs < 16u)
	{
		++logs;
		KYTY_LOG_DEBUG(
		             "KYTY_SHADER: linearized front→back continuation front_max_pc=0x%08" PRIx32
		             " back=0x%012" PRIx64 " entry_pc=0x%08" PRIx32 " insts=%u\n",
		             front_max_pc, back_code_addr, back_entry_pc,
		             static_cast<uint32_t>(back.GetInstructions().Size()));
	}
}

static bool ShaderGetMappedData(uint64_t addr, ShaderMappedData* data)
{
	EXIT_IF(g_shader_map == nullptr || data == nullptr);
	std::scoped_lock lock(g_shader_map_mutex);
	if (auto exact = g_shader_map->find(addr); exact != g_shader_map->end())
	{
		*data = exact->second;
		return true;
	}
	uint64_t                best_base = 0;
	const ShaderMappedData* best      = nullptr;
	for (const auto& [base, mapped]: *g_shader_map)
	{
		if (mapped.code_size_bytes != 0 && addr >= base && addr - base < mapped.code_size_bytes && (best == nullptr || base > best_base))
		{
			best_base = base;
			best      = &mapped;
		}
	}
	if (best == nullptr)
	{
		KYTY_LOG_DEBUG( "KYTY_SHADER_MAP_MISS addr=0x%016" PRIx64 " entries=%zu\n", addr, g_shader_map->size());
		int shown = 0;
		for (const auto& [base, mapped]: *g_shader_map)
		{
			if (shown++ >= 8)
			{
				break;
			}
			KYTY_LOG_DEBUG( "  map base=0x%016" PRIx64 " size=0x%08" PRIx32 " user=%p\n", base, mapped.code_size_bytes,
			             static_cast<void*>(mapped.user_data));
		}
		return false;
	}
	*data = *best;
	data->code_size_bytes -= static_cast<uint32_t>(addr - best_base);
	return true;
}

static void ShaderParseMappedLocked(uint64_t shader_addr, ShaderCode* code, ShaderContinuationMode continuation_mode)
{
	EXIT_IF(shader_addr == 0u || code == nullptr);
	if (Config::IsNextGen())
	{
		ShaderMappedData data;
		if (!ShaderGetMappedData(shader_addr, &data) || data.code_size_bytes == 0u)
		{
			EXIT("Gen5 shader has no bounded mapped code range: address=0x%016" PRIx64 "\n", shader_addr);
		}
		{
			const uint64_t continuation =
			    continuation_mode != ShaderContinuationMode::None ? ShaderLookupContinuation(shader_addr) : 0u;
			struct ParseContext
			{
				ShaderCode* code        = nullptr;
				bool        fused_front = false;
			} context {code, continuation != 0u};
			const bool parsed = Core::VirtualMemory::VisitReadableGuestRange(
			    shader_addr, data.code_size_bytes,
			    [](const void* source, uint64_t size, void* opaque)
			    {
				    auto* parse = static_cast<ParseContext*>(opaque);
				    if (source == nullptr || parse == nullptr || parse->code == nullptr || size > UINT32_MAX)
				    {
					    return false;
				    }
				    if (parse->fused_front)
				    {
					    ShaderParseFusedFront(static_cast<const uint32_t*>(source), static_cast<uint32_t>(size), parse->code);
				    } else
				    {
					    ShaderParse(static_cast<const uint32_t*>(source), static_cast<uint32_t>(size), parse->code);
				    }
				    return true;
			    },
			    &context);
			if (!parsed)
			{
				EXIT("shader code range became unreadable before parsing: address=0x%016" PRIx64 " size=0x%08" PRIx32 "\n",
				     shader_addr, data.code_size_bytes);
			}
			if (continuation != 0u)
			{
				if (continuation_mode == ShaderContinuationMode::Append)
				{
					ShaderAppendContinuation(code, continuation);
				}
			}
			return;
		}
	}
	ShaderParse(reinterpret_cast<const uint32_t*>(shader_addr), code);
}

static void ShaderParseMapped(uint64_t shader_addr, ShaderCode* code, ShaderContinuationMode continuation_mode)
{
	std::shared_lock lifetime_lock(g_shader_lifetime_mutex);
	ShaderParseMappedLocked(shader_addr, code, continuation_mode);
}

static std::shared_ptr<ShaderCode> GetCachedParsedVsIsa(uint64_t shader_addr, uint32_t hash0, uint32_t crc32)
{
	if (shader_addr == 0 || g_vs_isa_cache == nullptr)
	{
		return nullptr;
	}
	std::shared_lock lifetime_lock(g_shader_lifetime_mutex);
	{
		std::lock_guard<std::mutex> lock(g_vs_isa_cache_mutex);
		if (auto cached = g_vs_isa_cache->find(shader_addr); cached != g_vs_isa_cache->end())
		{
			if (cached->second->GetHash0() == hash0 && cached->second->GetCrc32() == crc32)
			{
				return cached->second;
			}
			g_vs_isa_cache->erase(cached);
		}
	}
	auto code = std::make_shared<ShaderCode>();
	code->SetType(ShaderType::Vertex);
	code->SetHash0(hash0);
	code->SetCrc32(crc32);
	ShaderParseMappedLocked(shader_addr, code.get(), ShaderContinuationMode::AllowTerminator);
	ShaderProbeWrite("vs", *code, nullptr, nullptr);
	std::lock_guard<std::mutex> lock(g_vs_isa_cache_mutex);
	if (auto cached = g_vs_isa_cache->find(shader_addr); cached != g_vs_isa_cache->end() &&
	    cached->second->GetHash0() == hash0 && cached->second->GetCrc32() == crc32)
	{
		return cached->second;
	}
	if (g_vs_isa_cache->size() >= 256u)
	{
		g_vs_isa_cache->clear();
	}
	g_vs_isa_cache->insert_or_assign(shader_addr, code);
	return code;
}

static bool IsDiscardInstruction(const Vector<ShaderInstruction>& code, uint32_t index)
{
	if (!(index == 0 || index + 1 >= code.Size()))
	{
		const auto& prev_inst = code.At(index - 1);
		const auto& inst      = code.At(index);
		const auto& next_inst = code.At(index + 1);

		return (inst.type == ShaderInstructionType::Exp && ShaderIsNullMrtDoneFormat(inst.format) &&
		        prev_inst.type == ShaderInstructionType::SMovB64 && prev_inst.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
		        prev_inst.dst.type == ShaderOperandType::ExecLo && prev_inst.src[0].type == ShaderOperandType::IntegerInlineConstant &&
		        prev_inst.src[0].constant.i == 0 && next_inst.type == ShaderInstructionType::SEndpgm);
	}
	return false;
}

// bool ShaderCode::IsDiscardBlock(uint32_t pc) const
//{
//	auto inst_count = m_instructions.Size();
//	for (uint32_t index = 0; index < inst_count; index++)
//	{
//		const auto& inst = m_instructions.At(index);
//		if (inst.pc == pc)
//		{
//			for (uint32_t i = index; i < inst_count; i++)
//			{
//				const auto& inst = m_instructions.At(i);
//
//				if (inst.type == ShaderInstructionType::SEndpgm || inst.type == ShaderInstructionType::SCbranchExecz ||
//				    inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchScc1 ||
//				    inst.type == ShaderInstructionType::SCbranchVccz)
//				{
//					return false;
//				}
//
//				if (IsDiscardInstruction(i))
//				{
//					return true;
//				}
//			}
//			return false;
//		}
//	}
//	return false;
// }

ShaderControlFlowBlock ShaderCode::ReadBlock(uint32_t pc) const
{
	ShaderControlFlowBlock ret;
	auto                   inst_count = m_instructions.Size();
	for (uint32_t index = 0; index < inst_count; index++)
	{
		const auto& inst = m_instructions.At(index);
		if (inst.pc == pc)
		{
			ret.pc       = pc;
			ret.is_valid = true;
			for (uint32_t i = index; i < inst_count; i++)
			{
				const auto& inst = m_instructions.At(i);

				if (inst.type == ShaderInstructionType::SEndpgm || inst.type == ShaderInstructionType::SCbranchExecz ||
				    inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchScc1 ||
				    inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz ||
				    inst.type == ShaderInstructionType::SBranch)
				{
					ret.last = inst;
					break;
				}

				if (IsDiscardInstruction(m_instructions, i))
				{
					ret.is_discard = true;
				}
			}
			break;
		}
	}
	return ret;
}

Vector<ShaderInstruction> ShaderCode::ReadIntructions(const ShaderControlFlowBlock& block) const
{
	Vector<ShaderInstruction> ret;

	auto inst_count = m_instructions.Size();
	for (uint32_t index = 0; index < inst_count; index++)
	{
		const auto& inst = m_instructions.At(index);
		if (inst.pc == block.pc)
		{
			for (uint32_t i = index; i < inst_count; i++)
			{
				const auto& inst = m_instructions.At(i);

				ret.Add(inst);

				if (inst.pc == block.last.pc)
				{
					break;
				}
			}
			break;
		}
	}

	return ret;
}



static void AddZeroSBufferResource(ShaderZeroSBufferResources* resources, int start_register)
{
	EXIT_IF(resources == nullptr);

	for (int i = 0; i < resources->buffers_num; ++i)
	{
		if (resources->start_register[i] == start_register)
		{
			return;
		}
	}

	if (resources->buffers_num >= ShaderZeroSBufferResources::BUFFERS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: resources->buffers_num >= ShaderZeroSBufferResources::BUFFERS_MAX condition ignored (continuing)\n"); }
	resources->start_register[resources->buffers_num++] = start_register;
}

static void ApplyDirectImageShape(const ShaderDirectImageUse& image, ShaderTextureDescriptor* descriptor)
{
	EXIT_IF(descriptor == nullptr);
	if (image.sampled_shape_conflict)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8,
		               "WARNING: sampled image resource uses multiple MIMG dimensions; descriptor shape retained\n");
		return;
	}
	if (!image.sampled_shape_known)
	{
		return;
	}
	descriptor->sampled_shape                  = image.sampled_shape;
	descriptor->sampled_shape_from_instruction = true;
}

void ShaderGetTextureBuffer(ShaderTextureResources* info, bool* direct_sgprs, int start_index, int slot, ShaderTextureUsage usage,
                                   const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	if (info->textures_num < 0 || info->textures_num >= ShaderTextureResources::RES_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: info->textures_num < 0 || info->textures_num >= ShaderTextureResources::RES_MAX condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(info->textures_num != slot);

	int  index    = info->textures_num;
	bool extended = (extended_buffer != nullptr);

	if (extended)
	{
		if (start_index < 16) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_index < 16 condition ignored (continuing)\n"); }
	} else
	{
		if (start_index < 0 || start_index + 7 >= HW::UserSgprInfo::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_index < 0 || start_index + 7 >= HW::UserSgprInfo::SGPRS_MAX condition ignored (continuing)\n"); }
	}

	info->desc[index].start_register = start_index;
	info->desc[index].extended       = extended;
	info->desc[index].slot           = slot;
	info->desc[index].usage          = usage;

	EXIT_IF(usage == ShaderTextureUsage::Unknown);

	if (!extended)
	{
		for (int j = 0; j < 8; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			if (type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region && type != HW::UserSgprType::Unknown) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region && type != HW::UserSgprType::Unknown condition ignored (continuing)\n"); }

			direct_sgprs[start_index + j] = false;
		}
	}

	info->desc[index].texture.fields[0] = (extended ? extended_buffer[start_index - 16 + 0] : user_sgpr.value[start_index + 0]);
	info->desc[index].texture.fields[1] = (extended ? extended_buffer[start_index - 16 + 1] : user_sgpr.value[start_index + 1]);
	info->desc[index].texture.fields[2] = (extended ? extended_buffer[start_index - 16 + 2] : user_sgpr.value[start_index + 2]);
	info->desc[index].texture.fields[3] = (extended ? extended_buffer[start_index - 16 + 3] : user_sgpr.value[start_index + 3]);
	info->desc[index].texture.fields[4] = (extended ? extended_buffer[start_index - 16 + 4] : user_sgpr.value[start_index + 4]);
	info->desc[index].texture.fields[5] = (extended ? extended_buffer[start_index - 16 + 5] : user_sgpr.value[start_index + 5]);
	info->desc[index].texture.fields[6] = (extended ? extended_buffer[start_index - 16 + 6] : user_sgpr.value[start_index + 6]);
	info->desc[index].texture.fields[7] = (extended ? extended_buffer[start_index - 16 + 7] : user_sgpr.value[start_index + 7]);
	info->desc[index].sampled_shape = ShaderGen5SampledTextureShapeForType(info->desc[index].texture.Type());

	if (usage == ShaderTextureUsage::ReadWrite)
	{
		info->textures2d_storage_num++;
		info->desc[index].textures2d_without_sampler = true;
	} else
	{
		switch (ShaderGen5SampledTextureShapeForType(info->desc[index].texture.Type()))
		{
			case ShaderGen5SampledTextureShape::ThreeDimensional: info->textures3d_sampled_num++; break;
			case ShaderGen5SampledTextureShape::TwoDimensionalArray: info->textures2d_array_sampled_num++; break;
			case ShaderGen5SampledTextureShape::TwoDimensional: info->textures2d_sampled_num++; break;
		}
		info->desc[index].textures2d_without_sampler = false;
	}

	info->textures_num++;
}

void ShaderGetSampler(ShaderSamplerResources* info, bool* direct_sgprs, int start_index, int slot, const HW::UserSgprInfo& user_sgpr,
                             const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	if (info->samplers_num < 0 || info->samplers_num >= ShaderSamplerResources::RES_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: info->samplers_num < 0 || info->samplers_num >= ShaderSamplerResources::RES_MAX condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(info->samplers_num != slot);

	int  index    = info->samplers_num;
	bool extended = (extended_buffer != nullptr);

	if (extended)
	{
		if (start_index < 16) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_index < 16 condition ignored (continuing)\n"); }
	} else
	{
		if (start_index < 0 || start_index + 3 >= HW::UserSgprInfo::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_index < 0 || start_index + 3 >= HW::UserSgprInfo::SGPRS_MAX condition ignored (continuing)\n"); }
	}

	info->start_register[index] = start_index;
	info->extended[index]       = extended;
	info->slots[index]          = slot;

	if (!extended)
	{
		for (int j = 0; j < 4; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			if (type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region && type != HW::UserSgprType::Unknown) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region && type != HW::UserSgprType::Unknown condition ignored (continuing)\n"); }

			direct_sgprs[start_index + j] = false;
		}
	}

	info->samplers[index].fields[0] = (extended ? extended_buffer[start_index - 16 + 0] : user_sgpr.value[start_index + 0]);
	info->samplers[index].fields[1] = (extended ? extended_buffer[start_index - 16 + 1] : user_sgpr.value[start_index + 1]);
	info->samplers[index].fields[2] = (extended ? extended_buffer[start_index - 16 + 2] : user_sgpr.value[start_index + 2]);
	info->samplers[index].fields[3] = (extended ? extended_buffer[start_index - 16 + 3] : user_sgpr.value[start_index + 3]);

	info->samplers_num++;
}

static void ShaderGetGdsPointer(ShaderGdsResources* info, bool* direct_sgprs, int start_index, int slot, const HW::UserSgprInfo& user_sgpr,
                                const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	if (info->pointers_num < 0 || info->pointers_num >= ShaderGdsResources::POINTERS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: info->pointers_num < 0 || info->pointers_num >= ShaderGdsResources::POINTERS_MAX condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(info->pointers_num != slot);

	int  index    = info->pointers_num;
	bool extended = (extended_buffer != nullptr);

	if (!extended && start_index >= 16) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !extended && start_index >= 16 condition ignored (continuing)\n"); }
	if (extended && !(start_index >= 16)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: extended && !(start_index >= 16) condition ignored (continuing)\n"); }

	info->start_register[index] = start_index;
	info->extended[index]       = extended;
	info->slots[index]          = slot;

	if (!extended)
	{
		auto type = user_sgpr.type[start_index];
		if (type != HW::UserSgprType::Unknown) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: type != HW::UserSgprType::Unknown condition ignored (continuing)\n"); }

		direct_sgprs[start_index] = false;
	}

	info->pointers[index].field = (extended ? extended_buffer[start_index - 16] : user_sgpr.value[start_index]);

	info->pointers_num++;
}

bool ShaderCanBindDirectSgpr(const ShaderUserData* user_data, int start_register, HW::UserSgprType type)
{
	if (type == HW::UserSgprType::Unknown)
	{
		return true;
	}

	if (type != HW::UserSgprType::Region || user_data == nullptr || start_register < 0)
	{
		return false;
	}

	return user_data->srt_size_dw == 0 || start_register < user_data->srt_size_dw;
}

static void ShaderGetDirectSgpr(ShaderDirectSgprsResources* info, int start_index, const HW::UserSgprInfo& user_sgpr,
                                const ShaderUserData* user_data)
{
	EXIT_IF(info == nullptr);

	if (info->sgprs_num < 0 || info->sgprs_num >= ShaderDirectSgprsResources::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: info->sgprs_num < 0 || info->sgprs_num >= ShaderDirectSgprsResources::SGPRS_MAX condition ignored (continuing)\n"); }

	int index = info->sgprs_num;

	if (start_index < 0 || start_index >= HW::UserSgprInfo::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: start_index < 0 || start_index >= HW::UserSgprInfo::SGPRS_MAX condition ignored (continuing)\n"); }

	info->start_register[index] = start_index;

	auto type = user_sgpr.type[start_index];
	if (!ShaderCanBindDirectSgpr(user_data, start_index, type))
	{
		KYTY_LOG_DEBUG("WARNING: unsupported direct user SGPR (continuing)\n");
	}

	info->sgprs[index].field = user_sgpr.value[start_index];

	info->sgprs_num++;
}

void ShaderCalcBindingIndices(ShaderBindResources* bind)
{
	KYTY_PROFILER_FUNCTION();

	int binding_index = 0;
	bind->textures2D.textures2d_sampled_uint_num       = 0;
	bind->textures2D.textures2d_array_sampled_uint_num = 0;
	bind->textures2D.textures3d_sampled_uint_num       = 0;
	if (Config::IsNextGen())
	{
		bind->textures2D.textures2d_sampled_num       = 0;
		bind->textures2D.textures2d_sampled_depth_num = 0;
		bind->textures2D.textures2d_array_sampled_num = 0;
		bind->textures2D.textures3d_sampled_num       = 0;
		for (int i = 0; i < bind->textures2D.textures_num; ++i)
		{
			const auto& descriptor = bind->textures2D.desc[i];
			if (descriptor.usage != ShaderTextureUsage::ReadOnly)
			{
				continue;
			}
			const auto shape = ShaderResolvedSampledTextureShape(descriptor);
			switch (shape)
			{
				case ShaderGen5SampledTextureShape::TwoDimensional:
					if (descriptor.sample_operation == State::ImageSampleOperation::DepthReference)
					{
						bind->textures2D.textures2d_sampled_depth_num++;
					} else
					{
						bind->textures2D.textures2d_sampled_num++;
					}
					break;
				case ShaderGen5SampledTextureShape::TwoDimensionalArray:
					bind->textures2D.textures2d_array_sampled_num++;
					break;
				case ShaderGen5SampledTextureShape::ThreeDimensional: bind->textures2D.textures3d_sampled_num++; break;
			}
			if (VulkanGen5ImageNumericType(descriptor.texture.Format()) != GuestImageNumericType::UnsignedInteger)
			{
				continue;
			}
			switch (shape)
			{
				case ShaderGen5SampledTextureShape::TwoDimensional: bind->textures2D.textures2d_sampled_uint_num++; break;
				case ShaderGen5SampledTextureShape::TwoDimensionalArray:
					bind->textures2D.textures2d_array_sampled_uint_num++;
					break;
				case ShaderGen5SampledTextureShape::ThreeDimensional: bind->textures2D.textures3d_sampled_uint_num++; break;
			}
		}
	}

	bind->push_constant_size = 0;

	if (bind->storage_buffers.buffers_num > 0)
	{
		bind->storage_buffers.binding_index = binding_index++;
		bind->push_constant_size += bind->storage_buffers.buffers_num * 16;
	}

	if (bind->textures2D.textures_num > 0)
	{
		bind->textures2D.binding_sampled_index = binding_index++;
		bind->textures2D.binding_storage_index = binding_index++;
		// Reserve every sampled-image shape. Descriptor layouts are keyed by the
		// aggregate sampled count while each shader can use a different subset.
		bind->textures2D.binding_sampled_array_index = binding_index++;
		bind->textures2D.binding_sampled_3d_index = binding_index++;
		bind->textures2D.binding_sampled_uint_index = binding_index++;
		bind->textures2D.binding_sampled_array_uint_index = binding_index++;
		bind->textures2D.binding_sampled_3d_uint_index = binding_index++;
		// Append depth-compare 2D after the existing shape slots so cached
		// SPIR-V binding numbers stay stable.
		bind->textures2D.binding_sampled_depth_index = binding_index++;

		bind->push_constant_size += bind->textures2D.textures_num * 32;
	}

	if (bind->samplers.samplers_num > 0)
	{
		bind->samplers.binding_index = binding_index++;
		bind->push_constant_size += bind->samplers.samplers_num * 16;
	}

	if (bind->gds_pointers.pointers_num > 0)
	{
		bind->gds_pointers.binding_index = binding_index++;
		bind->push_constant_size += (((bind->gds_pointers.pointers_num - 1) / 4) + 1) * 16;
	}

	if (bind->direct_sgprs.sgprs_num > 0)
	{
		bind->push_constant_size += (((bind->direct_sgprs.sgprs_num - 1) / 4) + 1) * 16;
	}

	EXIT_IF((bind->push_constant_size % 16) != 0);
	bind->vsharp_uniform_buffer = bind->push_constant_size > ShaderBindResources::PORTABLE_PUSH_CONSTANT_BYTES;
	bind->vsharp_binding_index  = bind->vsharp_uniform_buffer ? binding_index++ : -1;
}

ShaderStorageUsage ShaderGetDirectStorageUsage(const ShaderCode& code, int start_register)
{
	ShaderStorageUsage usage = ShaderStorageUsage::Unknown;

	for (const auto& inst: code.GetInstructions())
	{
		bool is_load  = false;
		bool is_store = false;
		switch (inst.type)
		{
			case ShaderInstructionType::BufferLoadUbyte:
			case ShaderInstructionType::BufferLoadDword:
			case ShaderInstructionType::BufferLoadDwordx2:
			case ShaderInstructionType::BufferLoadDwordx3:
			case ShaderInstructionType::BufferLoadDwordx4:
			case ShaderInstructionType::BufferLoadFormatX:
			case ShaderInstructionType::BufferLoadFormatXy:
			case ShaderInstructionType::BufferLoadFormatXyz:
			case ShaderInstructionType::BufferLoadFormatXyzw: is_load = true; break;
			case ShaderInstructionType::BufferStoreDword:
			case ShaderInstructionType::BufferStoreDwordx2:
			case ShaderInstructionType::BufferStoreDwordx3:
			case ShaderInstructionType::BufferStoreDwordx4:
		case ShaderInstructionType::BufferStoreFormatX:
		case ShaderInstructionType::BufferStoreFormatXy:
		case ShaderInstructionType::BufferStoreFormatXyzw:
		case ShaderInstructionType::BufferAtomicAdd: is_store = true; break;
			default: break;
		}

		if ((!is_load && !is_store) || inst.src_num < 2 || inst.src[1].type != ShaderOperandType::Sgpr ||
		    inst.src[1].register_id != start_register || inst.src[1].size != 4)
		{
			continue;
		}

		if (is_store)
		{
			return ShaderStorageUsage::ReadWrite;
		}
		usage = ShaderStorageUsage::ReadOnly;
	}

	return usage;
}

bool ShaderPreventsNoopPixelElision(const ShaderCode& code)
{
	bool prevents = false;
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		if (IsDiscardInstruction(code.GetInstructions(), index))
		{
			prevents = true;
			break;
		}
	}

	// Unsupported MUBUF/MTBUF loads and stores currently retain their packet
	// position as SBarrier. Fail closed until the parser preserves their exact
	// read/write opcode; this intentionally sacrifices elision, never effects.
	prevents = prevents || code.HasAnyOf({ShaderInstructionType::Unknown, ShaderInstructionType::SBarrier, ShaderInstructionType::SSendmsg,
	                                      ShaderInstructionType::SSetpcB64, ShaderInstructionType::SSwappcB64,
	                      ShaderInstructionType::BufferAtomicAdd, ShaderInstructionType::BufferAtomicAnd,
	                      ShaderInstructionType::BufferAtomicOr, ShaderInstructionType::BufferAtomicSmax,
	                      ShaderInstructionType::BufferAtomicSmin, ShaderInstructionType::BufferAtomicSub,
	                      ShaderInstructionType::BufferAtomicUmax, ShaderInstructionType::BufferAtomicUmin,
	                      ShaderInstructionType::BufferAtomicXor, ShaderInstructionType::BufferStoreDword,
	                      ShaderInstructionType::BufferStoreDwordx2, ShaderInstructionType::BufferStoreDwordx3,
	                      ShaderInstructionType::BufferStoreDwordx4, ShaderInstructionType::BufferStoreFormatX,
	                      ShaderInstructionType::BufferStoreFormatXy, ShaderInstructionType::BufferStoreFormatXyzw,
	                      ShaderInstructionType::DsAppend, ShaderInstructionType::DsConsume, ShaderInstructionType::DsAddU32,
	                      ShaderInstructionType::DsAndB32, ShaderInstructionType::DsDecU32, ShaderInstructionType::DsIncU32,
	                      ShaderInstructionType::DsMaxI32, ShaderInstructionType::DsMaxU32, ShaderInstructionType::DsMinI32,
	                      ShaderInstructionType::DsMinU32, ShaderInstructionType::DsOrB32, ShaderInstructionType::DsRsubU32,
	                      ShaderInstructionType::DsSubU32, ShaderInstructionType::DsXorB32, ShaderInstructionType::DsWriteB32,
	                      ShaderInstructionType::ImageStore, ShaderInstructionType::ImageStoreMip});

	return prevents;
}


// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderParseUsage(uint64_t addr, ShaderParsedUsage* info, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                      int user_sgpr_num)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(bind == nullptr);
	EXIT_IF(info == nullptr);

	const auto* src = reinterpret_cast<const uint32_t*>(addr);

	auto usages = GetUsageSlots(src);

	if (!usages.valid) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !usages.valid condition ignored (continuing)\n"); }

	info->fetch                     = false;
	info->fetch_reg                 = 0;
	info->vertex_buffer             = false;
	info->vertex_buffer_reg         = 0;
	info->storage_buffers_readonly  = 0;
	info->storage_buffers_constant  = 0;
	info->storage_buffers_readwrite = 0;
	info->textures2D_readonly       = 0;
	info->textures2D_readwrite      = 0;
	info->extended_buffer           = false;
	info->samplers                  = 0;
	info->gds_pointers              = 0;
	info->direct_sgprs              = 0;

	uint32_t* extended_buffer = nullptr;

	bool direct_sgprs[HW::UserSgprInfo::SGPRS_MAX];
	for (int i = 0; i < HW::UserSgprInfo::SGPRS_MAX; i++)
	{
		direct_sgprs[i] = (i < user_sgpr_num);
	}

	for (int i = 0; i < usages.slots_num; i++)
	{
		const auto& usage = usages.slots[i];
		switch (usage.type)
		{
			case 0x00:
				if (usage.flags != 0 && usage.flags != 3) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.flags != 0 && usage.flags != 3 condition ignored (continuing)\n"); }
				if (usage.flags == 0)
				{
					if (ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, usage.start_register, usage.slot,
					                           ShaderStorageUsage::ReadOnly, user_sgpr, extended_buffer))
					{
						info->storage_buffers_readonly++;
					}
				} else if (usage.flags == 3)
				{
					ShaderGetTextureBuffer(&bind->textures2D, direct_sgprs, usage.start_register, usage.slot, ShaderTextureUsage::ReadOnly,
					                       user_sgpr, extended_buffer);
					info->textures2D_readonly++;
				}
				break;

			case 0x01:
				if (usage.flags != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.flags != 0 condition ignored (continuing)\n"); }
				ShaderGetSampler(&bind->samplers, direct_sgprs, usage.start_register, usage.slot, user_sgpr, extended_buffer);
				info->samplers++;
				break;

			case 0x02:
				if (usage.flags != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.flags != 0 condition ignored (continuing)\n"); }
				if (ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, usage.start_register, usage.slot,
				                           ShaderStorageUsage::Constant, user_sgpr, extended_buffer))
				{
					info->storage_buffers_constant++;
				}
				break;

			case 0x04:
				if (usage.flags != 0 && usage.flags != 3) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.flags != 0 && usage.flags != 3 condition ignored (continuing)\n"); }
				if (usage.flags == 0)
				{
					if (ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, usage.start_register, usage.slot,
					                           ShaderStorageUsage::ReadWrite, user_sgpr, extended_buffer))
					{
						info->storage_buffers_readwrite++;
					}
				} else if (usage.flags == 3)
				{
					ShaderGetTextureBuffer(&bind->textures2D, direct_sgprs, usage.start_register, usage.slot, ShaderTextureUsage::ReadWrite,
					                       user_sgpr, extended_buffer);
					info->textures2D_readwrite++;
				}
				break;

			case 0x07:
				if (usage.flags != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.flags != 0 condition ignored (continuing)\n"); }
				ShaderGetGdsPointer(&bind->gds_pointers, direct_sgprs, usage.start_register, usage.slot, user_sgpr, extended_buffer);
				info->gds_pointers++;
				break;

			case 0x12:
				if (usage.slot != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.slot != 0 condition ignored (continuing)\n"); }
				if (usage.flags != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.flags != 0 condition ignored (continuing)\n"); }
				info->fetch                            = true;
				info->fetch_reg                        = usage.start_register;
				direct_sgprs[usage.start_register]     = false;
				direct_sgprs[usage.start_register + 1] = false;
				break;

			case 0x17:
				if (usage.slot != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.slot != 0 condition ignored (continuing)\n"); }
				if (usage.flags != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.flags != 0 condition ignored (continuing)\n"); }
				info->vertex_buffer                    = true;
				info->vertex_buffer_reg                = usage.start_register;
				direct_sgprs[usage.start_register]     = false;
				direct_sgprs[usage.start_register + 1] = false;
				break;

			case 0x1b:
				if (usage.flags != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.flags != 0 condition ignored (continuing)\n"); }
				if (usage.slot != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.slot != 1 condition ignored (continuing)\n"); }
				if (bind->extended.used) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: bind->extended.used condition ignored (continuing)\n"); }
				if (usage.start_register + 1 >= HW::UserSgprInfo::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.start_register + 1 >= HW::UserSgprInfo::SGPRS_MAX condition ignored (continuing)\n"); }
				bind->extended.used                    = true;
				bind->extended.slot                    = usage.slot;
				bind->extended.start_register          = usage.start_register;
				bind->extended.data.fields[0]          = user_sgpr.value[usage.start_register];
				bind->extended.data.fields[1]          = user_sgpr.value[usage.start_register + 1];
				extended_buffer                        = reinterpret_cast<uint32_t*>(bind->extended.data.Base());
				info->extended_buffer                  = true;
				direct_sgprs[usage.start_register]     = false;
				direct_sgprs[usage.start_register + 1] = false;
				break;

			default: KYTY_LOG_DEBUG("WARNING: unknown usage type in shader (continuing)\n"); break;
		}
	}

	for (int i = 0; i < HW::UserSgprInfo::SGPRS_MAX; i++)
	{
		if (direct_sgprs[i])
		{
			ShaderGetDirectSgpr(&bind->direct_sgprs, i, user_sgpr, nullptr);
			info->direct_sgprs++;
		}
	}
}

// Gen5 direct-resource type 5 is the EUD pointer when eud_size_dw != 0 and
// srt_size_dw == 0. Captured post-detile PS: user_sgpr_num=30, eud=12, type5 at
// SGPR 0x1c holds a guest pointer whose first 8 dwords are two S# descriptors
// for sharp sampler offsets 0x20 and 0x24.

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderParseUsage2(const ShaderUserData* user_data, ShaderParsedUsage* info, ShaderBindResources* bind,
                       const HW::UserSgprInfo& user_sgpr, int user_sgpr_num, const ShaderCode* code, int user_data_register_base,
                       bool vertex_resource_types)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(bind == nullptr);
	EXIT_IF(info == nullptr);

	info->fetch                     = false;
	info->fetch_reg                 = 0;
	info->vertex_buffer             = false;
	info->vertex_buffer_reg         = 0;
	info->storage_buffers_readonly  = 0;
	info->storage_buffers_constant  = 0;
	info->storage_buffers_readwrite = 0;
	info->textures2D_readonly       = 0;
	info->textures2D_readwrite      = 0;
	info->extended_buffer           = false;
	info->samplers                  = 0;
	info->gds_pointers              = 0;
	info->direct_sgprs              = 0;

	if (user_data == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_data == nullptr condition ignored (continuing)\n"); }
	// Two Gen5 EUD layouts are evidenced:
	// 1) No type-5 pointer: descriptors live in the user-SGPR window; eud_size
	//    must fit in that window (earlier capture: eud=12, user_sgpr_num=30).
	// 2) Type-5 pointer: overflow sharp offsets are fetched from guest memory
	//    at that pointer (post-detile: S#@0x20/0x24 in a 12-dword EUD).
	const bool has_eud_ptr = Gen5HasEudPointer(user_data);
	if (user_data->eud_size_dw != 0)
	{
		if (user_data->srt_size_dw != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_data->srt_size_dw != 0 condition ignored (continuing)\n"); }
		if (user_sgpr_num <= 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_sgpr_num <= 0 condition ignored (continuing)\n"); }
		if (!has_eud_ptr)
		{
			if (static_cast<uint32_t>(user_sgpr_num) < user_data->eud_size_dw) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: static_cast<uint32_t>(user_sgpr_num) < user_data->eud_size_dw condition ignored (continuing)\n"); }
		}
	}
	if (user_data->srt_size_dw > user_sgpr_num) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_data->srt_size_dw > user_sgpr_num condition ignored (continuing)\n"); }

	uint32_t* extended_buffer    = nullptr;
	bool       eud_pointer_valid = false;

	bool direct_sgprs[HW::UserSgprInfo::SGPRS_MAX];
	for (int i = 0; i < HW::UserSgprInfo::SGPRS_MAX; i++)
	{
		direct_sgprs[i] = (i < user_sgpr_num);
	}

	for (uint16_t type = 0; type < user_data->direct_resource_count; type++)
	{
		if (user_data->direct_resource_offset[type] == 0xffff)
		{
			continue;
		}

		int reg = user_data->direct_resource_offset[type];

		switch (type)
		{
			case 8:
				if (!vertex_resource_types)
				{
					ShaderGetTextureBuffer(&bind->textures2D, direct_sgprs, reg, bind->textures2D.textures_num,
					                       ShaderTextureUsage::ReadOnly, user_sgpr, nullptr);
					info->textures2D_readonly++;
					break;
				}
				info->vertex_buffer                       = true;
				info->vertex_buffer_reg                   = reg;
				direct_sgprs[info->vertex_buffer_reg]     = false;
				direct_sgprs[info->vertex_buffer_reg + 1] = false;
				break;

			case 10:
				if (!vertex_resource_types)
				{
					ShaderGetSampler(&bind->samplers, direct_sgprs, reg, bind->samplers.samplers_num, user_sgpr, nullptr);
					info->samplers++;
					break;
				}
				info->vertex_attrib                       = true;
				info->vertex_attrib_reg                   = reg;
				direct_sgprs[info->vertex_attrib_reg]     = false;
				direct_sgprs[info->vertex_attrib_reg + 1] = false;
				break;

			case k_gen5_eud_direct_type:
				if (has_eud_ptr)
				{
					if (bind->extended.used) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: bind->extended.used condition ignored (continuing)\n"); }
					if (reg < 0 || reg + 1 >= user_sgpr_num) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: reg < 0 || reg + 1 >= user_sgpr_num condition ignored (continuing)\n"); }
					bind->extended.used           = true;
					bind->extended.slot           = static_cast<int>(type);
					bind->extended.start_register = reg;
					bind->extended.data.fields[0] = user_sgpr.value[reg];
					bind->extended.data.fields[1] = user_sgpr.value[reg + 1];
					const uint64_t eud_base = bind->extended.data.Base();
					if (eud_base != 0)
					{
						extended_buffer    = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(eud_base));
						eud_pointer_valid = true;
					} else
					{
						ShaderReportMissingGen5EudPointer(user_data, reg, user_sgpr_num);
					}
					info->extended_buffer = eud_pointer_valid;
					direct_sgprs[reg]     = false;
					direct_sgprs[reg + 1] = false;
					break;
				}
				// No EUD pointer: fall through as a storage buffer.
				// fallthrough
			default:
			{
				if (code != nullptr)
				{
					const auto image = AnalyzeShaderDirectImageUse(*code, reg + user_data_register_base);
					if (image.texture != ShaderTextureUsage::Unknown)
					{
						ShaderGetTextureBuffer(&bind->textures2D, direct_sgprs, reg, bind->textures2D.textures_num, image.texture,
						                       user_sgpr, nullptr);
						if (image.texture == ShaderTextureUsage::ReadWrite)
						{
							info->textures2D_readwrite++;
						} else
						{
							info->textures2D_readonly++;
						}
						if (image.sampler_register >= 0)
						{
							const int sampler_register = image.sampler_register - user_data_register_base;
							if (sampler_register < 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sampler_register < 0 condition ignored (continuing)\n"); }
							ShaderGetSampler(&bind->samplers, direct_sgprs, sampler_register, bind->samplers.samplers_num, user_sgpr,
							                 nullptr);
							info->samplers++;
						}
						break;
					}
				}

				// When the instruction stream is unavailable (VS/PS Gen5 path),
				// default to ReadOnly rather than failing. CS passes &code and
				// reclassifies stores as ReadWrite via ShaderGetDirectStorageUsage.
				if (code == nullptr && !Gen5CodeUnavailableDirectResourceLooksStorage(user_sgpr, reg))
				{
					break;
				}
				auto usage = ShaderStorageUsage::ReadOnly;
				if (code != nullptr)
				{
					usage = ShaderGetDirectStorageUsage(*code, reg + user_data_register_base);
					if (usage == ShaderStorageUsage::Unknown)
					{
						usage = ShaderStorageUsage::ReadOnly;
					}
				}
				// Direct storage always indexes user_sgpr (pass null extended).
				if (ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, reg, bind->storage_buffers.buffers_num, usage, user_sgpr,
				                           nullptr))
				{
					if (usage == ShaderStorageUsage::ReadWrite)
					{
						info->storage_buffers_readwrite++;
					} else
					{
						info->storage_buffers_readonly++;
					}
				}
				break;
			}
		}
	}

	if (user_data->sharp_resource_count[0] != 0)
	{
		for (uint16_t slot = 0; slot < user_data->sharp_resource_count[0]; slot++)
		{
			if (user_data->sharp_resource_offset[0][slot].offset_dw == 0x7fff)
			{
				continue;
			}

			// sharp[0] = read-only texture slot. SizeFlag (0x8000) marks 4-dw V#;
			// clear flag is 8-dw T# when dword3 type nibble is 1D (8) or 2D (9).
			const auto sharp_size  = user_data->sharp_resource_offset[0][slot].size;
			const int  off         = user_data->sharp_resource_offset[0][slot].offset_dw;
			if (!eud_pointer_valid && Gen5SharpNeedsEud(off, 4, user_sgpr_num))
			{
				continue;
			}
			const bool use_texture = Gen5SharpUseTextureDescriptor(sharp_size != 0, off, user_sgpr_num, user_sgpr, extended_buffer);
			if (use_texture)
			{
				constexpr int   dwords = 8;
				const uint32_t* ebuf   = nullptr;
				int             api    = off;
				if (Gen5SharpNeedsEud(off, dwords, user_sgpr_num))
				{
					if (extended_buffer == nullptr)
					{
						continue;
					}
					api  = Gen5EudApiIndex(off, user_sgpr_num);
					ebuf = extended_buffer;
					if (!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw) condition ignored (continuing)\n"); }
				}
				ShaderGetTextureBuffer(&bind->textures2D, direct_sgprs, api, slot, ShaderTextureUsage::ReadOnly, user_sgpr, ebuf);
				info->textures2D_readonly++;
			} else
			{
				constexpr int   dwords = 4;
				const uint32_t* ebuf   = nullptr;
				int             api    = off;
				if (Gen5SharpNeedsEud(off, dwords, user_sgpr_num))
				{
					if (extended_buffer == nullptr)
					{
						continue;
					}
					api  = Gen5EudApiIndex(off, user_sgpr_num);
					ebuf = extended_buffer;
					if (!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw) condition ignored (continuing)\n"); }
				}
				if (ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, api, slot, ShaderStorageUsage::Constant, user_sgpr, ebuf,
				                           ShaderStorageBindingSource::MetadataSharp))
				{
					info->storage_buffers_constant++;
				}
			}
		}
	}

	if (user_data->sharp_resource_count[1] != 0)
	{
		// sharp[1] = read-write texture slot. Same SizeFlag / type-nibble
		// contract as sharp[0].
		for (uint16_t slot = 0; slot < user_data->sharp_resource_count[1]; slot++)
		{
			if (user_data->sharp_resource_offset[1][slot].offset_dw == 0x7fff)
			{
				continue;
			}

			const auto sharp_size  = user_data->sharp_resource_offset[1][slot].size;
			const int  off         = user_data->sharp_resource_offset[1][slot].offset_dw;
			if (!eud_pointer_valid && Gen5SharpNeedsEud(off, 4, user_sgpr_num))
			{
				continue;
			}
			const bool use_texture = Gen5SharpUseTextureDescriptor(sharp_size != 0, off, user_sgpr_num, user_sgpr, extended_buffer);
			if (use_texture)
			{
				constexpr int   dwords = 8;
				const uint32_t* ebuf   = nullptr;
				int             api    = off;
				if (Gen5SharpNeedsEud(off, dwords, user_sgpr_num))
				{
					if (extended_buffer == nullptr)
					{
						continue;
					}
					api  = Gen5EudApiIndex(off, user_sgpr_num);
					ebuf = extended_buffer;
					if (!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw) condition ignored (continuing)\n"); }
				}
				ShaderGetTextureBuffer(&bind->textures2D, direct_sgprs, api, slot, ShaderTextureUsage::ReadWrite, user_sgpr, ebuf);
				info->textures2D_readwrite++;
			} else
			{
				constexpr int   dwords = 4;
				const uint32_t* ebuf   = nullptr;
				int             api    = off;
				if (Gen5SharpNeedsEud(off, dwords, user_sgpr_num))
				{
					if (extended_buffer == nullptr)
					{
						continue;
					}
					api  = Gen5EudApiIndex(off, user_sgpr_num);
					ebuf = extended_buffer;
					if (!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw) condition ignored (continuing)\n"); }
				}
				if (ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, api, slot, ShaderStorageUsage::ReadWrite, user_sgpr, ebuf,
				                           ShaderStorageBindingSource::MetadataSharp))
				{
					info->storage_buffers_readwrite++;
				}
			}
		}
	}

	if (user_data->sharp_resource_count[2] != 0)
	{
		for (uint16_t slot = 0; slot < user_data->sharp_resource_count[2]; slot++)
		{
			if (user_data->sharp_resource_offset[2][slot].offset_dw == 0x7fff)
			{
				continue;
			}

			if (user_data->sharp_resource_offset[2][slot].size != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_data->sharp_resource_offset[2][slot].size != 1 condition ignored (continuing)\n"); }
			const int       off    = user_data->sharp_resource_offset[2][slot].offset_dw;
			constexpr int   dwords = 4;
			if (!eud_pointer_valid && Gen5SharpNeedsEud(off, dwords, user_sgpr_num))
			{
				continue;
			}
			const uint32_t* ebuf   = nullptr;
			int             api    = off;
			if (Gen5SharpNeedsEud(off, dwords, user_sgpr_num))
			{
				if (extended_buffer == nullptr)
				{
					continue;
				}
				api  = Gen5EudApiIndex(off, user_sgpr_num);
				ebuf = extended_buffer;
				if (!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw) condition ignored (continuing)\n"); }
			}
			ShaderGetSampler(&bind->samplers, direct_sgprs, api, slot, user_sgpr, ebuf);
			info->samplers++;
		}
	}

	if (user_data->sharp_resource_count[3] != 0)
	{
		for (uint16_t slot = 0; slot < user_data->sharp_resource_count[3]; slot++)
		{
			if (user_data->sharp_resource_offset[3][slot].offset_dw == 0x7fff)
			{
				continue;
			}

			if (user_data->sharp_resource_offset[3][slot].size != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_data->sharp_resource_offset[3][slot].size != 1 condition ignored (continuing)\n"); }
			const int       off    = user_data->sharp_resource_offset[3][slot].offset_dw;
			constexpr int   dwords = 4;
			if (!eud_pointer_valid && Gen5SharpNeedsEud(off, dwords, user_sgpr_num))
			{
				continue;
			}
			const uint32_t* ebuf   = nullptr;
			int             api    = off;
			if (Gen5SharpNeedsEud(off, dwords, user_sgpr_num))
			{
				if (extended_buffer == nullptr)
				{
					continue;
				}
				api  = Gen5EudApiIndex(off, user_sgpr_num);
				ebuf = extended_buffer;
				if (!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw) condition ignored (continuing)\n"); }
			}
			if (ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, api, slot, ShaderStorageUsage::Constant, user_sgpr, ebuf,
			                           ShaderStorageBindingSource::MetadataSharp))
			{
				info->storage_buffers_constant++;
			}
		}
	}

	if (code != nullptr && has_eud_ptr && extended_buffer != nullptr)
	{
		ShaderPruneUnusedMetadataStorage(*code, &bind->storage_buffers, user_sgpr_num, user_data_register_base);
		ShaderCollectDynamicScalarResources(*code, bind, user_sgpr, info, extended_buffer, user_data->eud_size_dw);
	}

	// Gen5 metadata is advisory: some shaders address an S# descriptor directly
	// from the EUD/user-SGPR namespace without listing it in the sharp table.
	// Reconcile decoded scalar-buffer accesses with the binding set, preserving
	// null-descriptor rejection and the same EUD translation used by metadata.
	if (code != nullptr)
	{
		for (const auto& inst: code->GetInstructions())
		{
			if (!ShaderInstructionIsScalarBufferLoad(inst) || inst.src_num == 0 || inst.src[0].type != ShaderOperandType::Sgpr ||
			    inst.src[0].size != 4 || ShaderIsDynamicScalarStorageConsumer(*bind, inst))
			{
				continue;
			}

			const int raw_start = inst.src[0].register_id - user_data_register_base;
			if (raw_start < 0)
			{
				continue;
			}
			const bool needs_eud = Gen5SharpNeedsEud(raw_start, 4, user_sgpr_num);
			if (needs_eud && (!has_eud_ptr || extended_buffer == nullptr || raw_start < ShaderGen5EudOffsetBase(user_sgpr_num)))
			{
				continue;
			}
			const int api_start     = needs_eud ? Gen5EudApiIndex(raw_start, user_sgpr_num) : raw_start;
			bool      already_bound = false;
			for (int i = 0; i < bind->storage_buffers.buffers_num; ++i)
			{
				already_bound = already_bound ||
				                (bind->storage_buffers.start_register[i] == api_start && bind->storage_buffers.extended[i] == needs_eud);
			}
			if (already_bound)
			{
				continue;
			}
			const int slot = bind->storage_buffers.buffers_num;
			if (ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, api_start, slot, ShaderStorageUsage::ReadOnly, user_sgpr,
			                           needs_eud ? extended_buffer : nullptr))
			{
				info->storage_buffers_readonly++;
			} else
			{
				AddZeroSBufferResource(&bind->zero_sbuffer_resources, inst.src[0].register_id);
			}
		}
	}

	for (int i = 0; i < HW::UserSgprInfo::SGPRS_MAX; i++)
	{
		if (direct_sgprs[i])
		{
			ShaderGetDirectSgpr(&bind->direct_sgprs, i, user_sgpr, user_data);
			info->direct_sgprs++;
		}
	}

	for (int i = 0; i < bind->storage_buffers.buffers_num; ++i)
	{
		const bool local_dynamic_sload = bind->storage_buffers.dynamic_sload[i];
		const bool has_dynamic_sload = ShaderStorageResourceHasDynamicSLoad(*bind, i);
		const int  binding_register = bind->storage_buffers.extended[i]
		                                  ? ShaderGen5EudOffsetBase(user_sgpr_num) + (bind->storage_buffers.start_register[i] - 16)
		                                  : bind->storage_buffers.start_register[i];
		const int  register_with_base = local_dynamic_sload ? binding_register : binding_register + user_data_register_base;
		auto exact_evidence = code != nullptr ? AnalyzeShaderStorageUse(*code, register_with_base) : ShaderStorageUseEvidence {};
		if (has_dynamic_sload)
		{
			// Each mapping is proven to reach one descriptor consumer before a clobber.
			// Merge that local raw-use proof with any independent static use of the
			// same physical descriptor instead of assigning a synthetic entry state.
			if (exact_evidence.access == ShaderStorageAccess::Unknown)
			{
				exact_evidence.access = ShaderStorageAccess::Raw;
			} else if (exact_evidence.access == ShaderStorageAccess::Typed)
			{
				exact_evidence.access = ShaderStorageAccess::Mixed;
			}
			for (int mapping = 0; mapping < bind->dynamic_sloads.mappings_num; ++mapping)
			{
				if (bind->dynamic_sloads.kind[mapping] != ShaderDynamicSLoadResourceKind::StorageBuffer ||
				    bind->dynamic_sloads.resource_index[mapping] != i)
				{
					continue;
				}
				if (bind->dynamic_sloads.raw_vmem_oob_guarded[mapping])
				{
					exact_evidence.raw_vmem_oob_guarded = true;
					continue;
				}
				exact_evidence.raw_smem_use            = true;
				exact_evidence.raw_smem_dynamic_offset = true;
			}
		}
		const auto exact = exact_evidence.access;
		ShaderStorageUseEvidence unbased_evidence {};
		if (!local_dynamic_sload && code != nullptr && user_data_register_base != 0)
		{
			unbased_evidence = AnalyzeShaderStorageUse(*code, bind->storage_buffers.start_register[i]);
		}
		const auto evidence = ResolveShaderStorageAccessEvidence(code != nullptr, bind->storage_buffers.sources[i], exact,
		                                                         unbased_evidence.access,
		                                                         exact_evidence.decoded_unknown, exact_evidence.indirect_descriptor_use);
		bind->storage_buffers.accesses[i]                = evidence.access;
		bind->storage_buffers.unknown_reasons[i]         = evidence.reason;
		bind->storage_buffers.code_available[i]          = evidence.code_available;
		bind->storage_buffers.exact_matches[i]           = evidence.exact_match;
		bind->storage_buffers.unbased_matches[i]         = evidence.unbased_match;
		bind->storage_buffers.decoded_unknown[i]         = exact_evidence.decoded_unknown;
		bind->storage_buffers.indirect_descriptor_use[i] = exact_evidence.indirect_descriptor_use;
		const auto& matched_evidence = evidence.exact_match ? exact_evidence : unbased_evidence;
		bind->storage_buffers.raw_vmem_oob_guarded[i]    = matched_evidence.raw_vmem_oob_guarded;
		bind->storage_buffers.raw_smem_use[i]            = matched_evidence.raw_smem_use;
		bind->storage_buffers.raw_tbuffer_use[i]         = matched_evidence.raw_tbuffer_use;
		bind->storage_buffers.raw_smem_required_bytes[i] = matched_evidence.raw_smem_required_bytes;
		bind->storage_buffers.raw_smem_dynamic_offset[i] = matched_evidence.raw_smem_dynamic_offset;

		if (evidence.access == ShaderStorageAccess::Raw && matched_evidence.raw_smem_use &&
		    ShaderGen5SBufferDescriptorAlwaysOutOfBounds(bind->storage_buffers.buffers[i]))
		{
			if (has_dynamic_sload)
			{
				for (int mapping = 0; mapping < bind->dynamic_sloads.mappings_num; ++mapping)
				{
					if (bind->dynamic_sloads.kind[mapping] == ShaderDynamicSLoadResourceKind::StorageBuffer &&
					    bind->dynamic_sloads.resource_index[mapping] == i)
					{
						AddZeroSBufferResource(&bind->zero_sbuffer_resources,
						                       bind->dynamic_sloads.destination_register[mapping]);
					}
				}
			} else
			{
				const int shader_start_register = evidence.exact_match ? register_with_base : binding_register;
				AddZeroSBufferResource(&bind->zero_sbuffer_resources, shader_start_register);
			}
		}
	}
	if (code != nullptr)
	{
		for (int i = 0; i < bind->textures2D.textures_num; ++i)
		{
			auto& descriptor = bind->textures2D.desc[i];
			if (descriptor.dynamic_sload || descriptor.usage != ShaderTextureUsage::ReadOnly)
			{
				continue;
			}
			const int binding_register = descriptor.extended
			                                 ? ShaderGen5EudOffsetBase(user_sgpr_num) + (descriptor.start_register - 16)
			                                 : descriptor.start_register;
			auto image = AnalyzeShaderDirectImageUse(*code, binding_register + user_data_register_base);
			if (image.texture == ShaderTextureUsage::Unknown && user_data_register_base != 0)
			{
				image = AnalyzeShaderDirectImageUse(*code, binding_register);
			}
			if (image.texture != ShaderTextureUsage::Unknown)
			{
				descriptor.sample_operation = image.sample_operation;
				ApplyDirectImageShape(image, &descriptor);
			}
		}
		for (int i = 0; i < bind->samplers.samplers_num; ++i)
		{
			if (bind->samplers.dynamic_sload[i])
			{
				continue;
			}
			const int binding_register = bind->samplers.extended[i]
			                                 ? ShaderGen5EudOffsetBase(user_sgpr_num) + (bind->samplers.start_register[i] - 16)
			                                 : bind->samplers.start_register[i];
			auto evidence = AnalyzeShaderSamplerOperationEvidence(*code, binding_register + user_data_register_base);
			if (!evidence.found && user_data_register_base != 0)
			{
				evidence = AnalyzeShaderSamplerOperationEvidence(*code, binding_register);
			}
			bind->samplers.operations[i] = evidence.operation;
		}
		ShaderAssociateSampledTextureSamplers(*code, bind, user_data_register_base);
	}

	ExcludeUnusedMetadataStorage(&bind->storage_buffers);
}

int32_t ShaderDetectVertexOffsetSgpr(const ShaderCode& code, uint32_t user_data_base, uint32_t user_data_count)
{
	int32_t candidate = -1;

	auto is_zero = [](const ShaderOperand& operand)
	{
		return (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant) &&
		       operand.constant.u == 0;
	};

	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::BufferLoadFormatX || inst.type == ShaderInstructionType::BufferLoadFormatXy ||
		    inst.type == ShaderInstructionType::BufferLoadFormatXyz || inst.type == ShaderInstructionType::BufferLoadFormatXyzw)
		{
			break;
		}
		if (inst.dst.type != ShaderOperandType::Vgpr)
		{
			continue;
		}

		const bool vertex_index = inst.dst.register_id == 0 || (user_data_base == 8 && inst.dst.register_id == 5);
		const bool add_offset   = inst.type == ShaderInstructionType::VAddI32 && inst.src_num >= 2 &&
		                          inst.src[0].type == ShaderOperandType::Sgpr && inst.src[1].type == ShaderOperandType::Vgpr &&
		                          inst.src[1].register_id == inst.dst.register_id;
		const bool sad_offset   = user_data_base == 8 && inst.dst.register_id == 5 && inst.type == ShaderInstructionType::VSadU32 &&
		                          inst.src_num >= 3 && inst.src[0].type == ShaderOperandType::Sgpr && is_zero(inst.src[1]) &&
		                          inst.src[2].type == ShaderOperandType::Vgpr && inst.src[2].register_id == inst.dst.register_id;
		if (!vertex_index || (!add_offset && !sad_offset))
		{
			continue;
		}

		const auto sgpr = static_cast<uint32_t>(inst.src[0].register_id);
		if (sgpr < user_data_base || sgpr - user_data_base >= user_data_count)
		{
			continue;
		}
		if (candidate >= 0 && candidate != static_cast<int32_t>(sgpr))
		{
			return -1;
		}
		candidate = static_cast<int32_t>(sgpr);
	}

	return candidate;
}

bool ShaderResolveVertexOffset(uint32_t index_offset, const ShaderVertexInputInfo& input_info, int32_t* resolved_offset,
	                            int32_t vertex_offset_add)
{
	if (resolved_offset == nullptr)
	{
		return false;
	}

	int32_t base = 0;
	if (index_offset != 0)
	{
		base = static_cast<int32_t>(index_offset);
	} else if (input_info.fetch_external && input_info.vertex_offset_sgpr >= 0)
	{
		base = static_cast<int32_t>(input_info.vertex_offset_value);
	}

	const int64_t resolved = static_cast<int64_t>(base) + vertex_offset_add;
	if (resolved < INT32_MIN || resolved > INT32_MAX)
	{
		return false;
	}
	*resolved_offset = static_cast<int32_t>(resolved);
	return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderGetInputInfoVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh, ShaderVertexInputInfo* info)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || regs == nullptr);

	info->bind                      = {};
	info->export_count              = static_cast<int>(sh->GetExportCount());
	info->bind.push_constant_offset = 0;
	info->bind.push_constant_size   = 0;
	info->bind.descriptor_set_slot  = 0;
	info->clip_probe                = {};
	info->clip_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
	info->float_mode                = 0;
	info->dx10_clamp                = false;
	info->ieee_mode                 = false;

	if (regs->vs_embedded)
	{
		return;
	}

	ShaderParsedUsage usage;

	bool gs_instead_of_vs =
	    (regs->vs_regs.data_addr == 0 && regs->gs_regs.data_addr == 0 && regs->es_regs.data_addr != 0 && regs->gs_regs.chksum != 0);
	info->float_mode = gs_instead_of_vs ? regs->gs_regs.rsrc1.float_mode : regs->vs_regs.rsrc1.float_mode;
	info->dx10_clamp = gs_instead_of_vs ? regs->gs_regs.rsrc1.dx10_clamp : regs->vs_regs.rsrc1.dx10_clamp;
	info->ieee_mode  = gs_instead_of_vs ? regs->gs_regs.rsrc1.ieee_mode : regs->vs_regs.rsrc1.ieee_mode;

	uint64_t                shader_addr   = (gs_instead_of_vs ? regs->es_regs.data_addr : regs->vs_regs.data_addr);
	const HW::UserSgprInfo& user_sgpr     = (gs_instead_of_vs ? regs->gs_user_sgpr : regs->vs_user_sgpr);
	auto                    user_sgpr_num = (gs_instead_of_vs ? regs->gs_regs.rsrc2.user_sgpr : regs->vs_regs.rsrc2.user_sgpr);

	bool ps5 = Config::IsNextGen();

	ShaderMappedData data;

	if (ps5)
	{
		(void)ShaderGetMappedData(shader_addr, &data);
	}

	if (ps5)
	{
		if (data.user_data == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: data.user_data == nullptr condition ignored (continuing)\n"); }
		if (!gs_instead_of_vs) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !gs_instead_of_vs condition ignored (continuing)\n"); }

		info->gs_prolog = true;

		// The fused ES-to-GS front reserves s0..s7 for NGG system state. API
		// user-data index zero is therefore addressed as s8 by the shader.
		// PS/CS already pass the decoded ISA so instruction-stream S#/V#
		// loads are bound; VS used to pass nullptr and dropped reused
		// constant buffers (measured: skybox projection at s[16:19]).
		constexpr int kGen5GsFrontUserDataBase = 8;
		const auto vs_isa = GetCachedParsedVsIsa(shader_addr, (regs->gs_regs.chksum >> 32u) & 0xffffffffu,
		                                         regs->gs_regs.chksum & 0xffffffffu);
		ShaderUserData  rebased_user {};
		ShaderSharp     sharp_copy[32] = {};
		const ShaderUserData* usage_user = data.user_data;
		if (data.user_data != nullptr && vs_isa != nullptr && data.user_data->sharp_resource_count[3] > 0 &&
		    data.user_data->sharp_resource_count[3] <= 32u && data.user_data->sharp_resource_offset[3] != nullptr)
		{
			rebased_user = *data.user_data;
			std::memcpy(sharp_copy, data.user_data->sharp_resource_offset[3],
			            static_cast<size_t>(data.user_data->sharp_resource_count[3]) * sizeof(ShaderSharp));
			rebased_user.sharp_resource_offset[3] = sharp_copy;
			RebaseNggConstantSharps(&rebased_user, vs_isa.get(), user_sgpr, static_cast<int>(user_sgpr_num));
			usage_user = &rebased_user;
		}
		// Do not pass the ISA into ParseUsage2 here: the instruction-stream
		// fallback AddZeroSBufferResource on a reused s[16:19] V# zeros the
		// projection CBV. Rebase the metadata sharp table instead.
		ShaderParseUsage2(usage_user, &usage, &info->bind, user_sgpr, static_cast<int>(user_sgpr_num), nullptr,
		                  kGen5GsFrontUserDataBase);
		if (vs_isa != nullptr)
		{
			ShaderAssociateSampledTextureSamplers(*vs_isa, &info->bind, kGen5GsFrontUserDataBase);
		}
	} else
	{
		if (gs_instead_of_vs) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: gs_instead_of_vs condition ignored (continuing)\n"); }

		info->gs_prolog = false;

		ShaderParseUsage(shader_addr, &usage, &info->bind, user_sgpr, user_sgpr_num);
	}

	if (usage.extended_buffer) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.extended_buffer condition ignored (continuing)\n"); }
	if (usage.gds_pointers > 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.gds_pointers > 0 condition ignored (continuing)\n"); }
	// Gen5 vertex shaders can use sampled textures/samplers for material and UI
	// paths. Descriptor allocation, sampler preparation and SPIR-V image sampling
	// are stage-generic here; keep the unsupported VS storage/GDS paths guarded.
	if (usage.storage_buffers_readonly > 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.storage_buffers_readonly > 0 condition ignored (continuing)\n"); }
	if (usage.storage_buffers_readwrite > 0 || usage.textures2D_readwrite > 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.storage_buffers_readwrite > 0 || usage.textures2D_readwrite > 0 condition ignored (continuing)\n"); }
	if (!ps5 && ((usage.fetch && !usage.vertex_buffer) || (!usage.fetch && usage.vertex_buffer))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ps5 && ((usage.fetch && !usage.vertex_buffer) || (!usage.fetch && usage.vertex_buffer)) condition ignored (continuing)\n"); }
	if (ps5 && ((usage.vertex_attrib && !usage.vertex_buffer) || (!usage.vertex_attrib && usage.vertex_buffer))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps5 && ((usage.vertex_attrib && !usage.vertex_buffer) || (!usage.vertex_attrib && usage.vertex_buffer)) condition ignored (continuing)\n"); }

	if (usage.vertex_buffer && usage.vertex_attrib)
	{
		info->fetch_external   = false;
		info->fetch_embedded   = true;
		info->fetch_inline     = false;
		info->fetch_attrib_reg = usage.vertex_attrib_reg;
		info->fetch_buffer_reg = usage.vertex_buffer_reg;

		if (usage.vertex_attrib_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.vertex_attrib_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX condition ignored (continuing)\n"); }
		if (usage.vertex_buffer_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.vertex_buffer_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX condition ignored (continuing)\n"); }

		const auto* attrib =
		    reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(user_sgpr.value[usage.vertex_attrib_reg]) |
		                                      (static_cast<uint64_t>(user_sgpr.value[usage.vertex_attrib_reg + 1]) << 32u));
		const auto* buffer =
		    reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(user_sgpr.value[usage.vertex_buffer_reg]) |
		                                      (static_cast<uint64_t>(user_sgpr.value[usage.vertex_buffer_reg + 1]) << 32u));

		if (attrib == nullptr || buffer == nullptr)
		{
			static uint32_t logs = 0;
			if (logs < 16u)
			{
				++logs;
				KYTY_LOG_DEBUG(
				             "SHADER_VERTEX_RESOURCE_REJECT shader=0x%016" PRIx64 " user_sgprs=%u attrib_reg=%d attrib=0x%016" PRIx64
				             " buffer_reg=%d buffer=0x%016" PRIx64 "\n",
				             shader_addr, user_sgpr_num, usage.vertex_attrib_reg, reinterpret_cast<uint64_t>(attrib),
				             usage.vertex_buffer_reg, reinterpret_cast<uint64_t>(buffer));
			}
			info->input_resources_valid = false;
			return;
		}

		if (data.input_semantics == nullptr || data.num_input_semantics == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: data.input_semantics == nullptr || data.num_input_semantics == 0 condition ignored (continuing)\n"); }

		ShaderParseAttrib(info, data.input_semantics, data.num_input_semantics, attrib, buffer);
		ShaderDetectBuffers(info, ps5);
		ShaderAppendVertexStreamStorage(info);

		constexpr uint32_t user_data_base = 8;
		const uint32_t shader_hash0 = (regs->gs_regs.chksum >> 32u) & 0xffffffffu;
		const uint32_t shader_crc32 = regs->gs_regs.chksum & 0xffffffffu;
		int32_t        detected_offset = -1;
		bool           offset_cached   = false;
		std::shared_lock shader_lifetime_lock(g_shader_lifetime_mutex);
		{
			std::lock_guard<std::mutex> lock(g_vertex_offset_sgpr_mutex);
			if (auto cached = g_vertex_offset_sgpr_map->find(shader_addr); cached != g_vertex_offset_sgpr_map->end())
			{
				if (cached->second.hash0 == shader_hash0 && cached->second.crc32 == shader_crc32)
				{
					detected_offset = cached->second.offset;
					offset_cached   = true;
				} else
				{
					g_vertex_offset_sgpr_map->erase(cached);
				}
			}
		}
		if (!offset_cached)
		{
			ShaderCode code;
			code.SetType(ShaderType::Vertex);
			ShaderParseMappedLocked(shader_addr, &code,
			                        gs_instead_of_vs ? ShaderContinuationMode::AllowTerminator : ShaderContinuationMode::None);
			detected_offset = ShaderDetectVertexOffsetSgpr(code, user_data_base, user_sgpr_num);
			std::lock_guard<std::mutex> lock(g_vertex_offset_sgpr_mutex);
			const auto entry = VertexOffsetCacheEntry {shader_hash0, shader_crc32, detected_offset};
			auto [cached, inserted] = g_vertex_offset_sgpr_map->insert_or_assign(shader_addr, entry);
			static_cast<void>(inserted);
			detected_offset = cached->second.offset;
		}
		info->vertex_offset_sgpr = detected_offset;
		if (info->vertex_offset_sgpr >= static_cast<int32_t>(user_data_base))
		{
			const auto index = static_cast<uint32_t>(info->vertex_offset_sgpr) - user_data_base;
			if (index < user_sgpr_num && index < static_cast<uint32_t>(HW::UserSgprInfo::SGPRS_MAX))
			{
				info->vertex_offset_value = user_sgpr.value[index];
			}
		}
	}

	if (usage.fetch && usage.vertex_buffer)
	{
		info->fetch_external   = true;
		info->fetch_embedded   = false;
		info->fetch_inline     = false;
		info->fetch_shader_reg = usage.fetch_reg;
		info->fetch_buffer_reg = usage.vertex_buffer_reg;

		if (usage.fetch_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.fetch_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX condition ignored (continuing)\n"); }
		if (usage.vertex_buffer_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.vertex_buffer_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX condition ignored (continuing)\n"); }

		const auto* fetch = reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(user_sgpr.value[usage.fetch_reg]) |
		                                                      (static_cast<uint64_t>(user_sgpr.value[usage.fetch_reg + 1]) << 32u));
		const auto* buffer =
		    reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(user_sgpr.value[usage.vertex_buffer_reg]) |
		                                      (static_cast<uint64_t>(user_sgpr.value[usage.vertex_buffer_reg + 1]) << 32u));

		if (fetch == nullptr || buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: fetch == nullptr || buffer == nullptr condition ignored (continuing)\n"); }

		ShaderParseFetch(info, fetch, buffer, user_sgpr_num);
		ShaderDetectBuffers(info, ps5);
		if (info->vertex_offset_sgpr >= 0 && static_cast<uint32_t>(info->vertex_offset_sgpr) < user_sgpr_num &&
		    static_cast<uint32_t>(info->vertex_offset_sgpr) < static_cast<uint32_t>(HW::UserSgprInfo::SGPRS_MAX))
		{
			info->vertex_offset_value = user_sgpr.value[info->vertex_offset_sgpr];
		}
	}

	ShaderCalcBindingIndices(&info->bind);
}

void ShaderGetInputInfoPS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh, const ShaderVertexInputInfo* vs_info,
                          ShaderPixelInputInfo* ps_info, bool allow_noop_stage_disable)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(vs_info == nullptr);
	EXIT_IF(ps_info == nullptr);
	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	*ps_info = {};
	ps_info->float_mode = regs->ps_regs.rsrc1.float_mode;
	ps_info->dx10_clamp = regs->ps_regs.rsrc1.dx10_clamp;
	ps_info->ieee_mode  = regs->ps_regs.rsrc1.ieee_mode;
	if (!regs->ps_embedded && regs->ps_regs.data_addr == 0)
	{
		ps_info->stage_enabled = false;
		return;
	}
	if (regs->ps_embedded)
	{
		return;
	}

	ps_info->input_num            = sh->ps_in_control & 0x3fu;
	ps_info->system_input_enable  = sh->ps_input_ena;
	ps_info->system_input_address = sh->ps_input_addr;
	ps_info->ps_pos_xy            = ShaderPixelPositionEnabled(sh->ps_input_ena, sh->ps_input_addr);
	ps_info->ps_pixel_kill_enable = sh->db_shader_control.shader_kill_enable;
	ps_info->ps_early_z           = (sh->db_shader_control.shader_z_behavior == 1);
	ps_info->ps_execute_on_noop   = sh->db_shader_control.shader_execute_on_noop;
	const bool ps_wave32 = (sh->ps_in_control & (Pm4::SPI_PS_IN_CONTROL_PS_W32_EN_MASK << Pm4::SPI_PS_IN_CONTROL_PS_W32_EN_SHIFT)) != 0;

	for (uint32_t i = 0; i < 32u; i++)
	{
		ps_info->interpolator_settings[i] =
		    ShaderResolvePixelInterpolatorSetting(sh->ps_interpolator_settings[i], sh->ps_interpolator_written_mask, i);
	}

	const bool vs_uses_descriptor     = ShaderBindRequiresDescriptorSet(vs_info->bind);
	ps_info->bind.descriptor_set_slot = (vs_uses_descriptor ? 1 : 0);
	ps_info->bind.push_constant_offset =
	    vs_info->bind.push_constant_offset + (vs_info->bind.vsharp_uniform_buffer ? 0u : vs_info->bind.push_constant_size);
	ps_info->bind.push_constant_size = 0;

	for (int i = 0; i < 8; i++)
	{
		ps_info->target_output_mode[i]  = sh->target_output_mode[i];
		ps_info->target_output_order[i] = sh->target_output_order[i];
	}

	bool ps5 = Config::IsNextGen();

	ShaderMappedData data;

	if (ps5)
	{
		(void)ShaderGetMappedData(regs->ps_regs.data_addr, &data);
	}

	ShaderParsedUsage usage;

	if (ps5)
	{
		if (data.user_data == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: data.user_data == nullptr condition ignored (continuing)\n"); }

		const auto analysis = g_shader_resolution_usage_cache.GetOrAnalyze(
		    {regs->ps_regs.data_addr, regs->ps_regs.chksum, kShaderTranslatorVersion},
		    [regs]()
		    {
			    auto code = std::make_shared<ShaderCode>();
			    code->SetType(ShaderType::Pixel);
			    code->SetHash0((regs->ps_regs.chksum >> 32u) & 0xffffffffu);
			    code->SetCrc32(regs->ps_regs.chksum & 0xffffffffu);
			    {
				    DebugStatsScopedTimer timer(RecordShaderInputAnalysis);
				    ShaderParseMapped(regs->ps_regs.data_addr, code.get());
			    }
			    ShaderProbeWrite("ps", *code, nullptr, nullptr);
			    return RenderResolutionShaderAnalysis {AnalyzeResolutionShaderUsage(*code), code};
		    });
		ps_info->integer_image_coordinates = analysis.usage.integer_image_coordinates;
		ps_info->image_size_query          = analysis.usage.image_size_query;
		ps_info->required_subgroup_size    = ShaderPixelRequiredSubgroupSize(*analysis.code, ps_wave32);
		if (allow_noop_stage_disable && !ShaderPreventsNoopPixelElision(*analysis.code))
		{
			ps_info->stage_enabled = false;
			return;
		}
		ShaderParseUsage2(data.user_data, &usage, &ps_info->bind, regs->ps_user_sgpr, regs->ps_regs.rsrc2.user_sgpr, analysis.code.get(), 0,
		                  false);
	} else
	{
		ShaderParseUsage(regs->ps_regs.data_addr, &usage, &ps_info->bind, regs->ps_user_sgpr, regs->ps_regs.rsrc2.user_sgpr);
	}

	// Gen5 user-data is shared by linked stages. A PS can therefore carry the
	// VS fetch/V#/attrib metadata in its direct-resource table even though its
	// instruction stream never consumes it. ShaderParseUsage2 records those
	// slots for the vertex path; they do not create PS descriptor bindings.
	if (usage.storage_buffers_readwrite > 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.storage_buffers_readwrite > 0 condition ignored (continuing)\n"); }
	if (usage.gds_pointers > 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.gds_pointers > 0 condition ignored (continuing)\n"); }

	ShaderCalcBindingIndices(&ps_info->bind);
}

void ShaderGetInputInfoCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* /*sh*/, ShaderComputeInputInfo* info)
{
	EXIT_IF(info == nullptr);
	EXIT_IF(regs == nullptr);

	info->bind           = {};
	info->meta_fill      = {};
	info->threads_num[0] = regs->cs_regs.num_thread_x;
	info->threads_num[1] = regs->cs_regs.num_thread_y;
	info->threads_num[2] = regs->cs_regs.num_thread_z;
	// COMPUTE_PGM_RSRC2.LDS_SIZE is expressed in 128-dword allocation units.
	info->lds_dwords     = ShaderComputeLdsDwords(regs->cs_regs.lds_size);
	info->group_id[0]    = regs->cs_regs.tgid_x_en != 0;
	info->group_id[1]    = regs->cs_regs.tgid_y_en != 0;
	info->group_id[2]    = regs->cs_regs.tgid_z_en != 0;
	info->thread_ids_num = regs->cs_regs.tidig_comp_cnt + 1;
	info->storage_image_write_only_mask = 0;

	info->workgroup_register = regs->cs_regs.user_sgpr;

	info->bind.push_constant_offset = 0;
	info->bind.push_constant_size   = 0;
	info->bind.descriptor_set_slot  = 0;

	ShaderParsedUsage usage;

	if (Config::IsNextGen())
	{
		// PS5 shaders describe their resources through the mapped shader user-data
		// block, not an embedded usage-slot table.
		ShaderMappedData data;
		(void)ShaderGetMappedData(regs->cs_regs.data_addr, &data);
		if (data.user_data == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: data.user_data == nullptr condition ignored (continuing)\n"); }
		ShaderCode code;
		code.SetType(ShaderType::Compute);
		{
			DebugStatsScopedTimer timer(RecordShaderInputAnalysis);
			ShaderParseMapped(regs->cs_regs.data_addr, &code);
		}
		const auto user_sgpr_num =
		    ShaderResolveGen5UserSgprCount(regs->cs_regs.user_sgpr, regs->cs_user_sgpr.count, data.user_data->eud_size_dw);
		ShaderParseUsage2(data.user_data, &usage, &info->bind, regs->cs_user_sgpr, static_cast<int>(user_sgpr_num), &code, 0, false);
		if (code.HasAnyOf({ShaderInstructionType::VLshlAddU32, ShaderInstructionType::VCmpxGtU32,
		                   ShaderInstructionType::BufferLoadFormatX, ShaderInstructionType::BufferStoreFormatX}))
		{
			for (int destination = 0; destination < info->bind.storage_buffers.buffers_num && !info->meta_fill.valid; ++destination)
			{
				if (info->bind.storage_buffers.usages[destination] != ShaderStorageUsage::ReadWrite ||
				    info->bind.storage_buffers.accesses[destination] != ShaderStorageAccess::Typed)
				{
					continue;
				}
				for (int source = 0; source < info->bind.storage_buffers.buffers_num && !info->meta_fill.valid; ++source)
				{
					if (source == destination || !ShaderStorageUsageIsReadOnly(info->bind.storage_buffers.usages[source]) ||
					    info->bind.storage_buffers.accesses[source] != ShaderStorageAccess::Typed)
					{
						continue;
					}
					for (int parameters = 0; parameters < info->bind.storage_buffers.buffers_num; ++parameters)
					{
						if (parameters == source || parameters == destination ||
						    !ShaderStorageUsageIsReadOnly(info->bind.storage_buffers.usages[parameters]) ||
						    info->bind.storage_buffers.accesses[parameters] != ShaderStorageAccess::Raw)
						{
							continue;
						}
						auto evidence = AnalyzeShaderComputeMetaFill(
						    code, info->bind.storage_buffers.start_register[source],
						    info->bind.storage_buffers.start_register[destination],
						    info->bind.storage_buffers.start_register[parameters]);
						if (evidence.valid)
						{
							info->meta_fill = evidence;
							break;
						}
					}
				}
			}
		}
		for (int i = 0; i < info->bind.textures2D.textures_num; ++i)
		{
			auto& descriptor = info->bind.textures2D.desc[i];
			if (!descriptor.textures2d_without_sampler)
			{
				continue;
			}
			const auto image_use = AnalyzeShaderDirectImageUse(code, descriptor.start_register);
			if (image_use.writes && !image_use.reads)
			{
				descriptor.storage_image_write_only = true;
				info->storage_image_write_only_mask |= 1u << static_cast<uint32_t>(i);
			}
		}
	} else
	{
		ShaderParseUsage(regs->cs_regs.data_addr, &usage, &info->bind, regs->cs_user_sgpr, regs->cs_regs.user_sgpr);
	}

	// Gen5 compute may bind S# samplers for image_sample / image_sample_lz
	// (same sharp[2] path as PS). PS already allows usage.samplers > 0.
	// As with PS, shared Gen5 user-data may include vertex-only direct-resource
	// metadata. Compute binding construction ignores those entries.

	ShaderCalcBindingIndices(&info->bind);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void ShaderDbgDumpResources(const ShaderBindResources& bind)
{
	KYTY_LOG_DEBUG("\t descriptor_set_slot            = %u\n", bind.descriptor_set_slot);
	KYTY_LOG_DEBUG("\t push_constant_offset           = %u\n", bind.push_constant_offset);
	KYTY_LOG_DEBUG("\t push_constant_size             = %u\n", bind.push_constant_size);
	KYTY_LOG_DEBUG("\t storage_buffers.buffers_num    = %d\n", bind.storage_buffers.buffers_num);
	KYTY_LOG_DEBUG("\t storage_buffers.binding_index  = %d\n", bind.storage_buffers.binding_index);
	KYTY_LOG_DEBUG("\t textures.textures_num          = %d\n", bind.textures2D.textures_num);
	KYTY_LOG_DEBUG("\t textures.binding_sampled_index = %d\n", bind.textures2D.binding_sampled_index);
	KYTY_LOG_DEBUG("\t textures.binding_storage_index = %d\n", bind.textures2D.binding_storage_index);
	KYTY_LOG_DEBUG("\t samplers.samplers_num          = %d\n", bind.samplers.samplers_num);
	KYTY_LOG_DEBUG("\t samplers.binding_index         = %d\n", bind.samplers.binding_index);
	KYTY_LOG_DEBUG("\t gds_pointers.pointers_num      = %d\n", bind.gds_pointers.pointers_num);
	KYTY_LOG_DEBUG("\t gds_pointers.binding_index     = %d\n", bind.gds_pointers.binding_index);
	KYTY_LOG_DEBUG("\t direct_sgprs.sgprs_num         = %d\n", bind.direct_sgprs.sgprs_num);
	KYTY_LOG_DEBUG("\t extended.used                  = %s\n", (bind.extended.used ? "true" : "false"));
	KYTY_LOG_DEBUG("\t extended.slot                  = %d\n", bind.extended.slot);
	KYTY_LOG_DEBUG("\t extended.start_register        = %d\n", bind.extended.start_register);
	KYTY_LOG_DEBUG("\t extended.data.Base             = %" PRIx64 "\n", bind.extended.data.Base());

	bool gen5 = Config::IsNextGen();

	for (int i = 0; i < bind.storage_buffers.buffers_num; i++)
	{
		const auto& r = bind.storage_buffers.buffers[i];

		KYTY_LOG_DEBUG("\t StorageBuffer %d\n", i);

		KYTY_LOG_DEBUG("\t\t fields           = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n", r.fields[3], r.fields[2], r.fields[1],
		       r.fields[0]);
		KYTY_LOG_DEBUG("\t\t Base()           = %" PRIx64 "\n", gen5 ? r.Base48() : r.Base44());
		KYTY_LOG_DEBUG("\t\t Stride()         = %" PRIu16 "\n", r.Stride());
		KYTY_LOG_DEBUG("\t\t SwizzleEnabled() = %s\n", r.SwizzleEnabled() ? "true" : "false");
		KYTY_LOG_DEBUG("\t\t NumRecords()     = %" PRIu32 "\n", r.NumRecords());
		KYTY_LOG_DEBUG("\t\t DstSelX()        = %" PRIu8 "\n", r.DstSelX());
		KYTY_LOG_DEBUG("\t\t DstSelY()        = %" PRIu8 "\n", r.DstSelY());
		KYTY_LOG_DEBUG("\t\t DstSelZ()        = %" PRIu8 "\n", r.DstSelZ());
		KYTY_LOG_DEBUG("\t\t DstSelW()        = %" PRIu8 "\n", r.DstSelW());
		if (!gen5)
		{
			KYTY_LOG_DEBUG("\t\t Nfmt()           = %" PRIu8 "\n", r.Nfmt());
			KYTY_LOG_DEBUG("\t\t Dfmt()           = %" PRIu8 "\n", r.Dfmt());
			KYTY_LOG_DEBUG("\t\t MemoryType()     = 0x%02" PRIx8 "\n", r.MemoryType());
		} else
		{
			KYTY_LOG_DEBUG("\t\t Format()         = %" PRIu8 "\n", r.Format());
			KYTY_LOG_DEBUG("\t\t OutOfBounds()    = %" PRIu8 "\n", r.OutOfBounds());
		}
		KYTY_LOG_DEBUG("\t\t AddTid()         = %s\n", r.AddTid() ? "true" : "false");
		KYTY_LOG_DEBUG("\t\t slot             = %d\n", bind.storage_buffers.slots[i]);
		KYTY_LOG_DEBUG("\t\t start_register   = %d\n", bind.storage_buffers.start_register[i]);
		KYTY_LOG_DEBUG("\t\t extended         = %s\n", (bind.storage_buffers.extended[i] ? "true" : "false"));
		KYTY_LOG_DEBUG("\t\t dynamic_sload    = %s\n", (bind.storage_buffers.dynamic_sload[i] ? "true" : "false"));
		KYTY_LOG_DEBUG("\t\t usage            = %s\n", Core::EnumName8(bind.storage_buffers.usages[i]).c_str());
	}
	for (int mapping = 0; mapping < bind.dynamic_sloads.mappings_num; ++mapping)
	{
		KYTY_LOG_DEBUG("\t DynamicSLoad %d: kind=%u resource=%d dst=%d pc=%08" PRIx32 " offset_dw=%d dwords=%d field=%d last_consumer=%08" PRIx32 "\n",
		       mapping, static_cast<unsigned>(bind.dynamic_sloads.kind[mapping]), bind.dynamic_sloads.resource_index[mapping],
		       bind.dynamic_sloads.destination_register[mapping], bind.dynamic_sloads.instruction_pc[mapping],
		       bind.dynamic_sloads.offset_dw[mapping], bind.dynamic_sloads.dword_count[mapping],
		       bind.dynamic_sloads.resource_field_offset[mapping],
		       bind.dynamic_sloads.last_consumer_pc[mapping]);
	}

	for (int i = 0; i < bind.textures2D.textures_num; i++)
	{
		const auto& r = bind.textures2D.desc[i].texture;

		KYTY_LOG_DEBUG("\t Texture %d\n", i);

		KYTY_LOG_DEBUG("\t\t fields = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n",
		       r.fields[7], r.fields[6], r.fields[5], r.fields[4], r.fields[3], r.fields[2], r.fields[1], r.fields[0]);
		KYTY_LOG_DEBUG("\t\t Base()          = %016" PRIx64 "\n", gen5 ? r.Base40() : r.Base38());
		KYTY_LOG_DEBUG("\t\t MinLod()        = %" PRIu16 "\n", r.MinLod());
		if (gen5)
		{
			KYTY_LOG_DEBUG("\t\t Format()        = %" PRIu16 "\n", r.Format());
			KYTY_LOG_DEBUG("\t\t BCSwizzle()     = %" PRIu8 "\n", r.BCSwizzle());
			KYTY_LOG_DEBUG("\t\t BaseArray5()    = %" PRIu16 "\n", r.BaseArray5());
			KYTY_LOG_DEBUG("\t\t ArrayPitch()    = %" PRIu8 "\n", r.ArrayPitch());
			KYTY_LOG_DEBUG("\t\t MaxMip()        = %" PRIu8 "\n", r.MaxMip());
			KYTY_LOG_DEBUG("\t\t MinLodWarn5()   = %" PRIu16 "\n", r.MinLodWarn5());
			KYTY_LOG_DEBUG("\t\t PerfMod5()      = %" PRIu8 "\n", r.PerfMod5());
			KYTY_LOG_DEBUG("\t\t CornerSample()  = %s\n", r.CornerSample() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t MipStatsCntEn() = %s\n", r.MipStatsCntEn() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t PrtDefColor()   = %s\n", r.PrtDefColor() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t MipStatsCntId() = %" PRIu8 "\n", r.MipStatsCntId());
			KYTY_LOG_DEBUG("\t\t MsaaDepth()     = %s\n", r.MsaaDepth() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t MaxUncBlkSize() = %" PRIu8 "\n", r.MaxUncompBlkSize());
			KYTY_LOG_DEBUG("\t\t MaxCompBlkSize()= %" PRIu8 "\n", r.MaxCompBlkSize());
			KYTY_LOG_DEBUG("\t\t MetaPipeAlign() = %s\n", r.MetaPipeAligned() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t WriteCompress() = %s\n", r.WriteCompress() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t MetaCompress()  = %s\n", r.MetaCompress() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t DccAlphaPos()   = %s\n", r.DccAlphaPos() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t DccColorTransf()= %s\n", r.DccColorTransf() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t MetaAddr()      = %" PRIx64 "\n", r.MetaAddr());

		} else
		{
			KYTY_LOG_DEBUG("\t\t Dfmt()          = %" PRIu8 "\n", r.Dfmt());
			KYTY_LOG_DEBUG("\t\t Nfmt()          = %" PRIu8 "\n", r.Nfmt());
			KYTY_LOG_DEBUG("\t\t PerfMod()       = %" PRIu8 "\n", r.PerfMod());
			KYTY_LOG_DEBUG("\t\t Interlaced()    = %s\n", r.Interlaced() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t MemoryType()    = 0x%02" PRIx8 "\n", r.MemoryType());
			KYTY_LOG_DEBUG("\t\t Pow2Pad()       = %s\n", r.Pow2Pad() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t Pitch()         = %" PRIu16 "\n", r.Pitch());
			KYTY_LOG_DEBUG("\t\t BaseArray()     = %" PRIu16 "\n", r.BaseArray());
			KYTY_LOG_DEBUG("\t\t LastArray()     = %" PRIu16 "\n", r.LastArray());
			KYTY_LOG_DEBUG("\t\t MinLodWarn()    = %" PRIu16 "\n", r.MinLodWarn());
			KYTY_LOG_DEBUG("\t\t LodHdwCntEn()   = %s\n", r.LodHdwCntEn() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t CounterBankId() = %" PRIu8 "\n", r.CounterBankId());
		}
		KYTY_LOG_DEBUG("\t\t Width()         = %" PRIu16 "\n", gen5 ? r.Width5() : r.Width4());
		KYTY_LOG_DEBUG("\t\t Height()        = %" PRIu16 "\n", gen5 ? r.Height5() : r.Height4());
		KYTY_LOG_DEBUG("\t\t DstSelX()       = %" PRIu8 "\n", r.DstSelX());
		KYTY_LOG_DEBUG("\t\t DstSelY()       = %" PRIu8 "\n", r.DstSelY());
		KYTY_LOG_DEBUG("\t\t DstSelZ()       = %" PRIu8 "\n", r.DstSelZ());
		KYTY_LOG_DEBUG("\t\t DstSelW()       = %" PRIu8 "\n", r.DstSelW());
		KYTY_LOG_DEBUG("\t\t BaseLevel()     = %" PRIu8 "\n", r.BaseLevel());
		KYTY_LOG_DEBUG("\t\t LastLevel()     = %" PRIu8 "\n", r.LastLevel());
		KYTY_LOG_DEBUG("\t\t TileMode()      = %" PRIu8 "\n", r.TileMode());
		KYTY_LOG_DEBUG("\t\t Type()          = %" PRIu8 "\n", r.Type());
		KYTY_LOG_DEBUG("\t\t Depth()         = %" PRIu16 "\n", r.Depth());
		KYTY_LOG_DEBUG("\t\t slot            = %d\n", bind.textures2D.desc[i].slot);
		KYTY_LOG_DEBUG("\t\t start_register  = %d\n", bind.textures2D.desc[i].start_register);
		KYTY_LOG_DEBUG("\t\t extended        = %s\n", (bind.textures2D.desc[i].extended ? "true" : "false"));
		KYTY_LOG_DEBUG("\t\t usage           = %s\n", Core::EnumName8(bind.textures2D.desc[i].usage).c_str());
	}

	for (int i = 0; i < bind.samplers.samplers_num; i++)
	{
		const auto& r = bind.samplers.samplers[i];

		KYTY_LOG_DEBUG("\t Sampler %d\n", i);

		KYTY_LOG_DEBUG("\t\t fields = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n", r.fields[3], r.fields[2], r.fields[1], r.fields[0]);

		KYTY_LOG_DEBUG("\t\t ClampX()           = %" PRIu8 "\n", r.ClampX());
		KYTY_LOG_DEBUG("\t\t ClampY()           = %" PRIu8 "\n", r.ClampY());
		KYTY_LOG_DEBUG("\t\t ClampZ()           = %" PRIu8 "\n", r.ClampZ());
		KYTY_LOG_DEBUG("\t\t MaxAnisoRatio()    = %" PRIu8 "\n", r.MaxAnisoRatio());
		KYTY_LOG_DEBUG("\t\t DepthCompareFunc() = %" PRIu8 "\n", r.DepthCompareFunc());
		KYTY_LOG_DEBUG("\t\t ForceUnormCoords() = %s\n", r.ForceUnormCoords() ? "true" : "false");
		KYTY_LOG_DEBUG("\t\t AnisoThreshold()   = %" PRIu8 "\n", r.AnisoThreshold());
		if (!gen5)
		{
			KYTY_LOG_DEBUG("\t\t McCoordTrunc()     = %s\n", r.McCoordTrunc() ? "true" : "false");
		} else
		{
			KYTY_LOG_DEBUG("\t\t SkipDegamma()      = %s\n", r.SkipDegamma() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t PointPreclamp()    = %s\n", r.PointPreclamp() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t AnisoOverride()    = %s\n", r.AnisoOverride() ? "true" : "false");
			KYTY_LOG_DEBUG("\t\t BlendZeroPrt()     = %s\n", r.BlendZeroPrt() ? "true" : "false");
		}
		KYTY_LOG_DEBUG("\t\t ForceDegamma()     = %s\n", r.ForceDegamma() ? "true" : "false");
		KYTY_LOG_DEBUG("\t\t AnisoBias()        = %" PRIu8 "\n", r.AnisoBias());
		KYTY_LOG_DEBUG("\t\t TruncCoord()       = %s\n", r.TruncCoord() ? "true" : "false");
		KYTY_LOG_DEBUG("\t\t DisableCubeWrap()  = %s\n", r.DisableCubeWrap() ? "true" : "false");
		KYTY_LOG_DEBUG("\t\t FilterMode()       = %" PRIu8 "\n", r.FilterMode());
		KYTY_LOG_DEBUG("\t\t MinLod()           = %" PRIu16 "\n", r.MinLod());
		KYTY_LOG_DEBUG("\t\t MaxLod()           = %" PRIu16 "\n", r.MaxLod());
		KYTY_LOG_DEBUG("\t\t PerfMip()          = %" PRIu8 "\n", r.PerfMip());
		KYTY_LOG_DEBUG("\t\t PerfZ()            = %" PRIu8 "\n", r.PerfZ());
		KYTY_LOG_DEBUG("\t\t LodBias()          = %" PRIu16 "\n", r.LodBias());
		KYTY_LOG_DEBUG("\t\t LodBiasSec()       = %" PRIu8 "\n", r.LodBiasSec());
		KYTY_LOG_DEBUG("\t\t XyMagFilter()      = %" PRIu8 "\n", r.XyMagFilter());
		KYTY_LOG_DEBUG("\t\t XyMinFilter()      = %" PRIu8 "\n", r.XyMinFilter());
		KYTY_LOG_DEBUG("\t\t ZFilter()          = %" PRIu8 "\n", r.ZFilter());
		KYTY_LOG_DEBUG("\t\t MipFilter()        = %" PRIu8 "\n", r.MipFilter());
		KYTY_LOG_DEBUG("\t\t BorderColorPtr()   = %" PRIu16 "\n", r.BorderColorPtr());
		KYTY_LOG_DEBUG("\t\t BorderColorType()  = %" PRIu8 "\n", r.BorderColorType());
		KYTY_LOG_DEBUG("\t\t slot               = %d\n", bind.samplers.slots[i]);
		KYTY_LOG_DEBUG("\t\t start_register     = %d\n", bind.samplers.start_register[i]);
		KYTY_LOG_DEBUG("\t\t extended           = %s\n", (bind.samplers.extended[i] ? "true" : "false"));
	}

	for (int i = 0; i < bind.gds_pointers.pointers_num; i++)
	{
		const auto& r = bind.gds_pointers.pointers[i];

		KYTY_LOG_DEBUG("\t Gds Pointer %d\n", i);

		KYTY_LOG_DEBUG("\t\t field = %08" PRIx32 "\n", r.field);

		KYTY_LOG_DEBUG("\t\t Base()         = %" PRIu16 "\n", r.Base());
		KYTY_LOG_DEBUG("\t\t Size()         = %" PRIu16 "\n", r.Size());
		KYTY_LOG_DEBUG("\t\t slot           = %d\n", bind.gds_pointers.slots[i]);
		KYTY_LOG_DEBUG("\t\t start_register = %d\n", bind.gds_pointers.start_register[i]);
		KYTY_LOG_DEBUG("\t\t extended       = %s\n", (bind.gds_pointers.extended[i] ? "true" : "false"));
	}

	for (int i = 0; i < bind.direct_sgprs.sgprs_num; i++)
	{
		const auto& r = bind.direct_sgprs.sgprs[i];

		KYTY_LOG_DEBUG("\t Direct Sgprs %d\n", i);

		KYTY_LOG_DEBUG("\t\t field = %08" PRIx32 "\n", r.field);

		KYTY_LOG_DEBUG("\t\t start_register = %d\n", bind.direct_sgprs.start_register[i]);
	}
}

void ShaderDbgDumpInputInfo(const ShaderVertexInputInfo* info)
{
	KYTY_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Vs)");

	KYTY_LOG_DEBUG("ShaderDbgDumpInputInfo()\n");

	KYTY_LOG_DEBUG("\t fetch_external = %s\n", info->fetch_external ? "true" : "false");
	KYTY_LOG_DEBUG("\t fetch_embedded = %s\n", info->fetch_embedded ? "true" : "false");
	KYTY_LOG_DEBUG("\t fetch_inline   = %s\n", info->fetch_inline ? "true" : "false");
	KYTY_LOG_DEBUG("\t export_count   = %d\n", info->export_count);

	bool gen5 = Config::IsNextGen();

	for (int i = 0; i < info->resources_num; i++)
	{
		KYTY_LOG_DEBUG("\t input %d\n", i);

		const auto& r  = info->resources[i];
		const auto& rd = info->resources_dst[i];

		KYTY_LOG_DEBUG("\t\t register_start   = %d\n", rd.register_start);
		KYTY_LOG_DEBUG("\t\t registers_num    = %d\n", rd.registers_num);
		KYTY_LOG_DEBUG("\t\t fields           = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n", r.fields[3], r.fields[2], r.fields[1],
		       r.fields[0]);
		KYTY_LOG_DEBUG("\t\t Base()           = %" PRIx64 "\n", gen5 ? r.Base48() : r.Base44());
		KYTY_LOG_DEBUG("\t\t Stride()         = %" PRIu16 "\n", r.Stride());
		KYTY_LOG_DEBUG("\t\t SwizzleEnabled() = %s\n", r.SwizzleEnabled() ? "true" : "false");
		KYTY_LOG_DEBUG("\t\t NumRecords()     = %" PRIu32 "\n", r.NumRecords());
		KYTY_LOG_DEBUG("\t\t DstSelX()        = %" PRIu8 "\n", r.DstSelX());
		KYTY_LOG_DEBUG("\t\t DstSelY()        = %" PRIu8 "\n", r.DstSelY());
		KYTY_LOG_DEBUG("\t\t DstSelZ()        = %" PRIu8 "\n", r.DstSelZ());
		KYTY_LOG_DEBUG("\t\t DstSelW()        = %" PRIu8 "\n", r.DstSelW());
		if (!gen5)
		{
			KYTY_LOG_DEBUG("\t\t Nfmt()           = %" PRIu8 "\n", r.Nfmt());
			KYTY_LOG_DEBUG("\t\t Dfmt()           = %" PRIu8 "\n", r.Dfmt());
			KYTY_LOG_DEBUG("\t\t MemoryType()     = 0x%02" PRIx8 "\n", r.MemoryType());
		} else
		{
			KYTY_LOG_DEBUG("\t\t Format()         = %" PRIu8 "\n", r.Format());
			KYTY_LOG_DEBUG("\t\t OutOfBounds()    = %" PRIu8 "\n", r.OutOfBounds());
		}
		KYTY_LOG_DEBUG("\t\t AddTid()         = %s\n", r.AddTid() ? "true" : "false");
	}

	for (int i = 0; i < info->buffers_num; i++)
	{
		KYTY_LOG_DEBUG("\t buffer %d\n", i);

		const auto& r = info->buffers[i];
		KYTY_LOG_DEBUG("\t\t addr        = %" PRIx64 "\n", r.addr);
		KYTY_LOG_DEBUG("\t\t stride      = %" PRIu32 "\n", r.stride);
		KYTY_LOG_DEBUG("\t\t num_records = %" PRIu32 "\n", r.num_records);
		KYTY_LOG_DEBUG("\t\t attr_num    = %" PRId32 "\n", r.attr_num);
		for (int j = 0; j < r.attr_num; j++)
		{
			KYTY_LOG_DEBUG("\t\t attr_indices[%d]  = %d\n", j, r.attr_indices[j]);
			KYTY_LOG_DEBUG("\t\t attr_offsets[%d]  = %u\n", j, r.attr_offsets[j]);
		}
	}

	ShaderDbgDumpResources(info->bind);
}

void ShaderDbgDumpInputInfo(const ShaderPixelInputInfo* info)
{
	KYTY_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Ps)");

	KYTY_LOG_DEBUG("ShaderDbgDumpInputInfo()\n");

	KYTY_LOG_DEBUG("\t input_num            = %u\n", info->input_num);
	KYTY_LOG_DEBUG("\t ps_pos_xy            = %s\n", info->ps_pos_xy ? "true" : "false");
	KYTY_LOG_DEBUG("\t ps_pixel_kill_enable = %s\n", info->ps_pixel_kill_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t ps_early_z           = %s\n", info->ps_early_z ? "true" : "false");
	KYTY_LOG_DEBUG("\t ps_execute_on_noop   = %s\n", info->ps_execute_on_noop ? "true" : "false");

	for (uint32_t i = 0; i < info->input_num; i++)
	{
		KYTY_LOG_DEBUG("\t interpolator_settings[%u] = %u\n", i, info->interpolator_settings[i]);
	}

	ShaderDbgDumpResources(info->bind);
}

void ShaderDbgDumpInputInfo(const ShaderComputeInputInfo* info)
{
	KYTY_LOG_DEBUG("ShaderDbgDumpInputInfo()\n");

	KYTY_LOG_DEBUG("\t workgroup_register = %d\n", info->workgroup_register);
	KYTY_LOG_DEBUG("\t thread_ids_num     = %d\n", info->thread_ids_num);
	KYTY_LOG_DEBUG("\t threads_num        = {%u, %u, %u}\n", info->threads_num[0], info->threads_num[1], info->threads_num[2]);
	KYTY_LOG_DEBUG("\t threadgroup_id     = {%s, %s, %s}\n", info->group_id[0] ? "true" : "false", info->group_id[1] ? "true" : "false",
	       info->group_id[2] ? "true" : "false");

	ShaderDbgDumpResources(info->bind);
}


ShaderCode ShaderParseVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::Amber300);

	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	ShaderCode code;
	code.SetType(ShaderType::Vertex);

	if (regs->vs_embedded)
	{
		code.SetVsEmbedded(true);
		code.SetVsEmbeddedId(regs->vs_embedded_id);
	} else
	{
		uint32_t hash0 = 0;
		uint32_t crc32 = 0;

		bool gs_instead_of_vs =
		    (regs->vs_regs.data_addr == 0 && regs->gs_regs.data_addr == 0 && regs->es_regs.data_addr != 0 && regs->gs_regs.chksum != 0);
		uint64_t shader_addr = (gs_instead_of_vs ? regs->es_regs.data_addr : regs->vs_regs.data_addr);

		const auto* src = reinterpret_cast<const uint32_t*>(shader_addr);

		if (src == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src == nullptr condition ignored (continuing)\n"); }

		vs_print("ShaderParseVS()", *regs, *sh);
		vs_check(*regs, *sh);

		if (gs_instead_of_vs)
		{
			if (regs->gs_regs.rsrc2.user_sgpr > regs->gs_user_sgpr.count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: regs->gs_regs.rsrc2.user_sgpr > regs->gs_user_sgpr.count condition ignored (continuing)\n"); }
		} else
		{
			if (regs->vs_regs.rsrc2.user_sgpr > regs->vs_user_sgpr.count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: regs->vs_regs.rsrc2.user_sgpr > regs->vs_user_sgpr.count condition ignored (continuing)\n"); }
		}

		if (Config::IsNextGen())
		{
			if (!gs_instead_of_vs) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !gs_instead_of_vs condition ignored (continuing)\n"); }

			hash0 = (regs->gs_regs.chksum >> 32u) & 0xffffffffu;
			crc32 = regs->gs_regs.chksum & 0xffffffffu;
		} else
		{
			const auto* header = GetBinaryInfo(src);

			if (header == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: header == nullptr condition ignored (continuing)\n"); }

			bi_print("ShaderParseVS():ShaderBinaryInfo", *header);

			hash0 = header->hash0;
			crc32 = header->crc32;
		}

		code.SetCrc32(crc32);
		code.SetHash0(hash0);
		// shader_parse(0, src, nullptr, &code);
		{
			DebugStatsScopedTimer timer(RecordShaderPipelineMissParse);
			ShaderParseMapped(shader_addr, &code,
			                  gs_instead_of_vs ? ShaderContinuationMode::Append : ShaderContinuationMode::None);
		}

		if (g_debug_printfs != nullptr)
		{
			auto id = (static_cast<uint64_t>(hash0) << 32u) | crc32;
			if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto id) { return cmd.id == id; }); g_debug_printfs->IndexValid(index))
			{
				code.GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
			}
		}
	}

	return code;
}

Vector<uint32_t> ShaderRecompileVS(const ShaderCode& code, const ShaderVertexInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::Amber300);

	String8          source;
	Vector<uint32_t> ret;
	ShaderLogHelper  log("vs");

	if (code.IsVsEmbedded())
	{
		EXIT_IF(input_info != nullptr && input_info->clip_probe.enabled);
		source = SpirvGetEmbeddedVs(code.GetVsEmbeddedId());
	} else
	{
		log.DumpOriginalShader(code);

		{
			DebugStatsScopedTimer timer(DebugStatsRecordSpirvSource);
			source = SpirvGenerateSource(code, input_info, nullptr, nullptr);
		}
	}

	log.DumpRecompiledShader(source);
	ShaderProbeWrite("vs", code, &source, nullptr);

	String8 err_msg;
	bool    spirv_ok = false;
	spirv_ok         = ShaderToolchain::Run(source, &ret, &err_msg);
	if (!spirv_ok)
	{
		KYTY_LOG_WARN("WARNING: vertex SpirvRun failed: %s\n", err_msg.c_str());
		return {};
	}

	log.DumpOptimizedShader(ret);
	ShaderProbeWrite("vs", code, nullptr, &ret);

	return ret;
}

ShaderCode ShaderParsePS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::Blue300);

	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	ShaderCode code;
	code.SetType(ShaderType::Pixel);

	if (regs->ps_embedded)
	{
		code.SetPsEmbedded(true);
		code.SetPsEmbeddedId(regs->ps_embedded_id);
	} else
	{
		uint32_t hash0 = 0;
		uint32_t crc32 = 0;

		ps_print("ShaderParsePS()", regs->ps_regs, *sh);
		ps_check(regs->ps_regs, *sh);

		if (regs->ps_regs.rsrc2.user_sgpr > regs->ps_user_sgpr.count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: regs->ps_regs.rsrc2.user_sgpr > regs->ps_user_sgpr.count condition ignored (continuing)\n"); }

		const auto* src = reinterpret_cast<const uint32_t*>(regs->ps_regs.data_addr);

		if (src == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src == nullptr condition ignored (continuing)\n"); }

		if (Config::IsNextGen())
		{
			hash0 = (regs->ps_regs.chksum >> 32u) & 0xffffffffu;
			crc32 = regs->ps_regs.chksum & 0xffffffffu;
		} else
		{
			const auto* header = GetBinaryInfo(src);

			if (header == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: header == nullptr condition ignored (continuing)\n"); }

			bi_print("ShaderParsePS():ShaderBinaryInfo", *header);

			hash0 = header->hash0;
			crc32 = header->crc32;
		}

		code.SetCrc32(crc32);
		code.SetHash0(hash0);
		// shader_parse(0, src, nullptr, &code);
		{
			DebugStatsScopedTimer timer(RecordShaderPipelineMissParse);
			ShaderParseMapped(regs->ps_regs.data_addr, &code);
		}

		if (g_debug_printfs != nullptr)
		{
			auto id = (static_cast<uint64_t>(hash0) << 32u) | crc32;
			if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto id) { return cmd.id == id; }); g_debug_printfs->IndexValid(index))
			{
				code.GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
			}
		}
	}

	return code;
}

Vector<uint32_t> ShaderRecompilePS(const ShaderCode& code, const ShaderPixelInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::Blue300);

	String8          source;
	Vector<uint32_t> ret;
	ShaderLogHelper  log("ps");

	if (code.IsPsEmbedded())
	{
		source = SpirvGetEmbeddedPs(code.GetPsEmbeddedId());
	} else
	{
		//		for (uint32_t i = 0; i < input_info->input_num; i++)
		//		{
		//			EXIT_NOT_IMPLEMENTED(input_info->interpolator_settings[i] != i);
		//		}

		log.DumpOriginalShader(code);

		{
			DebugStatsScopedTimer timer(DebugStatsRecordSpirvSource);
			source = SpirvGenerateSource(code, nullptr, input_info, nullptr);
		}
	}

	log.DumpRecompiledShader(source);
	ShaderProbeWrite("ps", code, &source, nullptr);

	String8 err_msg;
	bool    spirv_ok = false;
	spirv_ok         = ShaderToolchain::Run(source, &ret, &err_msg);
	if (!spirv_ok)
	{
		KYTY_LOG_WARN("WARNING: pixel SpirvRun failed: %s\n", err_msg.c_str());
		return {};
	}

	log.DumpOptimizedShader(ret);
	// Keep the recompiled text alongside the binary so CFG investigations can
	// compare both stages without a second run.
	ShaderProbeWrite("ps", code, &source, &ret);

	return ret;
}

ShaderCode ShaderParseCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* sh)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::CyanA700);

	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	const auto* src = reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr);

	if (src == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src == nullptr condition ignored (continuing)\n"); }

	cs_print("ShaderParseCS()", regs->cs_regs, *sh);
	cs_check(regs->cs_regs, *sh);

	if (regs->cs_regs.user_sgpr > regs->cs_user_sgpr.count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: regs->cs_regs.user_sgpr > regs->cs_user_sgpr.count condition ignored (continuing)\n"); }

	uint32_t hash0 = 0;
	uint32_t crc32 = 0;

	if (Config::IsNextGen())
	{
		// PS5 shaders carry their identity in the shader checksum register rather
		// than an embedded binary-info block.
		hash0 = (regs->cs_regs.chksum >> 32u) & 0xffffffffu;
		crc32 = regs->cs_regs.chksum & 0xffffffffu;
	} else
	{
		const auto* header = GetBinaryInfo(src);

		if (header == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: header == nullptr condition ignored (continuing)\n"); }

		bi_print("ShaderParseCS():ShaderBinaryInfo", *header);

		hash0 = header->hash0;
		crc32 = header->crc32;
	}

	ShaderCode code;
	code.SetType(ShaderType::Compute);

	code.SetCrc32(crc32);
	code.SetHash0(hash0);
	// shader_parse(0, src, nullptr, &code);
	{
		DebugStatsScopedTimer timer(RecordShaderPipelineMissParse);
		ShaderParseMapped(regs->cs_regs.data_addr, &code);
	}

	if (g_debug_printfs != nullptr)
	{
		auto id = (static_cast<uint64_t>(hash0) << 32u) | crc32;
		if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto id) { return cmd.id == id; }); g_debug_printfs->IndexValid(index))
		{
			code.GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
		}
	}

	return code;
}

Vector<uint32_t> ShaderRecompileCS(const ShaderCode& code, const ShaderComputeInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::CyanA700);

	ShaderLogHelper log("cs");

	Vector<uint32_t> ret;

	log.DumpOriginalShader(code);

	String8 source;
	{
		DebugStatsScopedTimer timer(DebugStatsRecordSpirvSource);
		source = SpirvGenerateSource(code, nullptr, nullptr, input_info);
	}

	log.DumpRecompiledShader(source);
	ShaderProbeWrite("cs", code, &source, nullptr);

	String8 err_msg;
	bool    spirv_ok = false;
	spirv_ok         = ShaderToolchain::Run(source, &ret, &err_msg);
	if (!spirv_ok)
	{
		KYTY_LOG_WARN("WARNING: compute SpirvRun failed: id=%016" PRIx64 " %s\n", ShaderCodeId(code), err_msg.c_str());
		return {};
	}

	log.DumpOptimizedShader(ret);
	// log.DumpGlslShader(ret);
	log.DumpBinary(ret);

	return ret;
}

//// NOLINTNEXTLINE(readability-function-cognitive-complexity)
// static ShaderBindParameters ShaderUpdateBindInfo(const ShaderCode& code, const ShaderBindResources* bind)
//{
//	ShaderBindParameters p {};
//
//	auto find_image_op = [&](int index, int s, bool& found, bool& without_sampler)
//	{
//		const auto& insts = code.GetInstructions();
//		int         size  = static_cast<int>(insts.Size());
//		for (int i = index; i < size; i++)
//		{
//			const auto& inst = insts.At(i);
//
//			if ((inst.dst.type == ShaderOperandType::Sgpr && s >= inst.dst.register_id && s < inst.dst.register_id + inst.dst.size) ||
//			    (inst.dst2.type == ShaderOperandType::Sgpr && s >= inst.dst2.register_id && s < inst.dst2.register_id + inst.dst2.size) ||
//			    inst.type == ShaderInstructionType::SEndpgm)
//			{
//				break;
//			}
//
//			if (inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip)
//			{
//				if (inst.src[1].register_id == s)
//				{
//					EXIT_NOT_IMPLEMENTED(found && !without_sampler);
//					without_sampler = true;
//					found           = true;
//				}
//			} else if (inst.type == ShaderInstructionType::ImageSample || inst.type == ShaderInstructionType::ImageLoad)
//			{
//				if (inst.src[1].register_id == s)
//				{
//					EXIT_NOT_IMPLEMENTED(found && without_sampler);
//					without_sampler = false;
//					found           = true;
//				}
//			}
//		}
//	};
//
//	if (bind->textures2D.textures_num > 0)
//	{
//		const auto& insts = code.GetInstructions();
//
//		for (int ti = 0; ti < bind->textures2D.textures_num; ti++)
//		{
//			bool found = false;
//			if (bind->textures2D.desc[ti].extended)
//			{
//				int s = bind->extended.start_register;
//
//				int index = 0;
//				for (const auto& inst: insts)
//				{
//					if ((inst.dst.type == ShaderOperandType::Sgpr && s >= inst.dst.register_id &&
//					     s < inst.dst.register_id + inst.dst.size) ||
//					    (inst.dst2.type == ShaderOperandType::Sgpr && s >= inst.dst2.register_id &&
//					     s < inst.dst2.register_id + inst.dst2.size) ||
//					    inst.type == ShaderInstructionType::SEndpgm)
//					{
//						break;
//					}
//
//					if (inst.type == ShaderInstructionType::SLoadDwordx8 && inst.src[0].register_id == s &&
//					    static_cast<int>(inst.src[1].constant.u >> 2u) + 16 == bind->textures2D.desc[ti].start_register)
//					{
//						find_image_op(index + 1, inst.dst.register_id, found, p.textures2d_without_sampler[ti]);
//					}
//
//					index++;
//				}
//			} else
//			{
//				find_image_op(0, bind->textures2D.desc[ti].start_register, found, p.textures2d_without_sampler[ti]);
//			}
//
//			EXIT_NOT_IMPLEMENTED(!found);
//
//			if (p.textures2d_without_sampler[ti])
//			{
//				p.textures2d_storage_num++;
//			} else
//			{
//				p.textures2d_sampled_num++;
//			}
//		}
//	}
//	return p;
//}
//
// ShaderBindParameters ShaderGetBindParametersVS(const ShaderCode& code, const ShaderVertexInputInfo* input_info)
//{
//	return ShaderUpdateBindInfo(code, &input_info->bind);
//}
//
// ShaderBindParameters ShaderGetBindParametersPS(const ShaderCode& code, const ShaderPixelInputInfo* input_info)
//{
//	return ShaderUpdateBindInfo(code, &input_info->bind);
//}
//
// ShaderBindParameters ShaderGetBindParametersCS(const ShaderCode& code, const ShaderComputeInputInfo* input_info)
//{
//	return ShaderUpdateBindInfo(code, &input_info->bind);
//}

static void ShaderGetBindIds(ShaderId* ret, const ShaderBindResources& bind)
{
	ret->ids.Add(bind.storage_buffers.buffers_num);

	for (int i = 0; i < bind.storage_buffers.buffers_num; i++)
	{
		// const auto& r = bind.storage_buffers.buffers[i];

		// ret->ids.Add(static_cast<uint32_t>(r.SwizzleEnabled()));
		// ret->ids.Add(r.DstSelX());
		// ret->ids.Add(r.DstSelY());
		// ret->ids.Add(r.DstSelZ());
		// ret->ids.Add(r.DstSelW());
		// ret->ids.Add(r.Nfmt());
		// ret->ids.Add(r.Dfmt());
		// ret->ids.Add(static_cast<uint32_t>(r.AddTid()));
		ret->ids.Add(bind.storage_buffers.slots[i]);
		ret->ids.Add(bind.storage_buffers.start_register[i]);
		ret->ids.Add(static_cast<uint32_t>(bind.storage_buffers.extended[i]));
		ret->ids.Add(static_cast<uint32_t>(bind.storage_buffers.usages[i]));
		ret->ids.Add(static_cast<uint32_t>(bind.storage_buffers.dynamic_sload[i]));
	}

	ret->ids.Add(bind.dynamic_sloads.mappings_num);
	for (int mapping = 0; mapping < bind.dynamic_sloads.mappings_num; ++mapping)
	{
		ret->ids.Add(static_cast<uint32_t>(bind.dynamic_sloads.kind[mapping]));
		ret->ids.Add(bind.dynamic_sloads.resource_index[mapping]);
		ret->ids.Add(bind.dynamic_sloads.destination_register[mapping]);
		ret->ids.Add(bind.dynamic_sloads.instruction_pc[mapping]);
		ret->ids.Add(static_cast<uint32_t>(bind.dynamic_sloads.offset_dw[mapping]));
		ret->ids.Add(static_cast<uint32_t>(bind.dynamic_sloads.dword_count[mapping]));
		ret->ids.Add(static_cast<uint32_t>(bind.dynamic_sloads.resource_field_offset[mapping]));
		ret->ids.Add(bind.dynamic_sloads.last_consumer_pc[mapping]);
		ret->ids.Add(static_cast<uint32_t>(bind.dynamic_sloads.raw_vmem_oob_guarded[mapping]));
	}

	ret->ids.Add(bind.zero_sbuffer_resources.buffers_num);
	for (int i = 0; i < bind.zero_sbuffer_resources.buffers_num; ++i)
	{
		ret->ids.Add(bind.zero_sbuffer_resources.start_register[i]);
	}

	ret->ids.Add(bind.textures2D.textures_num);

	for (int i = 0; i < bind.textures2D.textures_num; i++)
	{
		const auto& r = bind.textures2D.desc[i].texture;
		// ret->ids.Add(r.MinLod());
		// ret->ids.Add(r.Dfmt());
		// ret->ids.Add(r.Nfmt());
		// ret->ids.Add(r.Width());
		// ret->ids.Add(r.Height());
		// ret->ids.Add(r.PerfMod());
		// ret->ids.Add(static_cast<uint32_t>(r.Interlaced()));
		// ret->ids.Add(r.DstSelX());
		// ret->ids.Add(r.DstSelY());
		// ret->ids.Add(r.DstSelZ());
		// ret->ids.Add(r.DstSelW());
		// ret->ids.Add(r.BaseLevel());
		// ret->ids.Add(r.LastLevel());
		// ret->ids.Add(r.TilingIdx());
		// ret->ids.Add(static_cast<uint32_t>(r.Pow2Pad()));
		// Image type and format determine the SPIR-V image declaration. They
		// must participate in the module key so a pipeline cannot reuse a
		// shader specialized for a differently shaped or integer texture.
		ret->ids.Add(r.Type());
		ret->ids.Add(r.Format());
		// ret->ids.Add(r.Depth());
		// ret->ids.Add(r.Pitch());
		// ret->ids.Add(r.BaseArray());
		// ret->ids.Add(r.LastArray());
		// ret->ids.Add(r.MinLodWarn());
		// ret->ids.Add(r.CounterBankId());
		// ret->ids.Add(static_cast<uint32_t>(r.LodHdwCntEn()));
		ret->ids.Add(bind.textures2D.desc[i].slot);
		ret->ids.Add(bind.textures2D.desc[i].start_register);
		ret->ids.Add(static_cast<uint32_t>(bind.textures2D.desc[i].extended));
		ret->ids.Add(static_cast<uint32_t>(bind.textures2D.desc[i].dynamic_sload));
		ret->ids.Add(static_cast<uint32_t>(bind.textures2D.desc[i].usage));
		ret->ids.Add(static_cast<uint32_t>(ShaderResolvedSampledTextureShape(bind.textures2D.desc[i])));
		ret->ids.Add(static_cast<uint32_t>(bind.textures2D.desc[i].sample_operation));
	}

	ret->ids.Add(bind.samplers.samplers_num);

	for (int i = 0; i < bind.samplers.samplers_num; i++)
	{
		const auto& r = bind.samplers.samplers[i];

		// ret->ids.Add(r.ClampX());
		// ret->ids.Add(r.ClampY());
		// ret->ids.Add(r.ClampZ());
		// ret->ids.Add(r.MaxAnisoRatio());
		// ret->ids.Add(r.DepthCompareFunc());
		// ret->ids.Add(static_cast<uint32_t>(r.ForceUnormCoords()));
		// ret->ids.Add(r.AnisoThreshold());
		// ret->ids.Add(static_cast<uint32_t>(r.McCoordTrunc()));
		// ret->ids.Add(static_cast<uint32_t>(r.ForceDegamma()));
		// ret->ids.Add(r.AnisoBias());
		// ret->ids.Add(static_cast<uint32_t>(r.TruncCoord()));
		// ret->ids.Add(static_cast<uint32_t>(r.DisableCubeWrap()));
		// ret->ids.Add(r.FilterMode());
		// ret->ids.Add(r.MinLod());
		// ret->ids.Add(r.MaxLod());
		// ret->ids.Add(r.PerfMip());
		// ret->ids.Add(r.PerfZ());
		// ret->ids.Add(r.LodBias());
		// ret->ids.Add(r.LodBiasSec());
		// ret->ids.Add(r.XyMagFilter());
		// ret->ids.Add(r.XyMinFilter());
		// ret->ids.Add(r.ZFilter());
		// ret->ids.Add(r.MipFilter());
		// ret->ids.Add(r.BorderColorPtr());
		// ret->ids.Add(r.BorderColorType());
		ret->ids.Add(r.DepthCompareFunc());
		ret->ids.Add(static_cast<uint32_t>(r.ForceUnormCoords()));
		ret->ids.Add(bind.samplers.slots[i]);
		ret->ids.Add(bind.samplers.start_register[i]);
		ret->ids.Add(static_cast<uint32_t>(bind.samplers.extended[i]));
		ret->ids.Add(static_cast<uint32_t>(bind.samplers.dynamic_sload[i]));
		ret->ids.Add(static_cast<uint32_t>(bind.samplers.operations[i]));
	}

	ret->ids.Add(bind.gds_pointers.pointers_num);

	for (int i = 0; i < bind.gds_pointers.pointers_num; i++)
	{
		// const auto& r = bind.gds_pointers.pointers[i];

		ret->ids.Add(bind.gds_pointers.slots[i]);
		ret->ids.Add(bind.gds_pointers.start_register[i]);
		ret->ids.Add(static_cast<uint32_t>(bind.gds_pointers.extended[i]));
	}

	ret->ids.Add(bind.direct_sgprs.sgprs_num);

	for (int i = 0; i < bind.direct_sgprs.sgprs_num; i++)
	{
		ret->ids.Add(bind.direct_sgprs.start_register[i]);
	}

	ret->ids.Add(static_cast<uint32_t>(bind.extended.used));
	ret->ids.Add(bind.extended.slot);
	ret->ids.Add(bind.extended.start_register);
}

ShaderId ShaderGetIdVS(const HW::VertexShaderInfo* regs, const ShaderVertexInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION();

	ShaderId ret;

	if (regs->vs_embedded)
	{
		EXIT_IF(input_info != nullptr && input_info->clip_probe.enabled);
		ret.ids.Add(regs->vs_embedded_id);
		return ret;
	}

	ret.ids.Expand(64);

	bool gs_instead_of_vs =
	    (regs->vs_regs.data_addr == 0 && regs->gs_regs.data_addr == 0 && regs->es_regs.data_addr != 0 && regs->gs_regs.chksum != 0);
	uint64_t shader_addr = (gs_instead_of_vs ? regs->es_regs.data_addr : regs->vs_regs.data_addr);

	bool gen5 = Config::IsNextGen();

	if (gen5)
	{
		if (!gs_instead_of_vs) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !gs_instead_of_vs condition ignored (continuing)\n"); }

		ret.hash0 = (regs->gs_regs.chksum >> 32u) & 0xffffffffu;
		ret.crc32 = regs->gs_regs.chksum & 0xffffffffu;
	} else
	{
		const auto* src = reinterpret_cast<const uint32_t*>(shader_addr);

		if (src == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src == nullptr condition ignored (continuing)\n"); }

		const auto* header = GetBinaryInfo(src);

		if (header == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: header == nullptr condition ignored (continuing)\n"); }

		ret.hash0 = header->hash0;
		ret.crc32 = header->crc32;
		ret.ids.Add(header->length);
	}

	ret.ids.Add(static_cast<uint32_t>(input_info->fetch_external));
	ret.ids.Add(static_cast<uint32_t>(input_info->fetch_embedded));
	ret.ids.Add(static_cast<uint32_t>(input_info->fetch_inline));
	ret.ids.Add(static_cast<uint32_t>(input_info->gs_prolog));
	ret.ids.Add(input_info->float_mode);
	ret.ids.Add(static_cast<uint32_t>(input_info->dx10_clamp));
	ret.ids.Add(static_cast<uint32_t>(input_info->ieee_mode));
	ret.ids.Add(static_cast<uint32_t>(input_info->fetch_attrib_reg));
	ret.ids.Add(static_cast<uint32_t>(input_info->fetch_buffer_reg));
	ret.ids.Add(input_info->resources_num);
	ret.ids.Add(input_info->export_count);
	ret.ids.Add(static_cast<uint32_t>(input_info->fetch_attrib_data_num));
	for (int i = 0; i < input_info->fetch_attrib_data_num; i++)
	{
		ret.ids.Add(input_info->fetch_attrib_data[i]);
	}

	for (int i = 0; i < input_info->resources_num; i++)
	{
		const auto& r  = input_info->resources[i];
		const auto& rd = input_info->resources_dst[i];

		ret.ids.Add(rd.register_start);
		ret.ids.Add(rd.registers_num);
		ret.ids.Add(static_cast<uint32_t>(rd.semantic));
		ret.ids.Add(r.Stride());
		ret.ids.Add(static_cast<uint32_t>(r.SwizzleEnabled()));
		ret.ids.Add(r.DstSelX());
		ret.ids.Add(r.DstSelY());
		ret.ids.Add(r.DstSelZ());
		ret.ids.Add(r.DstSelW());
		if (gen5)
		{
			ret.ids.Add(r.Format());
			ret.ids.Add(r.OutOfBounds());
		} else
		{
			ret.ids.Add(r.Nfmt());
			ret.ids.Add(r.Dfmt());
		}
		ret.ids.Add(static_cast<uint32_t>(r.AddTid()));
	}

	ret.ids.Add(input_info->buffers_num);

	for (int i = 0; i < input_info->buffers_num; i++)
	{
		const auto& r = input_info->buffers[i];
		ret.ids.Add(r.attr_num);
		ret.ids.Add(r.stride);
		for (int j = 0; j < r.attr_num; j++)
		{
			ret.ids.Add(r.attr_indices[j]);
			ret.ids.Add(r.attr_offsets[j]);
		}
	}

	ShaderGetBindIds(&ret, input_info->bind);
	if (input_info->clip_probe.draw_scoped && input_info->clip_probe.enabled)
	{
		const uint32_t descriptor_set = input_info->clip_probe_descriptor_set;
		const uint64_t diagnostic_identity = VertexClipProbeDiagnosticIdentity(descriptor_set);
		EXIT_IF(descriptor_set == kVertexClipProbeInvalidDescriptorSet || descriptor_set > 2u || diagnostic_identity == 0 ||
		        input_info->clip_probe.diagnostic_identity != diagnostic_identity);
		ret.ids.Add(0x56435031u); // VCP1
		ret.ids.Add(static_cast<uint32_t>(diagnostic_identity));
		ret.ids.Add(static_cast<uint32_t>(diagnostic_identity >> 32u));
		ret.ids.Add(descriptor_set);
	}

	return ret;
}

ShaderId ShaderGetIdPS(const HW::PixelShaderInfo* regs, const ShaderPixelInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION();

	ShaderId ret;
	if (!input_info->stage_enabled)
	{
		ret.ids.Add(0x50534f46u);
		return ret;
	}

	if (regs->ps_embedded)
	{
		ret.ids.Add(regs->ps_embedded_id);
		return ret;
	}

	ret.ids.Expand(64);

	if (Config::IsNextGen())
	{
		ret.hash0 = (regs->ps_regs.chksum >> 32u) & 0xffffffffu;
		ret.crc32 = regs->ps_regs.chksum & 0xffffffffu;
	} else
	{
		const auto* src = reinterpret_cast<const uint32_t*>(regs->ps_regs.data_addr);

		if (src == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src == nullptr condition ignored (continuing)\n"); }

		const auto* header = GetBinaryInfo(src);

		if (header == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: header == nullptr condition ignored (continuing)\n"); }

		ret.hash0 = header->hash0;
		ret.crc32 = header->crc32;

		ret.ids.Add(header->length);
	}

	ret.ids.Add(input_info->input_num);
	ret.ids.Add(input_info->system_input_enable);
	ret.ids.Add(input_info->system_input_address);
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_pos_xy));
	ret.ids.Add(input_info->host_to_guest_scale.x_guest_numerator);
	ret.ids.Add(input_info->host_to_guest_scale.x_host_denominator);
	ret.ids.Add(input_info->host_to_guest_scale.y_guest_numerator);
	ret.ids.Add(input_info->host_to_guest_scale.y_host_denominator);
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_pixel_kill_enable));
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_early_z));
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_execute_on_noop));
	ret.ids.Add(input_info->float_mode);
	ret.ids.Add(static_cast<uint32_t>(input_info->dx10_clamp));
	ret.ids.Add(static_cast<uint32_t>(input_info->ieee_mode));

	// The export declarations and component order are part of the generated
	// SPIR-V interface. They must distinguish pipelines that use the same guest
	// shader with different render-target formats or COMP_SWAP values.
	for (int i = 0; i < 8; i++)
	{
		ret.ids.Add(input_info->target_output_mode[i]);
		ret.ids.Add(input_info->target_output_order[i]);
	}

	for (uint32_t i = 0; i < 32u; i++)
	{
		ret.ids.Add(input_info->interpolator_settings[i]);
	}

	ShaderGetBindIds(&ret, input_info->bind);
	if (input_info->fragment_tap.draw_scoped && input_info->fragment_tap.enabled)
	{
		ret.ids.Add(0x46535444u); // FSTD: fragment tap selected draw variant.
	}
	if (input_info->input0_probe.draw_scoped && input_info->input0_probe.enabled)
	{
		const uint32_t descriptor_set = input_info->input0_probe_descriptor_set;
		const bool sample_result = input_info->input0_probe.kind == ShaderPixelProbeKind::SampleResult;
		const bool mrt_result    = input_info->input0_probe.kind == ShaderPixelProbeKind::FinalMrtResult;
		const uint64_t diagnostic_identity = sample_result
		                                         ? PixelSampleProbeDiagnosticIdentity(
		                                               descriptor_set, input_info->input0_probe.sample_ordinal,
		                                               input_info->input0_probe.sparse_subgroup)
		                                     : mrt_result
		                                         ? PixelMrtProbeDiagnosticIdentity(descriptor_set,
		                                                                           input_info->input0_probe.mrt_target,
		                                                                           input_info->input0_probe.export_ordinal)
		                                         : VertexClipProbeDiagnosticIdentity(descriptor_set);
		EXIT_IF(descriptor_set == kVertexClipProbeInvalidDescriptorSet || descriptor_set > 2u || diagnostic_identity == 0 ||
		        input_info->input0_probe.diagnostic_identity != diagnostic_identity);
		ret.ids.Add(sample_result ? 0x50535031u : (mrt_result ? 0x504d5231u : 0x50493031u)); // PSP1 / PMR1 / PI01.
		ret.ids.Add(static_cast<uint32_t>(diagnostic_identity));
		ret.ids.Add(static_cast<uint32_t>(diagnostic_identity >> 32u));
		ret.ids.Add(descriptor_set);
		if (sample_result)
		{
			ret.ids.Add(input_info->input0_probe.sample_ordinal);
			ret.ids.Add(input_info->input0_probe.sparse_subgroup ? 1u : 0u);
		}
		if (mrt_result)
		{
			ret.ids.Add(input_info->input0_probe.mrt_target);
			ret.ids.Add(input_info->input0_probe.export_ordinal);
		}
	}

	return ret;
}

bool ShaderPixelMrtProbeMatchesInstruction(const ShaderCode& code, const ShaderPixelInputInfo& input_info,
	                                         const ShaderPixelInput0ProbeConfig& config)
{
	if (!config.enabled || config.kind != ShaderPixelProbeKind::FinalMrtResult || config.mrt_target > 3u ||
	    code.GetType() != ShaderType::Pixel || config.export_ordinal >= code.GetInstructions().Size())
	{
		return false;
	}
	const auto& inst = code.GetInstructions().At(config.export_ordinal);
	if (inst.type != ShaderInstructionType::Exp || inst.exp_enable_mask == 0u)
	{
		return false;
	}
	uint32_t target = 4u;
	using namespace ShaderInstructionFormat;
	switch (inst.format)
	{
		case Mrt0Vsrc0Vsrc1ComprVmDone:
		case Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone: target = 0u; break;
		case Mrt1Vsrc0Vsrc1ComprVm:
		case Mrt1Vsrc0Vsrc1Vsrc2Vsrc3Vm: target = 1u; break;
		case Mrt2Vsrc0Vsrc1ComprVm:
		case Mrt2Vsrc0Vsrc1Vsrc2Vsrc3Vm: target = 2u; break;
		case Mrt3Vsrc0Vsrc1ComprVm:
		case Mrt3Vsrc0Vsrc1Vsrc2Vsrc3Vm: target = 3u; break;
		default: return false;
	}
	return target == config.mrt_target && input_info.target_output_mode[target] != 0u;
}

bool ShaderPixelSampleProbeMatchesInstruction(const ShaderCode& code, const ShaderPixelInput0ProbeConfig& config)
{
	if (!config.enabled || config.kind != ShaderPixelProbeKind::SampleResult || code.GetType() != ShaderType::Pixel)
	{
		return false;
	}
	return config.sample_ordinal < code.GetInstructions().Size() &&
	       code.GetInstructions().At(config.sample_ordinal).type == ShaderInstructionType::ImageSampleB;
}

ShaderFragmentTapConfig ShaderResolveFragmentTapConfig(uint64_t code_id, bool indexed, uint32_t guest_count)
{
	const char* tap_selector = std::getenv("KYTY_FS_TAP");
	if (tap_selector == nullptr || tap_selector[0] == '\0')
	{
		return {};
	}
	char*          shader_end = nullptr;
	const uint64_t shader_id  = std::strtoull(tap_selector, &shader_end, 16);
	if (shader_end == tap_selector || *shader_end != ':' || shader_id != code_id)
	{
		return {};
	}

	const bool  select_ordinal = shader_end[1] == '@';
	const char* selector_value = shader_end + (select_ordinal ? 2 : 1);
	char*       selector_end   = nullptr;
	const uint64_t selector    = std::strtoull(selector_value, &selector_end, 0);
	if (selector_end == selector_value || *selector_end != '\0' || selector > UINT32_MAX)
	{
		return {};
	}
	const char* signed_mode = std::getenv("KYTY_FS_TAP_SIGNED");
	const bool  signed_tap  = signed_mode != nullptr && signed_mode[0] == '1' && signed_mode[1] == '\0';
	const char* lod_mode    = std::getenv("KYTY_FS_TAP_LOD");
	const bool  lod_tap     = lod_mode != nullptr && lod_mode[0] == '1' && lod_mode[1] == '\0';
	// Keep the mode bit plus an encoding-revision bit so persisted modules from
	// the superseded numeric visualization cannot satisfy the threshold tap.
	const uint64_t lod_identity = lod_tap ? 0x1800000000000000ull : 0ull;

	ShaderFragmentTapConfig result;
	result.enabled              = true;
	result.select_ordinal       = select_ordinal;
	result.signed_visualization = signed_tap;
	result.query_lod_visualization = lod_tap;
	result.selector             = static_cast<uint32_t>(selector);
	result.diagnostic_identity  = 0x8000000000000000ull | (select_ordinal ? 0x4000000000000000ull : 0ull) |
	                              (signed_tap ? 0x2000000000000000ull : 0ull) | lod_identity | selector;

	const char* draw_selector = std::getenv("KYTY_FS_TAP_DRAW");
	if (draw_selector == nullptr || draw_selector[0] == '\0')
	{
		return result;
	}
	result.draw_scoped         = true;
	result.enabled             = false;
	result.diagnostic_identity = 0;
	const char* kind = indexed ? "indexed:" : "auto:";
	const size_t kind_size = std::strlen(kind);
	if (std::strncmp(draw_selector, kind, kind_size) != 0)
	{
		return result;
	}
	char*          count_end = nullptr;
	const uint64_t count     = std::strtoull(draw_selector + kind_size, &count_end, 0);
	if (count_end == draw_selector + kind_size || *count_end != '\0' || count > UINT32_MAX)
	{
		return result;
	}
	result.enabled = static_cast<uint32_t>(count) == guest_count;
	if (result.enabled)
	{
		result.diagnostic_identity = 0x8000000000000000ull | (select_ordinal ? 0x4000000000000000ull : 0ull) |
		                             (signed_tap ? 0x2000000000000000ull : 0ull) | lod_identity | selector;
	}
	return result;
}

ShaderId ShaderGetIdCS(const HW::ComputeShaderInfo* regs, const ShaderComputeInputInfo* input_info)
{
	const auto* src = reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr);

	if (src == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src == nullptr condition ignored (continuing)\n"); }

	ShaderId ret;
	ret.ids.Expand(64);

	if (Config::IsNextGen())
	{
		ret.hash0 = (regs->cs_regs.chksum >> 32u) & 0xffffffffu;
		ret.crc32 = regs->cs_regs.chksum & 0xffffffffu;
	} else
	{
		const auto* header = GetBinaryInfo(src);

		if (header == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: header == nullptr condition ignored (continuing)\n"); }

		ret.hash0 = header->hash0;
		ret.crc32 = header->crc32;

		ret.ids.Add(header->length);
	}

	ret.ids.Add(input_info->workgroup_register);
	ret.ids.Add(input_info->thread_ids_num);
	ret.ids.Add(input_info->lds_dwords);

	for (int i = 0; i < 3; i++)
	{
		ret.ids.Add(input_info->threads_num[i]);
		ret.ids.Add(static_cast<uint32_t>(input_info->group_id[i]));
	}

	ShaderGetBindIds(&ret, input_info->bind);

	return ret;
}

bool ShaderIsDisabled(uint64_t addr)
{
	if (addr == 0)
	{
		return false;
	}

	const auto* src = reinterpret_cast<const uint32_t*>(addr);
	if (src == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src == nullptr condition ignored (continuing)\n"); }

	const auto* header = GetBinaryInfo(src);
	if (header == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: header == nullptr condition ignored (continuing)\n"); }

	auto id = (static_cast<uint64_t>(header->hash0) << 32u) | header->crc32;

	bool disabled = (g_disabled_shaders != nullptr && g_disabled_shaders->Contains(id));

	KYTY_LOG_DEBUG("Shader 0x%016" PRIx64 ": id = 0x%016" PRIx64 " - %s\n", addr, id, (disabled ? "disabled" : "enabled"));

	return disabled;
}

bool ShaderIsDisabled2(uint64_t addr, uint64_t chksum)
{
	bool disabled = (g_disabled_shaders != nullptr && g_disabled_shaders->Contains(chksum));

	KYTY_LOG_DEBUG("Shader 0x%016" PRIx64 ": id = 0x%016" PRIx64 " - %s\n", addr, chksum, (disabled ? "disabled" : "enabled"));

	return disabled;
}

void ShaderDisable(uint64_t id)
{
	if (g_disabled_shaders == nullptr)
	{
		g_disabled_shaders = new Vector<uint64_t>;
	}

	if (!g_disabled_shaders->Contains(id))
	{
		g_disabled_shaders->Add(id);
	}
}

void ShaderInjectDebugPrintf(uint64_t id, const ShaderDebugPrintf& cmd)
{
	if (g_debug_printfs == nullptr)
	{
		g_debug_printfs = new Vector<ShaderDebugPrintfCmds>;
	}

	for (auto& c: *g_debug_printfs)
	{
		if (c.id == id)
		{
			c.cmds.Add(cmd);
			return;
		}
	}

	ShaderDebugPrintfCmds c;
	c.id = id;
	c.cmds.Add(cmd);

	g_debug_printfs->Add(c);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
