#include "ShaderParseInternal.h"

#include "ShaderLaneFlow.h"

#include <map>
#include <string_view>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

using namespace LaneFlow;

namespace {

using Type = ShaderInstructionType;

bool Contains(Type type, std::string_view part)
{
	return ShaderInstructionTypeName(type).find(part) != std::string_view::npos;
}

bool IsVcc(unsigned word)
{
	return word == kVccLoWord || word == kVccLoWord + 1u;
}

bool Covers(const ShaderOperand& op, unsigned word)
{
	unsigned first = 0;
	unsigned count = 0;
	return ScalarRange(op, &first, &count) && word >= first && word < first + count;
}

void AddWords(const ShaderOperand& op, std::vector<unsigned>* words)
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &first, &count)) { return; }
	for (unsigned word = first; word < first + count; ++word) { words->push_back(word); }
}

// Over-approximates the implicit VCC writers: compares, class tests, carries and division steps.
bool MayWriteVcc(Type type)
{
	return StartsWith(type, "V") && (Contains(type, "Cmp") || Contains(type, "Class") || Contains(type, "Co") || Contains(type, "Div"));
}

// Over-approximates the implicit VCC readers: selects, carry-ins and the VCC branches.
bool MayReadVcc(Type type)
{
	return AnyPrefix(type, {"SCbranchVcc"}) || (StartsWith(type, "V") && (Contains(type, "Cndmask") || Contains(type, "Co") ||
	                                                                       Contains(type, "Div")));
}

bool Reads(const ShaderInstruction& inst, unsigned word)
{
	for (int source = 0; source < inst.src_num; ++source)
	{
		if (Covers(inst.src[source], word)) { return true; }
	}
	if (word == kM0Word) { return AnyPrefix(inst.type, {"Ds", "VInterp", "SSendmsg", "VMovrel", "SMovrel"}); }
	if (word == kSccWord) { return ReadsSccImplicitly(inst.type) || AnyPrefix(inst.type, {"SCbranchScc"}); }
	return IsVcc(word) && MayReadVcc(inst.type);
}

bool Kills(const ShaderInstruction& inst, unsigned word)
{
	if (word == kSccWord) { return IsScalarAlu(inst.type) && WritesScc(inst.type); }
	return Covers(inst.dst, word) || Covers(inst.dst2, word);
}

bool IsBranch(Type type)
{
	return type == Type::SBranch || StartsWith(type, "SCbranch");
}

// No path from instruction `from` reads `word` before redefining it. Only forward branches are followed, so
// one pass in program order sees every path; anything else counts as a read.
bool DeadFrom(const ShaderCode& code, uint32_t from, unsigned word)
{
	const auto&              instructions = code.GetInstructions();
	std::map<uint32_t, bool> reaching;
	bool                     live = true;
	for (uint32_t index = from; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (const auto target = reaching.find(inst.pc); target != reaching.end()) { live = live || target->second; }
		if (live && Reads(inst, word)) { return false; }
		if (Kills(inst, word)) { live = false; }
		if (IsBranch(inst.type))
		{
			const int64_t target = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
			if (target <= static_cast<int64_t>(inst.pc)) { return false; }
			reaching[static_cast<uint32_t>(target)] = reaching[static_cast<uint32_t>(target)] || live;
			if (inst.type == Type::SBranch) { live = false; }
		} else if (AnyPrefix(inst.type, {"SSetpc", "SSwappc"}))
		{
			if (live) { return false; }
		} else if (StartsWith(inst.type, "SEndpgm"))
		{
			live = false;
		}
	}
	return true;
}

bool WritesExec(const ShaderInstruction& inst)
{
	return Covers(inst.dst, kExecLoWord) || Covers(inst.dst, kExecLoWord + 1u) || Covers(inst.dst2, kExecLoWord) ||
	       Covers(inst.dst2, kExecLoWord + 1u) || WritesExecOnly(inst.type) || Contains(inst.type, "Saveexec");
}

