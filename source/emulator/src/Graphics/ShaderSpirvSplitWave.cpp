#include "ShaderMaskAnalysis.h"
#include "ShaderSpirvInternal.h"

#include <algorithm>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// A split guest wave runs as P host subgroups of L lanes each (wave64: P = 64 / L,
// L = 32, 16 or 8; wave32: P = 2, L = 16): subgroup P*w + p holds guest lanes
// p*L .. p*L+L-1 of wave w.
// The per-lane emitters see the guest lane in %gl_SubgroupInvocationID and
// call the helpers below for every wave-wide value; each helper call is a
// rendezvous of the parts, which execute the same sequence of calls because
// every branch they take depends only on wave-uniform state.
//
// Per guest wave the exchange area (ShaderSplitWaveExchangeDwords) holds
// [0..P-1] the number of exchanges each part has published, [P..3P-1] the
// words they published (P + part * 2 + parity) and two banks of one dword per
// guest lane for exchanges that cross parts. Two parities suffice: a part
// reuses a slot only after every other part reached the following exchange.

bool Spirv::UsesSplitComputeWave() const
{
	return (m_code.GetType() == ShaderType::Compute || UsesFragmentCompute()) && m_cs_input_info != nullptr &&
	       m_cs_input_info->native_wave_sensitive && m_cs_input_info->wave_layout.strategy == ShaderComputeWaveStrategy::Split64On32;
}

const ShaderLaneMaskPlan* Spirv::LaneMaskPlan() const
{
	const bool split_compute = UsesSplitComputeWave() && m_code.GetType() == ShaderType::Compute;
	if (!split_compute && (m_code.GetType() != ShaderType::Pixel || UsesFragmentCompute()))
	{
		return nullptr;
	}
	if (!m_lane_mask_plan_ready)
	{
		m_lane_mask_plan_ready = true;
		m_lane_mask_plan       = ShaderLaneMaskPlanOf(m_code);
	}
	return &m_lane_mask_plan;
}

bool Spirv::IsLaneLocalMaskResult(uint32_t index) const
{
	const auto* plan = LaneMaskPlan();
	return plan != nullptr && index < plan->local.size() && plan->local[index];
}

bool Spirv::IsLaneMaskGather(uint32_t index) const
{
	const auto* plan = LaneMaskPlan();
	return plan != nullptr && index < plan->gather.size() && plan->gather[index];
}

// Words one grouped lane read carries: the v4uint its helper returns.
static constexpr uint32_t kSplitReadGroupWords = 4;
// Most compares one grouped ballot carries: the %split_ballot_part words hold four 8-lane votes.
static constexpr uint32_t kSplitBallotGroupMembers = 4;

static bool IsGroupableLaneRead(const ShaderInstruction& inst)
{
	return inst.type == ShaderInstructionType::VReadlaneB32 && inst.src_num >= 2 && !inst.src[0].dpp &&
	       inst.src[0].type == ShaderOperandType::Vgpr &&
	       (inst.src[1].type == ShaderOperandType::Sgpr || inst.src[1].type == ShaderOperandType::VccLo ||
	        inst.src[1].type == ShaderOperandType::VccHi);
}

static bool IsSameSelector(const ShaderOperand& a, const ShaderOperand& b)
{
	return a.type == b.type && (a.type != ShaderOperandType::Sgpr || a.register_id == b.register_id);
}

// VCC halves an operand covers: bit 0 the low half, bit 1 the high half.
static uint32_t VccHalves(const ShaderOperand& operand)
{
	if (operand.type == ShaderOperandType::VccLo)
	{
		return operand.size == 2 ? 3u : 1u;
	}
	return operand.type == ShaderOperandType::VccHi ? 2u : 0u;
}

static bool WritesOperand(const ShaderOperand& dst, const ShaderOperand& operand)
{
	if (dst.type == ShaderOperandType::Sgpr && operand.type == ShaderOperandType::Sgpr)
	{
		const int dst_size = dst.size > 0 ? dst.size : 1;
		const int size     = operand.size > 0 ? operand.size : 1;
		return operand.register_id < dst.register_id + dst_size && dst.register_id < operand.register_id + size;
	}
	return (VccHalves(dst) & VccHalves(operand)) != 0u;
}

static std::vector<uint32_t> SortedBranchTargets(const ShaderCode& code)
{
	std::vector<uint32_t> targets;
	for (const auto& label: code.GetLabels())
	{
		if (!label.IsDisabled()) { targets.push_back(label.GetDst()); }
	}
	for (const auto& label: code.GetIndirectLabels())
	{
		if (!label.IsDisabled()) { targets.push_back(label.GetDst()); }
	}
	std::sort(targets.begin(), targets.end());
	return targets;
}

// Keeps a run of two or more members as a group and starts the next run.
static void CloseSplitGroup(std::vector<uint32_t>* run, std::vector<uint32_t>* group_of, std::vector<std::vector<uint32_t>>* groups)
{
	if (run->size() >= 2u)
	{
		for (const uint32_t member: *run) { (*group_of)[member] = static_cast<uint32_t>(groups->size()); }
		groups->push_back(*run);
	}
	run->clear();
}

