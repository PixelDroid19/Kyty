#include "ShaderDeadVgprMask.h"

#include "ShaderLaneFlow.h"
#include "ShaderNativeWaveInternal.h"
#include "ShaderSpirvInternal.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

using namespace LaneFlow;
using Type      = ShaderInstructionType;
using Operand   = ShaderOperandType;
using VectorSet = std::bitset<kVgprs>;

constexpr uint32_t kMaxSavedCopies = 16;

struct BasicBlock
{
	uint32_t              begin = 0; // inclusive instruction index
	uint32_t              end   = 0; // exclusive instruction index
	std::vector<uint32_t> succs;
	std::vector<uint32_t> preds;
	bool                  ends_program = false;
};

struct SelectionRegion
{
	uint32_t              header_block = 0;
	uint32_t              entry_block  = 0;
	uint32_t              merge_block  = 0;
	std::vector<uint32_t> blocks; // topological order from entry_block to merge_block (exclusive)
};

enum class ExecRel : uint8_t
{
	AtEntry = 0,
	SubsetOfEntry,
	WidenedWqm,
	WidenedWqmSubset,
	Unknown,
};

struct SavedExecCopy
{
	unsigned first = 0;
	unsigned count = 0;
	ExecRel  rel   = ExecRel::Unknown;
};

bool IsWqmRel(ExecRel rel)
{
	return rel == ExecRel::WidenedWqm || rel == ExecRel::WidenedWqmSubset;
}

ExecRel JoinExecRel(ExecRel a, ExecRel b)
{
	if (a == b) { return a; }
	if (a == ExecRel::Unknown || b == ExecRel::Unknown) { return ExecRel::Unknown; }
	if (IsWqmRel(a) || IsWqmRel(b)) { return ExecRel::WidenedWqmSubset; }
	return ExecRel::SubsetOfEntry;
}

struct ExecState
{
	bool                                      reachable   = false;
	bool                                      valid       = true;
	bool                                      seen_exec64 = false;
	ExecRel                                   exec        = ExecRel::AtEntry;
	uint32_t                                  wqm_epoch   = 0;
	std::array<SavedExecCopy, kMaxSavedCopies> saved {};
	uint32_t                                  saved_count = 0;

	ExecRel FindExact(unsigned first, unsigned count) const
	{
		for (uint32_t i = 0; i < saved_count; ++i)
		{
			if (saved[i].first == first && saved[i].count == count) { return saved[i].rel; }
		}
		return ExecRel::Unknown;
	}

	void ClearOverlapping(unsigned first, unsigned count)
	{
		uint32_t write = 0;
		for (uint32_t i = 0; i < saved_count; ++i)
		{
			const auto& s = saved[i];
			if (first < s.first + s.count && s.first < first + count) { continue; }
			saved[write++] = s;
		}
		saved_count = write;
	}

	void SetSaved(unsigned first, unsigned count, ExecRel rel)
	{
		ClearOverlapping(first, count);
		if (rel == ExecRel::Unknown) { return; }
		if (saved_count < kMaxSavedCopies) { saved[saved_count++] = {first, count, rel}; }
	}

	bool Merge(const ExecState& other)
	{
		if (!other.reachable) { return false; }
		if (!reachable)
		{
			*this = other;
			return true;
		}
		if (!valid) { return false; }
		if (!other.valid)
		{
			valid       = false;
			exec        = ExecRel::Unknown;
			wqm_epoch   = 0;
			saved_count = 0;
			return true;
		}
		if (wqm_epoch != other.wqm_epoch)
		{
			valid       = false;
			exec        = ExecRel::Unknown;
			wqm_epoch   = 0;
			saved_count = 0;
			return true;
		}
		bool          changed     = (!seen_exec64 && other.seen_exec64);
		const ExecRel next_exec   = JoinExecRel(exec, other.exec);
		if (next_exec != exec)
		{
			exec    = next_exec;
			changed = true;
		}
		seen_exec64 = seen_exec64 || other.seen_exec64;
		if (exec == ExecRel::Unknown)
		{
			valid       = false;
			saved_count = 0;
			return true;
		}
		uint32_t write = 0;
		for (uint32_t i = 0; i < saved_count; ++i)
		{
			const auto&   s         = saved[i];
			const ExecRel other_rel = other.FindExact(s.first, s.count);
			if (other_rel == ExecRel::Unknown)
			{
				changed = true;
				continue;
			}
			const ExecRel joined = JoinExecRel(s.rel, other_rel);
			if (joined != s.rel) { changed = true; }
			saved[write++] = {s.first, s.count, joined};
		}
		if (write != saved_count)
		{
			saved_count = write;
			changed     = true;
		}
		return changed;
	}
};

bool IsBranchType(Type type)
{
	return type == Type::SBranch || StartsWith(type, "SCbranch");
}

bool ContainsType(Type type, std::string_view needle)
{
	return ShaderInstructionTypeName(type).find(needle) != std::string_view::npos;
}

