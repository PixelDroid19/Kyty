#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicsRun.h"
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
#include <mutex>
#include <unordered_map>
#include <vector>

// #define SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
// #include "spirv_cross/spirv_glsl.hpp"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static RenderResolutionShaderUsageCache g_shader_resolution_usage_cache(512);


static void RecordShaderInputAnalysis(uint64_t elapsed_ns)
{
	DebugStatsRecordShaderIrParse(DebugStatsShaderParseKind::InputAnalysis, elapsed_ns);
}

static void RecordShaderPipelineMissParse(uint64_t elapsed_ns)
{
	DebugStatsRecordShaderIrParse(DebugStatsShaderParseKind::PipelineMiss, elapsed_ns);
}

static bool ShaderIsVccCompare(ShaderInstructionType type)
{
	const auto value = static_cast<uint32_t>(type);
	return (value >= static_cast<uint32_t>(ShaderInstructionType::VCmpEqF32) &&
	        value <= static_cast<uint32_t>(ShaderInstructionType::VCmpTU32)) ||
	       (value >= static_cast<uint32_t>(ShaderInstructionType::VCmpxEqF32) &&
	        value <= static_cast<uint32_t>(ShaderInstructionType::VCmpxNeU32));
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
		case ShaderOperandType::VccLo:
		case ShaderOperandType::VccHi:
		case ShaderOperandType::VccZ:
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
	if (inst.dst.type != ShaderOperandType::Vgpr || inst.dst.register_id != register_id)
	{
		return false;
	}

	return inst.dst.size <= 1;
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
	       inst.dst2.type == ShaderOperandType::ExecHi || inst.dst2.type == ShaderOperandType::ExecZ;
}

