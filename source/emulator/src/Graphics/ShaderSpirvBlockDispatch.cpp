#include "ShaderSpirvInternal.h"

#include <algorithm>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kExitBlock = 0xffffffffu;

bool IsGuestBranch(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SBranch:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchExecnz: return true;
		default: return false;
	}
}

bool EndsBlock(ShaderInstructionType type)
{
	return IsGuestBranch(type) || type == ShaderInstructionType::SEndpgm;
}

uint32_t BranchTarget(const ShaderInstruction& inst)
{
	return static_cast<uint32_t>(static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i);
}

} // namespace

// Paired compute programs run their guest CFG as a block dispatcher: one loop
// around a switch on the current block id. Every guest branch condition is a
// uniform SCC/VCC/EXEC word in paired mode, so the dispatcher is exact for any
// guest control flow, including loops with several back edges.
bool Spirv::UsesBlockDispatch() const
{
	if (!UsesComputeWaveBanks())
	{
		return false;
	}
	for (const auto& inst: m_code.GetInstructions())
	{
		if (IsGuestBranch(inst.type))
		{
			return true;
		}
	}
	return false;
}

void Spirv::BuildBlockDispatch()
{
	m_block_starts.clear();
	const auto& instructions = m_code.GetInstructions();
	for (uint32_t index = 0; index < instructions.Size(); index++)
	{
		const auto& inst = instructions.At(index);
		if (index == 0)
		{
			m_block_starts.push_back(inst.pc);
		}
		if (IsGuestBranch(inst.type))
		{
			m_block_starts.push_back(BranchTarget(inst));
		}
		if (EndsBlock(inst.type) && index + 1 < instructions.Size())
		{
			m_block_starts.push_back(instructions.At(index + 1).pc);
		}
	}
	std::sort(m_block_starts.begin(), m_block_starts.end());
	m_block_starts.erase(std::unique(m_block_starts.begin(), m_block_starts.end()), m_block_starts.end());
	// Only instruction boundaries can open a case; the control emitter fails
	// closed for a branch into the middle of an instruction.
	std::vector<uint32_t> pcs;
	for (const auto& inst: instructions)
	{
		pcs.push_back(inst.pc);
	}
	m_block_starts.erase(std::remove_if(m_block_starts.begin(), m_block_starts.end(),
	                                    [&](uint32_t pc) { return !std::binary_search(pcs.begin(), pcs.end(), pc); }),
	                     m_block_starts.end());
	m_block_terminated = false;
}

int Spirv::BlockId(uint32_t pc) const
{
	const auto it = std::lower_bound(m_block_starts.begin(), m_block_starts.end(), pc);
	return it != m_block_starts.end() && *it == pc ? static_cast<int>(it - m_block_starts.begin()) : -1;
}

String8 Spirv::BlockDispatchProlog() const
{
	String8 cases;
	for (size_t id = 0; id < m_block_starts.size(); id++)
	{
		cases += String8::FromPrintf(" %u %%cf_block_%u", static_cast<uint32_t>(id), static_cast<uint32_t>(id));
	}
	return String8::FromPrintf("OpStore %%cf_block %%uint_0\nOpBranch %%cf_header\n%%cf_header = OpLabel\n"
	                           "OpLoopMerge %%cf_merge %%cf_continue None\nOpBranch %%cf_dispatch\n%%cf_dispatch = OpLabel\n"
	                           "%%cf_current = OpLoad %%uint %%cf_block\nOpSelectionMerge %%cf_switch_merge None\n"
	                           "OpSwitch %%cf_current %%cf_switch_merge%s\n",
	                           cases.c_str());
}

// Opens the case of the block starting at instruction `index`, first leaving
// the previous block by fallthrough when it did not end in a branch.
String8 Spirv::BlockDispatchBoundary(uint32_t index)
{
	const int id = BlockId(m_code.GetInstructions().At(index).pc);
	if (id < 0)
	{
		return {};
	}
	String8 source;
	if (index != 0 && !m_block_terminated)
	{
		source += String8::FromPrintf("OpStore %%cf_block %%%s\nOpBranch %%cf_switch_merge\n", GetConstantUint(static_cast<uint32_t>(id)).c_str());
	}
	m_block_terminated = false;
	source += String8::FromPrintf("%%cf_block_%d = OpLabel\n", id);
	return source;
}