// Runs of V_READLANE at one selector, in one block, whose sources no instruction between them writes: only
// V_WRITELANE, which exchanges nothing, may sit between members. The head reads every member's source in one
// exchange, since V_READLANE ignores EXEC and nothing between changes what the members would read.
static void FindSplitReadGroups(const ShaderCode& code, std::vector<uint32_t>* group_of, std::vector<std::vector<uint32_t>>* groups)
{
	const auto& instructions = code.GetInstructions();
	const auto  targets      = SortedBranchTargets(code);

	group_of->assign(instructions.Size(), UINT32_MAX);
	std::vector<uint32_t> run;
	std::vector<int>      written;
	const auto close = [&]()
	{
		CloseSplitGroup(&run, group_of, groups);
		written.clear();
	};
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (std::binary_search(targets.begin(), targets.end(), inst.pc)) { close(); }
		if (IsGroupableLaneRead(inst))
		{
			const bool joins = !run.empty() && run.size() < kSplitReadGroupWords &&
			                   IsSameSelector(instructions.At(run[0]).src[1], inst.src[1]) &&
			                   std::find(written.begin(), written.end(), inst.src[0].register_id) == written.end();
			if (!joins) { close(); }
			run.push_back(index);
			if (WritesOperand(inst.dst, inst.src[1])) { close(); }
			continue;
		}
		if (!run.empty() && inst.type == ShaderInstructionType::VWritelaneB32 && inst.dst.type == ShaderOperandType::Vgpr)
		{
			written.push_back(inst.dst.register_id);
			continue;
		}
		close();
	}
	close();
}

const std::vector<uint32_t>* Spirv::SplitReadGroup(uint32_t index) const
{
	if (!UsesSplitComputeWave() || UsesFragmentWaveTier() || UsesFragmentPartialWave())
	{
		return nullptr;
	}
	if (!m_split_read_groups_ready)
	{
		m_split_read_groups_ready = true;
		FindSplitReadGroups(m_code, &m_split_read_group_of, &m_split_read_groups);
	}
	if (index >= m_split_read_group_of.size() || m_split_read_group_of[index] == UINT32_MAX)
	{
		return nullptr;
	}
	return &m_split_read_groups[m_split_read_group_of[index]];
}

static bool IsGroupableCompare(const ShaderInstruction& inst)
{
	const auto name = ShaderInstructionTypeName(inst.type);
	return name.rfind("VCmp", 0) == 0 && name.rfind("VCmpx", 0) != 0 &&
	       (inst.dst.type == ShaderOperandType::Sgpr || inst.dst.type == ShaderOperandType::VccLo);
}

// Runs of consecutive V_CMP in one block whose masks leave the lane and whose sources no earlier member writes:
// every member sees the EXEC and the sources it would see alone, so each votes at its own position and the
// tail publishes all votes in one exchange, then writes every member's mask in program order.
static void FindSplitBallotGroups(const ShaderCode& code, const std::vector<bool>* lane_local, uint32_t limit,
                                  std::vector<uint32_t>* group_of, std::vector<std::vector<uint32_t>>* groups)
{
	const auto& instructions = code.GetInstructions();
	const auto  targets      = SortedBranchTargets(code);

	group_of->assign(instructions.Size(), UINT32_MAX);
	std::vector<uint32_t> run;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (std::binary_search(targets.begin(), targets.end(), inst.pc)) { CloseSplitGroup(&run, group_of, groups); }
		const bool local = lane_local != nullptr && index < lane_local->size() && (*lane_local)[index];
		if (!IsGroupableCompare(inst) || local)
		{
			CloseSplitGroup(&run, group_of, groups);
			continue;
		}
		const auto reads_member = [&](uint32_t member)
		{
			const auto& dst = instructions.At(member).dst;
			for (int s = 0; s < inst.src_num; ++s)
			{
				if (WritesOperand(dst, inst.src[s])) { return true; }
			}
			return false;
		};
		if (run.size() >= limit || std::any_of(run.begin(), run.end(), reads_member)) { CloseSplitGroup(&run, group_of, groups); }
		run.push_back(index);
	}
	CloseSplitGroup(&run, group_of, groups);
}

// Members one grouped exchange carries: each part packs every member's L-bit vote in its word.
uint32_t Spirv::SplitBallotGroupLimit() const
{
	return std::min(32u / SplitWavePartLanes(), kSplitBallotGroupMembers);
}

const std::vector<uint32_t>* Spirv::SplitBallotGroup(uint32_t index) const
{
	if (!UsesSplitComputeWave() || m_code.GetType() != ShaderType::Compute || UsesFragmentWaveTier() || UsesFragmentPartialWave() ||
	    SplitBallotGroupLimit() < 2u)
	{
		return nullptr;
	}
	if (!m_split_ballot_groups_ready)
	{
		m_split_ballot_groups_ready = true;
		const auto* plan            = LaneMaskPlan();
		FindSplitBallotGroups(m_code, plan != nullptr ? &plan->local : nullptr, SplitBallotGroupLimit(), &m_split_ballot_group_of,
		                      &m_split_ballot_groups);
	}
	if (index >= m_split_ballot_group_of.size() || m_split_ballot_group_of[index] == UINT32_MAX)
	{
		return nullptr;
	}
	return &m_split_ballot_groups[m_split_ballot_group_of[index]];
}

String8 Spirv::HostLaneVariable() const
{
	return UsesSplitComputeWave() ? "split_host_lane" : "gl_SubgroupInvocationID";
}

uint32_t Spirv::SplitWavePartLanes() const
{
	EXIT_IF(!UsesSplitComputeWave());
	return m_cs_input_info->wave_layout.native_subgroup_size;
}

