#include "Kyty/Core/MagicEnum.h"
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
#include "Emulator/Graphics/DiagnosticDump.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/GraphicsGeState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveControlFlowAnalysis.h"
#include "Emulator/Graphics/ShaderScalarLiveness.h"
#include "Emulator/Graphics/ShaderComputeWaveNativeEquivalence.h"
#include "Emulator/Graphics/ShaderFragmentMaskFlow.h"
#include "Emulator/Graphics/ShaderNggFront.h"
#include "ShaderNativeWaveInternal.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderProgramSnapshot.h"
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
#include <array>
#include <atomic>
#include <climits>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

// #define SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
// #include "spirv_cross/spirv_glsl.hpp"

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

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

State::ImageSampleOperation ShaderTextureSampleOperation(const ShaderTextureResource& texture, State::ImageSampleOperation operation)
{
	if (Config::IsNextGen() && operation == State::ImageSampleOperation::DepthReference &&
	    (texture.TileMode() != 24u || State::Gen5DepthSampleBytesPerElement(texture.Format()) == 0u))
	{
		return State::ImageSampleOperation::Regular;
	}
	return operation;
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
	    (shape == ShaderGen5SampledTextureShape::TwoDimensionalArray &&
	     (resolved_view == ShaderSampledImageViewKind::Color2DArray || resolved_view == ShaderSampledImageViewKind::Depth2DArray));
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

bool ShaderInstructionTypeChangesExec(ShaderInstructionType type)
{
	if (type >= ShaderInstructionType::SAndSaveexecB32 && type <= ShaderInstructionType::SOrn1SaveexecB32)
	{
		return true;
	}
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
		case ShaderInstructionType::VLdexpF32:
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
		case ShaderInstructionType::VOr3B32:
		case ShaderInstructionType::VSubF32:
		case ShaderInstructionType::VSubI32:
		case ShaderInstructionType::VSubrevF32:
		case ShaderInstructionType::VSubrevI32:
		case ShaderInstructionType::VXnorB32:
		case ShaderInstructionType::VXorB32: return true;
		default: return false;
	}
}

bool ShaderInstructionWritesExec(const ShaderInstruction& inst)
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

bool ShaderInstructionIsControlFlowBoundary(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::SBranch:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchExecnz:
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

bool ShaderNativeMaskOperand(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::ExecLo || operand.type == ShaderOperandType::ExecHi ||
	       operand.type == ShaderOperandType::ExecZ || operand.type == ShaderOperandType::VccLo ||
	       operand.type == ShaderOperandType::VccHi || operand.type == ShaderOperandType::VccZ;
}

bool ShaderNativePackedResult(const ShaderInstruction& inst)
{
	// Include compares to arbitrary SGPR pairs and all compare widths, not only
	// the original VCC enum interval. Carry destinations likewise need packing.
	return ShaderInstructionTypeStartsWith(inst.type, "VCmp") ||
	       inst.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1 ||
	       inst.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2 ||
	       inst.format == ShaderInstructionFormat::Vdst2Sdst2Vsrc0Vsrc1Vsrc2Pair;
}

bool ShaderUsesNativeWaveState(const ShaderCode& code)
{
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.src_num < 0 || inst.src_num > 4 || inst.mimg_address_num < 0 ||
		    inst.mimg_address_num > static_cast<int>(std::size(inst.mimg_address))) { return true; }
		if (ShaderNativeMaskOperand(inst.dst) || ShaderNativeMaskOperand(inst.dst2) || ShaderNativePackedResult(inst) ||
		    ShaderInstructionTypeChangesExec(inst.type) || inst.type == ShaderInstructionType::VCndmaskB32 ||
		    inst.type == ShaderInstructionType::VMbcntLoU32B32 || inst.type == ShaderInstructionType::VMbcntHiU32B32 ||
		    inst.type == ShaderInstructionType::SCbranchExecz || inst.type == ShaderInstructionType::SCbranchExecnz ||
		    inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz ||
		    inst.type == ShaderInstructionType::DsAppend || inst.type == ShaderInstructionType::DsConsume)
		{
			return true;
		}
		for (int source = 0; source < inst.src_num; ++source)
		{
			if (ShaderNativeMaskOperand(inst.src[source]) || inst.src[source].dpp) { return true; }
		}
		for (int source = 0; source < inst.mimg_address_num; ++source)
		{
			if (ShaderNativeMaskOperand(inst.mimg_address[source])) { return true; }
		}
	}
	return UsesNativeLaneExchange(code);
}

// A separate whole-program mask-use proof. The neutral-region analyzer proves
// lane values, not that a copied EXEC/VCC word never escapes as numeric data.
// Taint is monotone: unknown CFG joins and later partial overwrites cannot erase
// evidence of a mask. This intentionally trades precision for a bounded proof.
static bool ShaderMasksStayLaneLocal(const ShaderCode& code, const std::vector<bool>& regions, uint32_t* refused_pc)
{
	std::bitset<128> mask_sgprs;
	bool mask_scc = false;
	const auto mask_source = [&mask_sgprs](const ShaderOperand& operand)
	{
		if (ShaderNativeMaskOperand(operand)) { return true; }
		if (operand.type == ShaderOperandType::Sgpr)
		{
			for (int word = 0; word < operand.size; ++word)
			{
				const int reg = operand.register_id + word;
				if (reg < 0 || reg >= 128 || mask_sgprs[static_cast<size_t>(reg)]) { return true; }
			}
		}
		return false;
	};
	const auto mark = [&mask_sgprs](const ShaderOperand& operand)
	{
		if (operand.type != ShaderOperandType::Sgpr) { return; }
		for (int word = 0; word < operand.size; ++word)
		{
			const int reg = operand.register_id + word;
			if (reg >= 0 && reg < 128) { mask_sgprs.set(static_cast<size_t>(reg)); }
		}
	};
	// Solve mask provenance to a fixed point before checking uses. A back edge
	// can consume an SGPR before its textual definition; one forward pass is not
	// a proof. No kill sets means each pass adds a bit or terminates (128 bits).
	for (uint32_t pass = 0; pass <= 128u; ++pass)
	{
		const auto before = mask_sgprs;
		for (const auto& inst: code.GetInstructions())
		{
			bool masked = ShaderNativePackedResult(inst) || ShaderInstructionTypeChangesExec(inst.type);
			for (int source = 0; source < inst.src_num; ++source) { masked = masked || mask_source(inst.src[source]); }
			if (masked) { mark(inst.dst); mark(inst.dst2); }
		}
		if (mask_sgprs == before) { break; }
	}
	// SCC is implicit on many scalar instructions. A mask-derived definition
	// anywhere in a loop also taints a preceding textual SCC consumer.
	for (const auto& inst: code.GetInstructions())
	{
		if (!ShaderInstructionTypeStartsWith(inst.type, "S") || inst.type == ShaderInstructionType::SMovB32 ||
		    inst.type == ShaderInstructionType::SMovB64) { continue; }
		mask_scc = mask_scc || ShaderInstructionTypeChangesExec(inst.type);
		for (int source = 0; source < inst.src_num; ++source) { mask_scc = mask_scc || mask_source(inst.src[source]); }
	}
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		*refused_pc = inst.pc;
		const bool copy = inst.type == ShaderInstructionType::SMovB32 || inst.type == ShaderInstructionType::SMovB64;
		const bool bits = inst.type == ShaderInstructionType::SAndB32 || inst.type == ShaderInstructionType::SAndB64 ||
		                  inst.type == ShaderInstructionType::SOrB32 || inst.type == ShaderInstructionType::SOrB64 ||
		                  inst.type == ShaderInstructionType::SXorB32 || inst.type == ShaderInstructionType::SXorB64 ||
		                  inst.type == ShaderInstructionType::SWqmB32 || inst.type == ShaderInstructionType::SWqmB64;
		const bool region = inst.type == ShaderInstructionType::SOrn2SaveexecB64 && regions[index];
		if (inst.type == ShaderInstructionType::SCbranchExecz || inst.type == ShaderInstructionType::SCbranchExecnz ||
		    inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz ||
		    (mask_scc && (inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchScc1 ||
		                  inst.type == ShaderInstructionType::SCselectB32 || inst.type == ShaderInstructionType::SCselectB64 ||
		                  inst.type == ShaderInstructionType::SCmovB32 || inst.type == ShaderInstructionType::SCmovB64 ||
		                  inst.type == ShaderInstructionType::SAddcU32)))
		{
			return false;
		}
		bool reads_mask = region;
		for (int address = 0; address < inst.mimg_address_num; ++address)
		{
			if (mask_source(inst.mimg_address[address])) { return false; }
		}
		for (int source = 0; source < inst.src_num; ++source)
		{
			const auto& operand = inst.src[source];
			if (operand.type == ShaderOperandType::ExecZ || operand.type == ShaderOperandType::VccZ ||
			    (mask_scc && operand.type == ShaderOperandType::Scc)) { return false; }
			if (!mask_source(operand)) { continue; }
			const bool lane_bit = (inst.type == ShaderInstructionType::VCndmaskB32 && source == 2) ||
			                      (inst.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2 && source == 2);
			if (lane_bit) { continue; }
			if (!copy && !bits && !region) { return false; }
			reads_mask = true;
		}
		if (reads_mask)
		{
			if (inst.dst.type != ShaderOperandType::Sgpr && !ShaderNativeMaskOperand(inst.dst)) { return false; }
			mark(inst.dst);
			mask_scc = mask_scc || bits || region;
		}
		if (ShaderNativePackedResult(inst))
		{
			mark(inst.dst);
			mark(inst.dst2);
		}
	}
	return true;
}

std::string ShaderDumpGuestProgram(const char* dump_dir, const char* kind, uint64_t id, uint64_t program_addr, const ShaderCode& code)
{
	char stem[40];
	std::snprintf(stem, sizeof(stem), "%s_%016" PRIx64, kind, id);
	auto& writer = DiagnosticDumpProcessWriter();

	std::string listing;
	uint32_t    program_bytes = 0;
	for (const auto& inst: code.GetInstructions())
	{
		if (listing.size() <= kDiagnosticDumpFileBytesMax)
		{
			char prefix[16];
			std::snprintf(prefix, sizeof(prefix), "0x%08x ", inst.pc);
			listing += prefix;
			listing += ShaderCode::DbgInstructionToStr(inst).c_str();
			listing += '\n';
		}
		program_bytes = std::max(program_bytes, inst.pc + 8u);
	}
	const auto text_status = writer.Write(dump_dir, (std::string(stem) + ".txt").c_str(), listing.data(), listing.size());

	if (code.GetInstructions().IsEmpty())
	{
		return std::string(".txt=") + DiagnosticDumpStatusName(text_status) + " .bin=empty";
	}
	// The byte count is guest-derived: read one byte past the limit so the writer
	// reports Truncated instead of allocating or writing an unbounded range.
	std::vector<uint8_t> bytes;
	const uint64_t       wanted = std::min<uint64_t>(program_bytes, kDiagnosticDumpFileBytesMax + 1u);
	const bool           read   = Core::VirtualMemory::VisitReadableGuestRange(
        program_addr, wanted,
        [](const void* source, uint64_t size, void* opaque)
        {
            auto* out = static_cast<std::vector<uint8_t>*>(opaque);
            if (source != nullptr)
            {
                const auto* begin = static_cast<const uint8_t*>(source);
                out->insert(out->end(), begin, begin + size);
            }
            return true;
        },
        &bytes);
	const char* bin_status = read ? DiagnosticDumpStatusName(writer.Write(dump_dir, (std::string(stem) + ".bin").c_str(), bytes.data(), bytes.size()))
	                             : "guest_range_unreadable";
	return std::string(".txt=") + DiagnosticDumpStatusName(text_status) + " .bin=" + bin_status;
}