bool UsesRelativeIndexing(const ShaderCode& code)
{
	for (const auto& inst: code.GetInstructions())
	{
		if (StartsWith(inst.type, "SMovrel") || StartsWith(inst.type, "VMovrel")) { return true; }
	}
	return false;
}

void AddVgprOp(const ShaderOperand& op, VectorSet* set)
{
	if (op.type != Operand::Vgpr || op.register_id < 0) { return; }
	const unsigned first = static_cast<unsigned>(op.register_id);
	const unsigned count = op.size > 0 ? static_cast<unsigned>(op.size) : 1u;
	for (unsigned reg = 0; reg < count && first + reg < static_cast<unsigned>(kVgprs); ++reg)
	{
		set->set(first + reg);
	}
}

uint32_t ImageDestinationCount(const ShaderInstruction& inst)
{
	if (inst.type == Type::ImageGather4 || inst.type == Type::ImageGather4C) { return 4u; }
	if (inst.mimg_dmask != 0)
	{
		uint32_t count = 0;
		for (uint32_t mask = inst.mimg_dmask; mask != 0; mask >>= 1u) { count += (mask & 1u); }
		return std::max<uint32_t>(count, inst.dst.size > 0 ? static_cast<uint32_t>(inst.dst.size) : 1u);
	}
	return inst.dst.size > 0 ? static_cast<uint32_t>(inst.dst.size) : 1u;
}

bool WritesFullDstDespiteOpSelAnd16(Type type)
{
	return type == Type::VFmaMixF32 || type == Type::VPackB32F16 || StartsWith(type, "VPk");
}

bool ReadsDestinationImplicitly(const ShaderInstruction& inst)
{
	if (inst.vop_sdwa || inst.mimg_d16) { return true; }
	if (WritesFullDstDespiteOpSelAnd16(inst.type)) { return false; }
	const auto name = ShaderInstructionTypeName(inst.type);
	return inst.vop3_op_sel != 0u || ContainsType(inst.type, "Mac") || ContainsType(inst.type, "Fmac") ||
	       ContainsType(inst.type, "Madmk") || ContainsType(inst.type, "Madak") || name.find("16") != std::string_view::npos;
}

bool HasExecSourceOperand(const ShaderInstruction& inst)
{
	for (int s = 0; s < inst.src_num; ++s)
	{
		const auto type = inst.src[s].type;
		if (type == Operand::ExecLo || type == Operand::ExecHi || type == Operand::ExecZ) { return true; }
	}
	return false;
}

// Restrict guard elimination to register-only vector ALU, interpolation and sampled image reads whose
// only EXEC dependence is masking the destination VGPR store.
bool EligibleForGuardElision(const ShaderInstruction& inst)
{
	if (StartsWith(inst.type, "S") || inst.type == Type::Exp || WritesExecOnly(inst.type) || IsLaneExchange(inst.type) ||
	    WritesMemory(inst.type) || IsBufferLoad(inst.type) || IsVectorLoad(inst.type) ||
	    AnyPrefix(inst.type, {"VCmp", "Ds", "Global", "Flat", "Scratch", "Bvh"}))
	{
		return false;
	}
	if (IsImageInstruction(inst) && IsStorageImageInstruction(inst)) { return false; }
	if (inst.dst.type != Operand::Vgpr || inst.dst.register_id < 0 ||
	    (inst.dst2.type != Operand::Unknown && inst.dst2.type != Operand::Null))
	{
		return false;
	}
	if (inst.vop_sdwa || HasExecSourceOperand(inst)) { return false; }
	for (int s = 0; s < inst.src_num; ++s)
	{
		if (inst.src[s].dpp) { return false; }
	}
	return true;
}

bool OverwritesWholeDestination(const ShaderInstruction& inst)
{
	return EligibleForGuardElision(inst) && !ReadsDestinationImplicitly(inst);
}

bool IsCrossLaneReader(const ShaderInstruction& inst)
{
	if (IsLaneExchange(inst.type) || UsesImplicitDerivatives(inst.type)) { return true; }
	for (int s = 0; s < inst.src_num; ++s)
	{
		if (inst.src[s].dpp) { return true; }
	}
	return IsImageInstruction(inst) && !IsStorageImageInstruction(inst) && !IsLaneLocalImageRead(inst.type);
}