String8 Spirv::SplitWaveTypes() const
{
	if (!UsesSplitComputeWave())
	{
		return {};
	}
	const uint32_t lanes  = SplitWavePartLanes();
	const uint32_t parts  = m_cs_input_info->wave_layout.guest_wave_size / lanes;
	const uint32_t stride = ShaderSplitWaveExchangeDwords(lanes);
	return String8::FromPrintf(R"(
%%split_c0 = OpConstant %%uint 0
%%split_c1 = OpConstant %%uint 1
%%split_c2 = OpConstant %%uint 2
%%split_c3 = OpConstant %%uint 3
%%split_c4 = OpConstant %%uint 4
%%split_c5 = OpConstant %%uint 5
%%split_c6 = OpConstant %%uint 6
%%split_c7 = OpConstant %%uint 7
%%split_c8 = OpConstant %%uint 8
%%split_c10 = OpConstant %%uint 10
%%split_c12 = OpConstant %%uint 12
%%split_c14 = OpConstant %%uint 14
%%split_c16 = OpConstant %%uint 16
%%split_c24 = OpConstant %%uint 24
%%split_c31 = OpConstant %%uint 31
%%split_c48 = OpConstant %%uint 48
%%split_c63 = OpConstant %%uint 63
%%split_parts = OpConstant %%uint %u
%%split_parts_mask = OpConstant %%uint %u
%%split_parts_shift = OpConstant %%uint %u
%%split_lanes_mask = OpConstant %%uint %u
%%split_lanes_shift = OpConstant %%uint %u
%%split_lane_area = OpConstant %%uint %u
%%split_scope_workgroup = OpConstant %%uint 2
%%split_scope_subgroup = OpConstant %%uint 3
%%split_acquire = OpConstant %%uint 258
%%split_release = OpConstant %%uint 260
%%split_acq_rel = OpConstant %%uint 264
%%split_stride = OpConstant %%uint %u
%%split_spin_limit = OpConstant %%uint 1048576
%%split_exchange_length = OpConstant %%uint %u
%%split_exchange_array = OpTypeArray %%uint %%split_exchange_length
%%_ptr_Workgroup_split_exchange = OpTypePointer Workgroup %%split_exchange_array
%%_ptr_Workgroup_split_uint = OpTypePointer Workgroup %%uint
%%_ptr_Private_split_uint = OpTypePointer Private %%uint
%%function_split_u = OpTypeFunction %%uint
%%function_split_b = OpTypeFunction %%bool
%%function_split_ballot = OpTypeFunction %%v4uint %%bool
%%function_split_read_group = OpTypeFunction %%v4uint %%uint %%uint %%uint %%uint %%uint
%%function_split_ballot_pack = OpTypeFunction %%uint %%uint %%bool %%uint
%%function_split_ballot_group = OpTypeFunction %%void %%uint
%%function_split_ballot_field = OpTypeFunction %%v4uint %%uint
%%split_lane_bits = OpConstant %%uint %u
)",
	                           parts, parts - 1u, parts == 8u ? 3u : parts == 4u ? 2u : 1u, lanes - 1u, lanes == 8u ? 3u : lanes == 16u ? 4u : 5u,
	                           parts * 3u, stride,
	                           m_cs_input_info->wave_layout.waves * stride,
	                           lanes >= 32u ? 0xffffffffu : (1u << lanes) - 1u);
}

String8 Spirv::SplitWaveVariables() const
{
	if (!UsesSplitComputeWave())
	{
		return {};
	}
	return R"(
%split_host_lane = OpVariable %_ptr_Input_uint Input
%gl_SubgroupInvocationID = OpVariable %_ptr_Private_split_uint Private
%split_exchange = OpVariable %_ptr_Workgroup_split_exchange Workgroup
%split_count = OpVariable %_ptr_Private_split_uint Private %split_c0
%split_read_word1 = OpVariable %_ptr_Private_split_uint Private
%split_read_word2 = OpVariable %_ptr_Private_split_uint Private
%split_read_word3 = OpVariable %_ptr_Private_split_uint Private
%split_ballot_packed = OpVariable %_ptr_Private_split_uint Private
%split_ballot_part0 = OpVariable %_ptr_Private_split_uint Private
%split_ballot_part1 = OpVariable %_ptr_Private_split_uint Private
%split_ballot_part2 = OpVariable %_ptr_Private_split_uint Private
%split_ballot_part3 = OpVariable %_ptr_Private_split_uint Private
%split_ballot_part4 = OpVariable %_ptr_Private_split_uint Private
%split_ballot_part5 = OpVariable %_ptr_Private_split_uint Private
%split_ballot_part6 = OpVariable %_ptr_Private_split_uint Private
%split_ballot_part7 = OpVariable %_ptr_Private_split_uint Private
)";
}

// After the native prolog: publish the guest lane, then clear this part's
// sequence before any part of the workgroup may wait on it.
String8 Spirv::SplitWaveProlog() const
{
	return R"(
%split_part = OpBitwiseAnd %uint %native_wave_subgroup_id %split_parts_mask
%split_lane_base = OpShiftLeftLogical %uint %split_part %split_lanes_shift
%split_guest_lane = OpBitwiseOr %uint %split_lane_base %native_wave_lane_id
OpStore %gl_SubgroupInvocationID %split_guest_lane
%split_wave = OpShiftRightLogical %uint %native_wave_subgroup_id %split_parts_shift
%split_wave_base = OpIMul %uint %split_wave %split_stride
%split_own_sequence = OpIAdd %uint %split_wave_base %split_part
%split_own_sequence_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_own_sequence
OpAtomicStore %split_own_sequence_ptr %split_scope_workgroup %split_c0 %split_c0
OpControlBarrier %split_scope_workgroup %split_scope_workgroup %split_acq_rel
)";
}