ShaderNativeWaveInfo ShaderNativeWaveVerdict::Get(const ShaderCode& code, uint32_t guest_wave_size) const
{
	if (guest_wave_size != 32u && guest_wave_size != 64u) { return ShaderAnalyzeNativeWave(code, guest_wave_size); }
	auto& slot = guest_wave_size == 32u ? m_wave32 : m_wave64;
	std::call_once(slot.once, [&] { slot.info = ShaderAnalyzeNativeWave(code, guest_wave_size); });
	return slot.info;
}

// Native pixel lowering keeps EXEC and VCC as packed words that every invocation of the subgroup holds
// alike (helpers receive their quad's real value), and every scalar is computed alike from them and from
// uniform inputs. A direct guest branch is therefore uniform host control flow and moves whole quads.
// Only an export, which kills each invocation whose own EXEC bit is clear, or an indirect jump, which is
// not lowered to that structured flow, can leave a quad without one of its members.
static bool ShaderFragmentQuadMayLoseMember(const ShaderInstruction& inst)
{
	return inst.type == ShaderInstructionType::Exp || inst.type == ShaderInstructionType::SSetpcB64 ||
	       inst.type == ShaderInstructionType::SSwappcB64;
}

ShaderNativeWaveInfo ShaderAnalyzeNativeWave(const ShaderCode& code, uint32_t guest_wave_size)
{
	ShaderNativeWaveInfo info;
	info.guest_wave_size = guest_wave_size;
	if (guest_wave_size != 32u && guest_wave_size != 64u)
	{
		info.refusal_reason = "guest wave width is not established by stage control";
		return info;
	}
	for (const auto& inst: code.GetInstructions())
	{
		if (!ShaderInstructionLoweringPreconditions(inst))
		{
			info.refusal_pc = inst.pc;
			info.refusal_reason = "invalid instruction before native wave analysis";
			return info;
		}
	}
	info.proof = ShaderUsesNativeWaveState(code) ? ShaderNativeWaveProof::ExactSubgroup : ShaderNativeWaveProof::LaneLocal;
	if (code.GetType() != ShaderType::Pixel) { return info; }

	std::vector<bool> regions(code.GetInstructions().Size(), false);
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		if (code.GetInstructions().At(index).type != ShaderInstructionType::SOrn2SaveexecB64 ||
		    !ShaderFragmentWaveInsideRegion(code, index)) { continue; }
		// The existing closed-region proof guarantees the first subsequent EXEC
		// writer is its exact restore; no copy of that proof is reimplemented here.
		regions[index] = true;
		while (++index < code.GetInstructions().Size())
		{
			regions[index] = true;
			if (ShaderInstructionWritesExec(code.GetInstructions().At(index))) { break; }
		}
	}
	uint32_t mask_pc = 0;
	// The whole-program monotone proof keeps every verdict it already gave. The flow-sensitive
	// proof runs on every program: besides rescuing what the monotone one refuses, it is what
	// accounts for EXEC writes (a WQM widening and its restore) that the monotone one cannot.
	const bool monotone_masks = ShaderMasksStayLaneLocal(code, regions, &mask_pc);
	const auto mask_flow      = ShaderAnalyzeFragmentMaskFlow(code);
	const bool local_masks    = monotone_masks || mask_flow.lane_local;
	bool has_region = false;
	bool writes_exec_outside_region = false;
	bool cross_lane = UsesNativeLaneExchange(code);
	bool quad_local = false;
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		const bool region = regions[index];
		has_region = has_region || region;
		const bool closed_wrapper  = mask_flow.lane_local && index < mask_flow.closed_exec_write.size() && mask_flow.closed_exec_write[index];
		writes_exec_outside_region = writes_exec_outside_region || (ShaderInstructionWritesExec(inst) && !region && !closed_wrapper);
		cross_lane = cross_lane || inst.type == ShaderInstructionType::VMbcntLoU32B32 ||
		             inst.type == ShaderInstructionType::VMbcntHiU32B32 || inst.type == ShaderInstructionType::DsAppend ||
		             inst.type == ShaderInstructionType::DsConsume;
		for (int source = 0; source < inst.src_num; ++source)
		{
			cross_lane = cross_lane || (inst.src[source].dpp && inst.src[source].dpp_ctrl > 0xffu);
			quad_local = quad_local || inst.src[source].dpp;
		}
	}
	if (local_masks && !cross_lane && !writes_exec_outside_region)
	{
		// Packed compare/carry results consumed only as this invocation's bit do
		// not observe missing upper lanes or all-helper quads. No EXEC widening.
		// Quad DPP has no dependence outside its complete four-invocation quad.
		info.proof = quad_local ? ShaderNativeWaveProof::QuadLocal : ShaderNativeWaveProof::LaneLocal;
	} else if (guest_wave_size == 64u && local_masks && has_region && !writes_exec_outside_region &&
	           ShaderAnalyzeFragmentNativeWaveTier(code).supported)
	{
		info.proof = ShaderNativeWaveProof::FragmentNeutral32;
	}
	if (!local_masks)
	{
		// The flow proof names the construct that broke it; the whole-program pass only knows
		// where it gave up on the first register it could not tell apart.
		info.refusal_pc     = mask_flow.located ? mask_flow.refused_pc : mask_pc;
		info.refusal_reason = mask_flow.located ? mask_flow.reason
		                                        : "fragment numeric mask observation lacks an all-helper/unavailable-quad proof";
		return info;
	}

	bool initial_exec = true;
	bool intact_participation = true;
	bool whole_quads = true;
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		const char* refusal = nullptr;
		if (inst.type == ShaderInstructionType::DsAppend || inst.type == ShaderInstructionType::DsConsume)
		{
			refusal = "fragment wave append/consume lacks a participating-lane proof";
		}
		// A packed compare is one bit per lane. When the mask flow proved that every use of
		// such a word is that lane's own bit, missing or non-participating neighbours
		// cannot change it.
		if (!whole_quads && ShaderNativePackedResult(inst) && !mask_flow.lane_local)
		{
			refusal = "fragment mask packing after an export or indirect jump lacks a quad participation proof";
		}
		if (inst.type == ShaderInstructionType::VReadfirstlaneB32 && !ShaderReadfirstlaneCanUseUniformCopy(code, index) &&
		    (!initial_exec || !intact_participation))
		{
			refusal = "READFIRSTLANE target quad is not proven to retain a participating real member";
		}
		if (inst.type == ShaderInstructionType::VReadlaneB32)
		{
			int reg = 0;
			int lane = 0;
			if (!IsStaticScalarSpillRead(inst, &reg, &lane) || !HasLiveScalarSpill(code, index, reg, lane))
			{
				refusal = "READLANE physical source may be unavailable or in an all-helper quad";
			}
		}
		if (inst.type == ShaderInstructionType::VPermlane16B32 || inst.type == ShaderInstructionType::VPermlanex16B32)
		{
			refusal = "fragment permutation source participation is unproven";
		}
		for (int source = 0; source < inst.src_num; ++source)
		{
			if (inst.src[source].dpp && !whole_quads)
			{
				refusal = "fragment DPP after an export or indirect jump lacks a quad participation proof";
			}
			if (inst.src[source].dpp && inst.src[source].dpp_ctrl > 0xffu)
			{
				refusal = "fragment row DPP source participation is unproven (neutral values do not prove helper recovery)";
			}
		}
		if (refusal != nullptr)
		{
			info.refusal_pc = inst.pc;
			info.refusal_reason = refusal;
			return info;
		}
		initial_exec = initial_exec && !ShaderInstructionWritesExec(inst);
		// Guest branches are translated into host control flow; without a CFG
		// participation proof do not claim the target quad still has a real peer.
		intact_participation = intact_participation && !ShaderInstructionIsControlFlowBoundary(inst) &&
		                       inst.type != ShaderInstructionType::Exp;
		whole_quads = whole_quads && !ShaderFragmentQuadMayLoseMember(inst);
	}
	return info;
}

uint32_t ShaderVertexGuestWaveSize(const GraphicsGeRawRegister& stages, bool next_gen, bool gs_front)
{
	if (!next_gen) { return 64u; } // GCN has fixed architectural Wave64.
	const auto decoded = GraphicsDecodeGeStages(stages, GraphicsGeGeneration::Gfx103);
	if (!stages.known || decoded.raw_unknown_bits != 0) { return 0; }
	if (gs_front && (decoded.kind == GraphicsGeStageKind::MergedEsGs || decoded.kind == GraphicsGeStageKind::NggPassthrough))
	{
		return decoded.gs_w32_en ? 32u : 64u;
	}
	return !gs_front && decoded.kind == GraphicsGeStageKind::LegacyVs ? (decoded.vs_w32_en ? 32u : 64u) : 0u;
}











static Vector<uint64_t>*                               g_disabled_shaders = nullptr;
static Vector<ShaderDebugPrintfCmds>*                  g_debug_printfs    = nullptr;
static std::unordered_map<uint64_t, ShaderMappedData>* g_shader_map       = nullptr;
static std::mutex                                      g_shader_map_mutex;
static std::shared_mutex                               g_shader_lifetime_mutex;
using ShaderGen5EudSnapshotTestHook = void (*)(void*);
static std::atomic<ShaderGen5EudSnapshotTestHook> g_shader_gen5_eud_snapshot_test_hook {nullptr};
static std::mutex                                 g_shader_gen5_eud_snapshot_test_hook_mutex;
static void*                                      g_shader_gen5_eud_snapshot_test_hook_context = nullptr;
struct VertexOffsetCacheEntry
{
	uint32_t hash0  = 0;
	uint32_t crc32  = 0;
	int32_t  offset = -1;
};
static std::unordered_map<uint64_t, VertexOffsetCacheEntry>* g_vertex_offset_sgpr_map = nullptr;
static std::mutex                                      g_vertex_offset_sgpr_mutex;
static std::unordered_map<uint64_t, uint64_t>*         g_shader_continuations  = nullptr;
struct ShaderVertexProgramKey
{
	uint64_t front_addr        = 0;
	uint64_t continuation_addr = 0;
	uint64_t bound_back_addr   = 0;
	uint64_t checksum          = 0;
	uint64_t generation        = 0;
	bool     gs_front          = false;

	bool operator==(const ShaderVertexProgramKey& other) const
	{
		return front_addr == other.front_addr && continuation_addr == other.continuation_addr &&
		       bound_back_addr == other.bound_back_addr && checksum == other.checksum && generation == other.generation &&
		       gs_front == other.gs_front;
	}
};

struct ShaderVertexProgram
{
	ShaderVertexProgramKey key;
	ShaderCode             front_code;
	ShaderCode             code;
	std::array<uint32_t, 4> fingerprint {};
	// Native-wave classification of `code`, computed once per guest width.
	ShaderNativeWaveVerdict native_wave;
	// Width-neutrality of the fused NGG front, proved once for this immutable code.
	ShaderNggFrontVerdict  ngg_front;
};