// Guest branches and s_endpgm select the next block and leave the case.
bool Spirv::BlockDispatchControl(const ShaderInstruction& inst, uint32_t index, String8* output)
{
	if (!EndsBlock(inst.type))
	{
		return false;
	}
	m_block_terminated = true;
	if (inst.type == ShaderInstructionType::SEndpgm)
	{
		*output += String8::FromPrintf("OpStore %%cf_block %%%s\nOpBranch %%cf_switch_merge\n", GetConstantUint(kExitBlock).c_str());
		return true;
	}
	const int target = BlockId(BranchTarget(inst));
	if (target < 0)
	{
		return false;
	}
	const auto target_id = GetConstantUint(static_cast<uint32_t>(target));
	if (inst.type == ShaderInstructionType::SBranch)
	{
		*output += String8::FromPrintf("OpStore %%cf_block %%%s\nOpBranch %%cf_switch_merge\n", target_id.c_str());
		return true;
	}
	const auto& instructions = m_code.GetInstructions();
	const int   next         = index + 1 < instructions.Size() ? BlockId(instructions.At(index + 1).pc) : -1;
	if (next < 0)
	{
		return false;
	}
	const char* lo      = "scc";
	const char* hi      = nullptr;
	bool        on_zero = false;
	switch (inst.type)
	{
		case ShaderInstructionType::SCbranchScc0: on_zero = true; break;
		case ShaderInstructionType::SCbranchScc1: break;
		case ShaderInstructionType::SCbranchVccz: on_zero = true; [[fallthrough]];
		case ShaderInstructionType::SCbranchVccnz:
			lo = "vcc_lo";
			hi = "vcc_hi";
			break;
		case ShaderInstructionType::SCbranchExecz: on_zero = true; [[fallthrough]];
		case ShaderInstructionType::SCbranchExecnz:
			lo = "exec_lo";
			hi = "exec_hi";
			break;
		default: return false;
	}
	*output += String8::FromPrintf("%%cf_lo_%u = OpLoad %%uint %%%s\n", index, lo);
	if (hi != nullptr)
	{
		*output += String8::FromPrintf("%%cf_hi_%u = OpLoad %%uint %%%s\n%%cf_word_%u = OpBitwiseOr %%uint %%cf_lo_%u %%cf_hi_%u\n", index, hi,
		                               index, index, index);
	} else
	{
		*output += String8::FromPrintf("%%cf_word_%u = OpCopyObject %%uint %%cf_lo_%u\n", index, index);
	}
	*output += String8::FromPrintf("%%cf_taken_%u = %s %%bool %%cf_word_%u %%uint_0\n"
	                               "%%cf_next_%u = OpSelect %%uint %%cf_taken_%u %%%s %%%s\n"
	                               "OpStore %%cf_block %%cf_next_%u\nOpBranch %%cf_switch_merge\n",
	                               index, on_zero ? "OpIEqual" : "OpINotEqual", index, index, index, target_id.c_str(),
	                               GetConstantUint(static_cast<uint32_t>(next)).c_str(), index);
	return true;
}

String8 Spirv::BlockDispatchEpilog() const
{
	String8 source;
	if (!m_block_terminated)
	{
		source += String8::FromPrintf("OpStore %%cf_block %%%s\nOpBranch %%cf_switch_merge\n", GetConstantUint(kExitBlock).c_str());
	}
	source += String8::FromPrintf("%%cf_switch_merge = OpLabel\nOpBranch %%cf_continue\n%%cf_continue = OpLabel\n"
	                              "%%cf_after = OpLoad %%uint %%cf_block\n%%cf_done = OpIEqual %%bool %%cf_after %%%s\n"
	                              "OpBranchConditional %%cf_done %%cf_merge %%cf_header\n%%cf_merge = OpLabel\nOpReturn\n",
	                              GetConstantUint(kExitBlock).c_str());
	return source;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
