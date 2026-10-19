#include "Emulator/Graphics/ShaderFragmentMaskFlow.h"

#include "ShaderLaneFlow.h"
#include "ShaderNativeWaveInternal.h"
#include "ShaderSpirvInternal.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

using namespace LaneFlow;

using Type    = ShaderInstructionType;
using Operand = ShaderOperandType;

// A mask that may lack any launched lane, down to whole quads.
constexpr uint8_t kPartial = kNotFull | kMissesQuad;
// Extent of the undefined entry VCC, and of each per-instruction fact not reached by the walk.
constexpr uint8_t kUnknownExtent = kExtent;

bool Plain(const ShaderOperand& operand)
{
	return !operand.dpp && !operand.absolute && !operand.negate && !operand.clamp;
}

bool IsCompare(Type type)
{
	return StartsWith(type, "SCmp") || StartsWith(type, "SBitcmp");
}

// Scalar ALU forms that always define SCC from their result (S_MUL/S_BFM/S_BREV and the
// bit-scan family do not), so they replace its taint instead of accumulating onto it.
bool DefinesScc(Type type)
{
	return AnyPrefix(type, {"SAnd", "SOr", "SXor", "SNand", "SNor", "SXnor", "SNot", "SWqm", "SLsh", "SAshr", "SAdd", "SSub", "SBfe",
	                        "SMin", "SMax", "SAbs", "SBcnt"}) &&
	       !StartsWith(type, "SMulk");
}

bool IsConditionalBranch(Type type)
{
	return type == Type::SCbranchScc0 || type == Type::SCbranchScc1 || type == Type::SCbranchVccz || type == Type::SCbranchVccnz ||
	       type == Type::SCbranchExecz || type == Type::SCbranchExecnz;
}

// A static scalar spill slot: (VGPR, lane).
using SpillSlot = std::pair<int, int>;

// The lane flow state plus the taint of each spill slot on this path. V_WRITELANE ignores EXEC, so a spill
// replaces its slot's taint; a compiler reuses a dead slot for an unrelated word.
struct MaskFlowState: FlowState
{
	std::map<SpillSlot, uint8_t> spills;

	bool Merge(const MaskFlowState& other);
};

bool MaskFlowState::Merge(const MaskFlowState& other)
{
	if (!other.reachable) { return false; }
	if (!reachable)
	{
		*this = other;
		return true;
	}
	bool changed = FlowState::Merge(other);
	for (const auto& [slot, taint]: other.spills)
	{
		auto&         mine   = spills[slot];
		const uint8_t merged = static_cast<uint8_t>(mine | taint);
		changed              = changed || merged != mine;
		mine                 = merged;
	}
	return changed;
}

// A branch region whose entry is lane divergent, with the scalar words and spill slots written in it.
struct Region
{
	uint32_t            end_pc = 0;
	std::bitset<kWords> written;
	std::set<SpillSlot> written_slots;
	// Mask words whose bit is zero for every lane outside the region (a subset of the EXEC it was entered with):
	// compares evaluated and EXEC copies taken while EXEC was such a subset (VOPC clears inactive lanes).
	std::bitset<kWords> zero_outside;
	// Mask words whose bit is one for every lane outside the region: the complement of such a subset, or a union with
	// one. D = D AND M with such an M keeps every outside lane's bit.
	std::bitset<kWords> one_outside;
	bool                exec_subset = true; // EXEC is still a subset of the entry EXEC
};

// A loop found from a backward branch; its body is the instructions with pc in [head_pc, back_pc].
struct Loop
{
	uint32_t head_pc = 0;
	uint32_t back_pc = 0;
	// A lane-divergent branch leaves the loop or repeats it, so lanes run different numbers of
	// iterations and the host subgroup's count is not the guest wave's.
	bool divergent = false;
	// Every scalar word and spill slot some instruction of the body may write.
	std::bitset<kWords> written;
	std::set<SpillSlot> written_slots;
	// Words whose every write in the body keeps the bit of each lane outside its region (D = D AND/OR/ANDN2 with a
	// mask confined that way): a lane's bit then follows only its own iterations, so leaving the loop does not
	// make the word divergent. Observed during a walk and confirmed by the next one.
	std::bitset<kWords> exempt;
	std::bitset<kWords> preserving_seen;
	std::bitset<kWords> violated; // a non-preserving write was seen; never exempt again

	[[nodiscard]] bool Contains(uint32_t pc) const { return pc >= head_pc && pc <= back_pc; }
};

// At most this many walks of the program before the flow gives up on a fixpoint. Taint only
// grows, so a walk that changes no loop-head state is the last; real shaders need a handful.
constexpr uint32_t kMaxPasses = 64;

bool BranchTarget(const ShaderInstruction& inst, uint32_t* target)
{
	if (inst.src_num < 1) { return false; }
	const int64_t where = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
	if (where < 0 || where > UINT32_MAX) { return false; }
	*target = static_cast<uint32_t>(where);
	return true;
}

bool IsLaneDependentBranch(Type type)
{
	return type == Type::SCbranchExecz || type == Type::SCbranchExecnz || type == Type::SCbranchVccz || type == Type::SCbranchVccnz;
}

// The scalar words an instruction can write (EXEC is never region state).
void CollectWrites(const ShaderInstruction& inst, std::bitset<kWords>* written)
{
	if (WritesExecOnly(inst.type)) { return; }
	for (const ShaderOperand* dst: {&inst.dst, &inst.dst2})
	{
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(*dst, &first, &count) || dst->type == Operand::ExecLo || dst->type == Operand::ExecHi) { continue; }
		for (unsigned word = 0; word < count; ++word) { written->set(first + word); }
	}
	if (IsCompare(inst.type) || (IsScalarAlu(inst.type) && WritesScc(inst.type))) { written->set(kSccWord); }
}

// Buffer, image and GDS counter writes. A Vulkan helper's write to them has no effect; LDS and scratch are
// invocation storage in a pixel program, which a helper does write.
bool WritesDeviceMemory(const ShaderInstruction& inst)
{
	if (inst.type == Type::DsAppend || inst.type == Type::DsConsume) { return inst.format == ShaderInstructionFormat::VdstGds; }
	for (const char* prefix: {"BufferStore", "BufferAtomic", "TBufferStore", "ImageStore", "ImageAtomic", "Global"})
	{
		if (StartsWith(inst.type, prefix)) { return true; }
	}
	return false;
}

bool IsAddtid(Type type)
{
	return type == Type::DsWriteAddtidB32 || type == Type::DsReadAddtidB32;
}

bool PlainWord(const ShaderOperand& operand)
{
	return Plain(operand) && operand.multiplier == 1.0f && operand.swizzle == 6u && operand.size == 1;
}

enum class ScalarOriginKind : uint8_t
{
	Unknown,
	Number,
	PixelParameter,
};

struct ScalarOrigin
{
	ScalarOriginKind kind = ScalarOriginKind::Unknown;
	uint32_t        number = 0;
};

bool SameOrigin(ScalarOrigin a, ScalarOrigin b)
{
	return a.kind == b.kind && (a.kind != ScalarOriginKind::Number || a.number == b.number);
}

// Track exact scalar moves only. Arithmetic discards provenance and a join keeps
// what its paths agree on; a saved parameter M0 remains identifiable even after
// its entry SGPR is reused.
class PixelScalarOrigins
{
public:
	explicit PixelScalarOrigins(int parameter_sgpr)
	{
		if (parameter_sgpr >= 0 && parameter_sgpr <= 32)
		{
			m_words[static_cast<unsigned>(parameter_sgpr)].kind = ScalarOriginKind::PixelParameter;
		}
	}
	bool Meet(const PixelScalarOrigins& other)
	{
		bool changed = false;
		for (unsigned word = 0; word < kWords; ++word)
		{
			if (m_words[word].kind == ScalarOriginKind::Unknown || SameOrigin(m_words[word], other.m_words[word])) { continue; }
			m_words[word] = {};
			changed       = true;
		}
		return changed;
	}
	[[nodiscard]] ScalarOrigin M0() const { return m_words[kM0Word]; }

