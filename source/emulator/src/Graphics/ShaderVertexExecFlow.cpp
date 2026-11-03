#include "Emulator/Graphics/ShaderVertexExecFlow.h"

#include "ShaderLaneFlow.h"
#include "ShaderNativeWaveInternal.h"
#include "ShaderSpirvInternal.h"

#include <array>
#include <map>
#include <optional>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

using LaneFlow::kExecLoWord;
using LaneFlow::kWords;
using LaneFlow::ScalarRange;
using Type    = ShaderInstructionType;
using Operand = ShaderOperandType;

enum class Word : uint8_t
{
	Unknown,
	FullExecLo,
	FullExecHi,
	WaveInfo,      // s3 as launched: primitives [15:8], vertices [7:0], both the live lane span
	LaneCount,     // s3 >> 8
	NegatedCounts, // 0 - s3 or 0 - (s3 >> 8): as a shift amount, the wave width minus the lane span
};

constexpr uint32_t kMaxPasses = 64;

struct ExecState
{
	std::array<Word, kWords>      words {};
	std::map<uint32_t, Word> spills; // (vgpr << 8 | lane) -> saved word; only full EXEC words are kept

	[[nodiscard]] bool ExecFull(bool wave32) const
	{
		return words[kExecLoWord] == Word::FullExecLo && (wave32 || words[kExecLoWord + 1u] == Word::FullExecHi);
	}

	bool Merge(const ExecState& other)
	{
		bool changed = false;
		for (unsigned word = 0; word < kWords; ++word)
		{
			if (words[word] != Word::Unknown && words[word] != other.words[word])
			{
				words[word] = Word::Unknown;
				changed     = true;
			}
		}
		for (auto spill = spills.begin(); spill != spills.end();)
		{
			const auto theirs = other.spills.find(spill->first);
			if (theirs == other.spills.end() || theirs->second != spill->second)
			{
				spill   = spills.erase(spill);
				changed = true;
			} else
			{
				++spill;
			}
		}
		return changed;
	}
};

bool IsAllOnes(const ShaderOperand& op)
{
	return (op.type == Operand::IntegerInlineConstant && op.constant.i == -1) ||
	       (op.type == Operand::LiteralConstant && op.constant.u == 0xffffffffu);
}

bool IsZero(const ShaderOperand& op)
{
	return op.type == Operand::IntegerInlineConstant && op.constant.i == 0;
}

Word OneWord(const ExecState& state, const ShaderOperand& op)
{
	unsigned first = 0;
	unsigned count = 0;
	return ScalarRange(op, &first, &count) && count == 1u ? state.words[first] : Word::Unknown;
}

void ClearVgprSpills(const ShaderOperand& dst, ExecState* state)
{
	if (dst.type != Operand::Vgpr) { return; }
	const uint32_t first = static_cast<uint32_t>(dst.register_id);
	const uint32_t last  = first + static_cast<uint32_t>(dst.size > 0 ? dst.size : 1);
	state->spills.erase(state->spills.lower_bound(first << 8u), state->spills.lower_bound(last << 8u));
}

void ClearWrites(const ShaderInstruction& inst, ExecState* state)
{
	if (inst.type == Type::VMovreldB32 || inst.type == Type::VMovrelsdB32) { state->spills.clear(); }
	if (LaneFlow::StartsWith(inst.type, "SMovrel")) { state->words.fill(Word::Unknown); }
	for (const ShaderOperand* dst: {&inst.dst, &inst.dst2})
	{
		if (LaneFlow::WritesExecOnly(inst.type)) { break; }
		ClearVgprSpills(*dst, state);
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(*dst, &first, &count)) { continue; }
		for (unsigned word = 0; word < count; ++word) { state->words[first + word] = Word::Unknown; }
	}
	if (ShaderInstructionWritesExec(inst))
	{
		state->words[kExecLoWord]      = Word::Unknown;
		state->words[kExecLoWord + 1u] = Word::Unknown;
	}
}

void SetExecFull(ExecState* state)
{
	state->words[kExecLoWord]      = Word::FullExecLo;
	state->words[kExecLoWord + 1u] = Word::FullExecHi;
}

void CopyWords(const ExecState& before, const ShaderOperand& dst, const ShaderOperand& src, ExecState* state)
{
	unsigned dst_first = 0;
	unsigned src_first = 0;
	unsigned dst_count = 0;
	unsigned src_count = 0;
	if (!ScalarRange(dst, &dst_first, &dst_count) || !ScalarRange(src, &src_first, &src_count) || dst_count != src_count) { return; }
	for (unsigned word = 0; word < dst_count; ++word) { state->words[dst_first + word] = before.words[src_first + word]; }
}