// LDS accesses of one guest wave are ordered on hardware; the parts order
// them with a rendezvous after each DS instruction.
String8 Spirv::SplitWaveDsOrder(uint32_t index) const
{
	// The subgroup barrier makes every lane's LDS access available before the elected lane publishes.
	return String8::FromPrintf("OpControlBarrier %%split_scope_subgroup %%split_scope_workgroup %%split_acq_rel\n"
	                           "%%split_ds_%u = OpFunctionCall %%uint %%split_sync\n",
	                           index);
}

void Spirv::CheckSplitWaveCollectives(const String8& body) const
{
	// Quad operations and the DPP shuffles, whose targets come from the host
	// lane and stay inside a 16-lane row, never leave one part.
	const String8 prefix = "OpGroupNonUniform";
	for (uint32_t at = body.FindIndex(prefix); at != Core::STRING8_INVALID_INDEX; at = body.FindIndex(prefix, at + 1))
	{
		const auto rest       = body.Mid(at + prefix.Size(), 16);
		const auto line_break = body.FindLastIndex('\n', at);
		const auto line_start = line_break == Core::STRING8_INVALID_INDEX ? 0u : line_break + 1u;
		const auto line       = body.Mid(line_start, at - line_start).TrimLeft();
		const bool allowed    = rest.StartsWith("QuadBroadcast ") || rest.StartsWith("QuadSwap ") ||
		                     (rest.StartsWith("Shuffle ") && line.StartsWith("%dpp_"));
		if (!allowed)
		{
			EXIT("split-wave compute collective without a cross-part lowering: %s\n", body.Mid(at, 40).c_str());
		}
	}
}

// Picks the word of each part of exchange <n>: this part's own value, or the
// slot another part published.
static String8 SplitWavePartWords(uint32_t parts, const char* tag)
{
	String8 text = String8::FromPrintf("%%%s_slots = OpIAdd %%uint %%%s_base %%split_parts\n"
	                                   "%%%s_parity = OpBitwiseAnd %%uint %%%s_n %%split_c1\n",
	                                   tag, tag, tag, tag);
	for (uint32_t part = 0; part < parts; ++part)
	{
		text += String8::FromPrintf("%%%s_slot%u = OpIAdd %%uint %%%s_slots %%split_c%u\n"
		                            "%%%s_index%u = OpIAdd %%uint %%%s_slot%u %%%s_parity\n"
		                            "%%%s_ptr%u = OpAccessChain %%_ptr_Workgroup_split_uint %%split_exchange %%%s_index%u\n"
		                            "%%%s_load%u = OpLoad %%uint %%%s_ptr%u\n"
		                            "%%%s_is%u = OpIEqual %%bool %%%s_part %%split_c%u\n"
		                            "%%%s_word%u = OpSelect %%uint %%%s_is%u %%%s_own %%%s_load%u\n",
		                            tag, part, tag, part * 2u, tag, part, tag, part, tag, tag, part, tag, part, tag, part, tag, part,
		                            tag, part, tag, part, tag, part, tag, part, tag, tag, part);
	}
	return text;
}

// Publishes this part's word of the next exchange and waits until every other
// part published its word of the same exchange.
static String8 SplitWavePublishFunction(uint32_t parts)
{
	String8 text = R"(
%split_publish = OpFunction %uint None %function_gen5_buffer_format_bytes
%split_publish_value = OpFunctionParameter %uint
%split_publish_entry = OpLabel
%split_publish_subgroup = OpLoad %uint %gl_SubgroupID
%split_publish_part = OpBitwiseAnd %uint %split_publish_subgroup %split_parts_mask
%split_publish_wave = OpShiftRightLogical %uint %split_publish_subgroup %split_parts_shift
%split_publish_base = OpIMul %uint %split_publish_wave %split_stride
%split_publish_previous = OpLoad %uint %split_count
%split_publish_n = OpIAdd %uint %split_publish_previous %split_c1
OpStore %split_count %split_publish_n
%split_publish_parity = OpBitwiseAnd %uint %split_publish_n %split_c1
%split_publish_slots = OpIAdd %uint %split_publish_base %split_parts
%split_publish_own2 = OpShiftLeftLogical %uint %split_publish_part %split_c1
%split_publish_own_slot = OpIAdd %uint %split_publish_own2 %split_publish_parity
%split_publish_own_index = OpIAdd %uint %split_publish_slots %split_publish_own_slot
%split_publish_own_sequence = OpIAdd %uint %split_publish_base %split_publish_part
%split_publish_own_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_publish_own_index
%split_publish_sequence_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_publish_own_sequence
%split_publish_elected = OpGroupNonUniformElect %bool %split_scope_subgroup
OpSelectionMerge %split_publish_published None
OpBranchConditional %split_publish_elected %split_publish_publish %split_publish_published
%split_publish_publish = OpLabel
OpStore %split_publish_own_ptr %split_publish_value
OpAtomicStore %split_publish_sequence_ptr %split_scope_workgroup %split_release %split_publish_n
OpBranch %split_publish_published
%split_publish_published = OpLabel
)";
	// One spin reads every part's sequence, so the parts' latencies overlap
	// instead of adding up; every seen value is n or n + 1. Only the last load
	// acquires: an acquire load per part serializes the loads behind one fence
	// each, while a spin of relaxed loads alone has no effect a caller may
	// observe and the compiler drops it. The fence after the loop orders the
	// reads of what the relaxed loads saw published.
	text += R"(OpBranch %split_publish_header