	void Step(const ShaderInstruction& inst)
	{
		ScalarOrigin moved;
		if ((inst.type == Type::SMovB32 || inst.type == Type::SMovkI32) && inst.src_num == 1 && PlainWord(inst.dst))
		{
			moved = Source(inst.src[0]);
		}
		for (const ShaderOperand* dst: {&inst.dst, &inst.dst2})
		{
			unsigned first = 0;
			unsigned count = 0;
			if (!ScalarRange(*dst, &first, &count)) { continue; }
			for (unsigned word = 0; word < count; ++word) { m_words[first + word] = {}; }
		}
		unsigned first = 0;
		unsigned count = 0;
		if (ScalarRange(inst.dst, &first, &count) && count == 1u) { m_words[first] = moved; }
	}

private:
	[[nodiscard]] ScalarOrigin Source(const ShaderOperand& operand) const
	{
		if (!Plain(operand) || operand.multiplier != 1.0f || operand.swizzle != 6u) { return {}; }
		if (operand.type == Operand::LiteralConstant || operand.type == Operand::IntegerInlineConstant)
		{
			return {ScalarOriginKind::Number, operand.constant.u};
		}
		unsigned first = 0;
		unsigned count = 0;
		return PlainWord(operand) && ScalarRange(operand, &first, &count) && count == 1u ? m_words[first] : ScalarOrigin {};
	}
	std::array<ScalarOrigin, kWords> m_words {};
};

constexpr uint32_t kAddtidPlaneBytes = 64u * sizeof(uint32_t);

// The byte base of an ADD_TID access: M0 is an exact aligned number and the whole
// wave64 plane (one dword per TID) fits the reserved extra-LDS prefix.
bool AddtidPlane(const ShaderInstruction& inst, ScalarOrigin m0, uint64_t reserved_bytes, uint32_t* base)
{
	if (m0.kind != ScalarOriginKind::Number || (m0.number & 3u) != 0u) { return false; }
	const bool write = inst.type == Type::DsWriteAddtidB32;
	const auto& data = write ? inst.src[0] : inst.dst;
	if (!PlainWord(data) || data.type != Operand::Vgpr || data.register_id < 0 || data.register_id >= kVgprs ||
	    inst.src_num != (write ? 1 : 0) ||
	    inst.format != (write ? ShaderInstructionFormat::VdataOffset : ShaderInstructionFormat::VdstOffset)) { return false; }
	const uint64_t start = static_cast<uint64_t>(m0.number) + inst.ds_offset;
	if ((start & 3u) != 0u || start + kAddtidPlaneBytes > reserved_bytes) { return false; }
	*base = static_cast<uint32_t>(start);
	return true;
}

// Basic blocks of a program without indirect jumps: leaders are the entry, every
// branch target and every instruction after a branch or S_ENDPGM.
struct SpillBlock
{
	uint32_t              begin = 0;
	uint32_t              end   = 0;
	std::vector<uint32_t> successors;
};

bool BuildSpillBlocks(const ShaderCode& code, std::vector<SpillBlock>* blocks)
{
	const auto&                  instructions = code.GetInstructions();
	const uint32_t               count        = instructions.Size();
	std::map<uint32_t, uint32_t> index_of_pc;
	for (uint32_t index = 0; index < count; ++index) { index_of_pc[instructions.At(index).pc] = index; }
	std::map<uint32_t, uint32_t> targets; // branch index -> target index
	std::set<uint32_t>           leaders {0u};
	for (uint32_t index = 0; index < count; ++index)
	{
		const auto& inst = instructions.At(index);
		if (inst.type != Type::SBranch && !IsConditionalBranch(inst.type) && inst.type != Type::SEndpgm) { continue; }
		leaders.insert(index + 1u);
		if (inst.type == Type::SEndpgm) { continue; }
		uint32_t   target = 0;
		const auto found  = BranchTarget(inst, &target) ? index_of_pc.find(target) : index_of_pc.end();
		if (found == index_of_pc.end()) { return false; }
		targets[index] = found->second;
		leaders.insert(found->second);
	}
	leaders.erase(count);
	std::map<uint32_t, uint32_t> block_of;
	for (const auto leader: leaders)
	{
		block_of[leader] = static_cast<uint32_t>(blocks->size());
		blocks->push_back({leader, count, {}});
		if (blocks->size() > 1u) { (*blocks)[blocks->size() - 2u].end = leader; }
	}
	for (auto& block: *blocks)
	{
		const auto& last = instructions.At(block.end - 1u);
		if (targets.count(block.end - 1u) != 0u) { block.successors.push_back(block_of[targets[block.end - 1u]]); }
		if (last.type != Type::SBranch && last.type != Type::SEndpgm && block.end < count)
		{
			block.successors.push_back(block_of[block.end]);
		}
	}
	return true;
}

// The scalar origins and the planes stored on every path to a program point.
struct AddtidSpillState
{
	PixelScalarOrigins origins;
	std::set<uint32_t> stored;
};

bool MergeSpillState(std::optional<AddtidSpillState>* entry, const AddtidSpillState& incoming)
{
	if (!entry->has_value())
	{
		*entry = incoming;
		return true;
	}
	auto&      state   = **entry;
	bool       changed = state.origins.Meet(incoming.origins);
	const auto before  = state.stored.size();
	for (auto plane = state.stored.begin(); plane != state.stored.end();)
	{
		plane = incoming.stored.count(*plane) != 0u ? std::next(plane) : state.stored.erase(plane);
	}
	return changed || state.stored.size() != before;
}

// Walk one block. With `planes`, also prove every access and every interpolation
// and collect each accessed plane.
bool StepSpillBlock(const ShaderCode& code, const SpillBlock& block, uint64_t reserved_bytes, AddtidSpillState* state,
                    std::set<uint32_t>* planes)
{
	for (uint32_t index = block.begin; index < block.end; ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		const auto  m0   = state->origins.M0();
		if (planes != nullptr && StartsWith(inst.type, "VInterp") && m0.kind != ScalarOriginKind::PixelParameter) { return false; }
		uint32_t   base  = 0;
		const bool exact = IsAddtid(inst.type) && AddtidPlane(inst, m0, reserved_bytes, &base);
		if (planes != nullptr && IsAddtid(inst.type))
		{
			const bool unstored = inst.type == Type::DsReadAddtidB32 && state->stored.count(base) == 0u;
			if (!exact || unstored) { return false; }
			planes->insert(base);
		}
		if (exact && inst.type == Type::DsWriteAddtidB32) { state->stored.insert(base); }
		state->origins.Step(inst);
	}
	return true;
}

} // namespace

bool ShaderAddtidAccessesArePrivatePlanes(const ShaderCode& code, int parameter_sgpr, uint64_t reserved_bytes)
{
	const auto& instructions = code.GetInstructions();
	bool        any_addtid   = false;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if ((StartsWith(inst.type, "Ds") && !IsAddtid(inst.type)) || inst.type == Type::SSetpcB64 ||
		    inst.type == Type::SSwappcB64 ||
		    (AnyPrefix(inst.type, {"Buffer", "TBuffer"}) && (inst.buffer_flags & 1u) != 0u)) { return false; }
		any_addtid = any_addtid || IsAddtid(inst.type);
	}
	std::vector<SpillBlock> blocks;
	if (!any_addtid || !BuildSpillBlocks(code, &blocks)) { return false; }
	std::vector<std::optional<AddtidSpillState>> entries(blocks.size());
	entries[0] = AddtidSpillState {PixelScalarOrigins(parameter_sgpr), {}};
	std::vector<uint32_t> work {0u};
	while (!work.empty())
	{
		const auto block = work.back();
		work.pop_back();
		auto state = *entries[block];
		StepSpillBlock(code, blocks[block], reserved_bytes, &state, nullptr);
		for (const auto successor: blocks[block].successors)
		{
			if (MergeSpillState(&entries[successor], state)) { work.push_back(successor); }
		}
	}
	std::set<uint32_t> planes;
	for (uint32_t block = 0; block < blocks.size(); ++block)
	{
		if (!entries[block].has_value()) { continue; } // statically unreachable
		auto state = *entries[block];
		if (!StepSpillBlock(code, blocks[block], reserved_bytes, &state, &planes)) { return false; }
	}
	return std::adjacent_find(planes.begin(), planes.end(),
	                          [](uint32_t low, uint32_t high) { return high - low < kAddtidPlaneBytes; }) == planes.end();
}