static bool ShaderOperandIsWaveUniform(const ShaderCode& code, const ShaderOperand& operand, uint32_t use_index,
	                                   uint32_t depth)
{
	if (ShaderIsWaveScalarOperand(operand))
	{
		return true;
	}
	if (operand.type != ShaderOperandType::Vgpr || depth >= 32 || use_index > code.GetInstructions().Size())
	{
		return false;
	}

	for (int index = static_cast<int>(use_index) - 1; index >= 0; --index)
	{
		const auto& definition = code.GetInstructions().At(static_cast<uint32_t>(index));
		if (!ShaderInstructionWritesVgpr(definition, operand.register_id))
		{
			continue;
		}

		if (!ShaderInstructionIsPureLaneAlu(definition))
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
	const auto compare_value = static_cast<uint32_t>(compare.type);
	const bool changes_exec = compare_value >= static_cast<uint32_t>(ShaderInstructionType::VCmpxEqF32) &&
	                          compare_value <= static_cast<uint32_t>(ShaderInstructionType::VCmpxNeU32);
	if (changes_exec || compare.src_num < 2)
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
static std::unordered_map<uint64_t, int32_t>*          g_vertex_offset_sgpr_map = nullptr;
static std::unordered_map<uint64_t, uint64_t>*         g_shader_continuations  = nullptr;

void ShaderInit()
{
	EXIT_IF(g_shader_map != nullptr);

	g_shader_map             = new std::unordered_map<uint64_t, ShaderMappedData>();
	g_vertex_offset_sgpr_map = new std::unordered_map<uint64_t, int32_t>();
	g_shader_continuations   = new std::unordered_map<uint64_t, uint64_t>();
}

void ShaderMapUserData(uint64_t addr, const ShaderMappedData& data)
{
	EXIT_IF(g_shader_map == nullptr);

	std::scoped_lock lock(g_shader_map_mutex);
	g_shader_map->insert_or_assign(addr, data);
}

void ShaderRegisterContinuation(uint64_t front_code_addr, uint64_t back_code_addr)
{
	EXIT_IF(g_shader_continuations == nullptr);
	if (front_code_addr == 0 || back_code_addr == 0 || front_code_addr == back_code_addr)
	{
		return;
	}
	std::scoped_lock lock(g_shader_map_mutex);
	g_shader_continuations->insert_or_assign(front_code_addr, back_code_addr);
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

// Linearize a Gen5 fused front→back chain: append the back half after the
// front's instructions and rewrite terminal s_setpc into a static branch so
// the SPIR-V CFG reaches position/param exports in the back half.
static void ShaderAppendContinuation(ShaderCode* code, uint64_t back_code_addr)
{
	EXIT_IF(code == nullptr || back_code_addr == 0);
	if (!code->HasAnyOf({ShaderInstructionType::SSetpcB64}))
	{
		return;
	}

	const auto* back_src = reinterpret_cast<const uint32_t*>(back_code_addr);
	if (back_src == nullptr)
	{
		return;
	}

	ShaderCode back;
	back.SetType(code->GetType());
	ShaderParse(back_src, &back);
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

	// Rewrite every s_setpc to a static branch into the back entry. Internal
	// getpc+add setpcs that already target a PC inside the front half would
	// need per-site resolution; fused halves always jump to the back entry.
	for (auto& inst: code->GetInstructions())
	{
		if (inst.type != ShaderInstructionType::SSetpcB64)
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
	return true;
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

bool ShaderPixelInputMaskSupported(uint32_t enable_mask, uint32_t address_mask)
{
	constexpr uint32_t kPerspectiveCenter   = 1u << 1u;
	constexpr uint32_t kPerspectiveCentroid = 1u << 2u;
	constexpr uint32_t kLinearCenter        = 1u << 5u;
	constexpr uint32_t kLinearCentroid      = 1u << 6u;
	constexpr uint32_t kPositionXy        = (1u << 8u) | (1u << 9u);
	constexpr uint32_t kInterpolation      = kPerspectiveCenter | kPerspectiveCentroid | kLinearCenter | kLinearCentroid;
	constexpr uint32_t kSupported          = kInterpolation | kPositionXy;

	if (enable_mask != address_mask || (enable_mask & ~kSupported) != 0)
	{
		return false;
	}
	// An all-zero mask is a pixel shader that reads no barycentrics and no
	// position: it exports a constant, discards, or samples with shader-supplied
	// coordinates only. It has no interpolation instructions to translate, so
	// there is nothing to reject.
	if (enable_mask == 0)
	{
		return true;
	}
	const uint32_t interpolation = enable_mask & kInterpolation;
	const uint32_t position      = enable_mask & kPositionXy;
	return interpolation != 0 && (position == 0 || position == kPositionXy);
}

bool ShaderPixelPositionEnabled(uint32_t enable_mask, uint32_t address_mask)
{
	constexpr uint32_t kPositionXy = (1u << 8u) | (1u << 9u);
	return (enable_mask & kPositionXy) == kPositionXy && (address_mask & kPositionXy) == kPositionXy;
}

uint32_t ShaderResolvePixelInterpolatorSetting(uint32_t stored_setting, uint32_t written_mask, uint32_t index)
{
	EXIT_IF(index >= 32u);
	return ((written_mask & (1u << index)) != 0 ? stored_setting : index);
}

uint32_t ShaderPixelCanonicalInterpolator(const ShaderPixelInputInfo& info, uint32_t index)
{
	EXIT_IF(index >= info.input_num);
	const uint32_t setting = info.interpolator_settings[index];
	for (uint32_t i = 0; i < index; ++i)
	{
		if (info.interpolator_settings[i] == setting)
		{
			return i;
		}
	}
	return index;
}

bool ShaderDecodePixelInterpolator(uint32_t setting, ShaderPixelInterpolator* interpolator)
{
	constexpr uint32_t kOffsetMask       = 0x3fu;
	constexpr uint32_t kDefaultOffset    = 0x20u;
	constexpr uint32_t kDefaultValueMask = 0x300u;
	constexpr uint32_t kFlatMask         = 0x400u;
	constexpr uint32_t kKnownMask        = kOffsetMask | kDefaultValueMask | kFlatMask;

	EXIT_IF(interpolator == nullptr);
	if ((setting & ~kKnownMask) != 0)
	{
		return false;
	}

	const uint32_t offset = setting & kOffsetMask;
	if (offset < kDefaultOffset)
	{
		if ((setting & kDefaultValueMask) != 0)
		{
			return false;
		}
		interpolator->source        = ShaderPixelInterpolatorSource::Parameter;
		interpolator->location      = offset;
		interpolator->flat          = (setting & kFlatMask) != 0;
		interpolator->default_value = 0;
		return true;
	}

	// OFFSET=0x20 selects a fixed-function default vector. The flat form is
	// ambiguous with a parameter-cache mode and requires producer metadata.
	if (offset != kDefaultOffset || (setting & kFlatMask) != 0)
	{
		return false;
	}

	interpolator->source        = ShaderPixelInterpolatorSource::Default;
	interpolator->location      = 0;
	interpolator->flat          = false;
	interpolator->default_value = (setting & kDefaultValueMask) >> 8u;
	return true;
}

float ShaderPixelInterpolatorDefaultComponent(const ShaderPixelInterpolator& interpolator, uint32_t component)
{
	EXIT_IF(interpolator.source != ShaderPixelInterpolatorSource::Default);
	EXIT_IF(component >= 4u);

	const bool one = (component == 3u ? (interpolator.default_value & 0x1u) != 0 : (interpolator.default_value & 0x2u) != 0);
	return (one ? 1.0f : 0.0f);
}


static const ShaderBinaryInfo* GetBinaryInfo(const uint32_t* code)
{
	EXIT_IF(code == nullptr);

	if (code[0] == 0xBEEB03FF)
	{
		return reinterpret_cast<const ShaderBinaryInfo*>(code + static_cast<size_t>(code[1] + 1) * 2);
	}

	return nullptr;
}

static ShaderUsageInfo GetUsageSlots(const uint32_t* code)
{
	EXIT_IF(code == nullptr);

	const auto* binary_info = GetBinaryInfo(code);

	ShaderUsageInfo ret;

	if (binary_info != nullptr)
	{
		EXIT_NOT_IMPLEMENTED(binary_info->chunk_usage_base_offset_dw == 0);

		ret.usage_masks = (binary_info->chunk_usage_base_offset_dw == 0
		                       ? nullptr
		                       : reinterpret_cast<const uint32_t*>(binary_info) - binary_info->chunk_usage_base_offset_dw);
		ret.slots_num   = binary_info->num_input_usage_slots;
		ret.slots       = (ret.slots_num == 0 ? nullptr : reinterpret_cast<const ShaderUsageSlot*>(ret.usage_masks) - ret.slots_num);
		ret.valid       = true;
	}

	return ret;
}

static void ShaderDetectBuffers(ShaderVertexInputInfo* info, bool ps5)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr);

	info->buffers_num = 0;

	for (int ri = 0; ri < info->resources_num; ri++)
	{
		const auto& r = info->resources[ri];
		// Empty V# descriptors occur in Gen5 metadata for attributes that are
		// inactive in the current draw. They have no backing range and cannot be
		// represented as a Vulkan vertex-buffer binding.
		if (r.NumRecords() == 0)
		{
			continue;
		}

		bool merged = false;
		for (int bi = 0; bi < info->buffers_num; bi++)
		{
			auto& b = info->buffers[bi];

			uint64_t stride = b.stride;

			if (stride == r.Stride())
			{
				uint64_t rbase   = (ps5 ? r.Base48() : r.Base44());
				uint64_t base    = std::min(rbase, b.addr);
				uint64_t offset1 = rbase - base;
				uint64_t offset2 = b.addr - base;

				if (offset1 < stride && offset2 < stride)
				{
					EXIT_NOT_IMPLEMENTED(b.num_records != r.NumRecords());
					b.addr = base;
					EXIT_NOT_IMPLEMENTED(b.attr_num >= ShaderVertexInputBuffer::ATTR_MAX);
					b.attr_indices[b.attr_num++] = ri;
					merged                       = true;
					break;
				}
			}
		}

		if (!merged)
		{
			EXIT_NOT_IMPLEMENTED(info->buffers_num >= ShaderVertexInputInfo::RES_MAX);
			int bi                            = info->buffers_num++;
			info->buffers[bi].addr            = (ps5 ? r.Base48() : r.Base44());
			info->buffers[bi].stride          = r.Stride();
			info->buffers[bi].num_records     = r.NumRecords();
			info->buffers[bi].attr_num        = 1;
			info->buffers[bi].attr_indices[0] = ri;
		}
	}

	for (int bi = 0; bi < info->buffers_num; bi++)
	{
		auto& b = info->buffers[bi];
		for (int ri = 0; ri < b.attr_num; ri++)
		{
			b.attr_offsets[ri] =
			    (ps5 ? info->resources[b.attr_indices[ri]].Base48() : info->resources[b.attr_indices[ri]].Base44()) - b.addr;
		}
	}
}

static void ShaderParseFetch(ShaderVertexInputInfo* info, const uint32_t* fetch, const uint32_t* buffer, uint32_t user_sgpr_num)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || fetch == nullptr || buffer == nullptr);

	KYTY_PROFILER_BLOCK("ShaderParseFetch::parse_code");

	ShaderCode code;
	code.SetType(ShaderType::Fetch);
	// shader_parse(0, fetch, nullptr, &code);
	{
		DebugStatsScopedTimer timer(RecordShaderInputAnalysis);
		ShaderParse(fetch, &code);
	}
	info->vertex_offset_sgpr = ShaderDetectVertexOffsetSgpr(code, 0, user_sgpr_num);

	KYTY_PROFILER_END_BLOCK;

	// KYTY_LOG_DEBUG("%s", code.DbgDump().c_str());

	KYTY_PROFILER_BLOCK("ShaderParseFetch::check_insts");

	const auto& insts = code.GetInstructions();
	uint32_t    size  = insts.Size();
	// int         temp_register = 0;
	uint32_t temp_value[104] = {0};
	int      s_num           = 0;
	int      v_num           = 0;

	for (uint32_t i = 0; i < size; i++)
	{
		const auto& inst = insts.At(i);

		if (inst.type == ShaderInstructionType::SLoadDwordx4)
		{
			EXIT_NOT_IMPLEMENTED(inst.src[1].type != ShaderOperandType::LiteralConstant || (inst.src[1].constant.u & 3u) != 0);
			EXIT_NOT_IMPLEMENTED(inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].register_id != 2);
			EXIT_NOT_IMPLEMENTED(inst.dst.type != ShaderOperandType::Sgpr);

			uint32_t index    = inst.src[1].constant.u >> 2u;
			int      t        = inst.dst.register_id;
			temp_value[t + 0] = buffer[index + 0];
			temp_value[t + 1] = buffer[index + 1];
			temp_value[t + 2] = buffer[index + 2];
			temp_value[t + 3] = buffer[index + 3];

			s_num++;
		}

		bool load_inst     = true;
		int  registers_num = 0;
		switch (inst.type)
		{
			case ShaderInstructionType::BufferLoadFormatX: registers_num = 1; break;
			case ShaderInstructionType::BufferLoadFormatXy: registers_num = 2; break;
			case ShaderInstructionType::BufferLoadFormatXyz: registers_num = 3; break;
			case ShaderInstructionType::BufferLoadFormatXyzw: registers_num = 4; break;
			default: load_inst = false;
		}

		if (load_inst && registers_num > 0)
		{
			// EXIT_NOT_IMPLEMENTED(!(i >= 2 && insts.At(i - 1).type == ShaderInstructionType::SWaitcnt &&
			//                       insts.At(i - 2).type == ShaderInstructionType::SLoadDwordx4));
			EXIT_NOT_IMPLEMENTED(inst.dst.type != ShaderOperandType::Vgpr);
			EXIT_NOT_IMPLEMENTED(inst.src[0].type != ShaderOperandType::Vgpr || inst.src[0].register_id != 0);
			EXIT_NOT_IMPLEMENTED(inst.src[1].type != ShaderOperandType::Sgpr);
			EXIT_NOT_IMPLEMENTED(inst.src[2].type != ShaderOperandType::IntegerInlineConstant || inst.src[2].constant.i != 0);

			EXIT_NOT_IMPLEMENTED(info->resources_num >= ShaderVertexInputInfo::RES_MAX);

			int t = inst.src[1].register_id;

			auto& r           = info->resources[info->resources_num];
			auto& rd          = info->resources_dst[info->resources_num];
			rd.register_start = inst.dst.register_id;
			rd.registers_num  = registers_num;
			rd.semantic       = info->resources_num;
			r.fields[0]       = temp_value[t + 0];
			r.fields[1]       = temp_value[t + 1];
			r.fields[2]       = temp_value[t + 2];
			r.fields[3]       = temp_value[t + 3];

			info->resources_num++;

			v_num++;
		}
	}

	KYTY_PROFILER_END_BLOCK;

	EXIT_NOT_IMPLEMENTED(s_num != v_num);
}