%split_publish_header = OpLabel
%split_publish_spins = OpPhi %uint %split_c0 %split_publish_published %split_publish_next %split_publish_continue
OpLoopMerge %split_publish_merge %split_publish_continue None
OpBranch %split_publish_body
%split_publish_body = OpLabel
)";
	String8 sequence = "%split_publish_n";
	for (uint32_t part = 0; part < parts; ++part)
	{
		const char* semantics = part + 1u == parts ? "%split_acquire" : "%split_c0";
		text += String8::FromPrintf("%%split_publish_sequence_index%u = OpIAdd %%uint %%split_publish_base %%split_c%u\n"
		                            "%%split_publish_sequence%u_ptr = OpAccessChain %%_ptr_Workgroup_split_uint %%split_exchange "
		                            "%%split_publish_sequence_index%u\n"
		                            "%%split_publish_seen%u = OpAtomicLoad %%uint %%split_publish_sequence%u_ptr %%split_scope_workgroup "
		                            "%s\n"
		                            "%%split_publish_upto%u = OpExtInst %%uint %%GLSL_std_450 UMin %s %%split_publish_seen%u\n",
		                            part, part, part, part, part, part, semantics, part, sequence.c_str(), part);
		sequence = String8::FromPrintf("%%split_publish_upto%u", part);
	}
	text += String8::FromPrintf(R"(%%split_publish_ready = OpIEqual %%bool %s %%split_publish_n
%%split_publish_next = OpIAdd %%uint %%split_publish_spins %%split_c1
%%split_publish_late = OpUGreaterThanEqual %%bool %%split_publish_next %%split_spin_limit
%%split_publish_done = OpLogicalOr %%bool %%split_publish_ready %%split_publish_late
OpBranchConditional %%split_publish_done %%split_publish_merge %%split_publish_continue
%%split_publish_continue = OpLabel
OpBranch %%split_publish_header
%%split_publish_merge = OpLabel
OpMemoryBarrier %%split_scope_workgroup %%split_acquire
OpReturnValue %s
OpFunctionEnd
)",
	                            sequence.c_str(), sequence.c_str());
	return text;
}

// The wave mask of the words %<tag>_word<p> as %<tag>_lo and %<tag>_hi: part p supplies bits p*L .. p*L+L-1.
static String8 SplitWaveMaskWords(uint32_t parts, uint32_t lanes, const char* tag)
{
	String8 text;
	// Mask word w collects the parts whose lanes start in bits 32w .. 32w+31.
	const uint32_t per_word = 32u / lanes;
	for (uint32_t word = 0; word < 2u; ++word)
	{
		const char* name = word == 0u ? "lo" : "hi";
		if (word * per_word >= parts)
		{
			text += String8::FromPrintf("%%%s_%s = OpCopyObject %%uint %%split_c0\n", tag, name);
			continue;
		}
		String8 merged = String8::FromPrintf("%s_word%u", tag, word * per_word);
		for (uint32_t slot = 1; slot < per_word; ++slot)
		{
			const uint32_t part = word * per_word + slot;
			text += String8::FromPrintf("%%%s_word%u_shifted = OpShiftLeftLogical %%uint %%%s_word%u %%split_c%u\n"
			                            "%%%s_%s_%u = OpBitwiseOr %%uint %%%s %%%s_word%u_shifted\n",
			                            tag, part, tag, part, slot * lanes, tag, name, slot, merged.c_str(), tag, part);
			merged = String8::FromPrintf("%s_%s_%u", tag, name, slot);
		}
		text += String8::FromPrintf("%%%s_%s = OpCopyObject %%uint %%%s\n", tag, name, merged.c_str());
	}
	return text;
}

// The wave mask: part p supplies bits p*L .. p*L+L-1.
static String8 SplitWaveBallotFunction(uint32_t parts, uint32_t lanes)
{
	String8 text = R"(
%split_ballot = OpFunction %v4uint None %function_split_ballot
%split_ballot_predicate = OpFunctionParameter %bool
%split_ballot_entry = OpLabel
%split_ballot_host = OpGroupNonUniformBallot %v4uint %split_scope_subgroup %split_ballot_predicate
%split_ballot_own = OpCompositeExtract %uint %split_ballot_host 0
%split_ballot_n = OpFunctionCall %uint %split_publish %split_ballot_own
%split_ballot_subgroup = OpLoad %uint %gl_SubgroupID
%split_ballot_part = OpBitwiseAnd %uint %split_ballot_subgroup %split_parts_mask
%split_ballot_wave = OpShiftRightLogical %uint %split_ballot_subgroup %split_parts_shift
%split_ballot_base = OpIMul %uint %split_ballot_wave %split_stride
)";
	text += SplitWavePartWords(parts, "split_ballot");
	text += SplitWaveMaskWords(parts, lanes, "split_ballot");
	text += "%split_ballot_result = OpCompositeConstruct %v4uint %split_ballot_lo %split_ballot_hi %split_c0 %split_c0\n"
	        "OpReturnValue %split_ballot_result\nOpFunctionEnd\n";
	return text;
}