struct ShaderVertexProgramKeyHash
{
	size_t operator()(const ShaderVertexProgramKey& key) const
	{
		size_t hash = 0;
		for (uint64_t value: {key.front_addr, key.continuation_addr, key.bound_back_addr, key.checksum, key.generation,
		                     static_cast<uint64_t>(key.gs_front)})
		{
			hash ^= std::hash<uint64_t> {}(value) + static_cast<size_t>(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
		}
		return hash;
	}
};

using ShaderVertexProgramCache =
    std::unordered_map<ShaderVertexProgramKey, std::shared_ptr<const ShaderVertexProgram>, ShaderVertexProgramKeyHash>;
static constexpr size_t   kVertexProgramCacheEntries = 256u;
static constexpr uint64_t kVertexProgramCacheBytes   = 64u * 1024u * 1024u;
static ShaderVertexProgramCache* g_vs_isa_cache = nullptr;
static std::mutex                g_vs_isa_cache_mutex;
static uint64_t                  g_vs_isa_cache_bytes = 0;
static uint64_t                  g_vs_program_generation = 1;

// Caller holds the exclusive lifetime lock. Lock order is lifetime -> map/cache;
// no cache holder may acquire lifetime. Both code owners and debug state use it.
static void ShaderInvalidateVertexProgramsLocked()
{
	EXIT_IF(g_vs_program_generation == UINT64_MAX);
	++g_vs_program_generation;
	std::scoped_lock cache_lock(g_vs_isa_cache_mutex, g_vertex_offset_sgpr_mutex);
	if (g_vs_isa_cache != nullptr) { g_vs_isa_cache->clear(); }
	g_vs_isa_cache_bytes = 0;
	if (g_vertex_offset_sgpr_map != nullptr) { g_vertex_offset_sgpr_map->clear(); }
}

// The builder already owns a lifetime lease; do not recursively acquire it.
static void ShaderCopyDebugPrintfsLocked(ShaderCode* code)
{
	if (g_debug_printfs == nullptr) { return; }
	const auto id = (static_cast<uint64_t>(code->GetHash0()) << 32u) | code->GetCrc32();
	if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto value) { return cmd.id == value; });
	    g_debug_printfs->IndexValid(index))
	{
		code->GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
	}
}

static void ShaderCopyDebugPrintfs(ShaderCode* code)
{
	std::shared_lock lifetime_lock(g_shader_lifetime_mutex);
	ShaderCopyDebugPrintfsLocked(code);
}

void ShaderSetGen5EudSnapshotTestHook(ShaderGen5EudSnapshotTestHook hook, void* context)
{
	std::scoped_lock lock(g_shader_gen5_eud_snapshot_test_hook_mutex);
	g_shader_gen5_eud_snapshot_test_hook.store(nullptr, std::memory_order_release);
	g_shader_gen5_eud_snapshot_test_hook_context = context;
	g_shader_gen5_eud_snapshot_test_hook.store(hook, std::memory_order_release);
}