static void ShaderParseAttrib(ShaderVertexInputInfo* info, const ShaderSemantic* input_semantics, uint32_t num_input_semantics,
                              const uint32_t* attrib, const uint32_t* buffer)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || attrib == nullptr || buffer == nullptr);

	info->fetch_attrib_data_num = 0;

	uint32_t max_semantic = 0;
	for (uint32_t i = 0; i < num_input_semantics; i++)
	{
		if (input_semantics[i].semantic + 1u > max_semantic)
		{
			max_semantic = input_semantics[i].semantic + 1u;
		}
	}
	EXIT_NOT_IMPLEMENTED(max_semantic > static_cast<uint32_t>(ShaderVertexInputInfo::RES_MAX));
	for (uint32_t i = 0; i < max_semantic; i++)
	{
		info->fetch_attrib_data[i] = attrib[i];
	}
	info->fetch_attrib_data_num = static_cast<int>(max_semantic);
	static const bool vertex_attr_trace = std::getenv("KYTY_VERTEX_ATTR_TRACE") != nullptr;

	for (uint32_t i = 0; i < num_input_semantics; i++)
	{
		const auto& in = input_semantics[i];

		EXIT_NOT_IMPLEMENTED(in.static_vb_index == 1 || in.static_attribute == 1);

		uint32_t reg  = in.hardware_mapping;
		uint32_t size = in.size_in_elements;

		if (vertex_attr_trace)
		{
			KYTY_LOG_DEBUG("reg = %u, size = %u, va[%u] = 0x%08" PRIx32 "\n", reg, size, i, attrib[in.semantic]);
		}

		size_t   index       = attrib[in.semantic] & 0x1fu;
		uint32_t format      = (attrib[in.semantic] >> 5u) & 0x1ffu;
		uint32_t offset      = (attrib[in.semantic] >> 14u) & 0xfffu;
		uint32_t fetch_index = (attrib[in.semantic] >> 26u) & 0x1u;

		EXIT_NOT_IMPLEMENTED(fetch_index != 0);

		EXIT_NOT_IMPLEMENTED(index >= ShaderVertexInputInfo::RES_MAX);

		const auto* sharp = &buffer[index * 4];
		if (vertex_attr_trace)
		{
			static std::atomic_uint32_t vertex_attr_logs {0};
			const auto                  vertex_attr_log = vertex_attr_logs.fetch_add(1, std::memory_order_relaxed);
			if (vertex_attr_log < 32u)
			{
				KYTY_LOG_DEBUG(
				             "KYTY_VERTEX_ATTR semantic=%u raw=0x%08" PRIx32 " index=%zu format=0x%03" PRIx32 " offset=0x%03" PRIx32
				             " attrib=%p buffer=%p sharp=%p words=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
				             in.semantic, attrib[in.semantic], index, format, offset, static_cast<const void*>(attrib),
				             static_cast<const void*>(buffer), static_cast<const void*>(sharp), sharp[0], sharp[1], sharp[2], sharp[3]);
			}
		}

		EXIT_NOT_IMPLEMENTED(info->resources_num >= ShaderVertexInputInfo::RES_MAX);

		auto& r           = info->resources[info->resources_num];
		auto& rd          = info->resources_dst[info->resources_num];
		rd.register_start = static_cast<int>(reg);
		rd.semantic       = static_cast<int>(in.semantic);
		r.fields[0]       = sharp[0];
		r.fields[1]       = sharp[1];
		r.fields[2]       = sharp[2];
		r.fields[3]       = sharp[3];
		if (format != 0)
		{
			const auto     input_format    = VulkanResolveGen5VertexAttribInputFormat(static_cast<uint16_t>(format));
			const uint32_t component_count = input_format.component_count;
			EXIT_NOT_IMPLEMENTED(input_format.format == VK_FORMAT_UNDEFINED || component_count == 0);
			EXIT_NOT_IMPLEMENTED(size == 0 || size > 4);
			uint32_t swizzle = DstSel(4, 0, 0, 1);
			switch (component_count)
			{
				case 2: swizzle = DstSel(4, 5, 0, 1); break;
				case 3: swizzle = DstSel(4, 5, 6, 1); break;
				case 4: swizzle = DstSel(4, 5, 6, 7); break;
				default: break;
			}
			r.fields[3] = (r.fields[3] & ~((0x7fu << 12u) | 0xfffu)) |
			              (static_cast<uint32_t>(input_format.unified_format) << 12u) | swizzle;
			// The semantic controls the shader input width; the backing format
			// controls storage and descriptor swizzles. Either may have more
			// components: shaders can ignore stored components or consume the
			// descriptor's default channels.
			rd.registers_num = static_cast<int>(size);
		} else
		{
			rd.registers_num = static_cast<int>(size);
		}
		if (offset != 0)
		{
			const uint64_t base = r.Base48();
			EXIT_NOT_IMPLEMENTED(base > UINT64_MAX - offset);
			r.UpdateAddress48(base + offset);
		}

		info->resources_num++;
	}
}