void CollectInstructionVgprs(const ShaderInstruction& inst, VectorSet* reads, VectorSet* cross_lane_reads, VectorSet* writes)
{
	if (IsImageInstruction(inst) && inst.type != Type::ImageBvhIntersectRay && inst.mimg_address_num > 0)
	{
		for (int a = 0; a < inst.mimg_address_num; ++a) { AddVgprOp(inst.mimg_address[a], reads); }
		for (int s = 1; s < inst.src_num; ++s) { AddVgprOp(inst.src[s], reads); }
	} else
	{
		for (int s = 0; s < inst.src_num; ++s) { AddVgprOp(inst.src[s], reads); }
		for (int a = 0; a < inst.mimg_address_num; ++a) { AddVgprOp(inst.mimg_address[a], reads); }
	}

	if (WritesMemory(inst.type) || IsStorageImageInstruction(inst) || inst.type == Type::Exp)
	{
		if (inst.type != Type::Exp)
		{
			AddVgprOp(inst.dst, reads);
			AddVgprOp(inst.dst2, reads);
		}
	} else
	{
		if (ReadsDestinationImplicitly(inst)) { AddVgprOp(inst.dst, reads); }
		if (inst.type != Type::VReadfirstlaneB32 && inst.type != Type::VReadlaneB32)
		{
			if (IsImageInstruction(inst) && inst.dst.type == Operand::Vgpr && inst.dst.register_id >= 0)
			{
				ShaderOperand dst = inst.dst;
				dst.size          = static_cast<int>(ImageDestinationCount(inst));
				AddVgprOp(dst, writes);
			} else
			{
				AddVgprOp(inst.dst, writes);
			}
			AddVgprOp(inst.dst2, writes);
		}
	}

	if (IsCrossLaneReader(inst)) { *cross_lane_reads = *reads; }
}

bool BuildCfg(const ShaderCode& code, std::vector<BasicBlock>* blocks, std::vector<uint32_t>* block_of)
{
	const auto& instructions = code.GetInstructions();
	const auto  n            = instructions.Size();
	if (n == 0) { return false; }

	std::unordered_map<uint32_t, uint32_t> pc_to_index;
	pc_to_index.reserve(n);
	for (uint32_t i = 0; i < n; ++i) { pc_to_index.emplace(instructions.At(i).pc, i); }

	std::vector<uint32_t> targets(n, UINT32_MAX);
	std::vector<bool>     leader(n, false);
	leader[0] = true;

	for (uint32_t i = 0; i < n; ++i)
	{
		const auto& inst = instructions.At(i);
		if (AnyPrefix(inst.type, {"SSetpc", "SSwappc", "SCall"})) { return false; }
		if (!IsBranchType(inst.type))
		{
			if (inst.type == Type::SEndpgm && i + 1u < n) { leader[i + 1u] = true; }
			continue;
		}
		if (inst.src_num < 1) { return false; }
		const int64_t target_pc = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
		if (target_pc < 0 || target_pc > UINT32_MAX) { return false; }
		const auto it = pc_to_index.find(static_cast<uint32_t>(target_pc));
		if (it == pc_to_index.end()) { return false; }
		targets[i]        = it->second;
		leader[it->second] = true;
		if (i + 1u < n) { leader[i + 1u] = true; }
	}

	block_of->assign(n, 0u);
	for (uint32_t i = 0; i < n;)
	{
		const uint32_t b     = static_cast<uint32_t>(blocks->size());
		uint32_t       end   = i + 1u;
		while (end < n && !leader[end]) { ++end; }
		BasicBlock blk;
		blk.begin = i;
		blk.end   = end;
		blocks->push_back(std::move(blk));
		for (uint32_t k = i; k < end; ++k) { (*block_of)[k] = b; }
		i = end;
	}

	for (uint32_t b = 0; b < blocks->size(); ++b)
	{
		const uint32_t last = (*blocks)[b].end - 1u;
		const auto&    inst = instructions.At(last);
		if (inst.type == Type::SEndpgm)
		{
			(*blocks)[b].ends_program = true;
			continue;
		}
		if (IsBranchType(inst.type))
		{
			const uint32_t tb = (*block_of)[targets[last]];
			(*blocks)[b].succs.push_back(tb);
			if (inst.type != Type::SBranch && (*blocks)[b].end < n)
			{
				const uint32_t fb = (*block_of)[(*blocks)[b].end];
				if (fb != tb) { (*blocks)[b].succs.push_back(fb); }
			}
		} else if ((*blocks)[b].end < n)
		{
			(*blocks)[b].succs.push_back((*block_of)[(*blocks)[b].end]);
		}
	}

	for (uint32_t b = 0; b < blocks->size(); ++b)
	{
		for (const uint32_t s: (*blocks)[b].succs) { (*blocks)[s].preds.push_back(b); }
	}
	return true;
}