namespace {

// The pixel emitter's per-invocation array holds the reserved extra-LDS prefix.
bool HasPrivatePixelAddtidSpills(const ShaderCode& code)
{
	if (code.GetPixelExtraLdsDwords() == 0u)
	{
		return false;
	}
	const uint64_t reserved_bytes = std::min(static_cast<uint64_t>(code.GetPixelExtraLdsDwords()) * sizeof(uint32_t),
	                                         static_cast<uint64_t>(kDsAddtidSpillDwords) * sizeof(uint32_t));
	return ShaderAddtidAccessesArePrivatePlanes(code, code.GetPixelParameterSgpr(), reserved_bytes);
}

class Flow
{
public:
	ShaderFragmentMaskFlow Run(const ShaderCode& code, bool wave32);

private:
	bool Fail(const ShaderInstruction& inst, const char* reason)
	{
		m_result.refused_pc = inst.pc;
		m_result.located    = true;
		m_result.reason     = reason;
		return false;
	}
	[[nodiscard]] ShaderOperand Vcc() const;
	[[nodiscard]] bool          IsMaskOp(Type type, Type b64, Type b32) const { return type == b64 || (m_wave32 && type == b32); }
	[[nodiscard]] uint8_t Taint(const ShaderOperand& operand) const;
	[[nodiscard]] uint8_t SourceTaint(const ShaderInstruction& inst) const;
	bool                  CheckSources(const ShaderInstruction& inst, bool* propagates);
	bool                  Define(const ShaderInstruction& inst, const ShaderOperand& dst, uint8_t taint);
	void                  MarkWritten(unsigned first, unsigned count);
	[[nodiscard]] bool    ZeroOutsideRegion(const ShaderOperand& operand) const;
	[[nodiscard]] bool    PreservesLanesOutsideRegion(const ShaderInstruction& inst) const;
	[[nodiscard]] bool    OneOutsideRegion(const ShaderOperand& operand) const;
	[[nodiscard]] bool    ResultOneOutsideRegion(const ShaderInstruction& inst) const;
	void                  MarkOneOutsideRegion(const ShaderOperand& dst);
	void                  TrackRegionSubset(const ShaderInstruction& inst, bool exec_subset_before);
	void                  UpdateScc(const ShaderInstruction& inst, uint8_t source_taint);
	bool                  UniformComparison(const ShaderInstruction& inst) const;
	void                  TrackUniformMask(const ShaderInstruction& inst, const std::bitset<kWords>& sources, bool uniform_comparison);
	[[nodiscard]] bool    IsUniformMask(const ShaderOperand& operand) const;
	[[nodiscard]] bool    IsUniformMaskIn(const std::bitset<kWords>& masks, const ShaderOperand& operand) const;
	void                  SetUniformMask(const ShaderOperand& dst, bool uniform);
	bool                  Step(const ShaderInstruction& inst);
	[[nodiscard]] bool    IsAnyLaneTest(const ShaderInstruction& inst) const;
	void                  AnyLaneTest(const ShaderInstruction& inst);
	[[nodiscard]] bool    IsScalarSpill(const ShaderInstruction& inst) const;
	bool                  ScalarSpill(const ShaderInstruction& inst);
	bool                  Branch(const ShaderInstruction& inst);
	void                  EnterJoin(uint32_t pc);
	bool                  Walk();
	void                  FindLoops();
	ShaderFragmentMaskFlow Fixpoint();
	bool                  RefineLoopExemptions(bool admitted);
	void                  LeaveLoops(MaskFlowState* state, uint32_t from_pc, uint32_t to_pc) const;
	bool                  HasInstructionAt(uint32_t pc) const;
	void                  MarkAcceptedExecWrites(const ShaderCode& code);
	bool                  ExecWide() const { return (m_state.scalar[kExecLoWord] & kWide) != 0u; }
	[[nodiscard]] uint8_t WideResult(const ShaderInstruction& inst) const;
	[[nodiscard]] uint8_t ExtentResult(const ShaderInstruction& inst) const;
	void                  RecordFacts(uint32_t index, const ShaderInstruction& inst);
	void                  PublishFacts();
	bool                  InertDeadTail(uint32_t pc) const;

	ShaderFragmentMaskFlow        m_result;
	MaskFlowState                     m_state;
	std::map<uint32_t, MaskFlowState> m_entry; // state arriving at each branch target, by pc; kept across walks
	std::vector<Region>           m_regions; // open lane-divergent regions
	std::vector<Loop>             m_loops;
	bool                          m_changed = false; // a back edge changed a loop-head state during this walk
	bool                          m_lane_preserving = false; // the write being defined keeps every inactive lane's bit
	bool                          m_addtid_spills = false;
	bool                          m_wave32        = false; // VCC is its low word (the program's lane masks are narrowed)
	// SCC holds "some lane of a mask is set", written by the last SCC writer with no join since.
	bool                          m_scc_any_lane         = false;
	bool                          m_scc_any_lane_uniform = false;
	// Per instruction, from the walk in progress: the extent of EXEC before it, and the taint of a
	// V_CNDMASK_B32 condition (all extent bits where the walk did not reach it).
	std::vector<uint8_t>          m_exec_extent;
	std::vector<uint8_t>          m_condition;
	const ShaderCode*             m_code    = nullptr;
	uint32_t                      m_index   = 0; // instruction being stepped
	uint32_t                      m_launch_exec_end = 0; // ShaderLaunchExecEnd of m_code
};

// The VCC a branch or VCCZ tests: a wave32 uses only its low word.
ShaderOperand Flow::Vcc() const
{
	ShaderOperand vcc {};
	vcc.type = Operand::VccLo;
	vcc.size = m_wave32 ? 1 : 2;
	return vcc;
}

uint8_t Flow::Taint(const ShaderOperand& operand) const
{
	if (operand.type == Operand::ExecZ || operand.type == Operand::VccZ) { return kMask; }
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(operand, &first, &count)) { return kClean; }
	uint8_t taint = kClean;
	bool    any_mask = false;
	bool    all_mask = true;
	for (unsigned word = 0; word < count; ++word)
	{
		taint |= m_state.scalar[first + word];
		any_mask = any_mask || (m_state.scalar[first + word] & kMask) != 0u;
		all_mask = all_mask && (m_state.scalar[first + word] & kMask) != 0u;
	}
	// A 64-bit mask with one word replaced by a number is neither a mask nor a number.
	return (any_mask && !all_mask) ? static_cast<uint8_t>(taint | kMixed) : taint;
}

uint8_t Flow::SourceTaint(const ShaderInstruction& inst) const
{
	uint8_t taint = kClean;
	for (int source = 0; source < inst.src_num; ++source) { taint |= Taint(inst.src[source]); }
	for (int address = 0; address < inst.mimg_address_num; ++address) { taint |= Taint(inst.mimg_address[address]); }
	return taint;
}