// Grouped compares: member k adds its part's L-bit vote at bits k*L of one word (%split_ballot_pack), the tail
// publishes that word in one exchange and keeps every part's word (%split_ballot_group), and %split_ballot_field
// takes member k's wave mask out of the kept words.
static String8 SplitWaveBallotGroupFunctions(uint32_t parts, uint32_t lanes)
{
	EXIT_IF(parts > 8u);
	String8 text = R"(
%split_ballot_pack = OpFunction %uint None %function_split_ballot_pack
%split_ballot_pack_packed = OpFunctionParameter %uint
%split_ballot_pack_predicate = OpFunctionParameter %bool
%split_ballot_pack_shift = OpFunctionParameter %uint
%split_ballot_pack_entry = OpLabel
%split_ballot_pack_host = OpGroupNonUniformBallot %v4uint %split_scope_subgroup %split_ballot_pack_predicate
%split_ballot_pack_own = OpCompositeExtract %uint %split_ballot_pack_host 0
%split_ballot_pack_vote = OpShiftLeftLogical %uint %split_ballot_pack_own %split_ballot_pack_shift
%split_ballot_pack_result = OpBitwiseOr %uint %split_ballot_pack_packed %split_ballot_pack_vote
OpReturnValue %split_ballot_pack_result
OpFunctionEnd
%split_ballot_group = OpFunction %void None %function_split_ballot_group
%split_ballot_group_own = OpFunctionParameter %uint
%split_ballot_group_entry = OpLabel
%split_ballot_group_n = OpFunctionCall %uint %split_publish %split_ballot_group_own
%split_ballot_group_subgroup = OpLoad %uint %gl_SubgroupID
%split_ballot_group_part = OpBitwiseAnd %uint %split_ballot_group_subgroup %split_parts_mask
%split_ballot_group_wave = OpShiftRightLogical %uint %split_ballot_group_subgroup %split_parts_shift
%split_ballot_group_base = OpIMul %uint %split_ballot_group_wave %split_stride
)";
	text += SplitWavePartWords(parts, "split_ballot_group");
	for (uint32_t part = 0; part < parts; ++part)
	{
		text += String8::FromPrintf("OpStore %%split_ballot_part%u %%split_ballot_group_word%u\n", part, part);
	}
	text += R"(OpReturn
OpFunctionEnd
%split_ballot_field = OpFunction %v4uint None %function_split_ballot_field
%split_ballot_field_shift = OpFunctionParameter %uint
%split_ballot_field_entry = OpLabel
)";
	for (uint32_t part = 0; part < parts; ++part)
	{
		text += String8::FromPrintf("%%split_ballot_field_load%u = OpLoad %%uint %%split_ballot_part%u\n"
		                            "%%split_ballot_field_down%u = OpShiftRightLogical %%uint %%split_ballot_field_load%u %%split_ballot_field_shift\n"
		                            "%%split_ballot_field_word%u = OpBitwiseAnd %%uint %%split_ballot_field_down%u %%split_lane_bits\n",
		                            part, part, part, part, part, part);
	}
	text += SplitWaveMaskWords(parts, lanes, "split_ballot_field");
	text += "%split_ballot_field_result = OpCompositeConstruct %v4uint %split_ballot_field_lo %split_ballot_field_hi %split_c0 %split_c0\n"
	        "OpReturnValue %split_ballot_field_result\nOpFunctionEnd\n";
	return text;
}

// Reads one guest lane per invocation through a dword per lane in the exchange
// area; the rendezvous publishes the stores, and the bank of this exchange's
// parity stays intact until every part has finished reading it.
static String8 SplitWaveLaneReadFunction()
{
	return R"(
%split_lane_read = OpFunction %uint None %function_u_u
%split_lane_read_value = OpFunctionParameter %uint
%split_lane_read_lane = OpFunctionParameter %uint
%split_lane_read_entry = OpLabel
%split_lane_read_subgroup = OpLoad %uint %gl_SubgroupID
%split_lane_read_wave = OpShiftRightLogical %uint %split_lane_read_subgroup %split_parts_shift
%split_lane_read_base = OpIMul %uint %split_lane_read_wave %split_stride
%split_lane_read_previous = OpLoad %uint %split_count
%split_lane_read_n = OpIAdd %uint %split_lane_read_previous %split_c1
%split_lane_read_parity = OpBitwiseAnd %uint %split_lane_read_n %split_c1
%split_lane_read_bank = OpShiftLeftLogical %uint %split_lane_read_parity %split_c6
%split_lane_read_banks = OpIAdd %uint %split_lane_read_base %split_lane_area
%split_lane_read_area = OpIAdd %uint %split_lane_read_banks %split_lane_read_bank
%split_lane_read_guest = OpLoad %uint %gl_SubgroupInvocationID
%split_lane_read_own_index = OpIAdd %uint %split_lane_read_area %split_lane_read_guest
%split_lane_read_own_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_lane_read_own_index
OpStore %split_lane_read_own_ptr %split_lane_read_value
OpControlBarrier %split_scope_subgroup %split_scope_workgroup %split_acq_rel
%split_lane_read_stored = OpFunctionCall %uint %split_sync
%split_lane_read_source = OpBitwiseAnd %uint %split_lane_read_lane %split_c63
%split_lane_read_index = OpIAdd %uint %split_lane_read_area %split_lane_read_source
%split_lane_read_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_lane_read_index
%split_lane_read_result = OpLoad %uint %split_lane_read_ptr
OpReturnValue %split_lane_read_result
OpFunctionEnd
)";
}