// An instruction whose only effect under an empty EXEC is the scalar words it defines. Vector work is masked
// by EXEC; scalar loads go through a buffer descriptor's bounds, never a raw address.
bool InertUnderEmptyExec(const ShaderInstruction& inst)
{
	if (WritesExec(inst)) { return false; }
	if (AnyPrefix(inst.type, {"SWaitcnt", "SNop", "SInstPrefetch", "SBufferLoad"}) || IsScalarAlu(inst.type)) { return true; }
	if (StartsWith(inst.type, "VWritelane")) { return false; }
	if (StartsWith(inst.type, "V")) { return true; }
	return (StartsWith(inst.type, "Image") || IsBufferLoad(inst.type)) && !WritesMemory(inst.type);
}

// Words the skipped range may define. SCC for scalar ALU, VCC for the implicit vector writers.
bool CollectDefinitions(const ShaderCode& code, uint32_t first, uint32_t end, std::vector<unsigned>* words)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = first; index < end; ++index)
	{
		const auto& inst = instructions.At(index);
		if (!InertUnderEmptyExec(inst)) { return false; }
		AddWords(inst.dst, words);
		AddWords(inst.dst2, words);
		if (IsScalarAlu(inst.type) && WritesScc(inst.type)) { words->push_back(kSccWord); }
		if (MayWriteVcc(inst.type))
		{
			words->push_back(kVccLoWord);
			words->push_back(kVccLoWord + 1u);
		}
	}
	return true;
}

bool FindIndex(const ShaderCode& code, uint32_t pc, uint32_t* index)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t i = 0; i < instructions.Size(); ++i)
	{
		if (instructions.At(i).pc == pc)
		{
			*index = i;
			return true;
		}
	}
	return false;
}

// Another branch lands strictly inside the skipped range: the skip and that branch's region share blocks
// without nesting, which no structured selection expresses.
bool EnteredFromOutside(const ShaderCode& code, uint32_t branch_pc, uint32_t target_pc)
{
	const auto inside = [&](uint32_t pc) { return pc > branch_pc && pc < target_pc; };
	for (const auto& label: code.GetLabels())
	{
		if (!label.IsDisabled() && label.GetSrc() != branch_pc && !inside(label.GetSrc()) && inside(label.GetDst()))
		{
			return true;
		}
	}
	return false;
}

// Running the skipped range with an empty EXEC leaves the same state at the target as the skip, except for the
// scalar words it defines; the skip is removable when none of them is read again. Only a skip whose range is
// entered from outside is removed: a nested skip keeps saving the work of an empty wave.
bool SkipIsRemovable(const ShaderCode& code, uint32_t branch)
{
	const auto& inst = code.GetInstructions().At(branch);
	if (inst.type != Type::SCbranchExecz) { return false; }
	const int64_t target_pc = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
	uint32_t      target    = 0;
	if (target_pc <= static_cast<int64_t>(inst.pc) || !FindIndex(code, static_cast<uint32_t>(target_pc), &target) ||
	    !EnteredFromOutside(code, inst.pc, static_cast<uint32_t>(target_pc)))
	{
		return false;
	}
	std::vector<unsigned> words;
	if (!CollectDefinitions(code, branch + 1u, target, &words)) { return false; }
	for (const unsigned word: words)
	{
		if (!DeadFrom(code, target, word)) { return false; }
	}
	return true;
}

} // namespace

void ShaderRemoveNeutralExecSkips(ShaderCode* code)
{
	auto& instructions = code->GetInstructions();
	for (uint32_t index = 0; index < instructions.Size();)
	{
		if (!SkipIsRemovable(*code, index))
		{
			++index;
			continue;
		}
		const uint32_t branch_pc = instructions.At(index).pc;
		for (auto* labels: {&code->GetLabels(), &code->GetIndirectLabels()})
		{
			for (auto& label: *labels)
			{
				if (label.GetSrc() == branch_pc) { label.Disable(); }
			}
		}
		instructions.RemoveAt(index);
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