// Each tainted source must be consumed as a lane bit or by mask algebra, and only
// scalar ALU may carry a diverged word or a pair that is half mask, half number (VCC
// enters undefined, so a load into VCC_LO leaves such a pair). Neither is refused until
// something observes it. `propagates` reports whether the result carries the taint
// of its sources.
bool Flow::CheckSources(const ShaderInstruction& inst, bool* propagates)
{
	*propagates = false;
	for (int address = 0; address < inst.mimg_address_num; ++address)
	{
		if (Taint(inst.mimg_address[address]) != kClean) { return Fail(inst, "mask used as an image address"); }
	}
	// A count of the launch EXEC is a number; whether its lane positions hold is the admission's question.
	const int first = ShaderCountsLaunchExec(*m_code, m_index, m_launch_exec_end) ? 1 : 0;
	for (int source = first; source < inst.src_num; ++source)
	{
		const auto& operand = inst.src[source];
		if (operand.type == Operand::ExecZ || operand.type == Operand::VccZ)
		{
			return Fail(inst, "mask summary flag observed");
		}
		const uint8_t taint = Taint(operand);
		if (taint == kClean) { continue; }
		if ((taint & kMixed) != 0u && !IsScalarAlu(inst.type)) { return Fail(inst, "mask pair partly overwritten by a number"); }
		if ((taint & kDiverged) != 0u && !IsScalarAlu(inst.type))
		{
			return Fail(inst, "scalar written under divergent control flow is read after the join");
		}
		// A select's choice is already checked against SCC; its data may be masks.
		if ((taint & kMask) != 0u && !IsLaneBit(inst, source) && !IsMaskAlgebra(inst.type) && !IsSelect(inst.type))
		{
			return Fail(inst, "mask observed as a number");
		}
		if (!IsLaneBit(inst, source)) { *propagates = true; }
	}
	return true;
}

void Flow::MarkWritten(unsigned first, unsigned count)
{
	const uint32_t pc = m_code->GetInstructions().At(m_index).pc;
	for (auto& loop: m_loops)
	{
		if (!loop.Contains(pc)) { continue; }
		for (unsigned word = 0; word < count; ++word)
		{
			(m_lane_preserving ? loop.preserving_seen : loop.violated).set(first + word);
		}
	}
	for (auto& region: m_regions)
	{
		for (unsigned word = 0; word < count; ++word)
		{
			region.zero_outside.reset(first + word);
			region.one_outside.reset(first + word);
			if (!m_lane_preserving) { region.written.set(first + word); }
		}
	}
}

bool Flow::ZeroOutsideRegion(const ShaderOperand& operand) const
{
	if (m_regions.empty()) { return false; }
	if (operand.type == Operand::ExecLo) { return m_regions.back().exec_subset; }
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(operand, &first, &count)) { return false; }
	for (unsigned word = 0; word < count; ++word)
	{
		if (!m_regions.back().zero_outside.test(first + word)) { return false; }
	}
	return true;
}

bool Flow::OneOutsideRegion(const ShaderOperand& operand) const
{
	unsigned first = 0;
	unsigned count = 0;
	if (m_regions.empty() || !ScalarRange(operand, &first, &count)) { return false; }
	for (unsigned word = 0; word < count; ++word)
	{
		if (!m_regions.back().one_outside.test(first + word)) { return false; }
	}
	return true;
}

static bool SameScalar(const ShaderOperand& a, const ShaderOperand& b)
{
	return a.type == b.type && a.register_id == b.register_id;
}

// D = D OR/ANDN2/XOR M with M zero outside the region, or D = D AND M with M one outside it, leaves the bit of every
// lane outside the region unchanged, so it does not matter that the guest runs the region for the wave while a host
// subgroup with no lane inside skips it.
bool Flow::PreservesLanesOutsideRegion(const ShaderInstruction& inst) const
{
	if (inst.src_num != 2 || inst.dst.type == Operand::ExecLo) { return false; }
	if (IsMaskOp(inst.type, Type::SAndB64, Type::SAndB32))
	{
		return (SameScalar(inst.dst, inst.src[0]) && OneOutsideRegion(inst.src[1])) ||
		       (SameScalar(inst.dst, inst.src[1]) && OneOutsideRegion(inst.src[0]));
	}
	if (!IsMaskOp(inst.type, Type::SOrB64, Type::SOrB32) && !IsMaskOp(inst.type, Type::SAndn2B64, Type::SAndn2B32) &&
	    !IsMaskOp(inst.type, Type::SXorB64, Type::SXorB32))
	{
		return false;
	}
	return SameScalar(inst.dst, inst.src[0]) && ZeroOutsideRegion(inst.src[1]);
}

// Evaluated before the write: whether the lane-mask result has every outside lane's bit set.
bool Flow::ResultOneOutsideRegion(const ShaderInstruction& inst) const
{
	if (m_regions.empty() || inst.dst.type == Operand::ExecLo || ShaderInstructionWritesExec(inst)) { return false; }
	const Type type = inst.type;
	const bool two  = inst.src_num == 2;
	if (IsMaskOp(type, Type::SOrn2B64, Type::SOrn2B32)) { return two && (OneOutsideRegion(inst.src[0]) || ZeroOutsideRegion(inst.src[1])); }
	if (IsMaskOp(type, Type::SOrB64, Type::SOrB32)) { return two && (OneOutsideRegion(inst.src[0]) || OneOutsideRegion(inst.src[1])); }
	if (IsMaskOp(type, Type::SAndB64, Type::SAndB32)) { return two && OneOutsideRegion(inst.src[0]) && OneOutsideRegion(inst.src[1]); }
	if (IsMaskOp(type, Type::SNotB64, Type::SNotB32)) { return inst.src_num == 1 && ZeroOutsideRegion(inst.src[0]); }
	if (IsMaskOp(type, Type::SMovB64, Type::SMovB32)) { return inst.src_num == 1 && OneOutsideRegion(inst.src[0]); }
	return false;
}

void Flow::MarkOneOutsideRegion(const ShaderOperand& dst)
{
	unsigned first = 0;
	unsigned count = 0;
	if (m_regions.empty() || !ScalarRange(dst, &first, &count)) { return; }
	for (unsigned word = 0; word < count; ++word) { m_regions.back().one_outside.set(first + word); }
}

// Keeps the innermost region's subset facts after `inst`: which mask words are zero outside the region and whether
// EXEC is still a subset of the EXEC the region was entered with. Narrowing keeps it, a restore from a subset copy
// re-establishes it, a widening (S_WQM) or any other EXEC write loses it.
void Flow::TrackRegionSubset(const ShaderInstruction& inst, bool exec_subset_before)
{
	if (m_regions.empty()) { return; }
	auto&      region     = m_regions.back();
	const auto subset_of  = [&](const ShaderOperand& op) {
		if (op.type == Operand::ExecLo) { return exec_subset_before; }
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(op, &first, &count)) { return false; }
		for (unsigned word = 0; word < count; ++word)
		{
			if (!region.zero_outside.test(first + word)) { return false; }
		}
		return true;
	};
	const auto mark = [&](const ShaderOperand& op) {
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(op, &first, &count)) { return; }
		for (unsigned word = 0; word < count; ++word) { region.zero_outside.set(first + word); }
	};
	const bool writes_exec = ShaderInstructionWritesExec(inst);
	if (StartsWith(inst.type, "VCmp") && !writes_exec && exec_subset_before) { mark(inst.dst); }
	const bool move       = IsMaskOp(inst.type, Type::SMovB64, Type::SMovB32);
	const bool and_saveexec = IsMaskOp(inst.type, Type::SAndSaveexecB64, Type::SAndSaveexecB32);
	if (move && inst.src[0].type == Operand::ExecLo && inst.dst.type != Operand::ExecLo && exec_subset_before) { mark(inst.dst); }
	if (!writes_exec) { return; }
	if (and_saveexec && exec_subset_before) { mark(inst.dst); }
	const bool to_exec = inst.dst.type == Operand::ExecLo;
	bool subset = false;
	if (WritesExecOnly(inst.type)) { subset = exec_subset_before; }
	if (move && to_exec) { subset = subset_of(inst.src[0]); }
	if (IsMaskOp(inst.type, Type::SAndB64, Type::SAndB32) && to_exec) { subset = subset_of(inst.src[0]) || subset_of(inst.src[1]); }
	if (and_saveexec) { subset = exec_subset_before || subset_of(inst.src[0]); }
	if (IsMaskOp(inst.type, Type::SAndn2B64, Type::SAndn2B32) && to_exec) { subset = subset_of(inst.src[0]); }
	region.exec_subset = subset;
}