// Finds acyclic single-entry/single-exit selection diamonds formed by a forward conditional branch
// where one edge skips straight to `merge_block` and the other runs through `[entry_block .. merge_block)`.
std::vector<SelectionRegion> FindSelectionRegions(const ShaderCode& code, const std::vector<BasicBlock>& blocks)
{
	std::vector<SelectionRegion> regions;
	const auto&                  instructions = code.GetInstructions();
	const uint32_t               nb           = static_cast<uint32_t>(blocks.size());

	for (uint32_t h = 0; h < nb; ++h)
	{
		const auto& inst = instructions.At(blocks[h].end - 1u);
		if (!StartsWith(inst.type, "SCbranch") || blocks[h].succs.size() != 2u) { continue; }
		// In blocks[h].succs: [0] is branch target, [1] is fallthrough.
		const uint32_t target_b = blocks[h].succs[0];
		const uint32_t fall_b   = blocks[h].succs[1];
		if (target_b <= h || fall_b <= h) { continue; }

		for (int dir = 0; dir < 2; ++dir)
		{
			const uint32_t entry_b = dir == 0 ? fall_b : target_b;
			const uint32_t merge_b = dir == 0 ? target_b : fall_b;
			if (entry_b >= merge_b) { continue; }

			// Collect blocks strictly reachable from entry_b before merge_b, and reject back-edges,
			// exits outside [entry_b .. merge_b], or side entries from outside the diamond.
			std::vector<bool>     in_reg(nb, false);
			std::vector<uint32_t> stack {entry_b};
			bool                  ok = true;
			in_reg[entry_b]          = true;
			while (!stack.empty() && ok)
			{
				const uint32_t u = stack.back();
				stack.pop_back();
				if (blocks[u].ends_program)
				{
					ok = false;
					break;
				}
				for (const uint32_t s: blocks[u].succs)
				{
					if (s == merge_b) { continue; }
					if (s <= u || s < entry_b || s > merge_b)
					{
						ok = false;
						break;
					}
					if (!in_reg[s])
					{
						in_reg[s] = true;
						stack.push_back(s);
					}
				}
			}
			if (!ok) { continue; }

			std::vector<uint32_t> body;
			for (uint32_t b = entry_b; b < merge_b && ok; ++b)
			{
				if (!in_reg[b]) { continue; }
				for (const uint32_t p: blocks[b].preds)
				{
					if (b == entry_b ? (p != h) : !in_reg[p])
					{
						ok = false;
						break;
					}
				}
				body.push_back(b);
			}
			if (ok && !body.empty()) { regions.push_back({h, entry_b, merge_b, std::move(body)}); }
		}
	}
	return regions;
}

ExecRel OperandExecRel(const ExecState& state, const ShaderOperand& op, unsigned expected_count)
{
	if (op.type == Operand::ExecLo)
	{
		return (expected_count == 2u || !state.seen_exec64) ? state.exec : ExecRel::Unknown;
	}
	if ((op.type == Operand::LiteralConstant || op.type == Operand::IntegerInlineConstant) && op.constant.u == 0u)
	{
		return ExecRel::SubsetOfEntry;
	}
	unsigned first = 0;
	unsigned count = 0;
	if (ScalarRange(op, &first, &count) && count == expected_count)
	{
		return (expected_count == 2u || !state.seen_exec64) ? state.FindExact(first, count) : ExecRel::Unknown;
	}
	return ExecRel::Unknown;
}

void ClearInstructionScalarDsts(const ShaderInstruction& inst, ExecState* state)
{
	for (const auto* dst: {&inst.dst, &inst.dst2})
	{
		unsigned first = 0;
		unsigned count = 0;
		if (ScalarRange(*dst, &first, &count)) { state->ClearOverlapping(first, count); }
	}
}