void Step(const ShaderInstruction& inst, bool wave32, ExecState* state)
{
	const ExecState before = *state;
	ClearWrites(inst, state);

	int vgpr = 0;
	int lane = 0;
	unsigned first = 0;
	unsigned count = 0;
	if (IsStaticScalarSpillWrite(inst, &vgpr, &lane))
	{
		const Word saved = OneWord(before, inst.src[0]);
		// Only this slot changes; the rest of the VGPR keeps the other saved words.
		state->spills = before.spills;
		const uint32_t key = (static_cast<uint32_t>(vgpr) << 8u) | static_cast<uint32_t>(lane);
		if (saved == Word::FullExecLo || saved == Word::FullExecHi) { state->spills[key] = saved; } else { state->spills.erase(key); }
		return;
	}
	if (IsStaticScalarSpillRead(inst, &vgpr, &lane))
	{
		const auto slot = before.spills.find((static_cast<uint32_t>(vgpr) << 8u) | static_cast<uint32_t>(lane));
		if (slot != before.spills.end() && ScalarRange(inst.dst, &first, &count) && count == 1u) { state->words[first] = slot->second; }
		return;
	}

	const bool writes_exec = inst.dst.type == Operand::ExecLo;
	const bool whole_mask  = inst.type == Type::SMovB64 || (inst.type == Type::SMovB32 && wave32);
	if (whole_mask && writes_exec && IsAllOnes(inst.src[0]))
	{
		SetExecFull(state);
		return;
	}
	if (inst.type == Type::SMovB64 || inst.type == Type::SMovB32)
	{
		CopyWords(before, inst.dst, inst.src[0], state);
		return;
	}
	if (LaneFlow::StartsWith(inst.type, "S") && ShaderInstructionTypeChangesExec(inst.type))
	{
		// S_*_SAVEEXEC: the destination is the old EXEC.
		ShaderOperand exec {};
		exec.type = Operand::ExecLo;
		exec.size = inst.dst.size;
		CopyWords(before, inst.dst, exec, state);
		return;
	}
	const bool lanes_shift = inst.type == Type::SLshrB64 || (inst.type == Type::SLshrB32 && wave32);
	if (lanes_shift && writes_exec && IsAllOnes(inst.src[0]) && OneWord(before, inst.src[1]) == Word::NegatedCounts)
	{
		SetExecFull(state);
		return;
	}
	if (inst.type == Type::SLshrB32 && OneWord(before, inst.src[0]) == Word::WaveInfo && inst.src[1].type == Operand::IntegerInlineConstant &&
	    inst.src[1].constant.i == 8 && ScalarRange(inst.dst, &first, &count))
	{
		state->words[first] = Word::LaneCount;
		return;
	}
	if (inst.type == Type::SSubI32 && IsZero(inst.src[0]) && ScalarRange(inst.dst, &first, &count))
	{
		const Word source = OneWord(before, inst.src[1]);
		if (source == Word::WaveInfo || source == Word::LaneCount) { state->words[first] = Word::NegatedCounts; }
	}
}

bool BranchTarget(const ShaderInstruction& inst, uint32_t* target)
{
	if (inst.src_num < 1) { return false; }
	const int64_t where = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
	if (where < 0 || where > UINT32_MAX) { return false; }
	*target = static_cast<uint32_t>(where);
	return true;
}

} // namespace

std::vector<bool> ShaderAnalyzeVertexExecFull(const ShaderCode& code, bool wave32)
{
	const auto&    list = code.GetInstructions();
	const uint32_t size = list.Size();
	if (size == 0u) { return {}; }

	std::unordered_map<uint32_t, uint32_t> index_of;
	for (uint32_t index = 0; index < size; ++index)
	{
		const auto type = list.At(index).type;
		if (type == Type::SSetpcB64 || type == Type::SSwappcB64) { return {}; }
		index_of[list.At(index).pc] = index;
	}

	std::vector<std::optional<ExecState>> entry(size + 1u);
	entry[0].emplace();
	SetExecFull(&*entry[0]);
	entry[0]->words[3] = Word::WaveInfo;

	const auto flow_to = [&entry](uint32_t index, const ExecState& state) {
		if (!entry[index].has_value())
		{
			entry[index] = state;
			return true;
		}
		return entry[index]->Merge(state);
	};

	bool changed = true;
	for (uint32_t pass = 0; changed; ++pass)
	{
		if (pass == kMaxPasses) { return {}; }
		changed = false;
		for (uint32_t index = 0; index < size; ++index)
		{
			if (!entry[index].has_value()) { continue; }
			const auto& inst  = list.At(index);
			ExecState       state = *entry[index];
			Step(inst, wave32, &state);
			if (ShaderInstructionIsControlFlowBoundary(inst) && inst.type != Type::SEndpgm)
			{
				uint32_t target = 0;
				if (!BranchTarget(inst, &target)) { return {}; }
				const auto where = index_of.find(target);
				if (where == index_of.end()) { return {}; }
				changed = flow_to(where->second, state) || changed;
			}
			if (inst.type != Type::SEndpgm && inst.type != Type::SBranch) { changed = flow_to(index + 1u, state) || changed; }
		}
	}

	std::vector<bool> full(size + 1u, false);
	for (uint32_t index = 0; index <= size; ++index) { full[index] = entry[index].has_value() && entry[index]->ExecFull(wave32); }
	return full;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