bool Flow::Define(const ShaderInstruction& inst, const ShaderOperand& dst, uint8_t taint)
{
	if (dst.type == Operand::ExecZ || dst.type == Operand::VccZ) { return Fail(inst, "mask summary flag written"); }
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(dst, &first, &count))
	{
		if (dst.type == Operand::Sgpr) { return Fail(inst, "scalar destination out of range"); }
		return true;
	}
	if (dst.type == Operand::ExecLo || dst.type == Operand::ExecHi)
	{
		if ((taint & kMask) == 0u) { return Fail(inst, "EXEC assigned a numeric value"); }
		if ((taint & kDiverged) != 0u) { return Fail(inst, "EXEC assigned a divergent scalar"); }
		if ((taint & kMixed) != 0u) { return Fail(inst, "EXEC assigned a mask pair partly overwritten by a number"); }
		m_state.scalar[kExecLoWord]      = static_cast<uint8_t>(kMask | (taint & (kWide | kExtent)));
		m_state.scalar[kExecLoWord + 1u] = m_state.scalar[kExecLoWord];
		return true; // EXEC is one bit per lane by construction; it is never region state
	}
	for (unsigned word = 0; word < count; ++word)
	{
		m_state.scalar[first + word] = taint;
		m_state.uniform_mask.reset(first + word);
	}
	MarkWritten(first, count);
	if (dst.type == Operand::VccLo || (dst.type == Operand::VccHi && !m_wave32)) { m_state.vcc_uniform = false; }
	return true;
}

// SCC is written implicitly by most scalar ALU operations. A compare always defines
// it from its sources, so it is the only instruction that can clear the taint; any
// other scalar operation that saw a tainted value may have made SCC tainted.
void Flow::UpdateScc(const ShaderInstruction& inst, uint8_t source_taint)
{
	if (IsCompare(inst.type) || DefinesScc(inst.type))
	{
		m_state.scalar[kSccWord] = source_taint;
		MarkWritten(kSccWord, 1u);
		return;
	}
	if (StartsWith(inst.type, "S") && WritesScc(inst.type) && (source_taint != kClean || ShaderInstructionTypeChangesExec(inst.type)))
	{
		m_state.scalar[kSccWord] |= static_cast<uint8_t>(source_taint | (ShaderInstructionTypeChangesExec(inst.type) ? kMask : kClean));
		MarkWritten(kSccWord, 1u);
	}
}

// Scalar words that hold a compare result identical in every lane. A VCC branch on such a mask decides
// the same way for the whole wave (and for a one-lane host invocation), so it opens no divergent region.
bool Flow::IsUniformMask(const ShaderOperand& operand) const
{
	return IsUniformMaskIn(m_state.uniform_mask, operand);
}

bool Flow::IsUniformMaskIn(const std::bitset<kWords>& masks, const ShaderOperand& operand) const
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(operand, &first, &count)) { return false; }
	for (unsigned word = 0; word < count; ++word)
	{
		if (!masks.test(first + word)) { return false; }
	}
	return true;
}

void Flow::SetUniformMask(const ShaderOperand& dst, bool uniform)
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(dst, &first, &count)) { return; }
	for (unsigned word = 0; word < count; ++word) { m_state.uniform_mask.set(first + word, uniform); }
	if (dst.type == Operand::VccLo || (dst.type == Operand::VccHi && !m_wave32)) { m_state.vcc_uniform = IsUniformMask(Vcc()); }
}

// Compare of uniform sources, a copy of such a mask, EXEC narrowed by one (nonzero exactly when the compare holds),
// or the AND/OR of two such masks: every active lane gets c1 && c2 (c1 || c2), the same bit. `sources` holds the
// uniform words, and `uniform_comparison` the compare's verdict, before the instruction wrote its destination,
// which may also be a source (S_AND VCC, VCC, ... or V_CMP s[8:9], 0, s9).
void Flow::TrackUniformMask(const ShaderInstruction& inst, const std::bitset<kWords>& sources, bool uniform_comparison)
{
	if (StartsWith(inst.type, "VCmp")) { SetUniformMask(inst.dst, uniform_comparison); return; }
	const auto uniform = [&](const ShaderOperand& operand) { return IsUniformMaskIn(sources, operand); };
	const bool copy     = (inst.type == Type::SMovB64 || inst.type == Type::SMovB32) && inst.src_num == 1 && uniform(inst.src[0]);
	const bool narrowed = (inst.type == Type::SAndB64 || inst.type == Type::SAndB32) && inst.src_num == 2 &&
	                      ((inst.src[0].type == Operand::ExecLo && uniform(inst.src[1])) ||
	                       (inst.src[1].type == Operand::ExecLo && uniform(inst.src[0])));
	const bool combined = (inst.type == Type::SAndB64 || inst.type == Type::SAndB32 || inst.type == Type::SOrB64 ||
	                       inst.type == Type::SOrB32) &&
	                      inst.src_num == 2 && uniform(inst.src[0]) && uniform(inst.src[1]);
	if (copy || narrowed || combined) { SetUniformMask(inst.dst, true); }
}

// A vector compare of constants and scalar numbers gives every lane the same bit.
bool Flow::UniformComparison(const ShaderInstruction& inst) const
{
	if (!StartsWith(inst.type, "VCmp") || ShaderInstructionTypeChangesExec(inst.type)) { return false; }
	for (int source = 0; source < inst.src_num; ++source)
	{
		const auto& operand = inst.src[source];
		unsigned    first   = 0;
		unsigned    count   = 0;
		if (IsConstant(operand)) { continue; }
		if (!ScalarRange(operand, &first, &count) || Taint(operand) != kClean) { return false; }
	}
	return true;
}

// Width of the mask an instruction produces: live-only (narrow) or possibly including the
// helper lanes of a quad (kWide). Bits of a compare exist only for the lanes that were active.
uint8_t Flow::WideResult(const ShaderInstruction& inst) const
{
	const Type type = inst.type;
	if (StartsWith(type, "SWqm")) { return kWide; }
	if (StartsWith(type, "VCmp") || ShaderNativePackedResult(inst)) { return ExecWide() ? kWide : kClean; }
	bool any_wide = false, all_wide = true, any_mask = false, nonzero_constant = false;
	for (int source = 0; source < inst.src_num; ++source)
	{
		const uint8_t taint = Taint(inst.src[source]);
		if ((taint & kMask) != 0u)
		{
			any_mask = true;
			any_wide = any_wide || (taint & kWide) != 0u;
			all_wide = all_wide && (taint & kWide) != 0u;
		} else if (IsConstant(inst.src[source]) && inst.src[source].constant.u != 0u)
		{
			nonzero_constant = true;
		}
	}
	const bool first_wide = inst.src_num > 0 && (Taint(inst.src[0]) & kWide) != 0u;
	// a & ~b is a subset of a; a & b of each operand; a complement is never confined to live lanes.
	if (StartsWith(type, "SAndn2")) { return first_wide ? kWide : kClean; }
	if (StartsWith(type, "SAnd")) { return (any_mask && all_wide) ? kWide : kClean; }
	if (AnyPrefix(type, {"SOrn2", "SNand", "SNor", "SXnor", "SNot"})) { return kWide; }
	return (any_wide || (nonzero_constant && StartsWith(type, "S") && !IsSelect(type) && !StartsWith(type, "SMov"))) ? kWide : kClean;
}