bool TrackExecStep(const ShaderInstruction& inst, ExecState* state, uint32_t wqm_reseed_epoch = 0)
{
	const bool allow_wqm_reseed =
	    wqm_reseed_epoch != 0u && ShaderInstructionWritesExec(inst) &&
	    (inst.type == Type::SWqmB64 || inst.type == Type::SWqmB32) && inst.src_num >= 1;
	if (!state->valid || state->exec == ExecRel::Unknown)
	{
		if (!allow_wqm_reseed)
		{
			state->valid = false;
			return false;
		}
		state->valid       = true;
		state->exec        = ExecRel::Unknown;
		state->wqm_epoch   = 0;
		state->saved_count = 0;
	}

	if (!ShaderInstructionWritesExec(inst))
	{
		ExecRel  copied_rel = ExecRel::Unknown;
		unsigned expected   = 0;
		if (inst.src_num == 1 && inst.dst2.type == Operand::Unknown &&
		    (inst.type == Type::SMovB64 || inst.type == Type::SMovB32))
		{
			expected   = (inst.type == Type::SMovB64) ? 2u : 1u;
			copied_rel = OperandExecRel(*state, inst.src[0], expected);
		}
		ClearInstructionScalarDsts(inst, state);
		unsigned dfirst = 0;
		unsigned dcount = 0;
		if (copied_rel != ExecRel::Unknown && ScalarRange(inst.dst, &dfirst, &dcount) && dcount == expected)
		{
			state->SetSaved(dfirst, dcount, copied_rel);
		}
		return true;
	}

	const auto     name         = ShaderInstructionTypeName(inst.type);
	const bool     is_64        = ContainsType(inst.type, "B64") || WritesExecOnly(inst.type);
	const unsigned expected     = (name.find("B32") != std::string_view::npos) ? 1u : 2u;
	ExecRel        next_exec    = ExecRel::Unknown;
	const ExecRel  saved_rel    = (expected == 2u || !state->seen_exec64) ? state->exec : ExecRel::Unknown;
	bool           saves_dst    = false;

	if (WritesExecOnly(inst.type))
	{
		next_exec = IsWqmRel(state->exec) ? ExecRel::WidenedWqmSubset : ExecRel::SubsetOfEntry;
	} else if (inst.type == Type::SAndSaveexecB64 || inst.type == Type::SAndn1SaveexecB64 ||
	           inst.type == Type::SAndSaveexecB32 || inst.type == Type::SAndn1SaveexecB32)
	{
		saves_dst = true;
		next_exec = IsWqmRel(state->exec) ? ExecRel::WidenedWqmSubset : ExecRel::SubsetOfEntry;
	} else if (inst.type == Type::SAndn2SaveexecB64 || inst.type == Type::SAndn2SaveexecB32)
	{
		const ExecRel src0_rel = inst.src_num >= 1 ? OperandExecRel(*state, inst.src[0], expected) : ExecRel::Unknown;
		saves_dst              = true;
		if (src0_rel == ExecRel::AtEntry || src0_rel == ExecRel::SubsetOfEntry)
		{
			next_exec = ExecRel::SubsetOfEntry;
		} else if (IsWqmRel(src0_rel))
		{
			next_exec = ExecRel::WidenedWqmSubset;
		}
	} else if ((inst.type == Type::SAndB64 || inst.type == Type::SAndB32) && inst.src_num >= 2)
	{
		const ExecRel src0_rel = OperandExecRel(*state, inst.src[0], expected);
		const ExecRel src1_rel = OperandExecRel(*state, inst.src[1], expected);
		if (src0_rel == ExecRel::AtEntry || src0_rel == ExecRel::SubsetOfEntry || src1_rel == ExecRel::AtEntry ||
		    src1_rel == ExecRel::SubsetOfEntry)
		{
			next_exec = ExecRel::SubsetOfEntry;
		} else if (IsWqmRel(src0_rel) || IsWqmRel(src1_rel))
		{
			next_exec = ExecRel::WidenedWqmSubset;
		}
	} else if ((inst.type == Type::SAndn2B64 || inst.type == Type::SAndn2B32) && inst.src_num >= 1)
	{
		const ExecRel src0_rel = OperandExecRel(*state, inst.src[0], expected);
		if (src0_rel == ExecRel::AtEntry || src0_rel == ExecRel::SubsetOfEntry)
		{
			next_exec = ExecRel::SubsetOfEntry;
		} else if (IsWqmRel(src0_rel))
		{
			next_exec = ExecRel::WidenedWqmSubset;
		}
	} else if ((inst.type == Type::SWqmB64 || inst.type == Type::SWqmB32) && inst.src_num >= 1)
	{
		const ExecRel src0_rel = OperandExecRel(*state, inst.src[0], expected);
		if (src0_rel == ExecRel::AtEntry || src0_rel == ExecRel::WidenedWqm)
		{
			next_exec = ExecRel::WidenedWqm;
		} else if (src0_rel == ExecRel::SubsetOfEntry || src0_rel == ExecRel::WidenedWqmSubset)
		{
			next_exec = ExecRel::WidenedWqmSubset;
		} else if (wqm_reseed_epoch != 0u)
		{
			// Establishing WQM(M) from an arbitrary scalar mask M starts a monotonic sub-epoch:
			// WQM(M) acts as the widening ceiling and M itself stays a subset of WQM(M).
			state->saved_count = 0;
			unsigned sfirst    = 0;
			unsigned scount    = 0;
			if (ScalarRange(inst.src[0], &sfirst, &scount) && scount == expected)
			{
				state->SetSaved(sfirst, scount, ExecRel::SubsetOfEntry);
			}
			state->wqm_epoch = wqm_reseed_epoch;
			next_exec        = ExecRel::WidenedWqm;
		}
	} else if ((inst.type == Type::SMovB64 || inst.type == Type::SMovB32) && inst.src_num == 1)
	{
		next_exec = OperandExecRel(*state, inst.src[0], expected);
	}

	if (!WritesExecOnly(inst.type)) { ClearInstructionScalarDsts(inst, state); }
	if (saves_dst && saved_rel != ExecRel::Unknown)
	{
		unsigned dfirst = 0;
		unsigned dcount = 0;
		if (ScalarRange(inst.dst, &dfirst, &dcount) && dcount == expected) { state->SetSaved(dfirst, dcount, saved_rel); }
	}
	if (is_64) { state->seen_exec64 = true; }
	state->exec = next_exec;
	if (next_exec == ExecRel::Unknown)
	{
		state->valid = false;
		return false;
	}
	return true;
}