static bool ShaderGetStorageBuffer(ShaderStorageResources* info, bool* direct_sgprs, int start_index, int slot, ShaderStorageUsage usage,
                                   const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer,
                                   ShaderStorageBindingSource source = ShaderStorageBindingSource::DirectResource)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->buffers_num < 0 || info->buffers_num >= ShaderStorageResources::BUFFERS_MAX);

	int  index    = info->buffers_num;
	bool extended = (extended_buffer != nullptr);

	// With Gen5 32-user-SGPR windows, slots 16..31 are direct user SGPRs (not
	// necessarily a separate EUD pointer). Only require extended when an
	// extended_buffer is supplied.
	if (extended)
	{
		EXIT_NOT_IMPLEMENTED(start_index < 16);
	} else
	{
		EXIT_NOT_IMPLEMENTED(start_index < 0 || start_index + 3 >= HW::UserSgprInfo::SGPRS_MAX);
	}

	ShaderBufferResource resource;
	resource.fields[0] = (extended ? extended_buffer[start_index - 16 + 0] : user_sgpr.value[start_index + 0]);
	resource.fields[1] = (extended ? extended_buffer[start_index - 16 + 1] : user_sgpr.value[start_index + 1]);
	resource.fields[2] = (extended ? extended_buffer[start_index - 16 + 2] : user_sgpr.value[start_index + 2]);
	resource.fields[3] = (extended ? extended_buffer[start_index - 16 + 3] : user_sgpr.value[start_index + 3]);

	// Fully zeroed sharp, or zero address+records with residual flag bits, is a
	// null buffer descriptor (Gen5 titles leave unused slots that way).
	if ((resource.fields[0] == 0 && resource.fields[1] == 0 && resource.fields[2] == 0 && resource.fields[3] == 0) ||
	    (resource.Base48() == 0 && resource.NumRecords() == 0))
	{
		return false;
	}

	info->start_register[index] = start_index;
	info->slots[index]          = slot;
	info->usages[index]         = usage;
	info->sources[index]        = source;
	info->extended[index]       = extended;
	info->buffers[index]        = resource;
	// info->extended_index[index] = extended_index;

	if (!extended)
	{
		for (int j = 0; j < 4; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			// Region/Vsharp markers may be unset when SGPRs were bulk-written;
			// Unknown is accepted for Gen5 full-window loads (reg_num=30).
			EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region && type != HW::UserSgprType::Unknown);

			direct_sgprs[start_index + j] = false;
		}
	}

	info->buffers_num++;
	return true;
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

	EXIT_NOT_IMPLEMENTED(resources->buffers_num >= ShaderZeroSBufferResources::BUFFERS_MAX);
	resources->start_register[resources->buffers_num++] = start_register;
}