static void ShaderNotifyGen5EudSnapshotTestHook()
{
	if (g_shader_gen5_eud_snapshot_test_hook.load(std::memory_order_acquire) == nullptr)
	{
		return;
	}
	std::scoped_lock lock(g_shader_gen5_eud_snapshot_test_hook_mutex);
	const auto hook = g_shader_gen5_eud_snapshot_test_hook.load(std::memory_order_relaxed);
	if (hook != nullptr)
	{
		hook(g_shader_gen5_eud_snapshot_test_hook_context);
	}
}

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
	g_vs_isa_cache           = new ShaderVertexProgramCache();
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
	ShaderInvalidateVertexProgramsLocked();
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
	ShaderInvalidateVertexProgramsLocked();
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
static void ShaderAppendContinuation(ShaderCode* code, const ShaderCode& back, uint64_t back_code_addr)
{
	EXIT_IF(code == nullptr || back_code_addr == 0 || back.GetInstructions().IsEmpty());
	if (!ShaderHasTerminalSetpc(*code))
	{
		return;
	}
	const uint32_t front_terminal_index = code->GetInstructions().Size() - 1u;

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
	EXIT_IF(front_max_pc > UINT32_MAX - 16u);
	const uint32_t pc_offset       = front_max_pc + 16u;
	const auto check_pc = [pc_offset](uint32_t pc) { EXIT_IF(pc > UINT32_MAX - pc_offset); };
	for (const auto& inst: back.GetInstructions()) { check_pc(inst.pc); }
	for (const auto& label: back.GetLabels()) { check_pc(label.GetDst()); check_pc(label.GetSrc()); }
	for (const auto& label: back.GetIndirectLabels()) { check_pc(label.GetDst()); check_pc(label.GetSrc()); }
	const uint32_t back_entry_pc   = back.GetInstructions().At(0).pc + pc_offset;
	const int64_t rel = static_cast<int64_t>(back_entry_pc) - code->GetInstructions().At(front_terminal_index).pc - 4;
	EXIT_IF(rel < INT32_MIN || rel > INT32_MAX);
	code->SetContinuationPc(pc_offset);

	for (auto inst: back.GetInstructions())
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
		inst.type                    = ShaderInstructionType::SBranch;
		inst.format                  = ShaderInstructionFormat::Label;
		inst.src_num                 = 1;
		inst.src[0]                  = {};
		inst.src[0].type             = ShaderOperandType::IntegerInlineConstant;
		inst.src[0].size             = 0;
		inst.src[0].constant.i       = static_cast<int32_t>(rel);
		inst.dst                     = {};
		code->GetLabels().Add(ShaderLabel(back_entry_pc, inst.pc));
		break;
	}

	static std::atomic_uint32_t logs {0};
	if (logs.fetch_add(1u, std::memory_order_relaxed) < 16u)
	{
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

const char* ShaderProgramSnapshotStatusName(ShaderProgramSnapshotStatus status)
{
	switch (status)
	{
		case ShaderProgramSnapshotStatus::Unmapped: return "unmapped";
		case ShaderProgramSnapshotStatus::InvalidRange: return "invalid_range";
		case ShaderProgramSnapshotStatus::Unreadable: return "unreadable";
		case ShaderProgramSnapshotStatus::Complete: return "complete";
		case ShaderProgramSnapshotStatus::Truncated: return "truncated";
	}
	return "unknown";
}

ShaderProgramSnapshot ShaderSnapshotMappedProgram(uint64_t address, uint32_t max_bytes)
{
	ShaderProgramSnapshot snapshot;
	if (address == 0u || (address & 3u) != 0u || max_bytes == 0u || (max_bytes & 3u) != 0u ||
	    max_bytes > kShaderProgramSnapshotBytesMax)
	{
		snapshot.status = ShaderProgramSnapshotStatus::InvalidRange;
		return snapshot;
	}
	std::shared_lock lifetime_lock(g_shader_lifetime_mutex);
	ShaderMappedData data;
	if (g_shader_map == nullptr || !ShaderGetMappedData(address, &data) || data.code_size_bytes == 0u)
	{
		return snapshot;
	}
	snapshot.mapped_bytes = data.code_size_bytes;
	if ((data.code_size_bytes & 3u) != 0u || address > UINT64_MAX - data.code_size_bytes)
	{
		snapshot.status = ShaderProgramSnapshotStatus::InvalidRange;
		return snapshot;
	}
	const uint32_t size = std::min(max_bytes, data.code_size_bytes);
	snapshot.words.resize(size / sizeof(uint32_t));
	const bool copied = Core::VirtualMemory::VisitReadableGuestRange(
	    address, size,
	    [](const void* source, uint64_t bytes, void* destination)
	    {
		    if (source == nullptr || destination == nullptr)
		    {
			    return false;
		    }
		    std::memcpy(destination, source, bytes);
		    return true;
	    },
	    snapshot.words.data());
	if (!copied)
	{
		snapshot.words.clear();
		snapshot.status = ShaderProgramSnapshotStatus::Unreadable;
		return snapshot;
	}
	snapshot.status = size == data.code_size_bytes ? ShaderProgramSnapshotStatus::Complete : ShaderProgramSnapshotStatus::Truncated;
	return snapshot;
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
				if (continuation_mode == ShaderContinuationMode::Append && ShaderHasTerminalSetpc(*code))
				{
					ShaderCode back;
					back.SetType(code->GetType());
					ShaderParseMappedLocked(continuation, &back, ShaderContinuationMode::None);
					ShaderAppendContinuation(code, back, continuation);
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

static std::vector<uint32_t> ShaderCopyProgramWords(uint64_t address, uint32_t bytes)
{
	if (address == 0u || (address & 3u) != 0u || bytes == 0u || (bytes & 3u) != 0u || address > UINT64_MAX - bytes)
	{
		EXIT("invalid vertex program range: address=0x%016" PRIx64 " size=0x%08" PRIx32 "\n", address, bytes);
	}
	std::vector<uint32_t> words;
	if (!Core::VirtualMemory::VisitReadableGuestRange(
	        address, bytes,
	        [](const void* source, uint64_t size, void* destination)
	        {
		        if (source == nullptr || destination == nullptr || size > UINT32_MAX) { return false; }
		        auto* copy = static_cast<std::vector<uint32_t>*>(destination);
		        copy->resize(static_cast<size_t>(size / sizeof(uint32_t)));
		        std::memcpy(copy->data(), source, static_cast<size_t>(size));
		        return true;
	        }, &words))
	{
		EXIT("vertex program range became unreadable: address=0x%016" PRIx64 " size=0x%08" PRIx32 "\n", address, bytes);
	}
	return words;
}

static std::array<uint32_t, 4> ShaderVertexProgramFingerprint(const ShaderVertexProgram& program,
                                                            const std::vector<uint32_t>& front,
                                                            const std::vector<uint32_t>& back)
{
	std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)> state(XXH3_createState(), &XXH3_freeState);
	EXIT_IF(state == nullptr || XXH3_128bits_reset(state.get()) != XXH_OK);
	const auto bytes = [&state](const void* data, size_t size)
	{
		if (size != 0u) { EXIT_IF(XXH3_128bits_update(state.get(), data, size) != XXH_OK); }
	};
	const auto word = [&bytes](uint32_t value)
	{
		const uint8_t encoded[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8u),
		                           static_cast<uint8_t>(value >> 16u), static_cast<uint8_t>(value >> 24u)};
		bytes(encoded, sizeof(encoded));
	};
	// LVP1: explicit little-endian framing, exact copied guest bytes, and the
	// link algorithm's resulting boundary. No addresses, epochs or object padding.
	word(0x4c565031u);
	word(static_cast<uint32_t>(program.key.gs_front));
	word(program.code.GetContinuationPc());
	for (const auto* segment: {&front, &back})
	{
		word(static_cast<uint32_t>(segment->size() * sizeof(uint32_t)));
		bytes(segment->data(), segment->size() * sizeof(uint32_t));
	}
	word(program.code.GetDebugPrintfs().Size());
	for (const auto& cmd: program.code.GetDebugPrintfs())
	{
		word(cmd.pc);
		word(cmd.format.Size());
		for (uint32_t i = 0; i < cmd.format.Size(); ++i) { word(static_cast<uint32_t>(cmd.format.At(i))); }
		word(cmd.types.Size());
		for (auto type: cmd.types) { word(static_cast<uint32_t>(type)); }
		word(cmd.args.Size());
		for (const auto& arg: cmd.args)
		{
			uint32_t multiplier = 0;
			static_assert(sizeof(multiplier) == sizeof(arg.multiplier));
			std::memcpy(&multiplier, &arg.multiplier, sizeof(multiplier)); // Scalar float bits, not a struct image.
			word(static_cast<uint32_t>(arg.type));
			word(arg.constant.u);
			word(static_cast<uint32_t>(arg.register_id));
			word(static_cast<uint32_t>(arg.size));
			word(multiplier);
			word(arg.absolute); word(arg.negate); word(arg.clamp); word(arg.swizzle);
			word(arg.dpp); word(arg.dpp_ctrl); word(arg.dpp_row_mask); word(arg.dpp_bank_mask);
			word(arg.dpp_fetch_inactive); word(arg.dpp_bound_ctrl);
		}
	}
	const auto hash = XXH3_128bits_digest(state.get());
	return {static_cast<uint32_t>(hash.low64), static_cast<uint32_t>(hash.low64 >> 32u),
	        static_cast<uint32_t>(hash.high64), static_cast<uint32_t>(hash.high64 >> 32u)};
}

static uint64_t ShaderVertexProgramCacheCost(const ShaderVertexProgram& program)
{
	// Budget retained IR capacities, counting shared front/linked storage twice
	// conservatively. Entry count separately bounds hash nodes/control blocks.
	// Debug strings/operands have nested allocations: keep those owners uncached.
	if (!program.code.GetDebugPrintfs().IsEmpty()) { return kVertexProgramCacheBytes + 1u; }
	uint64_t bytes = sizeof(ShaderVertexProgram);
	for (const auto* code: {&program.front_code, &program.code})
	{
		bytes += sizeof(Core::SimpleArray<ShaderInstruction>) +
		         static_cast<uint64_t>(code->GetInstructions().Capacity()) * sizeof(ShaderInstruction);
		bytes += 2u * sizeof(Core::SimpleArray<ShaderLabel>) +
		         static_cast<uint64_t>(code->GetLabels().Capacity()) * sizeof(ShaderLabel) +
		         static_cast<uint64_t>(code->GetIndirectLabels().Capacity()) * sizeof(ShaderLabel);
		bytes += sizeof(Core::SimpleArray<ShaderDebugPrintf>) +
		         static_cast<uint64_t>(code->GetDebugPrintfs().Capacity()) * sizeof(ShaderDebugPrintf);
	}
	return bytes;
}

// Caller retains the shared lifetime lock through resource-metadata consumption.
// This cache observes MapUserData/RegisterContinuation/debug generations; direct
// in-place guest code writes without a new mapping are NOT observed on a hit.
static std::shared_ptr<const ShaderVertexProgram> GetCachedVertexProgramLocked(const HW::VertexShaderInfo& regs,
                                                                              const ShaderMappedData& data, bool gs_front)
{
	EXIT_IF(g_vs_isa_cache == nullptr);
	const uint64_t shader_addr = gs_front ? regs.es_regs.data_addr : regs.vs_regs.data_addr;
	const ShaderVertexProgramKey key {shader_addr, gs_front ? ShaderLookupContinuation(shader_addr) : 0u,
	                                  regs.gs_back_addr, regs.gs_regs.chksum, g_vs_program_generation, gs_front};
	{
		std::lock_guard<std::mutex> lock(g_vs_isa_cache_mutex);
		if (auto cached = g_vs_isa_cache->find(key); cached != g_vs_isa_cache->end())
		{
			return cached->second;
		}
	}
	auto program = std::make_shared<ShaderVertexProgram>();
	program->key = key;
	program->front_code.SetType(ShaderType::Vertex);
	program->front_code.SetHash0(static_cast<uint32_t>(key.checksum >> 32u));
	program->front_code.SetCrc32(static_cast<uint32_t>(key.checksum));
	const auto front = ShaderCopyProgramWords(shader_addr, data.code_size_bytes);
	const auto boundary = key.continuation_addr != 0u ? ShaderParseBoundary::RegisteredFront : ShaderParseBoundary::CompleteProgram;
	if (!ShaderTryParseBounded(front.data(), data.code_size_bytes, &program->front_code, boundary))
	{
		EXIT("vertex front has no complete reachable terminator: address=0x%016" PRIx64 " size=0x%08" PRIx32 "\n",
		     shader_addr, data.code_size_bytes);
	}
	program->code = program->front_code;
	std::vector<uint32_t> back_words;
	if (ShaderHasTerminalSetpc(program->front_code))
	{
		if (key.continuation_addr == 0u || key.bound_back_addr != key.continuation_addr)
		{
			EXIT("vertex continuation binding mismatch: front=0x%016" PRIx64 " registered=0x%016" PRIx64
			     " bound=0x%016" PRIx64 "\n", shader_addr, key.continuation_addr, key.bound_back_addr);
		}
		ShaderMappedData back_data;
		if (!ShaderGetMappedData(key.continuation_addr, &back_data) || back_data.code_size_bytes == 0u)
		{
			EXIT("vertex continuation has no bounded mapped range: address=0x%016" PRIx64 "\n", key.continuation_addr);
		}
		back_words = ShaderCopyProgramWords(key.continuation_addr, back_data.code_size_bytes);
		ShaderCode back;
		back.SetType(ShaderType::Vertex);
		ShaderParse(back_words.data(), back_data.code_size_bytes, &back);
		ShaderAppendContinuation(&program->code, back, key.continuation_addr);
	}
	ShaderCopyDebugPrintfsLocked(&program->code);
	program->fingerprint = ShaderVertexProgramFingerprint(*program, front, back_words);
	ShaderProbeWrite("vs", program->code, nullptr, nullptr);
	const uint64_t cache_cost = ShaderVertexProgramCacheCost(*program);
	// This is a cache-retention budget, not a production program-size limit.
	if (cache_cost > kVertexProgramCacheBytes) { return program; }
	std::lock_guard<std::mutex> lock(g_vs_isa_cache_mutex);
	if (auto cached = g_vs_isa_cache->find(key); cached != g_vs_isa_cache->end())
	{
		return cached->second;
	}
	if (g_vs_isa_cache->size() >= kVertexProgramCacheEntries || cache_cost > kVertexProgramCacheBytes - g_vs_isa_cache_bytes)
	{
		g_vs_isa_cache->clear();
		g_vs_isa_cache_bytes = 0;
	}
	g_vs_isa_cache->emplace(key, program);
	g_vs_isa_cache_bytes += cache_cost;
	return program;
}

static bool IsDiscardInstruction(const Vector<ShaderInstruction>& code, uint32_t index)
{
	if (!(index == 0 || index + 1 >= code.Size()))
	{
		const auto& prev_inst = code.At(index - 1);
		const auto& inst      = code.At(index);
		const auto& next_inst = code.At(index + 1);

		const bool exec_zeroed =
		    (prev_inst.type == ShaderInstructionType::SMovB64 &&
		     prev_inst.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
		     prev_inst.dst.type == ShaderOperandType::ExecLo && prev_inst.src[0].type == ShaderOperandType::IntegerInlineConstant &&
		     prev_inst.src[0].constant.i == 0) ||
		    (prev_inst.type == ShaderInstructionType::SMovB32 &&
		     prev_inst.format == ShaderInstructionFormat::SVdstSVsrc0 &&
		     prev_inst.dst.type == ShaderOperandType::ExecLo && prev_inst.src[0].type == ShaderOperandType::IntegerInlineConstant &&
		     prev_inst.src[0].constant.i == 0);
		return (inst.type == ShaderInstructionType::Exp && ShaderIsNullMrtDoneFormat(inst.format) && exec_zeroed &&
		        next_inst.type == ShaderInstructionType::SEndpgm);
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
				    inst.type == ShaderInstructionType::SCbranchExecnz ||
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
	if (!image.sampled_shape_known || !ShaderGen5InstructionShapeAppliesToType(descriptor->texture.Type(), image.sampled_shape))
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

	if (info->textures_num < 0 || info->textures_num >= ShaderTextureResources::RES_MAX)
	{
		EXIT("shader texture resource capacity exceeded: count=%d capacity=%d register=%d slot=%d\n",
		     info->textures_num, ShaderTextureResources::RES_MAX, start_index, slot);
	}
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

// Ordered-append GDS use: ds_append/ds_consume index the global GDS buffer
// via m0, which the driver feeds from an SGPR holding the descriptor. Return
// that SGPR when the instruction stream shows both the GDS use and the m0
// feed, -1 otherwise.
static int ShaderGdsPointerSourceSgpr(const ShaderCode& code)
{
	bool uses_gds = false;
	int  source   = -1;
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.format == ShaderInstructionFormat::VdstGds)
		{
			uses_gds = true;
		}
		if (inst.dst.type == ShaderOperandType::M0 && inst.src_num > 0 && inst.src[0].type == ShaderOperandType::Sgpr)
		{
			source = inst.src[0].register_id;
		}
	}
	return uses_gds ? source : -1;
}

// A direct-resource slot is ambiguous in driver metadata when it can name the
// GDS ordered-append pointer instead of its declared kind. Classify by
// observed use so genuine descriptors keep their binding.
static bool ShaderDirectResourceIsGdsPointer(const ShaderCode& code, int start_register)
{
	return ShaderGdsPointerSourceSgpr(code) == start_register;
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
		// Keep sparse binding numbers stable while descriptor layouts reserve the
		// separate array size of each declared shape and numeric bank.
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
	// Device-address and thread-limit blocks precede the program base, which
	// stays the final block.
	bind->device_address_offset_dw = 0;
	if (bind->device_address_used)
	{
		bind->device_address_offset_dw = bind->push_constant_size / 4u;
		bind->push_constant_size += 16u;
	}
	bind->thread_limits_offset_dw = 0;
	if (bind->thread_limits_used)
	{
		bind->thread_limits_offset_dw = bind->push_constant_size / 4u;
		bind->push_constant_size += 16u;
	}
	bind->program_base_offset_dw = 0;
	if (bind->program_base_used)
	{
		bind->program_base_offset_dw = bind->push_constant_size / 4u;
		bind->push_constant_size += 16u;
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
		case ShaderInstructionType::BufferAtomicAdd:
		case ShaderInstructionType::BufferAtomicUmax: is_store = true; break;
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

bool ShaderHasOnlyNullPixelExports(const ShaderCode& code)
{
	if (code.GetType() != ShaderType::Pixel) { return false; }
	bool found = false;
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type != ShaderInstructionType::Exp) { continue; }
		if (inst.format != ShaderInstructionFormat::NullVmDone) { return false; }
		found = true;
	}
	return found;
}

bool ShaderPreventsNoopPixelElision(const ShaderCode& code)
{
	bool prevents = false;
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		if (IsDiscardInstruction(code.GetInstructions(), index) ||
		    (inst.type == ShaderInstructionType::Exp && inst.format == ShaderInstructionFormat::NullVmDone))
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
	                      ShaderInstructionType::BufferAtomicXor, ShaderInstructionType::BufferAtomicSwap,
	                      ShaderInstructionType::BufferStoreDword,
	                      ShaderInstructionType::BufferStoreDwordx2, ShaderInstructionType::BufferStoreDwordx3,
	                      ShaderInstructionType::BufferStoreDwordx4, ShaderInstructionType::BufferStoreFormatX,
	                      ShaderInstructionType::BufferStoreFormatXy, ShaderInstructionType::BufferStoreFormatXyzw,
	                      ShaderInstructionType::DsAppend, ShaderInstructionType::DsConsume, ShaderInstructionType::DsAddU32,
	                      ShaderInstructionType::DsAddRtnU32,
	                      ShaderInstructionType::DsAndB32, ShaderInstructionType::DsDecU32, ShaderInstructionType::DsIncU32,
	                      ShaderInstructionType::DsMaxI32, ShaderInstructionType::DsMaxU32, ShaderInstructionType::DsMinI32,
	                      ShaderInstructionType::DsMinU32, ShaderInstructionType::DsOrB32, ShaderInstructionType::DsRsubU32,
	                      ShaderInstructionType::DsSubU32, ShaderInstructionType::DsXorB32, ShaderInstructionType::DsWriteB32,
	                      ShaderInstructionType::DsWrite2B32, ShaderInstructionType::DsWrite2St64B32,
	                      ShaderInstructionType::ImageStore, ShaderInstructionType::ImageStoreMip,
	                      ShaderInstructionType::ImageAtomicAdd});

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

bool ShaderSnapshotGuestDescriptorTable(uint64_t guest_address, uint32_t dwords, std::vector<uint32_t>* snapshot)
{
	if (snapshot == nullptr || guest_address == 0u || dwords == 0u || dwords > SHADER_GEN5_EUD_MAX_DWORDS ||
	    snapshot->size() != static_cast<size_t>(dwords))
	{
		return false;
	}
	const uint64_t bytes = static_cast<uint64_t>(dwords) * sizeof(uint32_t);
	if (guest_address > UINT64_MAX - bytes)
	{
		return false;
	}

	std::vector<uint32_t> verification(dwords);
	constexpr uint32_t attempts = 2u;
	for (uint32_t attempt = 0; attempt < attempts; ++attempt)
	{
		if (!Core::VirtualMemory::CopyFromGuest(snapshot->data(), guest_address, bytes))
		{
			return false;
		}
		ShaderNotifyGen5EudSnapshotTestHook();
		if (!Core::VirtualMemory::CopyFromGuest(verification.data(), guest_address, bytes))
		{
			return false;
		}
		if (std::memcmp(snapshot->data(), verification.data(), static_cast<size_t>(bytes)) == 0)
		{
			return true;
		}
	}
	return false;
}

