#include "Emulator/Graphics/ShaderScalarLiveness.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

using Sgprs = std::bitset<kShaderScalarLivenessSgprs>;

void AddRange(const ShaderOperand& operand, Sgprs* set)
{
	if (operand.type != ShaderOperandType::Sgpr || operand.register_id < 0)
	{
		return;
	}
	const int count = operand.size > 0 ? operand.size : 1;
	for (int r = operand.register_id; r < operand.register_id + count && r < kShaderScalarLivenessSgprs; r++)
	{
		set->set(static_cast<size_t>(r));
	}
}

bool IsBranch(ShaderInstructionType type)
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

} // namespace

namespace {

struct ScalarFlow
{
	std::vector<Sgprs>                 uses;
	std::vector<Sgprs>                 defs;
	std::vector<std::vector<uint32_t>> successors;
};

// Operand-derived SGPR uses/defs and successors; false when unresolvable.
bool BuildScalarFlow(const ShaderCode& code, ScalarFlow* flow)
{
	const auto&    instructions = code.GetInstructions();
	const uint32_t count        = instructions.Size();

	std::vector<uint32_t> pcs(count);
	for (uint32_t i = 0; i < count; i++)
	{
		pcs[i] = instructions.At(i).pc;
	}
	auto& uses       = flow->uses;
	auto& defs       = flow->defs;
	auto& successors = flow->successors;
	uses.assign(count, {});
	defs.assign(count, {});
	successors.assign(count, {});
	for (uint32_t i = 0; i < count; i++)
	{
		const auto& inst = instructions.At(i);
		if (inst.src_num < 0 || inst.src_num > 4 || inst.mimg_address_num < 0 || inst.mimg_address_num > 13 ||
		    inst.type == ShaderInstructionType::SSetpcB64 || inst.type == ShaderInstructionType::SSwappcB64)
		{
			return false;
		}
		for (int s = 0; s < inst.src_num; s++)
		{
			AddRange(inst.src[s], &uses[i]);
		}
		for (int a = 0; a < inst.mimg_address_num; a++)
		{
			AddRange(inst.mimg_address[a], &uses[i]);
		}
		AddRange(inst.dst, &defs[i]);
		AddRange(inst.dst2, &defs[i]);
		if (inst.type == ShaderInstructionType::SEndpgm)
		{
			continue;
		}
		if (IsBranch(inst.type))
		{
			const auto target = static_cast<uint32_t>(static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i);
			const auto it     = std::lower_bound(pcs.begin(), pcs.end(), target);
			if (it == pcs.end() || *it != target)
			{
				return false;
			}
			successors[i].push_back(static_cast<uint32_t>(it - pcs.begin()));
			// A branch reads no SGPR data; its label constant is not a register.
			uses[i].reset();
			if (inst.type == ShaderInstructionType::SBranch)
			{
				continue;
			}
		}
		if (i + 1 < count)
		{
			successors[i].push_back(i + 1);
		}
	}

	return true;
}

Sgprs ComputeSgprsLiveAtEntry(const ShaderCode& code)
{
	const uint32_t count = code.GetInstructions().Size();
	Sgprs          all;
	all.set();
	ScalarFlow flow;
	if (count == 0)
	{
		return {};
	}
	if (!BuildScalarFlow(code, &flow))
	{
		return all;
	}
	const auto&        uses       = flow.uses;
	const auto&        defs       = flow.defs;
	const auto&        successors = flow.successors;
	std::vector<Sgprs> live_in(count);
	for (bool changed = true; changed;)
	{
		changed = false;
		for (uint32_t i = count; i-- > 0;)
		{
			Sgprs out;
			for (auto s: successors[i])
			{
				out |= live_in[s];
			}
			const auto updated = uses[i] | (out & ~defs[i]);
			if (updated != live_in[i])
			{
				live_in[i] = updated;
				changed    = true;
			}
		}
	}
	return live_in[0];
}

std::vector<Sgprs> ComputeSgprsHoldingEntryValue(const ShaderCode& code)
{
	const uint32_t count = code.GetInstructions().Size();
	ScalarFlow     flow;
	if (count == 0 || !BuildScalarFlow(code, &flow))
	{
		return std::vector<Sgprs>(count);
	}
	// Must-analysis: entry holds everything; any other instruction holds the
	// intersection of its predecessors' outputs. Start from "all hold".
	std::vector<std::vector<uint32_t>> predecessors(count);
	for (uint32_t i = 0; i < count; i++)
	{
		for (auto succ: flow.successors[i])
		{
			predecessors[succ].push_back(i);
		}
	}
	std::vector<Sgprs> in(count);
	for (auto& set: in)
	{
		set.set();
	}
	for (bool changed = true; changed;)
	{
		changed = false;
		for (uint32_t i = 0; i < count; i++)
		{
			Sgprs updated;
			updated.set();
			for (auto pred: predecessors[i])
			{
				updated &= in[pred] & ~flow.defs[pred];
			}
			if (i != 0 && predecessors[i].empty())
			{
				continue;
			}
			if (updated != in[i])
			{
				in[i]   = updated;
				changed = true;
			}
		}
	}
	return in;
}

// Every instruction field the scalar flow reads, in order: equal signatures
// yield equal analyses, so a memoized result is exact for the program.
std::vector<int64_t> FlowSignature(const ShaderCode& code)
{
	const auto&          instructions = code.GetInstructions();
	std::vector<int64_t> signature;
	signature.reserve(static_cast<size_t>(instructions.Size()) * 12u);
	const auto operand = [&signature](const ShaderOperand& op)
	{
		signature.push_back(static_cast<int64_t>(op.type));
		signature.push_back(op.register_id);
		signature.push_back(op.size);
	};
	for (const auto& inst: instructions)
	{
		signature.push_back(static_cast<int64_t>(inst.pc));
		signature.push_back(static_cast<int64_t>(inst.type));
		signature.push_back(inst.src_num);
		signature.push_back(inst.mimg_address_num);
		signature.push_back(inst.src[0].constant.i);
		for (int s = 0; s < inst.src_num && s < 4; s++)
		{
			operand(inst.src[s]);
		}
		for (int a = 0; a < inst.mimg_address_num && a < 13; a++)
		{
			operand(inst.mimg_address[a]);
		}
		operand(inst.dst);
		operand(inst.dst2);
	}
	return signature;
}

struct FlowSummary
{
	std::vector<int64_t> signature;
	Sgprs                live_at_entry;
	std::vector<Sgprs>   holding_entry_value;
};

// The CP thread analyzes the same programs every draw; keep the results.
std::shared_ptr<const FlowSummary> SummaryOf(const ShaderCode& code)
{
	constexpr size_t kMaxPrograms = 4096;
	static std::mutex                                                          mutex;
	static std::unordered_multimap<uint64_t, std::shared_ptr<const FlowSummary>> summaries;
	auto           signature = FlowSignature(code);
	const uint64_t key       = XXH3_64bits(signature.data(), signature.size() * sizeof(int64_t));
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto [first, last] = summaries.equal_range(key);
		for (auto it = first; it != last; ++it)
		{
			if (it->second->signature == signature)
			{
				return it->second;
			}
		}
	}
	auto summary = std::make_shared<FlowSummary>(
	    FlowSummary {std::move(signature), ComputeSgprsLiveAtEntry(code), ComputeSgprsHoldingEntryValue(code)});
	std::lock_guard<std::mutex> lock(mutex);
	if (summaries.size() >= kMaxPrograms)
	{
		summaries.clear();
	}
	summaries.emplace(key, summary);
	return summary;
}

} // namespace

Sgprs ShaderSgprsLiveAtEntry(const ShaderCode& code)
{
	return SummaryOf(code)->live_at_entry;
}

std::vector<Sgprs> ShaderSgprsHoldingEntryValue(const ShaderCode& code)
{
	return SummaryOf(code)->holding_entry_value;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
