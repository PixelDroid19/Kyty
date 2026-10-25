#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// A split guest wave64 runs as host subgroups 2w (lanes 0-31) and 2w+1 (lanes
// 32-63). The per-lane emitters see the guest lane in %gl_SubgroupInvocationID
// and call the helpers below for every wave-wide value; each helper call is a
// rendezvous of the two halves, which execute the same sequence of calls
// because every branch they take depends only on wave-uniform state.
//
// Per guest wave the exchange area holds, in kShaderSplitWaveExchangeDwords:
// [0..1] the number of exchanges each half has published, [2..5] the words
// they published (half * 2 + parity). Two parities suffice: a half reuses a
// slot only after its partner reached the following exchange.

bool Spirv::UsesSplitComputeWave() const
{
	return m_code.GetType() == ShaderType::Compute && m_cs_input_info != nullptr && m_cs_input_info->native_wave_sensitive &&
	       m_cs_input_info->wave_layout.strategy == ShaderComputeWaveStrategy::Split64On32;
}

String8 Spirv::HostLaneVariable() const
{
	return UsesSplitComputeWave() ? "split_host_lane" : "gl_SubgroupInvocationID";
}

String8 Spirv::SplitWaveTypes() const
{
	if (!UsesSplitComputeWave())
	{
		return {};
	}
	const uint32_t dwords = m_cs_input_info->wave_layout.waves * kShaderSplitWaveExchangeDwords;
	return String8::FromPrintf(R"(
%%split_c0 = OpConstant %%uint 0
%%split_c1 = OpConstant %%uint 1
%%split_c2 = OpConstant %%uint 2
%%split_c5 = OpConstant %%uint 5
%%split_c31 = OpConstant %%uint 31
%%split_c63 = OpConstant %%uint 63
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
)",
	                           kShaderSplitWaveExchangeDwords, dwords);
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
)";
}

// After the native prolog: publish the guest lane, then clear this half's
// sequence before any half of the workgroup may wait on it.
String8 Spirv::SplitWaveProlog() const
{
	return R"(
%split_half = OpBitwiseAnd %uint %native_wave_subgroup_id %split_c1
%split_lane_base = OpShiftLeftLogical %uint %split_half %split_c5
%split_guest_lane = OpBitwiseOr %uint %split_lane_base %native_wave_lane_id
OpStore %gl_SubgroupInvocationID %split_guest_lane
%split_wave = OpShiftRightLogical %uint %native_wave_subgroup_id %split_c1
%split_wave_base = OpIMul %uint %split_wave %split_stride
%split_own_sequence = OpIAdd %uint %split_wave_base %split_half
%split_own_sequence_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_own_sequence
OpAtomicStore %split_own_sequence_ptr %split_scope_workgroup %split_c0 %split_c0
OpControlBarrier %split_scope_workgroup %split_scope_workgroup %split_acq_rel
)";
}

// LDS accesses of one guest wave are ordered on hardware; the two halves
// order them with a rendezvous after each DS instruction.
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
	// lane and stay inside a 16-lane row, never leave one half.
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
			EXIT("split-wave compute collective without a two-half lowering: %s\n", body.Mid(at, 40).c_str());
		}
	}
}