void ShaderGetTextureBuffer(ShaderTextureResources* info, bool* direct_sgprs, int start_index, int slot, ShaderTextureUsage usage,
                                   const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->textures_num < 0 || info->textures_num >= ShaderTextureResources::RES_MAX);
	// EXIT_NOT_IMPLEMENTED(info->textures_num != slot);

	int  index    = info->textures_num;
	bool extended = (extended_buffer != nullptr);

	if (extended)
	{
		EXIT_NOT_IMPLEMENTED(start_index < 16);
	} else
	{
		EXIT_NOT_IMPLEMENTED(start_index < 0 || start_index + 7 >= HW::UserSgprInfo::SGPRS_MAX);
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
			EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region && type != HW::UserSgprType::Unknown);

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

	EXIT_NOT_IMPLEMENTED(info->samplers_num < 0 || info->samplers_num >= ShaderSamplerResources::RES_MAX);
	// EXIT_NOT_IMPLEMENTED(info->samplers_num != slot);

	int  index    = info->samplers_num;
	bool extended = (extended_buffer != nullptr);

	if (extended)
	{
		EXIT_NOT_IMPLEMENTED(start_index < 16);
	} else
	{
		EXIT_NOT_IMPLEMENTED(start_index < 0 || start_index + 3 >= HW::UserSgprInfo::SGPRS_MAX);
	}

	info->start_register[index] = start_index;
	info->extended[index]       = extended;
	info->slots[index]          = slot;

	if (!extended)
	{
		for (int j = 0; j < 4; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region && type != HW::UserSgprType::Unknown);

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

	EXIT_NOT_IMPLEMENTED(info->pointers_num < 0 || info->pointers_num >= ShaderGdsResources::POINTERS_MAX);
	// EXIT_NOT_IMPLEMENTED(info->pointers_num != slot);

	int  index    = info->pointers_num;
	bool extended = (extended_buffer != nullptr);

	EXIT_NOT_IMPLEMENTED(!extended && start_index >= 16);
	EXIT_NOT_IMPLEMENTED(extended && !(start_index >= 16));

	info->start_register[index] = start_index;
	info->extended[index]       = extended;
	info->slots[index]          = slot;

	if (!extended)
	{
		auto type = user_sgpr.type[start_index];
		EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Unknown);

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

	EXIT_NOT_IMPLEMENTED(info->sgprs_num < 0 || info->sgprs_num >= ShaderDirectSgprsResources::SGPRS_MAX);

	int index = info->sgprs_num;

	EXIT_NOT_IMPLEMENTED(start_index < 0 || start_index >= HW::UserSgprInfo::SGPRS_MAX);

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
		for (int i = 0; i < bind->textures2D.textures_num; ++i)
		{
			const auto& descriptor = bind->textures2D.desc[i];
			if (descriptor.usage != ShaderTextureUsage::ReadOnly ||
			    VulkanGen5ImageNumericType(descriptor.texture.Format()) != GuestImageNumericType::UnsignedInteger)
			{
				continue;
			}
			switch (ShaderGen5SampledTextureShapeForType(descriptor.texture.Type()))
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


// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderParseUsage(uint64_t addr, ShaderParsedUsage* info, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                      int user_sgpr_num)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(bind == nullptr);
	EXIT_IF(info == nullptr);

	const auto* src = reinterpret_cast<const uint32_t*>(addr);

	auto usages = GetUsageSlots(src);

	EXIT_NOT_IMPLEMENTED(!usages.valid);

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
				EXIT_NOT_IMPLEMENTED(usage.flags != 0 && usage.flags != 3);
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
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetSampler(&bind->samplers, direct_sgprs, usage.start_register, usage.slot, user_sgpr, extended_buffer);
				info->samplers++;
				break;

			case 0x02:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				if (ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, usage.start_register, usage.slot,
				                           ShaderStorageUsage::Constant, user_sgpr, extended_buffer))
				{
					info->storage_buffers_constant++;
				}
				break;

			case 0x04:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0 && usage.flags != 3);
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
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetGdsPointer(&bind->gds_pointers, direct_sgprs, usage.start_register, usage.slot, user_sgpr, extended_buffer);
				info->gds_pointers++;
				break;

			case 0x12:
				EXIT_NOT_IMPLEMENTED(usage.slot != 0);
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				info->fetch                            = true;
				info->fetch_reg                        = usage.start_register;
				direct_sgprs[usage.start_register]     = false;
				direct_sgprs[usage.start_register + 1] = false;
				break;

			case 0x17:
				EXIT_NOT_IMPLEMENTED(usage.slot != 0);
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				info->vertex_buffer                    = true;
				info->vertex_buffer_reg                = usage.start_register;
				direct_sgprs[usage.start_register]     = false;
				direct_sgprs[usage.start_register + 1] = false;
				break;

			case 0x1b:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				EXIT_NOT_IMPLEMENTED(usage.slot != 1);
				EXIT_NOT_IMPLEMENTED(bind->extended.used);
				EXIT_NOT_IMPLEMENTED(usage.start_register + 1 >= HW::UserSgprInfo::SGPRS_MAX);
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

	EXIT_NOT_IMPLEMENTED(user_data == nullptr);
	// Two Gen5 EUD layouts are evidenced:
	// 1) No type-5 pointer: descriptors live in the user-SGPR window; eud_size
	//    must fit in that window (earlier capture: eud=12, user_sgpr_num=30).
	// 2) Type-5 pointer: overflow sharp offsets are fetched from guest memory
	//    at that pointer (post-detile: S#@0x20/0x24 in a 12-dword EUD).
	const bool has_eud_ptr = Gen5HasEudPointer(user_data);
	if (user_data->eud_size_dw != 0)
	{
		EXIT_NOT_IMPLEMENTED(user_data->srt_size_dw != 0);
		EXIT_NOT_IMPLEMENTED(user_sgpr_num <= 0);
		if (!has_eud_ptr)
		{
			EXIT_NOT_IMPLEMENTED(static_cast<uint32_t>(user_sgpr_num) < user_data->eud_size_dw);
		}
	}
	EXIT_NOT_IMPLEMENTED(user_data->srt_size_dw > user_sgpr_num);

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
					EXIT_NOT_IMPLEMENTED(bind->extended.used);
					EXIT_NOT_IMPLEMENTED(reg < 0 || reg + 1 >= user_sgpr_num);
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
							EXIT_NOT_IMPLEMENTED(sampler_register < 0);
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
					EXIT_NOT_IMPLEMENTED(!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw));
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
					EXIT_NOT_IMPLEMENTED(!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw));
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
					EXIT_NOT_IMPLEMENTED(!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw));
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
					EXIT_NOT_IMPLEMENTED(!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw));
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

			EXIT_NOT_IMPLEMENTED(user_data->sharp_resource_offset[2][slot].size != 1);
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
				EXIT_NOT_IMPLEMENTED(!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw));
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

			EXIT_NOT_IMPLEMENTED(user_data->sharp_resource_offset[3][slot].size != 1);
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
				EXIT_NOT_IMPLEMENTED(!ShaderGen5EudSpanAllowed(api, dwords, user_data->eud_size_dw));
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
			// Each mapping is proven to reach an S_BUFFER_LOAD before a clobber.
			// Merge that local raw-use proof with any independent static use of the
			// same physical descriptor instead of assigning a synthetic entry state.
			if (exact_evidence.access == ShaderStorageAccess::Unknown)
			{
				exact_evidence.access = ShaderStorageAccess::Raw;
			} else if (exact_evidence.access == ShaderStorageAccess::Typed)
			{
				exact_evidence.access = ShaderStorageAccess::Mixed;
			}
			exact_evidence.raw_smem_use = true;
			exact_evidence.raw_smem_dynamic_offset = true;
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