// Extent of the mask an instruction produces against the launched quads. Only a copy keeps a full mask
// (every launched lane), and S_WQM makes one from a mask that meets every launched quad, as the entry EXEC
// does. Anything else may lose lanes; the complements and nonzero constants may also add lanes the host
// subgroup does not hold.
uint8_t Flow::ExtentResult(const ShaderInstruction& inst) const
{
	const Type type   = inst.type;
	const auto extent = [&](int source) { return static_cast<uint8_t>(Taint(inst.src[source]) & kExtent); };
	if ((type == Type::SMovB64 || type == Type::SMovB32) && inst.src_num == 1)
	{
		if (!IsConstant(inst.src[0])) { return extent(0); }
		return inst.src[0].constant.u == 0u ? kPartial : kExtent;
	}
	if (StartsWith(type, "SWqm") && inst.src_num == 1)
	{
		const uint8_t source = extent(0);
		return static_cast<uint8_t>((source & kOuter) | ((source & kMissesQuad) != 0u ? kPartial : kClean));
	}
	if (StartsWith(type, "VCmp") || ShaderNativePackedResult(inst))
	{
		return static_cast<uint8_t>((m_state.scalar[kExecLoWord] & kOuter) | kPartial);
	}
	bool any_mask = false, any_outer = false, all_outer = true, nonzero_constant = false;
	for (int source = 0; source < inst.src_num; ++source)
	{
		if ((Taint(inst.src[source]) & kMask) != 0u)
		{
			const bool outer = (extent(source) & kOuter) != 0u;
			any_mask         = true;
			any_outer        = any_outer || outer;
			all_outer        = all_outer && outer;
		} else if (IsConstant(inst.src[source]) && inst.src[source].constant.u != 0u)
		{
			nonzero_constant = true;
		}
	}
	if (StartsWith(type, "SAndn2")) { return static_cast<uint8_t>((inst.src_num > 0 ? extent(0) & kOuter : kOuter) | kPartial); }
	if (StartsWith(type, "SAnd")) { return static_cast<uint8_t>(((any_mask && all_outer) ? kOuter : kClean) | kPartial); }
	if (AnyPrefix(type, {"SOrn2", "SNand", "SNor", "SXnor", "SNot"})) { return kExtent; }
	return static_cast<uint8_t>(((any_outer || nonzero_constant) ? kOuter : kClean) | kPartial);
}

// Code that runs when no lane survives and produces nothing: optionally EXEC = 0, a null
// MRT export, S_ENDPGM. Reaching it early is what the hardware's wave-wide "any lane left"
// branch does, and the host model takes it per subgroup, so neither can be told apart.
bool Flow::InertDeadTail(uint32_t pc) const
{
	const auto& instructions = m_code->GetInstructions();
	uint32_t    index        = 0;
	while (index < instructions.Size() && instructions.At(index).pc != pc) { ++index; }
	if (index >= instructions.Size()) { return false; }
	const auto zero_exec = [](const ShaderInstruction& inst)
	{
		return (inst.type == Type::SMovB64 || inst.type == Type::SMovB32) && inst.dst.type == Operand::ExecLo && inst.src_num == 1 &&
		       inst.src[0].type == Operand::IntegerInlineConstant && inst.src[0].constant.i == 0;
	};
	if (zero_exec(instructions.At(index))) { ++index; }
	if (index + 2u != instructions.Size()) { return false; }
	const auto& export_inst = instructions.At(index);
	// An empty valid mask exported through an MRT slot or through the dedicated null target: either
	// way every lane of the wave is killed, which is also what the main path does when none survives.
	const bool null_export = ShaderIsNullMrtDoneFormat(export_inst.format) || export_inst.format == ShaderInstructionFormat::NullVmDone;
	return export_inst.type == Type::Exp && null_export && export_inst.src_num == 0 &&
	       instructions.At(index + 1u).type == Type::SEndpgm;
}

bool Flow::HasInstructionAt(uint32_t pc) const
{
	for (const auto& candidate: m_code->GetInstructions())
	{
		if (candidate.pc == pc) { return true; }
	}
	return false;
}

// Control leaving a loop whose lanes ran different iteration counts: every scalar the body
// writes holds, for a lane that stopped early, the value of an iteration the guest wave may
// have run further. Lanes still running are unaffected, so nothing changes inside the loop.
void Flow::LeaveLoops(MaskFlowState* state, uint32_t from_pc, uint32_t to_pc) const
{
	for (const auto& loop: m_loops)
	{
		if (!loop.divergent || !loop.Contains(from_pc) || loop.Contains(to_pc)) { continue; }
		for (unsigned word = 0; word < kWords; ++word)
		{
			if (loop.written.test(word) && !loop.exempt.test(word)) { state->scalar[word] |= kDiverged; }
		}
		for (const auto& slot: loop.written_slots) { state->spills[slot] |= kDiverged; }
	}
}

void Flow::FindLoops()
{
	const auto& instructions = m_code->GetInstructions();
	for (const auto& inst: instructions)
	{
		uint32_t target = 0;
		if ((inst.type == Type::SBranch || IsConditionalBranch(inst.type)) && BranchTarget(inst, &target) && target <= inst.pc)
		{
			Loop loop;
			loop.head_pc = target;
			loop.back_pc = inst.pc;
			m_loops.push_back(loop);
		}
	}
	for (auto& loop: m_loops)
	{
		for (const auto& inst: instructions)
		{
			if (!loop.Contains(inst.pc)) { continue; }
			CollectWrites(inst, &loop.written);
			int vgpr = 0;
			int lane = 0;
			if (IsStaticScalarSpillWrite(inst, &vgpr, &lane)) { loop.written_slots.insert({vgpr, lane}); }
			uint32_t target = 0;
			if (!IsLaneDependentBranch(inst.type) || !BranchTarget(inst, &target)) { continue; }
			loop.divergent = loop.divergent || !loop.Contains(target) || target == loop.head_pc;
		}
	}
}

bool Flow::Branch(const ShaderInstruction& inst)
{
	bool divergent = false;
	uint32_t target = 0;
	if (!BranchTarget(inst, &target)) { return Fail(inst, "branch without a target"); }
	switch (inst.type)
	{
		case Type::SBranch: break;
		case Type::SCbranchExecz:
		case Type::SCbranchExecnz: divergent = true; break;
		case Type::SCbranchVccz:
		case Type::SCbranchVccnz:
		{
			const auto vcc = Vcc();
			if ((Taint(vcc) & kMixed) != 0u) { return Fail(inst, "VCC branch on a mask pair partly overwritten by a number"); }
			divergent = !IsUniformMask(vcc);
			break;
		}
		case Type::SCbranchScc0:
		case Type::SCbranchScc1:
		{
			if (m_scc_any_lane)
			{
				// The question S_CBRANCH_VCCZ asks of VCC: a host subgroup answers it for its own lanes. Only
				// the VCC/EXEC forms make a loop lane divergent (FindLoops), so a back edge stays refused.
				if (target <= inst.pc) { return Fail(inst, "loop branch on an any-lane test"); }
				divergent = !m_scc_any_lane_uniform;
				break;
			}
			const uint8_t scc = m_state.scalar[kSccWord];
			// "No lane survives" (SCC of a narrow mask) may skip to a tail that does nothing.
			const bool survivors_only = (scc & kMask) != 0u && (scc & (kDiverged | kMixed | kWide)) == 0u;
			if (scc != kClean && !(survivors_only && target > inst.pc && InertDeadTail(target)))
			{
				return Fail(inst, "SCC branch on a mask or divergent value");
			}
			break;
		}
		default: return Fail(inst, "branch form is not supported");
	}
	if (!HasInstructionAt(target)) { return Fail(inst, "branch target is not an instruction"); }
	const bool backward = target <= inst.pc;
	MaskFlowState outgoing = m_state;
	LeaveLoops(&outgoing, inst.pc, target);
	const auto found = m_entry.find(target);
	if (found == m_entry.end())
	{
		m_entry.emplace(target, outgoing);
		m_changed = m_changed || backward;
	} else if (found->second.Merge(outgoing))
	{
		m_changed = m_changed || backward;
	}
	// A backward branch is the loop's own business (see Loop::divergent); a region has no end to reach.
	if (divergent && !backward) { m_regions.push_back({target, {}}); }
	if (inst.type == Type::SBranch)
	{
		m_state.reachable = false;
	} else if (backward)
	{
		LeaveLoops(&m_state, inst.pc, inst.pc + 4u); // falling through leaves the loop
	}
	return true;
}