// Reads up to four values at one guest lane in one exchange (Spirv::SplitReadGroup): host lane k of each part
// stores that part's word k into its lanes of the bank, so the source part's first four dwords hold the words.
static String8 SplitWaveReadGroupFunction()
{
	return R"(
%split_read_group = OpFunction %v4uint None %function_split_read_group
%split_read_group_value0 = OpFunctionParameter %uint
%split_read_group_value1 = OpFunctionParameter %uint
%split_read_group_value2 = OpFunctionParameter %uint
%split_read_group_value3 = OpFunctionParameter %uint
%split_read_group_lane = OpFunctionParameter %uint
%split_read_group_entry = OpLabel
%split_read_group_host_lane = OpBitwiseAnd %uint %split_read_group_lane %split_lanes_mask
%split_read_group_own0 = OpGroupNonUniformBroadcast %uint %split_scope_subgroup %split_read_group_value0 %split_read_group_host_lane
%split_read_group_own1 = OpGroupNonUniformBroadcast %uint %split_scope_subgroup %split_read_group_value1 %split_read_group_host_lane
%split_read_group_own2 = OpGroupNonUniformBroadcast %uint %split_scope_subgroup %split_read_group_value2 %split_read_group_host_lane
%split_read_group_own3 = OpGroupNonUniformBroadcast %uint %split_scope_subgroup %split_read_group_value3 %split_read_group_host_lane
%split_read_group_host = OpLoad %uint %split_host_lane
%split_read_group_is1 = OpIEqual %bool %split_read_group_host %split_c1
%split_read_group_is2 = OpIEqual %bool %split_read_group_host %split_c2
%split_read_group_is3 = OpIEqual %bool %split_read_group_host %split_c3
%split_read_group_word1 = OpSelect %uint %split_read_group_is1 %split_read_group_own1 %split_read_group_own0
%split_read_group_word2 = OpSelect %uint %split_read_group_is2 %split_read_group_own2 %split_read_group_word1
%split_read_group_word = OpSelect %uint %split_read_group_is3 %split_read_group_own3 %split_read_group_word2
%split_read_group_subgroup = OpLoad %uint %gl_SubgroupID
%split_read_group_wave = OpShiftRightLogical %uint %split_read_group_subgroup %split_parts_shift
%split_read_group_base = OpIMul %uint %split_read_group_wave %split_stride
%split_read_group_previous = OpLoad %uint %split_count
%split_read_group_n = OpIAdd %uint %split_read_group_previous %split_c1
%split_read_group_parity = OpBitwiseAnd %uint %split_read_group_n %split_c1
%split_read_group_bank = OpShiftLeftLogical %uint %split_read_group_parity %split_c6
%split_read_group_banks = OpIAdd %uint %split_read_group_base %split_lane_area
%split_read_group_area = OpIAdd %uint %split_read_group_banks %split_read_group_bank
%split_read_group_guest = OpLoad %uint %gl_SubgroupInvocationID
%split_read_group_own_index = OpIAdd %uint %split_read_group_area %split_read_group_guest
%split_read_group_own_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_read_group_own_index
OpStore %split_read_group_own_ptr %split_read_group_word
OpControlBarrier %split_scope_subgroup %split_scope_workgroup %split_acq_rel
%split_read_group_stored = OpFunctionCall %uint %split_sync
%split_read_group_source_lanes = OpShiftRightLogical %uint %split_read_group_lane %split_lanes_shift
%split_read_group_source = OpBitwiseAnd %uint %split_read_group_source_lanes %split_parts_mask
%split_read_group_source_first = OpShiftLeftLogical %uint %split_read_group_source %split_lanes_shift
%split_read_group_source_base = OpIAdd %uint %split_read_group_area %split_read_group_source_first
%split_read_group_ptr0 = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_read_group_source_base
%split_read_group_index1 = OpIAdd %uint %split_read_group_source_base %split_c1
%split_read_group_ptr1 = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_read_group_index1
%split_read_group_index2 = OpIAdd %uint %split_read_group_source_base %split_c2
%split_read_group_ptr2 = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_read_group_index2
%split_read_group_index3 = OpIAdd %uint %split_read_group_source_base %split_c3
%split_read_group_ptr3 = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_read_group_index3
%split_read_group_result0 = OpLoad %uint %split_read_group_ptr0
%split_read_group_result1 = OpLoad %uint %split_read_group_ptr1
%split_read_group_result2 = OpLoad %uint %split_read_group_ptr2
%split_read_group_result3 = OpLoad %uint %split_read_group_ptr3
%split_read_group_result = OpCompositeConstruct %v4uint %split_read_group_result0 %split_read_group_result1 %split_read_group_result2 %split_read_group_result3
OpReturnValue %split_read_group_result
OpFunctionEnd
)";
}