// Marks instructions inside `region` where EXEC is guaranteed identical to the region entry mask and
// treats any VGPR read executed under widened WQM brackets as cross-lane for the region.
bool AnalyzeRegionEntryMaskAndCrossLane(const ShaderCode& code, const std::vector<BasicBlock>& blocks,
                                        const SelectionRegion& region, const std::vector<VectorSet>& reads,
                                        const std::vector<VectorSet>& cross_reads, std::vector<uint32_t>* at_entry_indices,
                                        std::vector<VectorSet>* cross_after)
{
	const auto&            instructions = code.GetInstructions();
	const uint32_t         nb           = static_cast<uint32_t>(blocks.size());
	std::vector<bool>      in_reg(nb, false);
	std::vector<bool>      wqm_active(instructions.Size(), false);
	std::vector<ExecState> in_state(nb);
	for (const uint32_t b: region.blocks) { in_reg[b] = true; }
	in_state[region.entry_block].reachable = true;

	std::vector<uint32_t> candidates;
	for (const uint32_t b: region.blocks)
	{
		ExecState cur = in_state[b];
		if (!cur.reachable || !cur.valid || cur.exec == ExecRel::Unknown) { return false; }
		for (uint32_t i = blocks[b].begin; i < blocks[b].end; ++i)
		{
			if (cur.valid && cur.exec == ExecRel::AtEntry) { candidates.push_back(i); }
			if (IsWqmRel(cur.exec)) { wqm_active[i] = true; }
			if (!TrackExecStep(instructions.At(i), &cur)) { return false; }
		}
		for (const uint32_t s: blocks[b].succs)
		{
			if (s == region.merge_block)
			{
				if (IsWqmRel(cur.exec) || cur.exec == ExecRel::Unknown) { return false; }
			} else if (in_reg[s])
			{
				in_state[s].Merge(cur);
			}
		}
	}

	// Backward union of all cross-lane and WQM-widened VGPR reads reachable before merge_block inside the region.
	std::vector<VectorSet> block_cross_in(nb);
	cross_after->assign(instructions.Size(), VectorSet {});
	for (size_t idx = region.blocks.size(); idx > 0; --idx)
	{
		const uint32_t b = region.blocks[idx - 1u];
		VectorSet      cur;
		for (const uint32_t s: blocks[b].succs)
		{
			if (s != region.merge_block && in_reg[s]) { cur |= block_cross_in[s]; }
		}
		for (uint32_t i = blocks[b].end; i > blocks[b].begin; --i)
		{
			const uint32_t k  = i - 1u;
			(*cross_after)[k] = cur;
			cur |= cross_reads[k];
			if (wqm_active[k]) { cur |= reads[k]; }
		}
		block_cross_in[b] = cur;
	}

	*at_entry_indices = std::move(candidates);
	return true;
}