String8 Spirv::SplitWaveFunctions() const
{
	if (!UsesSplitComputeWave())
	{
		return {};
	}
	// %function_u_u is (uint, uint) -> uint and %function_gen5_buffer_format_bytes
	// is (uint) -> uint; SPIR-V forbids declaring a function type twice.
	return R"(
%split_wait = OpFunction %uint None %function_u_u
%split_wait_index = OpFunctionParameter %uint
%split_wait_n = OpFunctionParameter %uint
%split_wait_entry = OpLabel
%split_wait_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_wait_index
OpBranch %split_wait_header
%split_wait_header = OpLabel
%split_wait_spins = OpPhi %uint %split_c0 %split_wait_entry %split_wait_next %split_wait_continue
OpLoopMerge %split_wait_merge %split_wait_continue None
OpBranch %split_wait_body
%split_wait_body = OpLabel
%split_wait_seen = OpAtomicLoad %uint %split_wait_ptr %split_scope_workgroup %split_acquire
%split_wait_ready = OpUGreaterThanEqual %bool %split_wait_seen %split_wait_n
%split_wait_next = OpIAdd %uint %split_wait_spins %split_c1
%split_wait_late = OpUGreaterThanEqual %bool %split_wait_next %split_spin_limit
%split_wait_done = OpLogicalOr %bool %split_wait_ready %split_wait_late
OpBranchConditional %split_wait_done %split_wait_merge %split_wait_continue
%split_wait_continue = OpLabel
OpBranch %split_wait_header
%split_wait_merge = OpLabel
OpReturnValue %split_wait_seen
OpFunctionEnd

%split_word = OpFunction %uint None %function_gen5_buffer_format_bytes
%split_word_value = OpFunctionParameter %uint
%split_word_entry = OpLabel
%split_word_subgroup = OpLoad %uint %gl_SubgroupID
%split_word_half = OpBitwiseAnd %uint %split_word_subgroup %split_c1
%split_word_other = OpBitwiseXor %uint %split_word_half %split_c1
%split_word_wave = OpShiftRightLogical %uint %split_word_subgroup %split_c1
%split_word_base = OpIMul %uint %split_word_wave %split_stride
%split_word_previous = OpLoad %uint %split_count
%split_word_n = OpIAdd %uint %split_word_previous %split_c1
OpStore %split_count %split_word_n
%split_word_parity = OpBitwiseAnd %uint %split_word_n %split_c1
%split_word_slots = OpIAdd %uint %split_word_base %split_c2
%split_word_own2 = OpShiftLeftLogical %uint %split_word_half %split_c1
%split_word_other2 = OpShiftLeftLogical %uint %split_word_other %split_c1
%split_word_own_slot = OpIAdd %uint %split_word_own2 %split_word_parity
%split_word_other_slot = OpIAdd %uint %split_word_other2 %split_word_parity
%split_word_own_index = OpIAdd %uint %split_word_slots %split_word_own_slot
%split_word_other_index = OpIAdd %uint %split_word_slots %split_word_other_slot
%split_word_own_sequence = OpIAdd %uint %split_word_base %split_word_half
%split_word_other_sequence = OpIAdd %uint %split_word_base %split_word_other
%split_word_own_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_word_own_index
%split_word_sequence_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_word_own_sequence
%split_word_elected = OpGroupNonUniformElect %bool %split_scope_subgroup
OpSelectionMerge %split_word_published None
OpBranchConditional %split_word_elected %split_word_publish %split_word_published
%split_word_publish = OpLabel
OpStore %split_word_own_ptr %split_word_value
OpAtomicStore %split_word_sequence_ptr %split_scope_workgroup %split_release %split_word_n
OpBranch %split_word_published
%split_word_published = OpLabel
%split_word_seen = OpFunctionCall %uint %split_wait %split_word_other_sequence %split_word_n
%split_word_other_ptr = OpAccessChain %_ptr_Workgroup_split_uint %split_exchange %split_word_other_index
%split_word_result = OpLoad %uint %split_word_other_ptr
OpReturnValue %split_word_result
OpFunctionEnd

%split_ballot = OpFunction %v4uint None %function_split_ballot
%split_ballot_predicate = OpFunctionParameter %bool
%split_ballot_entry = OpLabel
%split_ballot_host = OpGroupNonUniformBallot %v4uint %split_scope_subgroup %split_ballot_predicate
%split_ballot_own = OpCompositeExtract %uint %split_ballot_host 0
%split_ballot_other = OpFunctionCall %uint %split_word %split_ballot_own
%split_ballot_subgroup = OpLoad %uint %gl_SubgroupID
%split_ballot_half = OpBitwiseAnd %uint %split_ballot_subgroup %split_c1
%split_ballot_low = OpIEqual %bool %split_ballot_half %split_c0
%split_ballot_lo = OpSelect %uint %split_ballot_low %split_ballot_own %split_ballot_other
%split_ballot_hi = OpSelect %uint %split_ballot_low %split_ballot_other %split_ballot_own
%split_ballot_result = OpCompositeConstruct %v4uint %split_ballot_lo %split_ballot_hi %split_c0 %split_c0
OpReturnValue %split_ballot_result
OpFunctionEnd

%split_read = OpFunction %uint None %function_u_u
%split_read_value = OpFunctionParameter %uint
%split_read_lane = OpFunctionParameter %uint
%split_read_entry = OpLabel
%split_read_host_lane = OpBitwiseAnd %uint %split_read_lane %split_c31
%split_read_host = OpGroupNonUniformBroadcast %uint %split_scope_subgroup %split_read_value %split_read_host_lane
%split_read_other = OpFunctionCall %uint %split_word %split_read_host
%split_read_subgroup = OpLoad %uint %gl_SubgroupID
%split_read_half = OpBitwiseAnd %uint %split_read_subgroup %split_c1
%split_read_source = OpShiftRightLogical %uint %split_read_lane %split_c5
%split_read_source_half = OpBitwiseAnd %uint %split_read_source %split_c1
%split_read_own = OpIEqual %bool %split_read_source_half %split_read_half
%split_read_result = OpSelect %uint %split_read_own %split_read_host %split_read_other
OpReturnValue %split_read_result
OpFunctionEnd

%split_sync = OpFunction %uint None %function_split_u
%split_sync_entry = OpLabel
%split_sync_result = OpFunctionCall %uint %split_word %split_c0
OpReturnValue %split_sync_result
OpFunctionEnd

%split_leader = OpFunction %bool None %function_split_b
%split_leader_entry = OpLabel
%split_leader_elected = OpGroupNonUniformElect %bool %split_scope_subgroup
%split_leader_subgroup = OpLoad %uint %gl_SubgroupID
%split_leader_half = OpBitwiseAnd %uint %split_leader_subgroup %split_c1
%split_leader_low = OpIEqual %bool %split_leader_half %split_c0
%split_leader_result = OpLogicalAnd %bool %split_leader_elected %split_leader_low
OpReturnValue %split_leader_result
OpFunctionEnd

%split_first = OpFunction %uint None %function_gen5_buffer_format_bytes
%split_first_value = OpFunctionParameter %uint
%split_first_entry = OpLabel
%split_first_own = OpGroupNonUniformBroadcastFirst %uint %split_scope_subgroup %split_first_value
%split_first_other = OpFunctionCall %uint %split_word %split_first_own
%split_first_subgroup = OpLoad %uint %gl_SubgroupID
%split_first_half = OpBitwiseAnd %uint %split_first_subgroup %split_c1
%split_first_low = OpIEqual %bool %split_first_half %split_c0
%split_first_result = OpSelect %uint %split_first_low %split_first_own %split_first_other
OpReturnValue %split_first_result
OpFunctionEnd
)";
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