// Paths rejoin here: merge the branch states, then mark what the skipped block wrote. Entries
// stay: the next walk starts from them again until no back edge changes anything.
void Flow::EnterJoin(uint32_t pc)
{
	if (const auto entry = m_entry.find(pc); entry != m_entry.end())
	{
		m_state.Merge(entry->second);
		m_scc_any_lane = false;
	}
	for (auto region = m_regions.begin(); region != m_regions.end();)
	{
		if (region->end_pc != pc)
		{
			++region;
			continue;
		}
		for (unsigned word = 0; word < kWords; ++word)
		{
			if (region->written.test(word)) { m_state.scalar[word] |= kDiverged; }
		}
		for (const auto& slot: region->written_slots) { m_state.spills[slot] |= kDiverged; }
		region = m_regions.erase(region);
	}
}

// A V_WRITELANE/V_READLANE with a constant lane that only moves a scalar through one VGPR lane and back (a
// compiler spill) is lowered to a private scalar slot, as the generic lowering proves with the same predicates,
// so it exchanges no lane. A run-time lane index or an unpaired move is not a spill.
bool Flow::IsScalarSpill(const ShaderInstruction& inst) const
{
	int vgpr = 0;
	int lane = 0;
	return (IsStaticScalarSpillWrite(inst, &vgpr, &lane) && IsScalarSpillStore(*m_code, m_index, vgpr, lane)) ||
	       (IsStaticScalarSpillRead(inst, &vgpr, &lane) && HasLiveScalarSpill(*m_code, m_index, vgpr, lane));
}

// The slot carries the taint of the word last spilled to it on this path: a spilled mask word is read back as a
// mask word. Like a scalar word, a slot written in a divergent region or loop differs between lanes after it.
bool Flow::ScalarSpill(const ShaderInstruction& inst)
{
	int vgpr = 0;
	int lane = 0;
	if (IsStaticScalarSpillWrite(inst, &vgpr, &lane))
	{
		m_state.spills[{vgpr, lane}] = Taint(inst.src[0]);
		for (auto& region: m_regions) { region.written_slots.insert({vgpr, lane}); }
		return true;
	}
	(void)IsStaticScalarSpillRead(inst, &vgpr, &lane);
	const auto slot = m_state.spills.find({vgpr, lane});
	return Define(inst, inst.dst, slot != m_state.spills.end() ? slot->second : kClean);
}

// S_CMP_LG_U64 / S_CMP_EQ_U64 of a lane mask against zero: whether any lane's bit is set. The pair must be
// lane bits only; a word that is also a number or divergent does not qualify. Helper bits under WQM may count,
// as they do for VCCZ: the packed host mask gathers every quad member's bit the guest compare produced.
bool Flow::IsAnyLaneTest(const ShaderInstruction& inst) const
{
	if ((inst.type != Type::SCmpLgU64 && inst.type != Type::SCmpEqU64) || inst.src_num != 2) { return false; }
	const auto zero = [](const ShaderOperand& operand)
	{ return Plain(operand) && operand.type == Operand::IntegerInlineConstant && operand.constant.i == 0; };
	const ShaderOperand* mask = zero(inst.src[1]) ? &inst.src[0] : (zero(inst.src[0]) ? &inst.src[1] : nullptr);
	unsigned first = 0;
	unsigned count = 0;
	if (mask == nullptr || !Plain(*mask) || !ScalarRange(*mask, &first, &count) || count != 2u) { return false; }
	const auto lane_bits = [](uint8_t taint) { return (taint & kMask) != 0u && (taint & ~(kMask | kWide | kExtent)) == 0u; };
	return lane_bits(m_state.scalar[first]) && lane_bits(m_state.scalar[first + 1u]);
}

void Flow::AnyLaneTest(const ShaderInstruction& inst)
{
	const auto& mask = IsConstant(inst.src[0]) ? inst.src[1] : inst.src[0];
	m_state.scalar[kSccWord] = kMask; // every other SCC reader still refuses it
	MarkWritten(kSccWord, 1u);
	m_scc_any_lane         = true;
	m_scc_any_lane_uniform = IsUniformMask(mask);
}

bool Flow::Step(const ShaderInstruction& inst)
{
	if (IsAnyLaneTest(inst))
	{
		AnyLaneTest(inst);
		return true;
	}
	if (IsCompare(inst.type) || (IsScalarAlu(inst.type) && WritesScc(inst.type))) { m_scc_any_lane = false; }
	// While EXEC holds no helper lane (the launch EXEC, or a mask narrowed from it), no Vulkan helper's
	// device-memory write is missing on the host either, so such a write observes the same lanes on both.
	const bool live_write = !ExecWide() && WritesDeviceMemory(inst);
	if (WritesMemory(inst.type) && !(m_addtid_spills && IsAddtid(inst.type)) && !live_write)
	{
		return Fail(inst, "memory write: helper lanes would be observable");
	}
	if (inst.type == Type::Exp && ExecWide()) { return Fail(inst, "export while EXEC may include helper lanes"); }
	if (inst.type == Type::SBarrier) { return Fail(inst, "barrier"); }
	if (inst.type == Type::SBranch || IsConditionalBranch(inst.type)) { return Branch(inst); }
	if (inst.type == Type::SEndpgm)
	{
		m_state.reachable = false; // this path ends; the next label revives the walk
		return true;
	}
	if (IsScalarSpill(inst)) { return ScalarSpill(inst); }
	// EXEC/VCC branches, forward exits and back edges alike, are emitted quad-uniform (helpers take their quad's
	// vote), so a quad enters every region and runs every loop iteration as one. A fetch inside them computes
	// derivatives from its whole quad, whose masked lanes keep the register values the hardware reads.
	// A select or conditional move reads SCC; a tainted SCC makes its choice lane dependent.
	if (ReadsSccImplicitly(inst.type) && m_state.scalar[kSccWord] != kClean)
	{
		return Fail(inst, "SCC derived from a mask selects a value");
	}
	bool propagates = false;
	if (!CheckSources(inst, &propagates)) { return false; }
	const uint8_t source_taint = SourceTaint(inst);
	uint8_t       produced     = propagates ? static_cast<uint8_t>(source_taint & (kMask | kDiverged | kMixed)) : kClean;
	if (ShaderNativePackedResult(inst) || ShaderInstructionTypeChangesExec(inst.type)) { produced |= kMask; }
	// An empty mask is a mask: EXEC = 0 ends a discarded lane's execution.
	const bool empty_mask = (inst.type == Type::SMovB64 || inst.type == Type::SMovB32) && inst.src_num == 1 &&
	                        inst.src[0].type == Operand::IntegerInlineConstant && inst.src[0].constant.i == 0;
	if (empty_mask && inst.dst.type == Operand::ExecLo) { produced |= kMask; }
	if ((produced & kMask) != 0u) { produced = static_cast<uint8_t>((produced & ~kWide) | WideResult(inst) | ExtentResult(inst)); }
	const uint8_t exec_extent = m_state.scalar[kExecLoWord] & kExtent;
	if (WritesExecOnly(inst.type))
	{
		// EXEC &= compare: it narrows, so it stays wide only if it already was.
		m_state.scalar[kExecLoWord]      = static_cast<uint8_t>(kMask | (ExecWide() ? kWide : kClean) | (exec_extent & kOuter) | kPartial);
		m_state.scalar[kExecLoWord + 1u] = m_state.scalar[kExecLoWord];
		return true;
	}
	// S_*_SAVEEXEC: the destination is a copy of the old EXEC, helper lanes included.
	if (ShaderInstructionTypeChangesExec(inst.type) && inst.dst.type != Operand::ExecLo)
	{
		produced = static_cast<uint8_t>((produced & ~(kExtent | kWide)) | exec_extent | (ExecWide() ? kWide : kClean));
	}
	const bool exec_subset_before = !m_regions.empty() && m_regions.back().exec_subset;
	const bool one_outside_after  = ResultOneOutsideRegion(inst);
	m_lane_preserving = PreservesLanesOutsideRegion(inst);
	const auto uniform_sources    = m_state.uniform_mask;
	const bool uniform_comparison = UniformComparison(inst);
	const bool defined = Define(inst, inst.dst, produced) && Define(inst, inst.dst2, produced);
	m_lane_preserving = false;
	if (!defined) { return false; }
	TrackRegionSubset(inst, exec_subset_before);
	if (one_outside_after) { MarkOneOutsideRegion(inst.dst); }
	if (ShaderInstructionTypeChangesExec(inst.type) && inst.dst.type != Operand::ExecLo)
	{
		// S_*_SAVEEXEC: the destination took the old EXEC; EXEC is now the combination.
		const bool narrowing = StartsWith(inst.type, "SAnd") && !StartsWith(inst.type, "SAndn");
		const bool wide = narrowing ? (ExecWide() && (Taint(inst.src[0]) & kWide) != 0u) : true;
		const bool outer = narrowing ? ((exec_extent & kOuter) != 0u && (Taint(inst.src[0]) & kOuter) != 0u) : true;
		m_state.scalar[kExecLoWord]      = static_cast<uint8_t>(kMask | (wide ? kWide : kClean) | (outer ? kOuter : kClean) | kPartial);
		m_state.scalar[kExecLoWord + 1u] = m_state.scalar[kExecLoWord];
	}
	TrackUniformMask(inst, uniform_sources, uniform_comparison);
	// SCC = "result is nonzero": it is as wide as the mask the instruction produced, not as the
	// sources it read (ANDN2 of a narrow mask by a wide one is narrow).
	const uint8_t scc_source = (produced & kMask) != 0u ? static_cast<uint8_t>((source_taint & ~kWide) | (produced & kWide)) : source_taint;
	UpdateScc(inst, scc_source);
	return true;
}