String8 Spirv::SplitWaveFunctions() const
{
	if (!UsesSplitComputeWave())
	{
		return {};
	}
	const uint32_t lanes = SplitWavePartLanes();
	const uint32_t parts = m_cs_input_info->wave_layout.guest_wave_size / lanes;
	// %function_u_u is (uint, uint) -> uint and %function_gen5_buffer_format_bytes
	// is (uint) -> uint; SPIR-V forbids declaring a function type twice.
	String8 text = SplitWavePublishFunction(parts);
	text += SplitWaveBallotFunction(parts, lanes);
	text += SplitWaveBallotGroupFunctions(parts, lanes);
	// This part's bits of the wave mask, the other parts' bits zero: enough
	// for a mask whose every reader takes only its own lane's bit.
	text += R"(
%split_part_ballot = OpFunction %v4uint None %function_split_ballot
%split_part_ballot_predicate = OpFunctionParameter %bool
%split_part_ballot_entry = OpLabel
%split_part_ballot_host = OpGroupNonUniformBallot %v4uint %split_scope_subgroup %split_part_ballot_predicate
%split_part_ballot_own = OpCompositeExtract %uint %split_part_ballot_host 0
%split_part_ballot_subgroup = OpLoad %uint %gl_SubgroupID
%split_part_ballot_part = OpBitwiseAnd %uint %split_part_ballot_subgroup %split_parts_mask
%split_part_ballot_first = OpShiftLeftLogical %uint %split_part_ballot_part %split_lanes_shift
%split_part_ballot_shift = OpBitwiseAnd %uint %split_part_ballot_first %split_c31
%split_part_ballot_upper = OpINotEqual %bool %split_part_ballot_first %split_part_ballot_shift
%split_part_ballot_bits = OpShiftLeftLogical %uint %split_part_ballot_own %split_part_ballot_shift
%split_part_ballot_lo = OpSelect %uint %split_part_ballot_upper %split_c0 %split_part_ballot_bits
%split_part_ballot_hi = OpSelect %uint %split_part_ballot_upper %split_part_ballot_bits %split_c0
%split_part_ballot_result = OpCompositeConstruct %v4uint %split_part_ballot_lo %split_part_ballot_hi %split_c0 %split_c0
OpReturnValue %split_part_ballot_result
OpFunctionEnd
)";
	text += R"(
%split_read = OpFunction %uint None %function_u_u
%split_read_value = OpFunctionParameter %uint
%split_read_lane = OpFunctionParameter %uint
%split_read_entry = OpLabel
%split_read_host_lane = OpBitwiseAnd %uint %split_read_lane %split_lanes_mask
%split_read_own = OpGroupNonUniformBroadcast %uint %split_scope_subgroup %split_read_value %split_read_host_lane
%split_read_n = OpFunctionCall %uint %split_publish %split_read_own
%split_read_subgroup = OpLoad %uint %gl_SubgroupID
%split_read_part = OpBitwiseAnd %uint %split_read_subgroup %split_parts_mask
%split_read_wave = OpShiftRightLogical %uint %split_read_subgroup %split_parts_shift
%split_read_base = OpIMul %uint %split_read_wave %split_stride
%split_read_source_lanes = OpShiftRightLogical %uint %split_read_lane %split_lanes_shift
%split_read_source = OpBitwiseAnd %uint %split_read_source_lanes %split_parts_mask
%split_read_source_is_own = OpIEqual %bool %split_read_source %split_read_part
%split_read_slots = OpIAdd %uint %split_read_base %split_parts
%split_read_parity = OpBitwiseAnd %uint %split_read_n %split_c1
%split_read_source2 = OpShiftLeftLogical %uint %split_read_source %split_c1
%split_read_source_slot = OpIAdd %uint %split_read_source2 %split_read_parity
%split_read_index = OpIAdd %uint %split_read_slots %split_read_source_slot
%split_read_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_read_index
%split_read_other = OpLoad %uint %split_read_ptr
%split_read_result = OpSelect %uint %split_read_source_is_own %split_read_own %split_read_other
OpReturnValue %split_read_result
OpFunctionEnd

%split_sync = OpFunction %uint None %function_split_u
%split_sync_entry = OpLabel
%split_sync_result = OpFunctionCall %uint %split_publish %split_c0
OpReturnValue %split_sync_result
OpFunctionEnd

%split_leader = OpFunction %bool None %function_split_b
%split_leader_entry = OpLabel
%split_leader_elected = OpGroupNonUniformElect %bool %split_scope_subgroup
%split_leader_subgroup = OpLoad %uint %gl_SubgroupID
%split_leader_part = OpBitwiseAnd %uint %split_leader_subgroup %split_parts_mask
%split_leader_first = OpIEqual %bool %split_leader_part %split_c0
%split_leader_result = OpLogicalAnd %bool %split_leader_elected %split_leader_first
OpReturnValue %split_leader_result
OpFunctionEnd

%split_first = OpFunction %uint None %function_gen5_buffer_format_bytes
%split_first_value = OpFunctionParameter %uint
%split_first_entry = OpLabel
%split_first_own = OpGroupNonUniformBroadcastFirst %uint %split_scope_subgroup %split_first_value
%split_first_n = OpFunctionCall %uint %split_publish %split_first_own
%split_first_subgroup = OpLoad %uint %gl_SubgroupID
%split_first_part = OpBitwiseAnd %uint %split_first_subgroup %split_parts_mask
%split_first_is_first = OpIEqual %bool %split_first_part %split_c0
%split_first_wave = OpShiftRightLogical %uint %split_first_subgroup %split_parts_shift
%split_first_base = OpIMul %uint %split_first_wave %split_stride
%split_first_parity = OpBitwiseAnd %uint %split_first_n %split_c1
%split_first_index0 = OpIAdd %uint %split_first_base %split_parts
%split_first_index = OpIAdd %uint %split_first_index0 %split_first_parity
%split_first_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_first_index
%split_first_other = OpLoad %uint %split_first_ptr
%split_first_result = OpSelect %uint %split_first_is_first %split_first_own %split_first_other
OpReturnValue %split_first_result
OpFunctionEnd
)";
	text += SplitWaveLaneReadFunction();
	text += SplitWaveReadGroupFunction();
	return text;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