// Gen5 direct-resource type 5 is the EUD pointer when eud_size_dw != 0,
// including stages that also declare SRT data. Captured post-detile PS: user_sgpr_num=30, eud=12, type5 at
// SGPR 0x1c holds a guest pointer whose first 8 dwords are two S# descriptors
// for sharp sampler offsets 0x20 and 0x24.

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderParseUsage2(const ShaderUserData* user_data, ShaderParsedUsage* info, ShaderBindResources* bind,
                       const HW::UserSgprInfo& user_sgpr, int user_sgpr_num, const ShaderCode* code, int user_data_register_base,
                       bool vertex_resource_types, const ShaderCode* eud_descriptor_code)
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
	const auto* eud_code = eud_descriptor_code != nullptr ? eud_descriptor_code : code;
	if (has_eud_ptr)
	{
		bind->extended.eud_user_sgpr_num = user_sgpr_num;
		bind->extended.eud_size_dw       = user_data->eud_size_dw;
		bind->extended.eud_offset_base    = ShaderGen5EudOffsetBase(user_sgpr_num);
	}
	if (user_data->eud_size_dw != 0)
	{
		if (user_sgpr_num <= 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_sgpr_num <= 0 condition ignored (continuing)\n"); }
		if (!has_eud_ptr)
		{
			if (static_cast<uint32_t>(user_sgpr_num) < user_data->eud_size_dw) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: static_cast<uint32_t>(user_sgpr_num) < user_data->eud_size_dw condition ignored (continuing)\n"); }
		}
	}
	if (user_data->srt_size_dw > user_sgpr_num) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_data->srt_size_dw > user_sgpr_num condition ignored (continuing)\n"); }

	const uint32_t* extended_buffer    = nullptr;
	uint64_t        eud_guest_address = 0u;
	bool            eud_pointer_valid = false;

	bool direct_sgprs[HW::UserSgprInfo::SGPRS_MAX];
	for (int i = 0; i < HW::UserSgprInfo::SGPRS_MAX; i++)
	{
		direct_sgprs[i] = (i < user_sgpr_num);
	}

	// A direct resource whose SGPRs every path redefines before reading them
	// is never consumed from user data; registering it would materialize an
	// arbitrary dispatch-time value as a descriptor.
	static const std::vector<std::bitset<kShaderScalarLivenessSgprs>> no_entry_values;
	std::bitset<kShaderScalarLivenessSgprs>                          entry_live;
	entry_live.set();
	const auto  flow         = code != nullptr ? ShaderScalarFlowOf(*code) : nullptr;
	const auto& entry_values = flow != nullptr ? flow->holding_entry_value : no_entry_values;
	if (flow != nullptr)
	{
		entry_live = flow->live_at_entry;
	}
	auto entry_reads = [&](int first, int dwords)
	{
		for (int r = first; r < first + dwords; r++)
		{
			if (r < 0 || r >= kShaderScalarLivenessSgprs || entry_live.test(static_cast<size_t>(r)))
			{
				return true;
			}
		}
		return false;
	};
	for (uint16_t type = 0; type < user_data->direct_resource_count; type++)
	{
		if (user_data->direct_resource_offset[type] == 0xffff)
		{
			continue;
		}

		int reg = user_data->direct_resource_offset[type];

		// Ordered-append GDS pointer: the slot whose SGPR feeds m0 ahead of
		// ds_append/ds_consume describes the global GDS buffer. Classify by
		// observed use before the generic paths.
		if (!vertex_resource_types && code != nullptr &&
		    ShaderDirectResourceIsGdsPointer(*code, reg + user_data_register_base))
		{
			ShaderGetGdsPointer(&bind->gds_pointers, direct_sgprs, reg, bind->gds_pointers.pointers_num, user_sgpr, nullptr);
			info->gds_pointers++;
			continue;
		}

		switch (type)
		{
			case 8:
				if (!vertex_resource_types && code != nullptr && !entry_reads(reg + user_data_register_base, 8))
				{
					break;
				}
				if (!vertex_resource_types)
				{
					ShaderGetTextureBuffer(&bind->textures2D, direct_sgprs, reg, bind->textures2D.textures_num,
					                       ShaderTextureUsage::ReadOnly, user_sgpr, nullptr);
					info->textures2D_readonly++;
					break;
				}
				info->vertex_buffer                       = true;
				info->vertex_buffer_reg                   = reg;
				// This is a guest table pointer, not a rewritten V#. Native
				// S_LOAD still needs both original words in the user-data window.
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
				// Attribute-table metadata does not consume the pointer value:
				// preserve it for guest S_LOAD using the normal register base.
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
						eud_guest_address = eud_base;
						eud_pointer_valid = true;
					} else
					{
						ShaderReportMissingGen5EudPointer(user_data, reg, user_sgpr_num);
					}
					direct_sgprs[reg]     = false;
					direct_sgprs[reg + 1] = false;
					break;
				}
				// No EUD pointer: fall through as a storage buffer.
				// fallthrough
			default:
			{
				if (code != nullptr && !entry_reads(reg + user_data_register_base, 4))
				{
					break;
				}
				if (code != nullptr)
				{
					const auto image = AnalyzeShaderDirectImageUse(*code, reg + user_data_register_base, &entry_values);
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

				// A four-dword direct storage descriptor cannot be inferred from a
				// partial overlap with the declared SRT span. Keep those SGPRs raw
				// unless the decoded instruction flow proves they reach a direct
				// storage consumer before any overwrite; that contradiction is strict.
				constexpr int direct_storage_descriptor_dwords = 4;
				const bool partial_srt_descriptor = user_data->srt_size_dw != 0 && reg < user_data->srt_size_dw &&
				                                    reg + direct_storage_descriptor_dwords > user_data->srt_size_dw;
				if (partial_srt_descriptor)
				{
					if (code != nullptr &&
					    AnalyzeShaderStorageUse(*code, reg + user_data_register_base).access != ShaderStorageAccess::Unknown)
					{
						EXIT("direct storage descriptor crosses declared SRT span: reg=%d dwords=%d srt_size_dw=%u\n", reg,
						     direct_storage_descriptor_dwords, static_cast<unsigned>(user_data->srt_size_dw));
					}
					break;
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
					// With the instruction stream available, only a slot that
					// reaches a storage consumer is a buffer descriptor; others (for
					// example 64-bit table pointers) stay ordinary SGPR data.
					if (AnalyzeShaderStorageUse(*code, reg + user_data_register_base).access == ShaderStorageAccess::Unknown)
					{
						break;
					}
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

	// Every resource in this draw/dispatch must come from one observed EUD
	// table version. Reading descriptor words directly from guest memory lets a
	// concurrent table update combine two versions into a valid-looking V#/T#.
	std::vector<uint32_t> eud_snapshot;
	if (eud_pointer_valid)
	{
		uint32_t required_end_dw = 0u;
		if (!ShaderGen5EudRequiredEndDwords(user_data, user_sgpr_num, bind->extended.start_register, eud_code,
		                                      user_data_register_base, &required_end_dw))
		{
			EXIT("invalid Gen5 EUD snapshot span: eud_dw=%u pointer_reg=%d\n",
			     static_cast<unsigned>(user_data->eud_size_dw), bind->extended.start_register);
		}
		bool snapshot_ready = false;
		for (uint32_t pass = 0; pass < 2u; ++pass)
		{
			if (required_end_dw == 0u || required_end_dw > SHADER_GEN5_EUD_MAX_DWORDS)
			{
				EXIT("invalid Gen5 EUD snapshot size: dwords=%u\n", static_cast<unsigned>(required_end_dw));
			}
			eud_snapshot.resize(required_end_dw);
			if (!ShaderSnapshotGuestDescriptorTable(eud_guest_address, required_end_dw, &eud_snapshot))
			{
				EXIT("unstable or unreadable Gen5 EUD snapshot: dwords=%u\n", static_cast<unsigned>(required_end_dw));
			}
			uint32_t expanded_end_dw = required_end_dw;
			if (!ShaderGen5EudExpandEndDwordsForSharpImages(user_data, user_sgpr_num, eud_snapshot.data(), required_end_dw,
			                                                &expanded_end_dw))
			{
				EXIT("invalid Gen5 EUD image sharp span: dwords=%u\n", static_cast<unsigned>(required_end_dw));
			}
			if (expanded_end_dw == required_end_dw)
			{
				snapshot_ready = true;
				break;
			}
			required_end_dw = expanded_end_dw;
		}
		if (!snapshot_ready)
		{
			EXIT("unstable Gen5 EUD image sharp classification\n");
		}
		extended_buffer       = eud_snapshot.data();
		info->extended_buffer = true;
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
	}
	if (eud_code != nullptr && has_eud_ptr && extended_buffer != nullptr)
	{
		ShaderCollectDynamicScalarResources(*eud_code, bind, user_sgpr, info, extended_buffer, user_data->eud_size_dw,
		                                    user_data_register_base);
	}
	if (code != nullptr && !vertex_resource_types)
	{
		ShaderCollectPointerTableResources(*code, bind, user_sgpr, info, user_data->srt_size_dw, user_data_register_base);
	}

	// Gen5 metadata is advisory: some shaders address an S# descriptor directly
	// from the EUD/user-SGPR namespace without listing it in the sharp table.
	// Reconcile decoded scalar-buffer accesses with the binding set, preserving
	// null-descriptor rejection and the same EUD translation used by metadata.
	if (code != nullptr)
	{
		uint32_t inst_index = 0;
		for (const auto& inst: code->GetInstructions())
		{
			const uint32_t current = inst_index++;
			if (!ShaderInstructionIsScalarBufferLoad(inst) || inst.src_num == 0 || inst.src[0].type != ShaderOperandType::Sgpr ||
			    inst.src[0].size != 4 || ShaderIsDynamicScalarStorageConsumer(*bind, inst))
			{
				continue;
			}
			// Only a base that still holds its user-data value here is a
			// dispatch-time descriptor; a redefined base is runtime data.
			bool entry_base = true;
			for (int r = inst.src[0].register_id; r < inst.src[0].register_id + 4; r++)
			{
				entry_base = entry_base && r >= 0 && r < kShaderScalarLivenessSgprs &&
				             entry_values[current].test(static_cast<size_t>(r));
			}
			if (!entry_base)
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

	// Ordered-append GDS fallback: the descriptor slot is not always declared
	// in direct_resource_offset, so bind the observed m0 source when the
	// metadata did not already provide a pointer. Runs before direct-SGPR
	// consumption so the GDS register is not double-bound.
	if (code != nullptr && !vertex_resource_types && bind->gds_pointers.pointers_num == 0)
	{
		const int gds_sgpr = ShaderGdsPointerSourceSgpr(*code);
		const int raw_gds  = gds_sgpr - user_data_register_base;
		if (raw_gds >= 0 && raw_gds < user_sgpr_num && raw_gds < HW::UserSgprInfo::SGPRS_MAX && direct_sgprs[raw_gds])
		{
			ShaderGetGdsPointer(&bind->gds_pointers, direct_sgprs, raw_gds, bind->gds_pointers.pointers_num, user_sgpr, nullptr);
			info->gds_pointers++;
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
			// Each mapping is proven to reach its descriptor consumers before a clobber.
			// Merge that local raw-use proof with any independent static use of the
			// same physical descriptor instead of assigning a synthetic entry state.
			if (exact_evidence.access == ShaderStorageAccess::Unknown)
			{
				exact_evidence.access = ShaderStorageAccess::Raw;
			} else if (exact_evidence.access == ShaderStorageAccess::Typed)
			{
				exact_evidence.access = ShaderStorageAccess::Mixed;
			}
			for (uint32_t mapping = 0; mapping < bind->dynamic_sloads.records.Size(); ++mapping)
			{
				const auto& record = bind->dynamic_sloads.records.At(mapping);
				if (record.kind != ShaderDynamicSLoadResourceKind::StorageBuffer || record.resource_index != i)
				{
					continue;
				}
				if (record.raw_vmem_oob_guarded)
				{
					exact_evidence.raw_vmem_oob_guarded = true;
					continue;
				}
				exact_evidence.raw_smem_use = true;
				if (eud_code == nullptr || !ShaderDynamicSLoadScalarSpan(*eud_code, record, &exact_evidence.raw_smem_required_bytes))
				{
					exact_evidence.raw_smem_dynamic_offset = true;
				}
			}
		}
		const auto exact = exact_evidence.access;
		ShaderStorageUseEvidence unbased_evidence {};
		if (!local_dynamic_sload && code != nullptr && user_data_register_base != 0)
		{
			unbased_evidence = AnalyzeShaderStorageUse(*code, bind->storage_buffers.start_register[i]);
		}
		const bool code_available = code != nullptr || (has_dynamic_sload && eud_code != nullptr);
		const auto evidence = ResolveShaderStorageAccessEvidence(code_available, bind->storage_buffers.sources[i], exact,
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
				for (uint32_t mapping = 0; mapping < bind->dynamic_sloads.records.Size(); ++mapping)
				{
					const auto& record = bind->dynamic_sloads.records.At(mapping);
					if (record.kind == ShaderDynamicSLoadResourceKind::StorageBuffer && record.resource_index == i)
					{
						AddZeroSBufferResource(&bind->zero_sbuffer_resources,
						                       record.destination_register);
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
				descriptor.sample_operation = ShaderTextureSampleOperation(descriptor.texture, image.sample_operation);
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
	// After compaction, so the recorded resource indices stay final.
	if (code != nullptr && !vertex_resource_types)
	{
		ShaderCollectAssembledBufferDescriptors(*code, bind, user_sgpr, user_sgpr_num, user_data_register_base);
	}
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

uint32_t ShaderVertexStreamRecordOffset(const ShaderVertexInputInfo& input_info)
{
	return !input_info.fetch_external && input_info.vertex_offset_sgpr >= 0 ? input_info.vertex_offset_value : 0u;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Kyty::Core::String8 ShaderVertexNggFrontRefusal(const ShaderVertexInputInfo& info)
{
	return info.program != nullptr && info.gs_prolog ? info.program->ngg_front.Refusal(info.native_wave.guest_wave_size)
	                                                  : Kyty::Core::String8();
}

void ShaderGetInputInfoVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh, ShaderVertexInputInfo* info,
                          const GraphicsGeRawRegister* shader_stages)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || regs == nullptr);

	info->bind                      = {};
	info->export_count              = static_cast<int>(sh->GetExportCount());
	info->position1_usage            = ShaderDecodeVertexPosition1Usage(sh->m_spiShaderPosFormat, sh->m_paClVsOutCntl,
	                                                                  Config::IsNextGen());
	info->bind.push_constant_offset = 0;
	info->bind.push_constant_size   = 0;
	info->bind.descriptor_set_slot  = 0;
	info->clip_probe                = {};
	info->clip_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
	info->float_mode                = 0;
	info->dx10_clamp                = false;
	info->ieee_mode                 = false;
	info->fp16_overflow             = false;
	info->fp16_overflow_known       = false;
	info->native_wave               = {};
	info->required_subgroup_size    = 0;
	info->program.reset();

	if (regs->vs_embedded)
	{
		return;
	}

	ShaderParsedUsage usage;

	bool gs_instead_of_vs =
	    (regs->vs_regs.data_addr == 0 && regs->gs_regs.data_addr == 0 && regs->es_regs.data_addr != 0 && regs->gs_regs.chksum != 0);
	const uint32_t guest_wave_size = ShaderVertexGuestWaveSize(shader_stages != nullptr ? *shader_stages : GraphicsGeRawRegister {},
	                                                          Config::IsNextGen(), gs_instead_of_vs);
	info->float_mode = gs_instead_of_vs ? regs->gs_regs.rsrc1.float_mode : regs->vs_regs.rsrc1.float_mode;
	info->dx10_clamp = gs_instead_of_vs ? regs->gs_regs.rsrc1.dx10_clamp : regs->vs_regs.rsrc1.dx10_clamp;
	info->ieee_mode  = gs_instead_of_vs ? regs->gs_regs.rsrc1.ieee_mode : regs->vs_regs.rsrc1.ieee_mode;
	info->fp16_overflow = gs_instead_of_vs ? regs->gs_regs.rsrc1.fp16_overflow : regs->vs_regs.rsrc1.fp16_overflow;
	info->fp16_overflow_known =
	    gs_instead_of_vs ? regs->gs_regs.rsrc1.fp16_overflow_known : regs->vs_regs.rsrc1.fp16_overflow_known;

	uint64_t                shader_addr   = (gs_instead_of_vs ? regs->es_regs.data_addr : regs->vs_regs.data_addr);
	const HW::UserSgprInfo& user_sgpr     = (gs_instead_of_vs ? regs->gs_user_sgpr : regs->vs_user_sgpr);
	auto                    user_sgpr_num = (gs_instead_of_vs ? regs->gs_regs.rsrc2.user_sgpr : regs->vs_regs.rsrc2.user_sgpr);

	bool ps5 = Config::IsNextGen();

	ShaderMappedData data;
	std::shared_lock<std::shared_mutex> shader_lifetime_lock(g_shader_lifetime_mutex, std::defer_lock);

	if (ps5)
	{
		// Match shallow mapping metadata and both code segments to one generation.
		// This is not a snapshot of arbitrary writes to the pointed-to resources.
		// Nested code/debug helpers must use their Locked forms under this lease.
		shader_lifetime_lock.lock();
		if (!ShaderGetMappedData(shader_addr, &data) || data.code_size_bytes == 0u)
		{
			EXIT("Gen5 vertex shader has no bounded mapped code range: address=0x%016" PRIx64 "\n", shader_addr);
		}
		info->program = GetCachedVertexProgramLocked(*regs, data, gs_instead_of_vs);
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
		const auto* vs_isa = &info->program->front_code;
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
			RebaseNggConstantSharps(&rebased_user, vs_isa, user_sgpr, static_cast<int>(user_sgpr_num));
			usage_user = &rebased_user;
		}
		// Keep general ISA analysis disabled: the scalar-buffer fallback on a
		// reused s[16:19] V# zeros the projection CBV. EUD load destinations
		// are resolved separately from the rebased metadata sharp table.
		ShaderParseUsage2(usage_user, &usage, &info->bind, user_sgpr, static_cast<int>(user_sgpr_num), nullptr,
		                  kGen5GsFrontUserDataBase, true, vs_isa);
		if (vs_isa != nullptr)
		{
			info->native_wave = info->program->native_wave.Get(info->program->code, guest_wave_size);
			info->required_subgroup_size =
			    ShaderApplyNggFrontProof(&info->native_wave, info->program->code, shader_stages, info->program->ngg_front)
			        ? guest_wave_size
			        : 0u;
			ShaderAssociateSampledTextureSamplers(*vs_isa, &info->bind, kGen5GsFrontUserDataBase);
			// Constant tables embedded after the code are addressed from GETPC;
			// the base stays runtime data so a cached pipeline can relocate.
			info->bind.program_base_used   = vs_isa->HasAnyOf({ShaderInstructionType::SGetpcB64});
			info->bind.program_base        = info->bind.program_base_used ? shader_addr : 0u;
			info->bind.device_address_used =
			    ShaderHasUnboundBufferLoad(*vs_isa, info->bind, kGen5GsFrontUserDataBase) || ShaderHasGlobalMemoryLoad(*vs_isa);
		}
	} else
	{
		if (gs_instead_of_vs) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: gs_instead_of_vs condition ignored (continuing)\n"); }

		info->gs_prolog = false;

		ShaderParseUsage(shader_addr, &usage, &info->bind, user_sgpr, user_sgpr_num);
		ShaderCode code;
		code.SetType(ShaderType::Vertex);
		ShaderParseMapped(shader_addr, &code);
		info->native_wave = ShaderAnalyzeNativeWave(code, guest_wave_size);
		info->required_subgroup_size = ShaderUsesNativeWaveState(code) ? guest_wave_size : 0u;
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
		std::shared_lock<std::shared_mutex> offset_lifetime_lock(g_shader_lifetime_mutex, std::defer_lock);
		if (!ps5) { offset_lifetime_lock.lock(); }
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
			if (ps5)
			{
				detected_offset = ShaderDetectVertexOffsetSgpr(info->program->front_code, user_data_base, user_sgpr_num);
			} else
			{
				ShaderCode code;
				code.SetType(ShaderType::Vertex);
				ShaderParseMappedLocked(shader_addr, &code,
				                        gs_instead_of_vs ? ShaderContinuationMode::AllowTerminator : ShaderContinuationMode::None);
				detected_offset = ShaderDetectVertexOffsetSgpr(code, user_data_base, user_sgpr_num);
			}
			std::lock_guard<std::mutex> lock(g_vertex_offset_sgpr_mutex);
			if (g_vertex_offset_sgpr_map->size() >= kVertexProgramCacheEntries) { g_vertex_offset_sgpr_map->clear(); }
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

	if (gs_instead_of_vs)
	{
		// RDNA initializes the fused ES+GS front with the GS user-data
		// address in s0:s1, before the user SGPRs at s8 and above.
		auto& direct = info->bind.direct_sgprs;
		EXIT_IF(direct.sgprs_num > ShaderDirectSgprsResources::SGPRS_MAX - 2);
		for (int word = 0; word < 2; ++word)
		{
			const int index = direct.sgprs_num++;
			direct.start_register[index]   = word;
			direct.absolute_register[index] = true;
			direct.sgprs[index].field = static_cast<uint32_t>(regs->gs_user_data_addr >> (word * 32));
		}
	}
	ShaderCalcBindingIndices(&info->bind);
}

// An S_LOAD through a guest pointer that neither the EUD path nor a PC-keyed
// descriptor mapping resolves reads plain data at a runtime address.
static bool ShaderScalarLoadNeedsGuestAddress(const ShaderInstruction& inst, const ShaderBindResources& bind)
{
	if (!ShaderInstructionTypeStartsWith(inst.type, "SLoad") || inst.src_num < 1 ||
	    (inst.src[0].type != ShaderOperandType::Sgpr && inst.src[0].type != ShaderOperandType::VccLo))
	{
		return false;
	}
	if (inst.src[0].type == ShaderOperandType::Sgpr && bind.extended.used && inst.src[0].register_id == bind.extended.start_register)
	{
		return false;
	}
	for (const auto& record: bind.dynamic_sloads.records)
	{
		if (record.instruction_pc == inst.pc)
		{
			return false;
		}
	}
	return true;
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
	ps_info->fp16_overflow       = regs->ps_regs.rsrc1.fp16_overflow;
	ps_info->fp16_overflow_known = !regs->ps_embedded && regs->ps_regs.rsrc1.fp16_overflow_known;
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
	ps_info->front_face_all_bits  = (sh->baryc_cntl & (1u << 24u)) != 0;
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
			    return RenderResolutionShaderAnalysis {AnalyzeResolutionShaderUsage(*code), code, std::make_shared<ShaderNativeWaveVerdict>()};
		    });
		ps_info->integer_image_coordinates = analysis.usage.integer_image_coordinates;
		ps_info->image_size_query          = analysis.usage.image_size_query;
		ps_info->native_wave = analysis.native_wave->Get(*analysis.code, ps_wave32 ? 32u : 64u);
		ps_info->required_subgroup_size = ShaderUsesNativeWaveState(*analysis.code) ? ps_info->native_wave.guest_wave_size : 0u;
		ps_info->has_only_null_exports     = ShaderHasOnlyNullPixelExports(*analysis.code);
		if (allow_noop_stage_disable && !ShaderPreventsNoopPixelElision(*analysis.code))
		{
			ps_info->stage_enabled = false;
			return;
		}
		ShaderResolveCustomInterpolation(*analysis.code, *vs_info, ps_info);
		ShaderParseUsage2(data.user_data, &usage, &ps_info->bind, regs->ps_user_sgpr, regs->ps_regs.rsrc2.user_sgpr, analysis.code.get(), 0,
		                  false);
		// GETPC uses runtime metadata so relocated pixel programs keep their
		// inline tables. A constructed V# must read guest bytes, not index the
		// descriptor array using its base address.
		ps_info->bind.program_base_used = analysis.code->HasAnyOf({ShaderInstructionType::SGetpcB64});
		ps_info->bind.program_base = ps_info->bind.program_base_used ? regs->ps_regs.data_addr : 0u;
		const auto& instructions = analysis.code->GetInstructions();
		ps_info->bind.device_address_used =
		    ShaderHasGlobalMemoryLoad(*analysis.code) ||
		    std::any_of(instructions.begin(), instructions.end(), [&bind = ps_info->bind](const auto& inst) {
			    return ShaderScalarBufferUsesRuntimeDescriptor(bind, inst) || ShaderScalarLoadNeedsGuestAddress(inst, bind);
		    });
	} else
	{
		ShaderParseUsage(regs->ps_regs.data_addr, &usage, &ps_info->bind, regs->ps_user_sgpr, regs->ps_regs.rsrc2.user_sgpr);
		ShaderCode code;
		code.SetType(ShaderType::Pixel);
		ShaderParseMapped(regs->ps_regs.data_addr, &code);
		ps_info->native_wave = ShaderAnalyzeNativeWave(code, 64u);
		ps_info->required_subgroup_size = ShaderUsesNativeWaveState(code) ? 64u : 0u;
	}

	// Gen5 user-data is shared by linked stages. A PS can therefore carry the
	// VS fetch/V#/attrib metadata in its direct-resource table even though its
	// instruction stream never consumes it. ShaderParseUsage2 records those
	// slots for the vertex path; they do not create PS descriptor bindings.
	if (usage.storage_buffers_readwrite > 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.storage_buffers_readwrite > 0 condition ignored (continuing)\n"); }
	if (usage.gds_pointers > 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage.gds_pointers > 0 condition ignored (continuing)\n"); }

	ShaderCalcBindingIndices(&ps_info->bind);
}

// Instructions that dereference guest memory through a computed address: BVH
// traversal, and scalar loads whose base is not the mapped extended pointer.
// A vector buffer load whose V# register range is not a bound storage buffer
// reads through a descriptor built at run time.
bool ShaderHasUnboundBufferLoad(const ShaderCode& code, const ShaderBindResources& bind, int user_data_register_base)
{
	for (const auto& inst: code.GetInstructions())
	{
		if (!ShaderInstructionTypeStartsWith(inst.type, "BufferLoad") || inst.src_num < 2 || inst.src[1].type != ShaderOperandType::Sgpr)
		{
			continue;
		}
		if (!ShaderStorageBufferResourceIsBound(bind, inst.src[1], user_data_register_base))
		{
			return true;
		}
	}
	return false;
}

bool ShaderHasGlobalMemoryLoad(const ShaderCode& code)
{
	return code.HasAnyOf({ShaderInstructionType::GlobalLoadDword, ShaderInstructionType::GlobalLoadDwordx2,
	                      ShaderInstructionType::GlobalLoadDwordx3, ShaderInstructionType::GlobalLoadDwordx4});
}

static bool ShaderUsesGuestDeviceAddress(const ShaderCode& code, const ShaderBindResources& bind)
{
	if (ShaderHasUnboundBufferLoad(code, bind) || ShaderHasGlobalMemoryLoad(code))
	{
		return true;
	}
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::ImageBvhIntersectRay)
		{
			return true;
		}
		if (ShaderScalarLoadNeedsGuestAddress(inst, bind))
		{
			return true;
		}
	}
	return false;
}

void ShaderGetInputInfoCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* /*sh*/, uint32_t dispatch_mode,
                          ShaderComputeInputInfo* info)
{
	EXIT_IF(info == nullptr);
	EXIT_IF(regs == nullptr);

	info->dispatch_mode  = dispatch_mode;
	info->float_mode     = regs->cs_regs.float_mode;
	info->dx10_clamp     = regs->cs_regs.dx10_clamp;
	info->ieee_mode      = regs->cs_regs.ieee_mode;
	info->fp_mode_known  = regs->cs_regs.fp_mode_known;
	info->fp16_overflow       = regs->cs_regs.fp16_overflow;
	info->fp16_overflow_known = regs->cs_regs.fp16_overflow_known;
	info->bind                = {};
	info->meta_fill           = {};
	info->uniform_buffer_fill = {};
	info->threads_num[0] = regs->cs_regs.num_thread_x;
	info->threads_num[1] = regs->cs_regs.num_thread_y;
	info->threads_num[2] = regs->cs_regs.num_thread_z;
	// COMPUTE_PGM_RSRC2.LDS_SIZE is expressed in 128-dword allocation units.
	info->lds_dwords                    = ShaderComputeLdsDwords(regs->cs_regs.lds_size);
	info->barrier_workspace_dwords      = 0;
	info->group_id[0]                   = regs->cs_regs.tgid_x_en != 0;
	info->group_id[1]                   = regs->cs_regs.tgid_y_en != 0;
	info->group_id[2]                   = regs->cs_regs.tgid_z_en != 0;
	info->thread_ids_num                = regs->cs_regs.tidig_comp_cnt + 1;
	info->storage_image_write_only_mask = 0;
	info->empty_gate = {};
	for (auto& coverage: info->storage_image_tile_coverage)
	{
		coverage = {};
	}

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
		// Resource-coupled S_LOAD admission needs the exact per-PC EUD mapping.
		// This is still before shader-cache, pipeline and descriptor preparation.
		if (info->native_equivalence_required)
		{
			const auto native = ShaderAnalyzeComputeWaveNativeEquivalence(code);
			if (!native.supported)
			{
				EXIT("wave64 dispatch has no paired layout and is not wave-width independent: mode=0x%08" PRIx32
				     " local=%ux%ux%u pc=0x%08" PRIx32 " reason=%s\n",
				     dispatch_mode, info->threads_num[0], info->threads_num[1], info->threads_num[2], native.unsupported_pc,
				     native.reason.c_str());
			}
		} else if (info->wave_layout.strategy == ShaderComputeWaveStrategy::Paired64On32)
		{
			// A program proven wave-width independent runs one guest lane per
			// invocation: half the code of paired banks, over the same workgroup.
			const auto native = info->native_equivalent_valid ? ShaderAnalyzeComputeWaveNativeEquivalence(code)
			                                                  : ShaderComputeWaveAnalysisResult {};
			const auto admission = native.supported ? ShaderComputeWaveAnalysisResult {true, 0, {}}
			                                        : ShaderAnalyzeComputeWaveCode(code, *info);
			if (native.supported)
			{
				info->wave_layout = info->native_equivalent_layout;
			} else if (!admission.supported)
			{
				std::string dump_note;
				if (const char* dump_dir = std::getenv("KYTY_TRANSPORT_DUMP"); dump_dir != nullptr && dump_dir[0] != '\0')
				{
					dump_note = " (compute dump: " + ShaderDumpGuestProgram(dump_dir, "cs", regs->cs_regs.chksum, regs->cs_regs.data_addr, code) + ")";
				}
				EXIT("paired-wave dispatch admission unsupported: mode=0x%08" PRIx32 " pc=0x%08" PRIx32 " reason=%s; "
				     "native-equivalence %s pc=0x%08" PRIx32 " reason=%s%s\n",
				     dispatch_mode, admission.unsupported_pc, admission.reason.c_str(),
				     info->native_equivalent_valid ? "rejected" : "unavailable", native.unsupported_pc, native.reason.c_str(),
				     dump_note.c_str());
			}
		}
		info->barrier_workspace_dwords = ShaderComputeBarrierWorkspaceDwords(code, info->wave_layout);
		// Compute parsing preserves byte PCs relative to this dispatch's start.
		// The base must remain runtime data when a cached pipeline is relocated.
		info->bind.program_base_used = code.HasAnyOf({ShaderInstructionType::SGetpcB64});
		info->bind.program_base      = info->bind.program_base_used ? regs->cs_regs.data_addr : 0u;
		info->bind.device_address_used = ShaderUsesGuestDeviceAddress(code, info->bind);
		info->bind.thread_limits_used = info->thread_limits_used;
		for (int axis = 0; axis < 3; axis++)
		{
			info->bind.thread_limits[axis] = info->thread_limits[axis];
		}
		info->uniform_buffer_fill = AnalyzeShaderComputeUniformBufferFill(code);
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
			const bool native_xy_thread_ids =
			    info->wave_layout.strategy == ShaderComputeWaveStrategy::Native && info->thread_ids_num >= 2;
			const bool paired_xy_thread_ids =
			    info->wave_layout.strategy == ShaderComputeWaveStrategy::Paired64On32 && info->thread_ids_num >= 2;
			const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, info->bind, i, info->workgroup_register,
			                                                             info->threads_num, native_xy_thread_ids,
			                                                             info->group_id[0] && info->group_id[1],
			                                                             paired_xy_thread_ids);
			if (coverage.width != 0)
			{
				info->storage_image_tile_coverage[i] = coverage;
				descriptor.storage_image_write_only = true;
				info->storage_image_write_only_mask |= 1u << static_cast<uint32_t>(i);
				continue;
			}
			if (descriptor.dynamic_sload)
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
		info->empty_gate = AnalyzeShaderComputeEmptyGate(code, info->bind);
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
	for (uint32_t mapping = 0; mapping < bind.dynamic_sloads.records.Size(); ++mapping)
	{
		const auto& record = bind.dynamic_sloads.records.At(mapping);
		KYTY_LOG_DEBUG("\t DynamicSLoad %d: kind=%u resource=%d dst=%d pc=%08" PRIx32 " offset_dw=%d dwords=%d field=%d last_consumer=%08" PRIx32 "\n",
		       static_cast<int>(mapping), static_cast<unsigned>(record.kind), record.resource_index,
		       record.destination_register, record.instruction_pc, record.offset_dw, record.dword_count,
		       record.resource_field_offset, record.last_consumer_pc);
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
	KYTY_LOG_DEBUG("\t fp16_overflow       = %s\n", info->fp16_overflow ? "true" : "false");
	KYTY_LOG_DEBUG("\t fp16_overflow_known = %s\n", info->fp16_overflow_known ? "true" : "false");

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
	KYTY_LOG_DEBUG("\t fp16_overflow       = %s\n", info->fp16_overflow ? "true" : "false");
	KYTY_LOG_DEBUG("\t fp16_overflow_known = %s\n", info->fp16_overflow_known ? "true" : "false");

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

	KYTY_LOG_DEBUG("\t fp_mode_known      = %s\n", info->fp_mode_known ? "true" : "false");
	KYTY_LOG_DEBUG("\t fp16_overflow      = %s\n", info->fp16_overflow ? "true" : "false");
	KYTY_LOG_DEBUG("\t fp16_overflow_known = %s\n", info->fp16_overflow_known ? "true" : "false");
	KYTY_LOG_DEBUG("\t float_mode         = 0x%02x\n", static_cast<uint32_t>(info->float_mode));
	KYTY_LOG_DEBUG("\t dx10_clamp         = %s\n", info->dx10_clamp ? "true" : "false");
	KYTY_LOG_DEBUG("\t ieee_mode          = %s\n", info->ieee_mode ? "true" : "false");
	KYTY_LOG_DEBUG("\t workgroup_register = %d\n", info->workgroup_register);
	KYTY_LOG_DEBUG("\t thread_ids_num     = %d\n", info->thread_ids_num);
	KYTY_LOG_DEBUG("\t threads_num        = {%u, %u, %u}\n", info->threads_num[0], info->threads_num[1], info->threads_num[2]);
	KYTY_LOG_DEBUG("\t threadgroup_id     = {%s, %s, %s}\n", info->group_id[0] ? "true" : "false", info->group_id[1] ? "true" : "false",
	       info->group_id[2] ? "true" : "false");

	ShaderDbgDumpResources(info->bind);
}


void ShaderRequireVertexProgram(const HW::VertexShaderInfo* regs, const ShaderVertexInputInfo* input_info)
{
	EXIT_IF(regs == nullptr || input_info == nullptr);
	if (!Config::IsNextGen() || regs->vs_embedded) { return; }
	if (input_info->program == nullptr) { EXIT("Gen5 vertex program owner is required before cache lookup\n"); }
	const bool gs_front =
	    regs->vs_regs.data_addr == 0 && regs->gs_regs.data_addr == 0 && regs->es_regs.data_addr != 0 && regs->gs_regs.chksum != 0;
	const auto& key = input_info->program->key;
	if (key.front_addr != (gs_front ? regs->es_regs.data_addr : regs->vs_regs.data_addr) || key.gs_front != gs_front ||
	    key.checksum != regs->gs_regs.chksum || key.bound_back_addr != regs->gs_back_addr)
	{
		EXIT("vertex program owner does not match the draw binding\n");
	}
	// Do not compare with the CURRENT registry generation: an in-flight draw
	// owns its prior immutable program even after a later remap or registration.
}

ShaderCode ShaderParseVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh,
                         const ShaderVertexInputInfo* input_info)
{
	EXIT_IF(sh == nullptr);
	ShaderRequireVertexProgram(regs, input_info);
	if (Config::IsNextGen() && !regs->vs_embedded)
	{
		vs_print("ShaderParseVS()", *regs, *sh);
		vs_check(*regs, *sh);
		return input_info->program->code;
	}
	return ShaderParseVS(regs, sh);
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

		ShaderCopyDebugPrintfs(&code);
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

		ShaderCopyDebugPrintfs(&code);
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

	ShaderCopyDebugPrintfs(&code);

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
	ret->ids.Add(static_cast<uint32_t>(bind.device_address_used));
	if (bind.device_address_used)
	{
		ret->ids.Add(bind.device_address_offset_dw);
	}
	ret->ids.Add(static_cast<uint32_t>(bind.thread_limits_used));
	if (bind.thread_limits_used)
	{
		ret->ids.Add(bind.thread_limits_offset_dw);
	}
	ret->ids.Add(static_cast<uint32_t>(bind.program_base_used));
	if (bind.program_base_used)
	{
		ret->ids.Add(bind.program_base_offset_dw);
	}
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

	ret->ids.Add(bind.dynamic_sloads.records.Size());
	for (uint32_t mapping = 0; mapping < bind.dynamic_sloads.records.Size(); ++mapping)
	{
		const auto& record = bind.dynamic_sloads.records.At(mapping);
		ret->ids.Add(static_cast<uint32_t>(record.kind));
		ret->ids.Add(record.resource_index);
		ret->ids.Add(record.destination_register);
		ret->ids.Add(record.instruction_pc);
		ret->ids.Add(static_cast<uint32_t>(record.offset_dw));
		ret->ids.Add(static_cast<uint32_t>(record.dword_count));
		ret->ids.Add(static_cast<uint32_t>(record.resource_field_offset));
		ret->ids.Add(record.last_consumer_pc);
		ret->ids.Add(static_cast<uint32_t>(record.raw_vmem_oob_guarded));
	}

	ret->ids.Add(bind.assembled_descriptors.Size());
	for (const auto& record: bind.assembled_descriptors)
	{
		ret->ids.Add(record.consumer_pc);
		ret->ids.Add(static_cast<uint32_t>(record.register_id));
		ret->ids.Add(static_cast<uint32_t>(record.resource_index));
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
		// Storage resinfo admission depends on the resource's mip view.
		// A module admitted for one mip must not bypass that check on reuse.
		const bool storage = bind.textures2D.desc[i].usage == ShaderTextureUsage::ReadWrite;
		ret->ids.Add(storage ? r.BaseLevel() : 0u);
		ret->ids.Add(storage ? r.LastLevel() : 0u);
		ret->ids.Add(storage ? r.MaxMip() : 0u);
		// Image stores apply a channel selection the storage view cannot express.
		ret->ids.Add(storage && ShaderStorageImageSwizzleInShader(r.DstSelXYZW()) ? r.DstSelXYZW() : 0u);
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
		ret->ids.Add(static_cast<uint32_t>(bind.textures2D.desc[i].textures2d_without_sampler));
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
		// Manual comparison filtering specializes the base-level footprint.
		ret->ids.Add(r.MinLod());
		ret->ids.Add(r.FilterMode());
		ret->ids.Add(r.XyMagFilter());
		ret->ids.Add(r.XyMinFilter());
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
		ret->ids.Add(static_cast<uint32_t>(bind.direct_sgprs.absolute_register[i]));
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
		// Keep metadata-only identity fixtures usable. Production callers require
		// the owner BEFORE lookup, including hits that never invoke the compiler.
		if (input_info->program != nullptr)
		{
			ShaderRequireVertexProgram(regs, input_info);
			ret.hash0 = input_info->program->code.GetHash0();
			ret.crc32 = input_info->program->code.GetCrc32();
			ret.ids.Add(0x4c565031u); // LVP1: exact copied front/back content and frozen debug commands.
			for (uint32_t word: input_info->program->fingerprint) { ret.ids.Add(word); }
		}
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
	ret.ids.Add(static_cast<uint32_t>(input_info->position1_usage));
	ret.ids.Add(0x4e574d31u); // NWM1: architectural width and native mapping proof.
	ret.ids.Add(input_info->native_wave.guest_wave_size);
	ret.ids.Add(static_cast<uint32_t>(input_info->native_wave.proof));
	ret.ids.Add(input_info->required_subgroup_size);
	ret.ids.Add(input_info->native_wave.refusal_reason != nullptr ? 1u : 0u);
	ret.ids.Add(input_info->float_mode);
	ret.ids.Add(static_cast<uint32_t>(input_info->dx10_clamp));
	ret.ids.Add(static_cast<uint32_t>(input_info->ieee_mode));
	ret.ids.Add(static_cast<uint32_t>(input_info->fp16_overflow));
	ret.ids.Add(static_cast<uint32_t>(input_info->fp16_overflow_known));
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
	if (input_info->FrontFaceEnabled())
	{
		ret.ids.Add(0x46464931u); // FFI1: explicit front-face system input.
		ret.ids.Add(static_cast<uint32_t>(input_info->front_face_all_bits));
	}
	if (input_info->custom_interpolation.Enabled())
	{
		ret.ids.Add(0x43504932u); // CPI2: raw inputs and qualified parameter aliases through geometry.
		ret.ids.Add(input_info->custom_interpolation.inputs);
		ret.ids.Add(input_info->custom_interpolation.per_vertex_inputs);
		ret.ids.Add(input_info->custom_interpolation.aliased_parameter_inputs);
	}
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_pos_xy));
	ret.ids.Add(input_info->host_to_guest_scale.x_guest_numerator);
	ret.ids.Add(input_info->host_to_guest_scale.x_host_denominator);
	ret.ids.Add(input_info->host_to_guest_scale.y_guest_numerator);
	ret.ids.Add(input_info->host_to_guest_scale.y_host_denominator);
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_pixel_kill_enable));
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_early_z));
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_execute_on_noop));
	ret.ids.Add(0x4e574d31u); // NWM1: distinct from the selected physical size.
	ret.ids.Add(input_info->native_wave.guest_wave_size);
	ret.ids.Add(static_cast<uint32_t>(input_info->native_wave.proof));
	ret.ids.Add(input_info->required_subgroup_size);
	ret.ids.Add(input_info->native_wave.refusal_reason != nullptr ? 1u : 0u);
	ret.ids.Add(input_info->float_mode);
	ret.ids.Add(static_cast<uint32_t>(input_info->dx10_clamp));
	ret.ids.Add(static_cast<uint32_t>(input_info->ieee_mode));
	ret.ids.Add(static_cast<uint32_t>(input_info->fp16_overflow));
	ret.ids.Add(static_cast<uint32_t>(input_info->fp16_overflow_known));

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
		case Mrt4Vsrc0Vsrc1ComprVm:
		case Mrt4Vsrc0Vsrc1Vsrc2Vsrc3Vm: target = 4u; break;
		case Mrt5Vsrc0Vsrc1ComprVm:
		case Mrt5Vsrc0Vsrc1Vsrc2Vsrc3Vm: target = 5u; break;
		case Mrt6Vsrc0Vsrc1ComprVm:
		case Mrt6Vsrc0Vsrc1Vsrc2Vsrc3Vm: target = 6u; break;
		case Mrt7Vsrc0Vsrc1ComprVm:
		case Mrt7Vsrc0Vsrc1Vsrc2Vsrc3Vm: target = 7u; break;
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
	if (config.sample_ordinal >= code.GetInstructions().Size()) { return false; }
	const auto& inst = code.GetInstructions().At(config.sample_ordinal);
	return inst.type == ShaderInstructionType::ImageSampleB ||
	       (inst.type == ShaderInstructionType::ImageSample && inst.mimg_dimension == 1u &&
	        inst.format == ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF) ||
	       (inst.type == ShaderInstructionType::ImageSampleLz && inst.mimg_dimension == 1u &&
	        (inst.format == ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7 ||
	         inst.format == ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF));
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

	ret.ids.Add(input_info->float_mode);
	ret.ids.Add(static_cast<uint32_t>(input_info->dx10_clamp));
	ret.ids.Add(static_cast<uint32_t>(input_info->ieee_mode));
	ret.ids.Add(static_cast<uint32_t>(input_info->fp_mode_known));
	ret.ids.Add(static_cast<uint32_t>(input_info->fp16_overflow));
	ret.ids.Add(static_cast<uint32_t>(input_info->fp16_overflow_known));
	ret.ids.Add(input_info->workgroup_register);
	ret.ids.Add(input_info->thread_ids_num);
	ret.ids.Add(input_info->lds_dwords);
	ret.ids.Add(static_cast<uint32_t>(input_info->wave_layout.strategy));
	ret.ids.Add(input_info->wave_layout.guest_wave_size);
	ret.ids.Add(input_info->wave_layout.native_subgroup_size);
	ret.ids.Add(input_info->wave_layout.banks);
	ret.ids.Add(input_info->wave_layout.waves);

	for (int i = 0; i < 3; i++)
	{
		ret.ids.Add(input_info->threads_num[i]);
		ret.ids.Add(static_cast<uint32_t>(input_info->group_id[i]));
		ret.ids.Add(input_info->wave_layout.guest_local[i]);
		ret.ids.Add(input_info->wave_layout.physical_local[i]);
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
	std::unique_lock lifetime_lock(g_shader_lifetime_mutex);
	ShaderInvalidateVertexProgramsLocked();
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