// Every EXEC write the walk accepted is pointwise mask algebra over existing lanes. The
// widening ones are safe because an export or a memory write is refused while EXEC may
// include helper lanes (the kWide bit), so no closed-bracket shape has to be recognized.
void Flow::MarkAcceptedExecWrites(const ShaderCode& code)
{
	const auto& instructions = code.GetInstructions();
	m_result.closed_exec_write.assign(instructions.Size(), false);
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		m_result.closed_exec_write[index] = ShaderInstructionWritesExec(instructions.At(index));
	}
}

// The state a walk steps an instruction with already joins every path into it, so the facts of the
// walk that reaches the fixpoint hold for the program.
void Flow::RecordFacts(uint32_t index, const ShaderInstruction& inst)
{
	m_exec_extent[index] = static_cast<uint8_t>(m_state.scalar[kExecLoWord] & kExtent);
	m_condition[index]   = inst.type == Type::VCndmaskB32 && inst.src_num == 3 ? Taint(inst.src[2]) : kUnknownExtent;
}

void Flow::PublishFacts()
{
	const size_t size = m_exec_extent.size();
	m_result.exec_in_quads.assign(size, false);
	m_result.exec_full.assign(size, false);
	m_result.live_condition.assign(size, false);
	for (size_t index = 0; index < size; ++index)
	{
		const uint8_t condition = m_condition[index];
		m_result.exec_in_quads[index]  = (m_exec_extent[index] & kOuter) == 0u;
		m_result.exec_full[index]      = m_exec_extent[index] == kClean;
		m_result.live_condition[index] = (condition & kMask) != 0u && (condition & (kWide | kOuter | kMixed | kDiverged)) == 0u;
	}
}

// One pass over the program in address order from the entry state, merging the states that
// arrive at branch targets. Forward edges are complete within a pass; back edges feed the next.
bool Flow::Walk()
{
	m_state   = {};
	m_changed = false;
	m_scc_any_lane = false;
	m_regions.clear();
	// EXEC, VCC and SCC enter undefined or lane dependent: treat them as masks. The entry EXEC holds
	// the covered lanes, at least one of each launched quad; nothing is known of the entry VCC.
	for (unsigned word : {kVccLoWord, kVccLoWord + 1u, kExecLoWord, kExecLoWord + 1u, kSccWord}) { m_state.scalar[word] = kMask; }
	for (unsigned word : {kVccLoWord, kVccLoWord + 1u}) { m_state.scalar[word] |= kUnknownExtent; }
	for (unsigned word : {kExecLoWord, kExecLoWord + 1u}) { m_state.scalar[word] |= kNotFull; }
	const auto& instructions = m_code->GetInstructions();
	m_exec_extent.assign(instructions.Size(), kUnknownExtent);
	m_condition.assign(instructions.Size(), kUnknownExtent);
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		EnterJoin(inst.pc);
		if (!m_state.reachable) { continue; }
		m_index = index;
		RecordFacts(index, inst);
		if (!Step(inst)) { return false; }
	}
	if (!m_regions.empty())
	{
		m_result.reason = "a divergent region was never closed";
		return false;
	}
	return true;
}

ShaderFragmentMaskFlow Flow::Run(const ShaderCode& code, bool wave32)
{
	m_code                   = &code;
	m_wave32                 = wave32;
	m_launch_exec_end        = ShaderLaunchExecEnd(code);
	const auto& instructions = code.GetInstructions();
	if (code.GetType() != ShaderType::Pixel || instructions.IsEmpty() ||
	    instructions.At(instructions.Size() - 1u).type != Type::SEndpgm)
	{
		m_result.reason = "not a pixel program ending in S_ENDPGM";
		return m_result;
	}
	m_addtid_spills = HasPrivatePixelAddtidSpills(code);
	FindLoops();
	// Each word enters a loop's exemption at most once and leaves it at most once.
	for (unsigned attempt = 0; attempt <= 2u * kWords; ++attempt)
	{
		const auto result = Fixpoint();
		if (!RefineLoopExemptions(result.lane_local)) { return result; }
	}
	m_result = {};
	m_result.reason = "loop exemptions did not settle";
	return m_result;
}

ShaderFragmentMaskFlow Flow::Fixpoint()
{
	m_result = {};
	m_entry.clear();
	for (auto& loop: m_loops) { loop.preserving_seen.reset(); }
	for (uint32_t pass = 0; pass < kMaxPasses; ++pass)
	{
		if (!Walk()) { return m_result; }
		if (!m_changed)
		{
			m_result.lane_local = true;
			MarkAcceptedExecWrites(*m_code);
			PublishFacts();
			return m_result;
		}
	}
	m_result.reason = "the mask flow did not reach a fixpoint";
	return m_result;
}

// Returns whether another fixpoint is needed. An admitted result stands once no exempt word saw a
// non-preserving write; a refused one is retried with every word only ever written preservingly.
bool Flow::RefineLoopExemptions(bool admitted)
{
	bool changed = false;
	for (auto& loop: m_loops)
	{
		auto next = admitted ? loop.exempt : (loop.exempt | (loop.preserving_seen & loop.written));
		next &= ~loop.violated;
		changed = changed || next != loop.exempt;
		loop.exempt = next;
	}
	return changed;
}

} // namespace

ShaderFragmentMaskFlow ShaderAnalyzeFragmentMaskFlow(const ShaderCode& code, uint32_t guest_wave_size)
{
	Flow flow;
	if (guest_wave_size != 32u) { return flow.Run(code, false); }
	const auto narrowed = ShaderWave32LaneMaskProgram(code);
	auto       result   = flow.Run(narrowed, true);
	result.guest_wave_size = 32u;
	return result;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