// Rule C: an instruction running at entry EXEC (or WQM(entry EXEC)) whose subsequent reachable path
// never widens above that mask and never feeds a cross-lane/implicit-LOD read cannot observe stale
// VGPR values on inactive lanes, so its destination select guard is unnecessary.
void ApplyMonotonicTailGuards(const ShaderCode& code, const std::vector<BasicBlock>& blocks,
                              const std::vector<VectorSet>& cross_reads, const std::vector<VectorSet>& writes,
                              const std::vector<bool>& eligible, const std::vector<bool>& overwrites,
                              std::vector<bool>* unguarded)
{
	const auto&    instructions = code.GetInstructions();
	const uint32_t n            = instructions.Size();
	const uint32_t nb           = static_cast<uint32_t>(blocks.size());
	if (nb == 0) { return; }

	std::vector<ExecState> in_state(nb);
	in_state[0].reachable = true;

	auto is_wqm_op = [](const ShaderInstruction& inst)
	{
		return ShaderInstructionWritesExec(inst) && (inst.type == Type::SWqmB64 || inst.type == Type::SWqmB32);
	};

	const uint32_t max_fwd_iters = nb * 8u + 16u;
	bool           fwd_changed   = true;
	uint32_t       fwd_iter      = 0;
	while (fwd_changed && fwd_iter < max_fwd_iters)
	{
		fwd_changed = false;
		++fwd_iter;
		for (uint32_t b = 0; b < nb; ++b)
		{
			ExecState cur = in_state[b];
			if (!cur.reachable) { continue; }
			for (uint32_t i = blocks[b].begin; i < blocks[b].end; ++i)
			{
				const auto& inst = instructions.At(i);
				if (cur.valid || is_wqm_op(inst)) { TrackExecStep(inst, &cur, i + 1u); }
			}
			for (const uint32_t s: blocks[b].succs)
			{
				if (in_state[s].Merge(cur)) { fwd_changed = true; }
			}
		}
	}
	if (fwd_changed) { return; }

	std::vector<ExecRel>  pre_exec(n, ExecRel::Unknown);
	std::vector<ExecRel>  post_exec(n, ExecRel::Unknown);
	std::vector<uint32_t> pre_epoch(n, 0u);
	std::vector<uint32_t> post_epoch(n, 0u);
	std::vector<uint32_t> block_out_epoch(nb, 0u);
	std::vector<bool>     block_out_valid(nb, false);
	for (uint32_t b = 0; b < nb; ++b)
	{
		ExecState cur = in_state[b];
		if (!cur.reachable) { continue; }
		for (uint32_t i = blocks[b].begin; i < blocks[b].end; ++i)
		{
			const auto& inst = instructions.At(i);
			pre_exec[i]      = cur.valid ? cur.exec : ExecRel::Unknown;
			pre_epoch[i]     = cur.valid ? cur.wqm_epoch : 0u;
			if (cur.valid || is_wqm_op(inst)) { TrackExecStep(inst, &cur, i + 1u); }
			post_exec[i]  = cur.valid ? cur.exec : ExecRel::Unknown;
			post_epoch[i] = cur.valid ? cur.wqm_epoch : 0u;
		}
		block_out_valid[b] = cur.valid;
		block_out_epoch[b] = cur.valid ? cur.wqm_epoch : 0u;
	}

	std::vector<VectorSet> cross_in(nb);
	std::vector<bool>      in_unknown(nb, false);
	std::vector<bool>      in_wqm_state(nb, false);
	std::vector<bool>      in_wqm_op(nb, false);

	bool bwd_changed = true;
	while (bwd_changed)
	{
		bwd_changed = false;
		for (uint32_t b = nb; b > 0; --b)
		{
			const uint32_t bi = b - 1u;
			if (!in_state[bi].reachable) { continue; }

			VectorSet cross_cur;
			bool      unk_cur    = false;
			bool      wqm_st_cur = false;
			bool      wqm_op_cur = false;
			for (const uint32_t s: blocks[bi].succs)
			{
				if (!in_state[s].reachable) { continue; }
				cross_cur |= cross_in[s];
				unk_cur    = unk_cur || in_unknown[s];
				wqm_st_cur = wqm_st_cur || in_wqm_state[s];
				wqm_op_cur = wqm_op_cur || in_wqm_op[s];
				if (!block_out_valid[bi] || !in_state[s].valid || block_out_epoch[bi] != in_state[s].wqm_epoch)
				{
					unk_cur    = true;
					wqm_st_cur = true;
				}
			}

			for (uint32_t i = blocks[bi].end; i > blocks[bi].begin; --i)
			{
				const uint32_t k = i - 1u;
				cross_cur |= cross_reads[k];
				if (pre_exec[k] == ExecRel::Unknown || post_exec[k] == ExecRel::Unknown || pre_epoch[k] != post_epoch[k])
				{
					unk_cur    = true;
					wqm_st_cur = true;
				} else if (IsWqmRel(pre_exec[k]) || IsWqmRel(post_exec[k]))
				{
					wqm_st_cur = true;
				}
				if (is_wqm_op(instructions.At(k))) { wqm_op_cur = true; }
			}

			if (cross_cur != cross_in[bi] || unk_cur != in_unknown[bi] || wqm_st_cur != in_wqm_state[bi] ||
			    wqm_op_cur != in_wqm_op[bi])
			{
				cross_in[bi]     = cross_cur;
				in_unknown[bi]   = unk_cur;
				in_wqm_state[bi] = wqm_st_cur;
				in_wqm_op[bi]    = wqm_op_cur;
				bwd_changed      = true;
			}
		}
	}

	for (uint32_t bi = 0; bi < nb; ++bi)
	{
		if (!in_state[bi].reachable) { continue; }
		VectorSet cross_after;
		bool      reaches_unknown   = false;
		bool      reaches_wqm_state = false;
		bool      reaches_wqm_op    = false;
		for (const uint32_t s: blocks[bi].succs)
		{
			if (!in_state[s].reachable) { continue; }
			cross_after |= cross_in[s];
			reaches_unknown   = reaches_unknown || in_unknown[s];
			reaches_wqm_state = reaches_wqm_state || in_wqm_state[s];
			reaches_wqm_op    = reaches_wqm_op || in_wqm_op[s];
			if (!block_out_valid[bi] || !in_state[s].valid || block_out_epoch[bi] != in_state[s].wqm_epoch)
			{
				reaches_unknown   = true;
				reaches_wqm_state = true;
			}
		}

		for (uint32_t i = blocks[bi].end; i > blocks[bi].begin; --i)
		{
			const uint32_t k = i - 1u;
			if (!(*unguarded)[k] && eligible[k] && overwrites[k] && (writes[k] & cross_after).none())
			{
				if ((pre_exec[k] == ExecRel::AtEntry && !reaches_wqm_state) ||
				    (pre_exec[k] == ExecRel::WidenedWqm && !reaches_unknown && !reaches_wqm_op))
				{
					(*unguarded)[k] = true;
				}
			}

			cross_after |= cross_reads[k];
			if (pre_exec[k] == ExecRel::Unknown || post_exec[k] == ExecRel::Unknown || pre_epoch[k] != post_epoch[k])
			{
				reaches_unknown   = true;
				reaches_wqm_state = true;
			} else if (IsWqmRel(pre_exec[k]) || IsWqmRel(post_exec[k]))
			{
				reaches_wqm_state = true;
			}
			if (is_wqm_op(instructions.At(k))) { reaches_wqm_op = true; }
		}
	}
}

} // namespace