int32_t ShaderResolveVertexOffset(uint32_t index_offset, const ShaderVertexInputInfo& input_info)
{
	if (index_offset != 0)
	{
		return static_cast<int32_t>(index_offset);
	}
	if (!input_info.fetch_external || input_info.vertex_offset_sgpr < 0)
	{
		return 0;
	}
	return static_cast<int32_t>(input_info.vertex_offset_value);
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

	if (regs->vs_embedded)
	{
		return;
	}

	ShaderParsedUsage usage;

	bool gs_instead_of_vs =
	    (regs->vs_regs.data_addr == 0 && regs->gs_regs.data_addr == 0 && regs->es_regs.data_addr != 0 && regs->gs_regs.chksum != 0);

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
		EXIT_NOT_IMPLEMENTED(data.user_data == nullptr);
		EXIT_NOT_IMPLEMENTED(!gs_instead_of_vs);

		info->gs_prolog = true;

		// The fused ES-to-GS front reserves s0..s7 for NGG system state. API
		// user-data index zero is therefore addressed as s8 by the shader.
		constexpr int kGen5GsFrontUserDataBase = 8;
		ShaderParseUsage2(data.user_data, &usage, &info->bind, user_sgpr, static_cast<int>(user_sgpr_num), nullptr,
		                  kGen5GsFrontUserDataBase);
	} else
	{
		EXIT_NOT_IMPLEMENTED(gs_instead_of_vs);

		info->gs_prolog = false;

		ShaderParseUsage(shader_addr, &usage, &info->bind, user_sgpr, user_sgpr_num);
	}

	EXIT_NOT_IMPLEMENTED(usage.extended_buffer);
	EXIT_NOT_IMPLEMENTED(usage.gds_pointers > 0);
	// Gen5 vertex shaders can use sampled textures/samplers for material and UI
	// paths. Descriptor allocation, sampler preparation and SPIR-V image sampling
	// are stage-generic here; keep the unsupported VS storage/GDS paths guarded.
	EXIT_NOT_IMPLEMENTED(usage.storage_buffers_readonly > 0);
	EXIT_NOT_IMPLEMENTED(usage.storage_buffers_readwrite > 0 || usage.textures2D_readwrite > 0);
	EXIT_NOT_IMPLEMENTED(!ps5 && ((usage.fetch && !usage.vertex_buffer) || (!usage.fetch && usage.vertex_buffer)));
	EXIT_NOT_IMPLEMENTED(ps5 && ((usage.vertex_attrib && !usage.vertex_buffer) || (!usage.vertex_attrib && usage.vertex_buffer)));

	if (usage.vertex_buffer && usage.vertex_attrib)
	{
		info->fetch_external   = false;
		info->fetch_embedded   = true;
		info->fetch_inline     = false;
		info->fetch_attrib_reg = usage.vertex_attrib_reg;
		info->fetch_buffer_reg = usage.vertex_buffer_reg;

		EXIT_NOT_IMPLEMENTED(usage.vertex_attrib_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX);
		EXIT_NOT_IMPLEMENTED(usage.vertex_buffer_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX);

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

		EXIT_NOT_IMPLEMENTED(data.input_semantics == nullptr || data.num_input_semantics == 0);

		ShaderParseAttrib(info, data.input_semantics, data.num_input_semantics, attrib, buffer);
		ShaderDetectBuffers(info, ps5);

		constexpr uint32_t user_data_base = 8;
		auto               cached         = g_vertex_offset_sgpr_map->find(shader_addr);
		if (cached == g_vertex_offset_sgpr_map->end())
		{
			ShaderCode code;
			code.SetType(ShaderType::Vertex);
			ShaderParse(reinterpret_cast<const uint32_t*>(shader_addr), &code);
			const int32_t detected = ShaderDetectVertexOffsetSgpr(code, user_data_base, user_sgpr_num);
			cached                 = g_vertex_offset_sgpr_map->insert({shader_addr, detected}).first;
		}
		info->vertex_offset_sgpr = cached->second;
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

		EXIT_NOT_IMPLEMENTED(usage.fetch_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX);
		EXIT_NOT_IMPLEMENTED(usage.vertex_buffer_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX);

		const auto* fetch = reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(user_sgpr.value[usage.fetch_reg]) |
		                                                      (static_cast<uint64_t>(user_sgpr.value[usage.fetch_reg + 1]) << 32u));
		const auto* buffer =
		    reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(user_sgpr.value[usage.vertex_buffer_reg]) |
		                                      (static_cast<uint64_t>(user_sgpr.value[usage.vertex_buffer_reg + 1]) << 32u));

		EXIT_NOT_IMPLEMENTED(fetch == nullptr || buffer == nullptr);

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
                          ShaderPixelInputInfo* ps_info)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(vs_info == nullptr);
	EXIT_IF(ps_info == nullptr);
	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	*ps_info = {};
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
		EXIT_NOT_IMPLEMENTED(data.user_data == nullptr);

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
				    ShaderParse(reinterpret_cast<const uint32_t*>(regs->ps_regs.data_addr), code.get());
			    }
			    ShaderProbeWrite("ps", *code, nullptr, nullptr);
			    return RenderResolutionShaderAnalysis {AnalyzeResolutionShaderUsage(*code), code};
		    });
		ps_info->integer_image_coordinates = analysis.usage.integer_image_coordinates;
		ps_info->image_size_query          = analysis.usage.image_size_query;
		ps_info->required_subgroup_size    = ShaderPixelRequiredSubgroupSize(*analysis.code, ps_wave32);
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
	EXIT_NOT_IMPLEMENTED(usage.storage_buffers_readwrite > 0);
	EXIT_NOT_IMPLEMENTED(usage.gds_pointers > 0);

	ShaderCalcBindingIndices(&ps_info->bind);
}

void ShaderGetInputInfoCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* /*sh*/, ShaderComputeInputInfo* info)
{
	EXIT_IF(info == nullptr);
	EXIT_IF(regs == nullptr);

	info->bind           = {};
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
		EXIT_NOT_IMPLEMENTED(data.user_data == nullptr);
		ShaderCode code;
		code.SetType(ShaderType::Compute);
		{
			DebugStatsScopedTimer timer(RecordShaderInputAnalysis);
			ShaderParse(reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr), &code);
		}
		const auto user_sgpr_num =
		    ShaderResolveGen5UserSgprCount(regs->cs_regs.user_sgpr, regs->cs_user_sgpr.count, data.user_data->eud_size_dw);
		ShaderParseUsage2(data.user_data, &usage, &info->bind, regs->cs_user_sgpr, static_cast<int>(user_sgpr_num), &code, 0, false);
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
		KYTY_LOG_DEBUG("\t DynamicSLoad %d: kind=%u resource=%d dst=%d pc=%08" PRIx32 " offset_dw=%d dwords=%d last_consumer=%08" PRIx32 "\n",
		       mapping, static_cast<unsigned>(bind.dynamic_sloads.kind[mapping]), bind.dynamic_sloads.resource_index[mapping],
		       bind.dynamic_sloads.destination_register[mapping], bind.dynamic_sloads.instruction_pc[mapping],
		       bind.dynamic_sloads.offset_dw[mapping], bind.dynamic_sloads.dword_count[mapping],
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

		EXIT_NOT_IMPLEMENTED(src == nullptr);

		vs_print("ShaderParseVS()", *regs, *sh);
		vs_check(*regs, *sh);

		if (gs_instead_of_vs)
		{
			EXIT_NOT_IMPLEMENTED(regs->gs_regs.rsrc2.user_sgpr > regs->gs_user_sgpr.count);
		} else
		{
			EXIT_NOT_IMPLEMENTED(regs->vs_regs.rsrc2.user_sgpr > regs->vs_user_sgpr.count);
		}

		if (Config::IsNextGen())
		{
			EXIT_NOT_IMPLEMENTED(!gs_instead_of_vs);

			hash0 = (regs->gs_regs.chksum >> 32u) & 0xffffffffu;
			crc32 = regs->gs_regs.chksum & 0xffffffffu;
		} else
		{
			const auto* header = GetBinaryInfo(src);

			EXIT_NOT_IMPLEMENTED(header == nullptr);

			bi_print("ShaderParseVS():ShaderBinaryInfo", *header);

			hash0 = header->hash0;
			crc32 = header->crc32;
		}

		code.SetCrc32(crc32);
		code.SetHash0(hash0);
		// shader_parse(0, src, nullptr, &code);
		{
			DebugStatsScopedTimer timer(RecordShaderPipelineMissParse);
			ShaderParse(src, &code);
		}

		if (gs_instead_of_vs)
		{
			const uint64_t continuation = ShaderLookupContinuation(shader_addr);
			if (continuation != 0)
			{
				ShaderAppendContinuation(&code, continuation);
			}
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

		EXIT_NOT_IMPLEMENTED(regs->ps_regs.rsrc2.user_sgpr > regs->ps_user_sgpr.count);

		const auto* src = reinterpret_cast<const uint32_t*>(regs->ps_regs.data_addr);

		EXIT_NOT_IMPLEMENTED(src == nullptr);

		if (Config::IsNextGen())
		{
			hash0 = (regs->ps_regs.chksum >> 32u) & 0xffffffffu;
			crc32 = regs->ps_regs.chksum & 0xffffffffu;
		} else
		{
			const auto* header = GetBinaryInfo(src);

			EXIT_NOT_IMPLEMENTED(header == nullptr);

			bi_print("ShaderParsePS():ShaderBinaryInfo", *header);

			hash0 = header->hash0;
			crc32 = header->crc32;
		}

		code.SetCrc32(crc32);
		code.SetHash0(hash0);
		// shader_parse(0, src, nullptr, &code);
		{
			DebugStatsScopedTimer timer(RecordShaderPipelineMissParse);
			ShaderParse(src, &code);
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

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	cs_print("ShaderParseCS()", regs->cs_regs, *sh);
	cs_check(regs->cs_regs, *sh);

	EXIT_NOT_IMPLEMENTED(regs->cs_regs.user_sgpr > regs->cs_user_sgpr.count);

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

		EXIT_NOT_IMPLEMENTED(header == nullptr);

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
		ShaderParse(src, &code);
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
		ret->ids.Add(bind.dynamic_sloads.last_consumer_pc[mapping]);
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
	}

	ret->ids.Add(bind.samplers.samplers_num);

	for (int i = 0; i < bind.samplers.samplers_num; i++)
	{
		// const auto& r = bind.samplers.samplers[i];

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
		ret->ids.Add(bind.samplers.slots[i]);
		ret->ids.Add(bind.samplers.start_register[i]);
		ret->ids.Add(static_cast<uint32_t>(bind.samplers.extended[i]));
		ret->ids.Add(static_cast<uint32_t>(bind.samplers.dynamic_sload[i]));
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
		EXIT_NOT_IMPLEMENTED(!gs_instead_of_vs);

		ret.hash0 = (regs->gs_regs.chksum >> 32u) & 0xffffffffu;
		ret.crc32 = regs->gs_regs.chksum & 0xffffffffu;
	} else
	{
		const auto* src = reinterpret_cast<const uint32_t*>(shader_addr);

		EXIT_NOT_IMPLEMENTED(src == nullptr);

		const auto* header = GetBinaryInfo(src);

		EXIT_NOT_IMPLEMENTED(header == nullptr);

		ret.hash0 = header->hash0;
		ret.crc32 = header->crc32;
		ret.ids.Add(header->length);
	}

	ret.ids.Add(static_cast<uint32_t>(input_info->fetch_external));
	ret.ids.Add(static_cast<uint32_t>(input_info->fetch_embedded));
	ret.ids.Add(static_cast<uint32_t>(input_info->fetch_inline));
	ret.ids.Add(static_cast<uint32_t>(input_info->gs_prolog));
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

		EXIT_NOT_IMPLEMENTED(src == nullptr);

		const auto* header = GetBinaryInfo(src);

		EXIT_NOT_IMPLEMENTED(header == nullptr);

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

	return ret;
}

ShaderId ShaderGetIdCS(const HW::ComputeShaderInfo* regs, const ShaderComputeInputInfo* input_info)
{
	const auto* src = reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr);

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	ShaderId ret;
	ret.ids.Expand(64);

	if (Config::IsNextGen())
	{
		ret.hash0 = (regs->cs_regs.chksum >> 32u) & 0xffffffffu;
		ret.crc32 = regs->cs_regs.chksum & 0xffffffffu;
	} else
	{
		const auto* header = GetBinaryInfo(src);

		EXIT_NOT_IMPLEMENTED(header == nullptr);

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
	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);
	EXIT_NOT_IMPLEMENTED(header == nullptr);

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
