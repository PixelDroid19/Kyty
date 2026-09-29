#include "ShaderSpirvInternal.h"

#include "Emulator/Graphics/ShaderComputeWaveControlFlowAnalysis.h"

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
	return IsGuestBranch(type) || type == ShaderInstructionType::SEndpgm || type == ShaderInstructionType::SBarrier;
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

bool Spirv::UsesBarrierPhases() const
{
	return UsesComputeWaveBanks() && ShaderComputeBarrierWorkspaceDwords(m_code, m_cs_input_info->wave_layout) != 0u;
}

String8 Spirv::BarrierPhaseTypes() const
{
	if (!UsesBarrierPhases())
	{
		return {};
	}
	return String8::FromPrintf("%%cf_phase_waves = OpConstant %%uint %u\n"
	                           "%%cf_phase_array = OpTypeArray %%uint %%cf_phase_waves\n"
	                           "%%cf_phase_array_ptr = OpTypePointer Workgroup %%cf_phase_array\n"
	                           "%%cf_phase_word_ptr = OpTypePointer Workgroup %%uint\n",
	                           m_cs_input_info->wave_layout.waves);
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
	String8 source = "OpStore %cf_block %uint_0\n";
	if (UsesBarrierPhases())
	{
		// Different guest waves can reach s_barrier on different dispatcher
		// iterations. Reconverge the workgroup outside that loop before waiting.
		source += "OpBranch %cf_phase_header\n%cf_phase_header = OpLabel\n"
		          "OpLoopMerge %cf_phase_merge %cf_phase_continue None\nOpBranch %cf_phase_start\n"
		          "%cf_phase_start = OpLabel\nOpStore %cf_phase_pending %uint_0\n";
	}
	return source + String8::FromPrintf("OpBranch %%cf_header\n%%cf_header = OpLabel\n"
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
	if (inst.type == ShaderInstructionType::SBarrier)
	{
		const auto& instructions = m_code.GetInstructions();
		const int next = index + 1 < instructions.Size() ? BlockId(instructions.At(index + 1).pc) : -1;
		EXIT_IF(!UsesBarrierPhases());
		if (next < 0)
		{
			EXIT("paired-wave barrier has no resumable instruction: pc=0x%08x\n", inst.pc);
		}
		*output += String8::FromPrintf("OpStore %%cf_block %%%s\nOpStore %%cf_phase_pending %%uint_1\n"
		                               "OpBranch %%cf_switch_merge\n", GetConstantUint(static_cast<uint32_t>(next)).c_str());
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

String8 Spirv::BarrierPhaseJoin() const
{
	String8 source = R"(
%cf_phase_current = OpLoad %uint %cf_block
%cf_phase_is_done = OpIEqual %bool %cf_phase_current %uint_0xffffffff
%cf_phase_done_word = OpSelect %uint %cf_phase_is_done %uint_1 %uint_0
%cf_phase_leader = OpIEqual %bool %wave_lane_id %uint_0
OpSelectionMerge %cf_phase_flags_written None
OpBranchConditional %cf_phase_leader %cf_phase_write_flag %cf_phase_flags_written
%cf_phase_write_flag = OpLabel
%cf_phase_flag_ptr = OpAccessChain %cf_phase_word_ptr %cf_phase_flags %wave_subgroup_id
OpStore %cf_phase_flag_ptr %cf_phase_done_word
OpBranch %cf_phase_flags_written
%cf_phase_flags_written = OpLabel
OpControlBarrier %uint_2 %uint_2 %uint_0x00000108
)";
	String8 all = "%uint_1";
	for (uint32_t wave = 0; wave < m_cs_input_info->wave_layout.waves; wave++)
	{
		source += String8::FromPrintf("%%cf_phase_read_ptr_%u = OpAccessChain %%cf_phase_word_ptr %%cf_phase_flags %%%s\n"
		                             "%%cf_phase_read_%u = OpLoad %%uint %%cf_phase_read_ptr_%u\n"
		                             "%%cf_phase_all_%u = OpBitwiseAnd %%uint %s %%cf_phase_read_%u\n",
		                             wave, GetConstantUint(wave).c_str(), wave, wave, wave, all.c_str(), wave);
		all = String8::FromPrintf("%%cf_phase_all_%u", wave);
	}
	// Every wave reads this generation before a fast wave can overwrite it.
	source += String8::FromPrintf("%%cf_phase_all_done = OpINotEqual %%bool %s %%uint_0\n"
	                              "OpControlBarrier %%uint_2 %%uint_2 %%uint_0x00000108\n"
	                              "OpBranch %%cf_phase_continue\n%%cf_phase_continue = OpLabel\n"
	                              "OpBranchConditional %%cf_phase_all_done %%cf_phase_merge %%cf_phase_header\n"
	                              "%%cf_phase_merge = OpLabel\nOpReturn\n", all.c_str());
	return source;
}

String8 Spirv::BlockDispatchEpilog() const
{
	String8 source;
	if (!m_block_terminated)
	{
		source += String8::FromPrintf("OpStore %%cf_block %%%s\nOpBranch %%cf_switch_merge\n", GetConstantUint(kExitBlock).c_str());
	}
	source += String8::FromPrintf("%%cf_switch_merge = OpLabel\nOpBranch %%cf_continue\n%%cf_continue = OpLabel\n"
	                              "%%cf_after = OpLoad %%uint %%cf_block\n%%cf_ended = OpIEqual %%bool %%cf_after %%%s\n",
	                              GetConstantUint(kExitBlock).c_str());
	if (UsesBarrierPhases())
	{
		source += "%cf_phase_pending_value = OpLoad %uint %cf_phase_pending\n"
		          "%cf_phase_waiting = OpINotEqual %bool %cf_phase_pending_value %uint_0\n"
		          "%cf_done = OpLogicalOr %bool %cf_ended %cf_phase_waiting\n";
	} else
	{
		source += "%cf_done = OpCopyObject %bool %cf_ended\n";
	}
	source += "OpBranchConditional %cf_done %cf_merge %cf_header\n%cf_merge = OpLabel\n";
	source += UsesBarrierPhases() ? BarrierPhaseJoin() : FragmentEpilog() + "OpReturn\n";
	return source;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