std::vector<bool> ShaderAnalyzeDeadVgprDstGuards(const ShaderCode& code, const std::vector<bool>& exec_full)
{
	const auto& instructions = code.GetInstructions();
	const auto  n            = instructions.Size();
	if (n == 0 || (!exec_full.empty() && exec_full.size() != n) || UsesRelativeIndexing(code)) { return {}; }

	// A real shader communicates results via exports or memory writes; synthetic instruction
	// fixtures terminate at S_ENDPGM without either and observe the register file directly.
	bool has_program_output = false;
	for (uint32_t i = 0; i < n; ++i)
	{
		const auto type = instructions.At(i).type;
		if (type == Type::Exp || WritesMemory(type))
		{
			has_program_output = true;
			break;
		}
	}
	if (!has_program_output) { return {}; }

	std::vector<BasicBlock> blocks;
	std::vector<uint32_t>   block_of;
	if (!BuildCfg(code, &blocks, &block_of)) { return {}; }

	std::vector<VectorSet> reads(n);
	std::vector<VectorSet> cross_reads(n);
	std::vector<VectorSet> writes(n);
	std::vector<bool>      eligible(n, false);
	std::vector<bool>      overwrites(n, false);
	for (uint32_t i = 0; i < n; ++i)
	{
		const auto& inst = instructions.At(i);
		CollectInstructionVgprs(inst, &reads[i], &cross_reads[i], &writes[i]);
		eligible[i]   = EligibleForGuardElision(inst) && writes[i].any();
		overwrites[i] = OverwritesWholeDestination(inst) && writes[i].any();
	}

	// Start from instructions already executed under full EXEC (they never need per-lane select
	// guards and act as unconditional kills during backward liveness).
	std::vector<bool> unguarded(n, false);
	for (uint32_t i = 0; i < n; ++i)
	{
		if (!exec_full.empty() && exec_full[i] && overwrites[i]) { unguarded[i] = true; }
	}
	ApplyMonotonicTailGuards(code, blocks, cross_reads, writes, eligible, overwrites, &unguarded);

	const auto regions = FindSelectionRegions(code, blocks);
	struct AnalyzedRegion
	{
		uint32_t              merge_block = 0;
		std::vector<uint32_t> entry_mask_insts;
		std::vector<VectorSet> cross_after;
	};
	std::vector<AnalyzedRegion> valid_regions;
	for (const auto& reg: regions)
	{
		AnalyzedRegion analyzed;
		analyzed.merge_block = reg.merge_block;
		if (AnalyzeRegionEntryMaskAndCrossLane(code, blocks, reg, reads, cross_reads, &analyzed.entry_mask_insts,
		                                       &analyzed.cross_after))
		{
			valid_regions.push_back(std::move(analyzed));
		}
	}

	const uint32_t         nb = static_cast<uint32_t>(blocks.size());
	std::vector<VectorSet> live_in(nb);
	std::vector<VectorSet> live_out(nb);

	constexpr uint32_t kMaxIterations = 8;
	for (uint32_t iter = 0; iter < kMaxIterations; ++iter)
	{
		bool flow_changed = true;
		while (flow_changed)
		{
			flow_changed = false;
			for (uint32_t b = nb; b > 0; --b)
			{
				const uint32_t bi = b - 1u;
				VectorSet      out;
				for (const uint32_t s: blocks[bi].succs) { out |= live_in[s]; }
				live_out[bi] = out;

				VectorSet cur = out;
				for (uint32_t i = blocks[bi].end; i > blocks[bi].begin; --i)
				{
					const uint32_t k = i - 1u;
					if (unguarded[k] && overwrites[k]) { cur &= ~writes[k]; }
					cur |= reads[k];
				}
				if (cur != live_in[bi])
				{
					live_in[bi]  = cur;
					flow_changed = true;
				}
			}
		}

		bool added_guard = false;
		// Rule A: VGPRs never read after instruction k across any path in the shader.
		for (uint32_t bi = 0; bi < nb; ++bi)
		{
			VectorSet cur = live_out[bi];
			for (uint32_t i = blocks[bi].end; i > blocks[bi].begin; --i)
			{
				const uint32_t k = i - 1u;
				if (!unguarded[k] && eligible[k] && (writes[k] & cur).none())
				{
					unguarded[k] = true;
					added_guard  = true;
				}
				if (unguarded[k] && overwrites[k]) { cur &= ~writes[k]; }
				cur |= reads[k];
			}
		}

		// Rule B: writes at region entry mask inside an acyclic selection diamond whose written VGPRs
		// are dead at the merge and never feed a cross-lane/implicit-LOD read later in the region.
		for (const auto& reg: valid_regions)
		{
			const VectorSet& merge_live = live_in[reg.merge_block];
			for (const uint32_t k: reg.entry_mask_insts)
			{
				if (!unguarded[k] && eligible[k] && (writes[k] & merge_live).none() &&
				    (writes[k] & reg.cross_after[k]).none())
				{
					unguarded[k] = true;
					added_guard  = true;
				}
			}
		}

		if (!added_guard) { break; }
	}

	return unguarded;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
