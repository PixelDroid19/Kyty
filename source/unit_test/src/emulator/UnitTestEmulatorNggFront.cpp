#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GraphicsGeState.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"
#include "Emulator/Graphics/ShaderNggFront.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "NggPassthroughFixture.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

UT_BEGIN(EmulatorNggFront);

using namespace Libs::Graphics;

namespace {

using namespace NggFixture;

// GE_STAGES words as decoded by GraphicsDecodeGeStages.
constexpr uint32_t kNggPassthroughStages = 0x02002000u;
constexpr uint32_t kMergedEsGsStages     = 0x00002030u;
constexpr uint32_t kLegacyVsStages       = 0x00000000u;
constexpr uint32_t kUnknownBitStages     = kNggPassthroughStages | 0x80000000u;

GraphicsGeRawRegister Stages(uint32_t value)
{
	GraphicsGeRawRegister stages {};
	stages.SetRaw(value);
	return stages;
}

// Admission of the fixture as it stands before any proof: this is the verdict the
// proof replaces, so every test that expects LaneLocal first pins this precondition.
ShaderNativeWaveInfo Generic(const ShaderCode& code, uint32_t guest_wave_size)
{
	const auto wave = ShaderAnalyzeNativeWave(code, guest_wave_size);
	Check(ShaderUsesNativeWaveState(code), "the front writes EXEC, so the generic test demands a native wave");
	Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::ExactSubgroup, "generic verdict is ExactSubgroup");
	return wave;
}

// Host range of the Intel Arc device this work targets: subgroups of 8..32 lanes.
ShaderComputeWaveVulkanState Max32Host()
{
	ShaderComputeWaveVulkanState state {};
	state.extension_advertised = state.extension_enabled = true;
	state.extension_revision                             = 2;
	state.size_control_feature_supported = state.size_control_feature_enabled = true;
	state.full_subgroups_feature_supported = state.full_subgroups_feature_enabled = true;
	state.vertex_required_size_supported = state.fragment_required_size_supported = state.compute_required_size_supported = true;
	state.min_subgroup_size                                                        = 8;
	state.max_subgroup_size                                                        = 32;
	return state;
}

bool HostCanRun(const ShaderNativeWaveInfo& wave)
{
	const bool lane_local = wave.proof == ShaderNativeWaveProof::LaneLocal;
	return ShaderSelectNativeSubgroup(Max32Host(), VK_SHADER_STAGE_VERTEX_BIT, 32u, wave.guest_wave_size, lane_local, false, true).supported;
}


// The wave-count prologue of the shared fixture, through the write that leaves EXEC
// equal to the vertex-count mask, followed by a caller-chosen body.
Words PrologueThenBody(const Words& body)
{
	const auto fixture = Fixture();
	const auto code    = Parse(fixture);
	uint32_t   seen    = 0;
	uint32_t   end     = 0;
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::SLshrB64 && ++seen == 2u) { end = inst.pc / 4u + 1u; }
	}
	Check(seen == 2u, "fixture has the primitive and the vertex EXEC writes");
	Words words(fixture.begin(), fixture.begin() + end);
	Append(words, body);
	return words;
}

constexpr uint32_t kExecLoReg = 126u;

// Scalar constant, uniform compare, uniform branch over a fetch-like block, lane-bit
// select, exports. Registers and values are synthetic.
// `sdwa_src1_select` != 0 encodes the compare as SDWA with that select of the second source (6 =
// DWORD, the identity); the plain VOPC form is used when it is 0.
Words FetchBody(bool uniform_compare = true, bool lane_data_to_vector = false, uint32_t sdwa_src1_select = 0u,
                bool branch_bypasses_done_pos0 = false)
{
	Words body;
	body.push_back(0xb0000000u | (24u << 16u) | 72u);          // s_movk_i32 s24, 72
	body.insert(body.end(), {0xf4200508u, 0xfa000004u});       // s_buffer_load_dword s20, s[16:19], 4
	Sopp(body, 12, 0xc07f);                                    // s_waitcnt lgkmcnt(0)
	Vop1(body, 5, 12, 20);                                     // v_cvt_f32_i32 v12, s20
	if (sdwa_src1_select == 0u)
	{
		Append(body, Vopc(0xc5, 20, uniform_compare ? 12u : 5u));  // v_cmp_ne_u32 vcc, s20, v12 (or v5: per-lane)
	} else
	{
		// v_cmp_ne_u32_sdwa vcc, s20, v12: SGPR source 0, VCC destination, DWORD source 0 select.
		body.push_back(0x7c000000u | (0xc5u << 17u) | (12u << 9u) | 249u);
		body.push_back(20u | (6u << 16u) | (1u << 23u) | (sdwa_src1_select << 24u));
	}
	Vop1(body, 1, 13, Inline(0));                              // v_mov_b32 v13, 0
	Vop1(body, 1, 14, Inline(0));                              // v_mov_b32 v14, 0
	Sopp(body, 7, branch_bypasses_done_pos0 ? 6u : 4u);        // optionally branch over the two-dword POS0 export too
	Vop1(body, 1, 13, lane_data_to_vector ? 3u : 256u + 5u);   // v_mov_b32 v13, v5 (or s3: the wave info)
	Append(body, Sopc(6, 20, Inline(0)));                      // s_cmp_eq_u32 s20, 0
	Sop2(body, 11, 106, kExecLoReg, Inline(0));                // s_cselect_b64 vcc, exec, 0
	Vop2(body, 1, 14, 256u + 5u, 8u);                          // v_cndmask_b32 v14, v5, v8, vcc
	Exp(body, 12, 15, true, 13, 13, 14, 14);                   // exp pos0 v13, v13, v14, v14 done
	Sopp(body, 1);                                             // s_endpgm
	return body;
}

Words FetchBodyWithPositionAfterDone()
{
	Words body = FetchBody();
	body.pop_back(); // replace the terminal S_ENDPGM with a deliberately invalid split-position suffix
	Exp(body, 13, 1, false, 13, 13, 13, 13); // accepted POS1-X control form, after POS0 DONE
	Exp(body, 32, 15, false, 13, 13, 13, 13); // accepted parameter export; every source is defined
	Sopp(body, 1);
	return body;
}

Words FetchBodyWithParameterAfterPositionDone()
{
	Words body = FetchBody();
	body.pop_back();
	Exp(body, 32, 15, false, 13, 13, 13, 13); // raw PARAM0 with four defined vector sources
	Sopp(body, 1);
	return body;
}

Words FetchBodyWithBranchMissingPos0()
{
	Words body = FetchBody(true, false, 0u, true);
	body.pop_back(); // replace the terminal S_ENDPGM with a parameter export at the branch join
	Exp(body, 32, 15, false, 13, 13, 13, 13); // accepted parameter export; every source is defined
	Sopp(body, 1);
	return body;
}

Words PartialExecUniformCompareBody()
{
	Words body;
	body.push_back(0xb0000000u | (24u << 16u) | 72u); // s_movk_i32 s24, 72
	Vop1(body, 5, 12, 24u);                            // v_cvt_f32_i32 v12, s24 (uniform in all lanes)
	Sop1(body, 4, 40, kExecLoReg);                    // save full EXEC
	Append(body, Vopc(0xd5, Inline(1), 5u));           // v_cmpx_ne_u32 1, v5: enter a proper subset
	Append(body, Vopc(0xc5, 24u, 12u));                // uniform operands, but compare only under partial EXEC
	Sop1(body, 4, kExecLoReg, 40);                     // restore full EXEC before the scalar branch
	Sopp(body, 6, 0);                                  // s_cbranch_vccz to the following export
	Exp(body, 12, 15, true, 5, 5, 5, 5);
	Sopp(body, 1);
	return body;
}

Words VopcSdwaF32(uint32_t opcode, bool vcc_destination, uint32_t destination, uint32_t src0, bool src0_is_vgpr,
                  uint32_t src1, bool src1_is_vgpr)
{
	const uint32_t instruction = 0x7c000000u | (opcode << 17u) | (src1 << 9u) | 249u;
	const uint32_t dst_fields  = vcc_destination ? 0u : (destination << 8u) | (1u << 15u);
	const uint32_t s0          = src0_is_vgpr ? 0u : 1u;
	const uint32_t s1          = src1_is_vgpr ? 0u : 1u;
	const uint32_t control     = src0 | dst_fields | (6u << 16u) | (s0 << 23u) | (6u << 24u) | (s1 << 31u);
	return {instruction, control};
}

Words SdwaFloatCompareBody(uint32_t src1_modifiers)
{
	constexpr uint32_t kVgpr = 21u;
	constexpr uint32_t kSgpr = 34u;
	Words body;
	body.push_back(0xb0000000u | (24u << 16u) | 3u); // s_movk_i32 s24, 3
	body.push_back(0xb0000000u | (kSgpr << 16u) | 5u); // s_movk_i32 s34, 5
	Vop1(body, 5, kVgpr, 24u); // v_cvt_f32_i32 v21, s24
	const auto compare = VopcSdwaF32(0x04, true, 0u, kVgpr, true, kSgpr, false); // v_cmp_gt_f32_sdwa vcc, v21, modifier(s34)
	body.insert(body.end(), compare.begin(), compare.end());
	body.back() |= src1_modifiers;
	Vop1(body, 1, 13, Inline(0));
	Sopp(body, 6, 0); // uniform VCC branch to the following export
	Exp(body, 12, 15, true, 13, 13, 13, 13);
	Sopp(body, 1);
	return body;
}

Words CompareMaskAnd64Body(bool partial_vcc, bool varying_sgpr_mask)
{
	Words body;
	body.push_back(0xb0000000u | (24u << 16u) | 3u); // s_movk_i32 s24, 3
	body.push_back(0xb0000000u | (46u << 16u) | 5u); // s_movk_i32 s46, 5
	if (partial_vcc)
	{
		const auto scalar_mask = VopcSdwaF32(0x02, false, 40u, 46u, false, Inline(0), false);
		body.insert(body.end(), scalar_mask.begin(), scalar_mask.end()); // v_cmp_eq_f32 s[40:41], s46, 0
		Sop1(body, 4, 50u, kExecLoReg);                 // save full EXEC
		Append(body, Vopc(0xd5, Inline(1), 5u));        // v_cmpx_ne_u32 1, v5: narrow EXEC
		const auto partial_vcc = VopcSdwaF32(0x06, true, 0u, 24u, false, Inline(0), false);
		body.insert(body.end(), partial_vcc.begin(), partial_vcc.end()); // v_cmp_ge_f32 vcc, s24, 0 under partial EXEC
		Sop1(body, 4, kExecLoReg, 50u);                 // restore full EXEC
	} else
	{
		const auto vcc_mask = VopcSdwaF32(0x06, true, 0u, 24u, false, Inline(0), false);
		body.insert(body.end(), vcc_mask.begin(), vcc_mask.end()); // v_cmp_ge_f32 vcc, s24, 0
		const auto scalar_mask = VopcSdwaF32(0x02, false, 40u, varying_sgpr_mask ? 5u : 46u, varying_sgpr_mask,
		                                    Inline(0), false);
		body.insert(body.end(), scalar_mask.begin(), scalar_mask.end()); // v_cmp_eq_f32 s[40:41], s46-or-v5, 0
	}
	Sop2(body, 0x0f, 106u, 106u, 40u); // s_and_b64 vcc, vcc, s[40:41]
	Vop1(body, 1, 13, Inline(0));
	Sopp(body, 6, 0);
	Exp(body, 12, 15, true, 13, 13, 13, 13);
	Sopp(body, 1);
	return body;
}

Words ExecAndUniformMaskUnderPartialExecBody()
{
	Words body;
	body.push_back(0xb0000000u | (24u << 16u) | 3u); // s_movk_i32 s24, 3
	const auto vcc_mask = VopcSdwaF32(0x06, true, 0u, 24u, false, Inline(0), false);
	body.insert(body.end(), vcc_mask.begin(), vcc_mask.end()); // full-EXEC uniform compare
	Sop1(body, 4, 50u, kExecLoReg);                    // save full EXEC
	Append(body, Vopc(0xd5, Inline(1), 5u));           // v_cmpx_ne_u32 1, v5: narrow EXEC
	Sop2(body, 0x0f, 106u, 106u, kExecLoReg);          // s_and_b64 vcc, vcc, exec under partial EXEC
	Sop1(body, 4, kExecLoReg, 50u);                    // restore full EXEC
	Vop1(body, 1, 13, Inline(0));
	Sopp(body, 6, 0);
	Exp(body, 12, 15, true, 13, 13, 13, 13);
	Sopp(body, 1);
	return body;
}

Words VmacF32OldDestinationBody(uint32_t destination)
{
	Words body;
	body.push_back(0xb0000000u | (24u << 16u) | 3u); // s_movk_i32 s24, 3
	body.push_back(0xb0000000u | (26u << 16u) | 5u); // s_movk_i32 s26, 5
	Vop1(body, 1, 20, Inline(2));                    // v_mov_b32 v20, 2
	Vop2(body, 0x2b, destination, 24u, 20u);         // v_fmac_f32 vdst, s24, v20; reads old vdst
	Append(body, Vopc(0x04, 26u, destination));      // v_cmp_gt_f32 vcc, s26, vdst
	Vop1(body, 1, 13, Inline(0));
	Sopp(body, 6, 0);
	Exp(body, 12, 15, true, 13, 13, 13, 13);
	Sopp(body, 1);
	return body;
}

uint32_t InstructionIndex(const ShaderCode& code, ShaderInstructionType type, unsigned occurrence = 0u)
{
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		if (code.GetInstructions().At(index).type != type) { continue; }
		if (occurrence == 0u) { return index; }
		--occurrence;
	}
	Check(false, "requested instruction exists");
	return 0;
}

void CheckAnd64VccTuple(const ShaderCode& code, ShaderOperandType rhs_type, int rhs_register)
{
	const auto& and_inst = code.GetInstructions().At(InstructionIndex(code, ShaderInstructionType::SAndB64));
	Check(and_inst.dst.type == ShaderOperandType::VccLo && and_inst.dst.size == 2 && and_inst.src_num == 2 &&
	          and_inst.src[0].type == ShaderOperandType::VccLo && and_inst.src[0].size == 2 && and_inst.src[1].type == rhs_type &&
	          and_inst.src[1].size == 2 && and_inst.src[1].register_id == rhs_register,
	      "the fixture decodes to canonical two-word AND into VCC");
}

uint32_t WordIndexOf(const ShaderCode& code, ShaderInstructionType type)
{
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type == type) { return inst.pc / 4u; }
	}
	Check(false, "requested instruction exists");
	return 0;
}

Words FetchBodyWithNonFinalPos0()
{
	Words body = FetchBody();
	const uint32_t export_word = WordIndexOf(Parse(body), ShaderInstructionType::Exp);
	body[export_word] &= ~0x800u; // clear DONE in the fixture header
	return body;
}

Words FetchBodyWithMissingPositionDone()
{
	Words body = FetchBodyWithNonFinalPos0();
	body.pop_back();
	Exp(body, 32, 15, false, 13, 13, 13, 13); // PARAM may follow position exports but cannot close them
	Sopp(body, 1);
	return body;
}

Words FetchBodyWithDuplicatePos0()
{
	Words body = FetchBodyWithNonFinalPos0();
	body.pop_back();
	Exp(body, 12, 15, true, 13, 13, 13, 13); // second POS0 target is invalid even though this one has DONE
	Exp(body, 32, 15, false, 13, 13, 13, 13);
	Sopp(body, 1);
	return body;
}

Words FetchBodyWithFinalPos1()
{
	Words body = FetchBodyWithNonFinalPos0();
	body.pop_back();
	Exp(body, 13, 1, true, 13, 13, 13, 13); // POS1-X is the final position export
	Exp(body, 32, 15, false, 13, 13, 13, 13);
	Sopp(body, 1);
	return body;
}

Words UniformBranchPositionAlternativesBody()
{
	Words body = FetchBody();
	const uint32_t branch_word = WordIndexOf(Parse(body), ShaderInstructionType::SCbranchVccnz);
	body.resize(branch_word + 1u);
	body.back() = (body.back() & 0xffff0000u) | 3u; // target after POS0-DONE and the fallthrough S_BRANCH
	Exp(body, 12, 15, true, 13, 13, 13, 13); // first arm: POS0 is final
	Sopp(body, 2, 4);                         // S_BRANCH skips the alternative arm to the common PARAM
	Exp(body, 12, 15, false, 13, 13, 13, 13); // second arm: POS0 is non-final
	Exp(body, 13, 1, true, 13, 13, 13, 13);   // second arm: POS1-X closes the position sequence
	Exp(body, 32, 15, false, 13, 13, 13, 13);
	Sopp(body, 1);
	return body;
}

Words FetchBodyWithPartialExecPosition()
{
	Words body = FetchBody();
	const uint32_t export_word = WordIndexOf(Parse(body), ShaderInstructionType::Exp);
	Words        narrow_exec;
	Sop1(narrow_exec, 4, 40u, kExecLoReg);
	Append(narrow_exec, Vopc(0xd5, Inline(1), 5u)); // narrow EXEC to a proper lane subset
	body.insert(body.begin() + export_word, narrow_exec.begin(), narrow_exec.end());
	Words restore_exec;
	Sop1(restore_exec, 4, kExecLoReg, 40u);
	const uint32_t restore_word = export_word + static_cast<uint32_t>(narrow_exec.size()) + 2u;
	body.insert(body.begin() + restore_word, restore_exec.begin(), restore_exec.end());
	return body;
}

Words FetchBodyWithCopiedExecMask(bool partial_mask, bool overwrite_high_word)
{
	Words body = FetchBody();
	const uint32_t export_word = WordIndexOf(Parse(body), ShaderInstructionType::Exp);
	Words restore;
	Sop1(restore, 4, 106u, kExecLoReg); // save the vertex mask in VCC
	if (partial_mask) { Append(restore, Vopc(0xd5, Inline(1), 5u)); }
	Sop1(restore, 4, 40u, partial_mask ? kExecLoReg : 106u); // copy the saved mask through an SGPR pair
	Sop1(restore, 4, 40u, 40u); // an aliased copy must snapshot its source before the write
	Sop1(restore, 4, 42u, 40u);
	if (overwrite_high_word) { Sop1(restore, 3, 43u, Inline(0)); }
	Append(restore, Vopc(0xd5, Inline(1), 5u)); // narrow EXEC while retaining the saved pair
	Words masked;
	Vop1(masked, 1, 13u, 256u + 5u);
	Sopp(restore, 8, static_cast<uint32_t>(masked.size())); // an empty subgroup may skip the vector write
	Append(restore, masked);
	Sop1(restore, 4, kExecLoReg, 42u); // restore the copied mask before position export
	body.insert(body.begin() + export_word, restore.begin(), restore.end());
	return body;
}

} // namespace

TEST(EmulatorNggFront, ProvesEveryLaunchOfBothArchitecturalWidths)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(Fixture());
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(front.lane_local && front.rejection == ShaderNggPassthroughRejection::None && front.reason.IsEmpty(),
			      "universal proof for the wave width");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, AdmitsThePassthroughFrontOnAHostWithoutTheGuestWidth)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(Fixture());
		for (uint32_t width: {32u, 64u})
		{
			auto wave = Generic(code, width);
			if (width == 64u) { Check(!HostCanRun(wave), "a 32-lane host refuses the generic Wave64 verdict"); }
			const auto stages = Stages(kNggPassthroughStages);
			const ShaderNggFrontVerdict verdict;
			Check(!ShaderApplyNggFrontProof(&wave, code, &stages, verdict), "no native wave is required");
			Check(wave.proof == ShaderNativeWaveProof::LaneLocal && wave.refusal_reason == nullptr, "verdict is LaneLocal");
			Check(wave.guest_wave_size == width, "the guest width is kept for diagnostics");
			Check(HostCanRun(wave), "the 32-lane host runs the proven front");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, OtherStageShapesKeepTheGenericVerdict)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(Fixture());
		const auto merged  = Stages(kMergedEsGsStages);
		const auto legacy  = Stages(kLegacyVsStages);
		const auto unknown = Stages(kUnknownBitStages);
		GraphicsGeRawRegister partial {};
		partial.SetDecoded();
		const GraphicsGeRawRegister* cases[] = {&merged, &legacy, &unknown, &partial, nullptr};
		for (const GraphicsGeRawRegister* stages: cases)
		{
			auto wave = Generic(code, 64u);
			const ShaderNggFrontVerdict verdict;
			Check(ShaderApplyNggFrontProof(&wave, code, stages, verdict), "a native wave is still required");
			Check(wave.proof == ShaderNativeWaveProof::ExactSubgroup, "verdict is unchanged");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, ARefusedWidthStaysExactAndPreviousRefusalsAreNotOverwritten)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code   = Parse(Fixture());
		const auto stages = Stages(kNggPassthroughStages);
		ShaderNggFrontVerdict verdict;

		auto refused = Generic(code, 64u);
		refused.refusal_reason = "guest wave width is not established by stage control";
		Check(ShaderApplyNggFrontProof(&refused, code, &stages, verdict), "a refused classification is never upgraded");
		Check(refused.proof == ShaderNativeWaveProof::ExactSubgroup, "proof field untouched");

		auto absent = Generic(code, 64u);
		Check(ShaderApplyNggFrontProof(nullptr, code, &stages, verdict), "no wave information, no upgrade");
		Check(!ShaderProveNggFrontLaneLocal(code, 0u).lane_local && !ShaderProveNggFrontLaneLocal(code, 16u).lane_local &&
		          !ShaderProveNggFrontLaneLocal(code, 128u).lane_local,
		      "only explicit 32 and 64 lane waves are provable");
		Check(!verdict.LaneLocal(code, 16u), "no verdict slot for another width");
		Check(verdict.LaneLocal(code, 32u) && verdict.LaneLocal(code, 64u) && verdict.LaneLocal(code, 64u), "stable verdicts per width");
		Check(absent.proof == ShaderNativeWaveProof::ExactSubgroup, "independent wave info unchanged");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, ACountSpecificMaskIsNotAProofForEveryLaunch)
{
	ASSERT_EXIT(([] {
		Initialize();
		// EXEC is a constant 32-bit mask for three vertices and one primitive: correct for
		// that launch of a Wave32, wrong for every other one, so the universal proof
		// must refuse it.
		const auto code = Parse(Fixture(3, 1, true));
		Check(ShaderAnalyzeNggPassthrough(code, {32, 3, 1}).proven, "the single planned launch is provable");
		const auto front = ShaderProveNggFrontLaneLocal(code, 32u);
		Check(!front.lane_local && front.rejection != ShaderNggPassthroughRejection::None && !front.reason.IsEmpty(), "refused with a reason");
		Check(front.refused_counts.guest_wave_size == 32u && front.refused_counts.es_vertex_count >= 1u &&
		          front.refused_counts.gs_primitive_count >= 1u, "the failing launch is reported");
		auto wave = Generic(code, 32u);
		const auto stages = Stages(kNggPassthroughStages);
		const ShaderNggFrontVerdict verdict;
		Check(ShaderApplyNggFrontProof(&wave, code, &stages, verdict) && wave.proof == ShaderNativeWaveProof::ExactSubgroup,
		      "the generic verdict stands");
		Check(verdict.Refusal(32u).ContainsStr("ngg_front_refusal{wave=32") && verdict.Refusal(64u).IsEmpty(),
		      "the refusal is explained for the width that was asked");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, WaveDerivedScalarReachingAVectorInstructionIsRefused)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto words = Fixture();
		const auto base = Parse(words);
		// v_mov_b32 v30, s3: the packed vertex/primitive counts become vertex data.
		Words leak;
		Vop1(leak, 1, 30, 3);
		const auto at = WordIndexOf(base, ShaderInstructionType::VMovB32);
		words.insert(words.begin() + at, leak.begin(), leak.end());
		const auto code = Parse(words);
		for (uint32_t width: {32u, 64u}) { Check(!ShaderProveNggFrontLaneLocal(code, width).lane_local, "numeric wave state is refused"); }
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}


TEST(EmulatorNggFront, ProvesAFusedFrontWithScalarLoadsUniformBranchesAndLaneSelects)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(FetchBody()));
		Check(!ShaderAnalyzeNggPassthrough(code, {64, 1, 1}).proven, "the pure analyzer alone cannot take the body");
		Check(ShaderAnalyzeNggPassthroughPrologue(code, {64, 3, 2}).prologue_proven, "the prologue is proved on its own");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(front.lane_local, "prologue plus body is lane local");
		}
		auto wave = Generic(code, 64u);
		const auto stages = Stages(kNggPassthroughStages);
		const ShaderNggFrontVerdict verdict;
		Check(!ShaderApplyNggFrontProof(&wave, code, &stages, verdict) && wave.proof == ShaderNativeWaveProof::LaneLocal, "admitted");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, AScalarPairCopyRetainsTheSavedVertexExecMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(FetchBodyWithCopiedExecMask(false, false)));
		Check(!ShaderAnalyzeNggPassthrough(code, {64, 1, 1}).proven, "the pure analyzer cannot admit the scalar-load body");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			if (!front.lane_local) { std::fprintf(stderr, "refused: %s\n", front.reason.c_str()); }
			Check(front.lane_local, "an intact two-word mask copy restores the complete vertex cohort");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, AScalarPairCopyDoesNotPromoteAPartialOrOverwrittenMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		for (bool partial: {false, true})
		{
			const auto code = Parse(PrologueThenBody(FetchBodyWithCopiedExecMask(partial, !partial)));
			const auto position_pc = code.GetInstructions().At(InstructionIndex(code, ShaderInstructionType::Exp, 1u)).pc;
			for (uint32_t width: {32u, 64u})
			{
				const auto front = ShaderProveNggFrontLaneLocal(code, width);
				Check(!front.lane_local && front.rejection == ShaderNggPassthroughRejection::UnsupportedInstruction &&
				          front.refusal_pc == position_pc,
				      "a copied partial mask or overwritten half cannot establish the full vertex cohort");
			}
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, RefusesPositionFormatWithRawParameterTargetAfterPositionDone)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto valid = Parse(PrologueThenBody(FetchBody()));
		const uint32_t pos0 = InstructionIndex(valid, ShaderInstructionType::Exp, 1u);
		const auto& parsed_pos0 = valid.GetInstructions().At(pos0);
		Check(parsed_pos0.format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done &&
		          parsed_pos0.raw_word == 0xf80008cfu && parsed_pos0.exp_enable_mask == 15u && parsed_pos0.exp_control == 2u &&
		          parsed_pos0.src_num == 4,
		      "the real parser establishes the valid POS0-DONE body export");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(valid, width);
			Check(front.lane_local && front.rejection == ShaderNggPassthroughRejection::None && front.reason.IsEmpty(),
			      "valid FetchBody proves at both architectural widths");
		}

		const auto with_parameter = Parse(PrologueThenBody(FetchBodyWithParameterAfterPositionDone()));
		const uint32_t injected_index = InstructionIndex(with_parameter, ShaderInstructionType::Exp, 2u);
		const auto& parsed_parameter = with_parameter.GetInstructions().At(injected_index);
		Check(parsed_parameter.format == ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3 &&
		          parsed_parameter.raw_word == 0xf800020fu && parsed_parameter.exp_enable_mask == 15u &&
		          parsed_parameter.exp_control == 0u && parsed_parameter.src_num == 4,
		      "the parser establishes a full raw PARAM0 export after POS0-DONE");
		for (int source = 0; source < 4; ++source)
		{
			const auto& operand = parsed_parameter.src[source];
			Check(operand.type == ShaderOperandType::Vgpr && operand.size == 1 && operand.register_id == 13,
			      "every cloned-format source is a previously defined VGPR");
		}

		auto malformed = with_parameter;
		auto& cloned_position = malformed.GetInstructions()[injected_index];
		cloned_position.format = parsed_pos0.format;
		Check(cloned_position.format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done &&
		          cloned_position.raw_word == 0xf800020fu && cloned_position.exp_enable_mask == 15u &&
		          cloned_position.exp_control == 0u && cloned_position.src_num == 4,
		      "the injected IR keeps PARAM0 raw target and controls under a cloned POS0 format");
		Check(ShaderInstructionLoweringPreconditions(cloned_position), "the raw-target mismatch remains valid generic IR");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(malformed, width);
			Check(!front.lane_local && front.rejection != ShaderNggPassthroughRejection::None && !front.reason.IsEmpty(),
			      "body proof refuses the PARAM-target POS0-format clone after position DONE");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, RefusesPositionExportsAfterPos0Done)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(FetchBodyWithPositionAfterDone()));
		Check(!ShaderAnalyzeNggPassthrough(code, {64, 1, 1}).proven,
		      "the whole-program analyzer cannot take this load-bearing body");
		Check(ShaderAnalyzeNggPassthroughPrologue(code, {64, 3, 2}).prologue_proven,
		      "the independently proved prologue leaves the body route available");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(!front.lane_local, "the body proof rejects POS1-X following POS0 DONE");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, RefusesBranchPathWithoutPos0AtBodyJoin)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(FetchBodyWithBranchMissingPos0()));
		Check(!ShaderAnalyzeNggPassthrough(code, {64, 1, 1}).proven,
		      "the whole-program analyzer cannot take this load-bearing body");
		Check(ShaderAnalyzeNggPassthroughPrologue(code, {64, 3, 2}).prologue_proven,
		      "the independently proved prologue leaves the body route available");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(!front.lane_local, "the body proof rejects a path that reaches the join without POS0");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, ProvesNonFinalPos0ThenFinalPos1XAndParam)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(FetchBodyWithFinalPos1()));
		Check(!ShaderAnalyzeNggPassthrough(code, {64, 1, 1}).proven,
		      "the whole-program analyzer cannot take this load-bearing body");
		Check(ShaderAnalyzeNggPassthroughPrologue(code, {64, 3, 2}).prologue_proven,
		      "the independently proved prologue leaves the body route available");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(front.lane_local, front.reason.IsEmpty() ? "the final POS1-X closes the non-final POS0 sequence before PARAM" : front.reason.c_str());
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, ProvesUniformBranchAlternativePositionHistories)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(UniformBranchPositionAlternativesBody()));
		Check(!ShaderAnalyzeNggPassthrough(code, {64, 1, 1}).proven,
		      "the whole-program analyzer cannot take this load-bearing body");
		Check(ShaderAnalyzeNggPassthroughPrologue(code, {64, 3, 2}).prologue_proven,
		      "the independently proved prologue leaves the body route available");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(front.lane_local,
			      front.reason.IsEmpty() ? "both uniform branch histories reach POS0 and a final position DONE before PARAM" : front.reason.c_str());
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, RefusesPositionSequenceWithoutFinalDone)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(FetchBodyWithMissingPositionDone()));
		Check(!ShaderAnalyzeNggPassthrough(code, {64, 1, 1}).proven,
		      "the whole-program analyzer cannot take this load-bearing body");
		Check(ShaderAnalyzeNggPassthroughPrologue(code, {64, 3, 2}).prologue_proven,
		      "the independently proved prologue leaves the body route available");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(!front.lane_local, "a PARAM export cannot replace final position DONE");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, RefusesDuplicatePos0TargetBeforeFinalDone)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(FetchBodyWithDuplicatePos0()));
		Check(!ShaderAnalyzeNggPassthrough(code, {64, 1, 1}).proven,
		      "the whole-program analyzer cannot take this load-bearing body");
		Check(ShaderAnalyzeNggPassthroughPrologue(code, {64, 3, 2}).prologue_proven,
		      "the independently proved prologue leaves the body route available");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(!front.lane_local, "a second POS0 is refused even when it carries DONE");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, RefusesPositionExportUnderPartialExec)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(FetchBodyWithPartialExecPosition()));
		Check(!ShaderAnalyzeNggPassthrough(code, {64, 1, 1}).proven,
		      "the whole-program analyzer cannot take this load-bearing body");
		Check(ShaderAnalyzeNggPassthroughPrologue(code, {64, 3, 2}).prologue_proven,
		      "the independently proved prologue leaves the body route available");
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(!front.lane_local, "a position export under a strict EXEC subset is refused");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, AnSdwaCompareOfWholeDwordsPreservesUniformity)
{
	ASSERT_EXIT(([] {
		Initialize();
		// Default selects (DWORD, no modifiers) make the SDWA form the ordinary compare.
		const auto identity = Parse(PrologueThenBody(FetchBody(true, false, 6u)));
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(identity, width);
			Check(front.lane_local, "an identity SDWA compare of uniform sources steers a uniform branch");
		}
		// A sub-dword select changes what is compared: no claim is made about it.
		const auto word = Parse(PrologueThenBody(FetchBody(true, false, 4u)));
		const auto refused = ShaderProveNggFrontLaneLocal(word, 64u);
		Check(!refused.lane_local && refused.reason.ContainsStr("not wave-uniform"), "a WORD_0 select is not an identity");
		const auto unmodified_float = Parse(PrologueThenBody(SdwaFloatCompareBody(0u)));
		for (uint32_t width: {32u, 64u})
		{
			const auto plain = ShaderProveNggFrontLaneLocal(unmodified_float, width);
			Check(plain.lane_local, plain.reason.IsEmpty() ? "an unmodified whole-dword F32 compare is uniform" : plain.reason.c_str());
			// NEG and ABS change a float operand, but equal uniform inputs still yield the same comparison bit per lane.
			for (uint32_t modifier: {1u << 28u, 1u << 29u})
			{
				const auto modified = Parse(PrologueThenBody(SdwaFloatCompareBody(modifier)));
				const auto front = ShaderProveNggFrontLaneLocal(modified, width);
				Check(front.lane_local, front.reason.IsEmpty() ? "a whole-dword F32 source modifier preserves a uniform compare" : front.reason.c_str());
			}
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, AUniformCompareUnderPartialExecCannotSteerAUniformBranch)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto partial = Parse(PrologueThenBody(PartialExecUniformCompareBody()));
		const auto front = ShaderProveNggFrontLaneLocal(partial, 64u);
		Check(!front.lane_local && front.reason.ContainsStr("not wave-uniform"),
		      "restoring EXEC does not make a VCC mask uniform when inactive lanes were not compared");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, And64OfTwoFullExecUniformCompareMasksPreservesUniformity)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(CompareMaskAnd64Body(false, false)));
		const auto& vcc_compare = code.GetInstructions().At(InstructionIndex(code, ShaderInstructionType::VCmpGeF32));
		const auto& sgpr_compare = code.GetInstructions().At(InstructionIndex(code, ShaderInstructionType::VCmpEqF32));
		Check(vcc_compare.dst.type == ShaderOperandType::VccLo && vcc_compare.dst.size == 2 &&
		          sgpr_compare.dst.type == ShaderOperandType::Sgpr && sgpr_compare.dst.register_id == 40 && sgpr_compare.dst.size == 2,
		      "the fixture defines a two-word VCC comparison and an independent SGPR mask comparison");
		CheckAnd64VccTuple(code, ShaderOperandType::Sgpr, 40);
		for (uint32_t width: {32u, 64u})
		{
			const auto front = ShaderProveNggFrontLaneLocal(code, width);
			Check(front.lane_local, front.reason.IsEmpty() ? "AND64 of two uniform masks remains uniform" : front.reason.c_str());
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, And64DoesNotProveUniformityFromAVaryingOrPartialCompareMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto varying = Parse(PrologueThenBody(CompareMaskAnd64Body(false, true)));
		const auto& varying_compare = varying.GetInstructions().At(InstructionIndex(varying, ShaderInstructionType::VCmpEqF32));
		Check(varying_compare.dst.type == ShaderOperandType::Sgpr && varying_compare.dst.register_id == 40 &&
		          varying_compare.src[0].type == ShaderOperandType::Vgpr && varying_compare.src[0].register_id == 5,
		      "the negative tuple combines VCC with an SGPR mask from varying V5");
		CheckAnd64VccTuple(varying, ShaderOperandType::Sgpr, 40);
		const auto varying_front = ShaderProveNggFrontLaneLocal(varying, 64u);
		Check(!varying_front.lane_local && varying_front.reason.ContainsStr("not wave-uniform"),
		      "AND64 cannot make a varying compare mask uniform");

		const auto partial = Parse(PrologueThenBody(CompareMaskAnd64Body(true, false)));
		const auto& partial_compare = partial.GetInstructions().At(InstructionIndex(partial, ShaderInstructionType::VCmpGeF32));
		Check(partial_compare.dst.type == ShaderOperandType::VccLo && partial_compare.src[0].type == ShaderOperandType::Sgpr &&
		          partial_compare.src[0].register_id == 24,
		      "the negative tuple defines VCC with uniform operands only under partial EXEC");
		CheckAnd64VccTuple(partial, ShaderOperandType::Sgpr, 40);
		const auto partial_front = ShaderProveNggFrontLaneLocal(partial, 64u);
		Check(!partial_front.lane_local && partial_front.reason.ContainsStr("not wave-uniform"),
		      "AND64 cannot make a partial-EXEC compare mask uniform after EXEC is restored");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, And64WithExecAndAUniformMaskRequiresFullExec)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(PrologueThenBody(ExecAndUniformMaskUnderPartialExecBody()));
		CheckAnd64VccTuple(code, ShaderOperandType::ExecLo, 0);
		const auto front = ShaderProveNggFrontLaneLocal(code, 64u);
		Check(!front.lane_local && front.reason.ContainsStr("not wave-uniform"),
		      "EXEC ANDed with a uniform compare mask under partial EXEC is not a uniform VCC proof");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, VmacF32PreservesItsVaryingOldDestinationForUniformity)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto varying = Parse(PrologueThenBody(VmacF32OldDestinationBody(5u)));
		const auto& mac = varying.GetInstructions().At(InstructionIndex(varying, ShaderInstructionType::VMacF32));
		Check(mac.dst.type == ShaderOperandType::Vgpr && mac.dst.register_id == 5 && mac.src_num == 2 &&
		          mac.src[0].type == ShaderOperandType::Sgpr && mac.src[0].register_id == 24 &&
		          mac.src[1].type == ShaderOperandType::Vgpr && mac.src[1].register_id == 20,
		      "the fixture decodes to an FMA with two explicit inputs and a VGPR destination");
		const auto& compare = varying.GetInstructions().At(InstructionIndex(varying, ShaderInstructionType::VCmpGtF32));
		Check(compare.src_num == 2 && compare.src[0].type == ShaderOperandType::Sgpr && compare.src[0].register_id == 26 &&
		          compare.src[1].type == ShaderOperandType::Vgpr && compare.src[1].register_id == 5,
		      "the follow-up comparison reads the FMA VGPR destination");
		const auto varying_front = ShaderProveNggFrontLaneLocal(varying, 64u);
		Check(!varying_front.lane_local && varying_front.reason.ContainsStr("not wave-uniform"),
		      "uniform FMA inputs do not erase the varying old destination");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, VmacF32RejectsItsUndefinedOldDestination)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto undefined = Parse(PrologueThenBody(VmacF32OldDestinationBody(21u)));
		const auto& mac = undefined.GetInstructions().At(InstructionIndex(undefined, ShaderInstructionType::VMacF32));
		Check(mac.dst.type == ShaderOperandType::Vgpr && mac.dst.register_id == 21 && mac.src_num == 2 &&
		          mac.src[0].type == ShaderOperandType::Sgpr && mac.src[0].register_id == 24 &&
		          mac.src[1].type == ShaderOperandType::Vgpr && mac.src[1].register_id == 20,
		      "the undefined fixture still decodes to an FMA with a VGPR destination");
		const auto& compare = undefined.GetInstructions().At(InstructionIndex(undefined, ShaderInstructionType::VCmpGtF32));
		Check(compare.src_num == 2 && compare.src[0].type == ShaderOperandType::Sgpr && compare.src[0].register_id == 26 &&
		          compare.src[1].type == ShaderOperandType::Vgpr && compare.src[1].register_id == 21,
		      "the follow-up comparison reads the FMA VGPR destination");
		const auto undefined_front = ShaderProveNggFrontLaneLocal(undefined, 64u);
		Check(!undefined_front.lane_local && undefined_front.reason.ContainsStr("before it is defined"),
		      "the implicit FMA destination is a read before its first definition");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, RefusesLaunchDataInTheBodyAndNonUniformBranches)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto leak = Parse(PrologueThenBody(FetchBody(true, true)));
		const auto leaked = ShaderProveNggFrontLaneLocal(leak, 64u);
		Check(!leaked.lane_local && leaked.reason.ContainsStr("launch-dependent scalar"), "wave info read as vector data");
		const auto divergent = Parse(PrologueThenBody(FetchBody(false)));
		const auto diverged  = ShaderProveNggFrontLaneLocal(divergent, 64u);
		Check(!diverged.lane_local && diverged.reason.ContainsStr("not wave-uniform"), "a per-lane comparison cannot steer a branch");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, ARedefinedWaveWordStopsBeingLaunchDependent)
{
	ASSERT_EXIT(([] {
		Initialize();
		// s3 held the counts; a scalar load replaces it, so reading it afterwards is a
		// read of ordinary data. Without the load it is a read of the counts.
		for (bool reload: {true, false})
		{
			Words body;
			if (reload) { body.insert(body.end(), {0xf4200008u | (3u << 6u), 0xfa000004u}); } // s_buffer_load_dword s3, s[16:19], 4
			Sopp(body, 12, 0xc07f);
			Vop1(body, 1, 13, 3u);                                                     // v_mov_b32 v13, s3
			Exp(body, 12, 15, true, 13, 13, 13, 13);
			Sopp(body, 1);
			const auto code  = Parse(PrologueThenBody(body));
			const auto front = ShaderProveNggFrontLaneLocal(code, 64u);
			Check(front.lane_local == reload, reload ? "a redefined word is clean" : "the counts cannot reach a vector");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, BodyRefusesSelectsBackEdgesMaskNumbersUndefinedInputsAndExecRewrites)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto refused = [](const Words& body, const char* reason, const char* why) {
			const auto front = ShaderProveNggFrontLaneLocal(Parse(PrologueThenBody(body)), 64u);
			Check(!front.lane_local && front.reason.ContainsStr(reason), why);
		};
		Words select_by_wave_scc;
		Sop2(select_by_wave_scc, 11, 106, kExecLoReg, Inline(0)); // SCC is still the prologue's: launch dependent
		Exp(select_by_wave_scc, 12, 15, true, 5, 5, 5, 5);
		Sopp(select_by_wave_scc, 1);
		refused(select_by_wave_scc, "launch-dependent SCC", "select by wave SCC");

		Words mask_number;
		Sop2(mask_number, 0, 24, kExecLoReg, Inline(1)); // s_add_u32 s24, exec_lo, 1
		Exp(mask_number, 12, 15, true, 5, 5, 5, 5);
		Sopp(mask_number, 1);
		refused(mask_number, "mask observed as a number", "mask as a number");

		Words back_edge;
		Vop1(back_edge, 1, 13, Inline(0));
		Sopp(back_edge, 2, 0xffff); // s_branch -1
		Exp(back_edge, 12, 15, true, 13, 13, 13, 13);
		Sopp(back_edge, 1);
		refused(back_edge, "forward branches", "back edge");

		Words undefined;
		Exp(undefined, 12, 15, true, 30, 30, 30, 30);
		Sopp(undefined, 1);
		refused(undefined, "before it is defined", "undefined vertex input");

		Words exec_rewrite;
		Sop1(exec_rewrite, 4, kExecLoReg, 20); // s_mov_b64 exec, s[20:21]
		Exp(exec_rewrite, 12, 15, true, 5, 5, 5, 5);
		Sopp(exec_rewrite, 1);
		refused(exec_rewrite, "EXEC", "EXEC rewrite");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// An if/else over a per-lane compare in the body: s_mov_b64 s[40:41], exec; v_cmpx_ne_u32 1, v5; skip the then
// block when no lane is left; then; s_andn2_b64 exec, s[40:41], exec; skip the else block; else; restore EXEC.
Words IfElseBody(const Words& then_block, const Words& else_block, const Words& after_restore)
{
	Words body;
	Sop1(body, 4, 40, kExecLoReg);                                      // s_mov_b64 s[40:41], exec
	Append(body, Vopc(0xd5, Inline(1), 5u));                            // v_cmpx_ne_u32 1, v5
	Sopp(body, 8, static_cast<uint32_t>(then_block.size()));            // s_cbranch_execz over the then block
	Append(body, then_block);
	Sop2(body, 0x15, kExecLoReg, 40, kExecLoReg);                       // s_andn2_b64 exec, s[40:41], exec
	Sopp(body, 8, static_cast<uint32_t>(else_block.size()));            // s_cbranch_execz over the else block
	Append(body, else_block);
	Sop1(body, 4, kExecLoReg, 40);                                      // s_mov_b64 exec, s[40:41]
	Append(body, after_restore);
	Exp(body, 12, 15, true, 13, 13, 13, 13);                            // exp pos0 v13
	Sopp(body, 1);
	return body;
}

// VOP3 v_writelane_b32 / v_readlane_b32: VDST holds the VGPR (write) or the SGPR (read).
void LaneMove(Words& words, uint32_t opcode, uint32_t dst, uint32_t src0, uint32_t lane)
{
	words.push_back(0xd4000000u | (opcode << 16u) | dst);
	words.push_back(src0 | (lane << 9u));
}

// A compiler spill moves one scalar through a constant lane of a VGPR and back.
Words ScalarSpillBody(uint32_t spilled, uint32_t read_lane)
{
	Words body;
	body.push_back(0xb0000000u | (24u << 16u) | 72u); // s_movk_i32 s24, 72
	LaneMove(body, 0x361, 20, spilled, Inline(2));    // v_writelane_b32 v20, s<spilled>, 2
	LaneMove(body, 0x360, 25, 256u + 20u, read_lane); // v_readlane_b32 s25, v20, <lane>
	Vop1(body, 5, 13, 25);                            // v_cvt_f32_i32 v13, s25
	Exp(body, 12, 15, true, 13, 13, 13, 13);          // exp pos0 v13 done
	Sopp(body, 1);                                    // s_endpgm
	return body;
}

TEST(EmulatorNggFront, AStaticScalarSpillThroughAVgprLaneExchangesNoLane)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto spill = ShaderProveNggFrontLaneLocal(Parse(PrologueThenBody(ScalarSpillBody(24u, Inline(2)))), 64u);
		if (!spill.lane_local) { std::fprintf(stderr, "refused: %s\n", spill.reason.c_str()); }
		Check(spill.lane_local, "the read yields the spilled scalar in every lane");

		const auto dynamic = ShaderProveNggFrontLaneLocal(Parse(PrologueThenBody(ScalarSpillBody(24u, 24u))), 64u);
		Check(!dynamic.lane_local && dynamic.reason.ContainsStr("lane exchange"), "a run-time lane index is a real exchange");

		const auto wave_info = ShaderProveNggFrontLaneLocal(Parse(PrologueThenBody(ScalarSpillBody(3u, Inline(2)))), 64u);
		Check(!wave_info.lane_local && wave_info.reason.ContainsStr("launch-dependent"), "the slot keeps the launch dependence");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, AnIfElseThatDefinesBothArmsDefinesTheVgprForEveryLane)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words then_block;
		Vop1(then_block, 1, 13, Inline(0)); // v_mov_b32 v13, 0
		Words else_block;
		Vop1(else_block, 1, 13, 256u + 5u); // v_mov_b32 v13, v5
		const auto both = ShaderProveNggFrontLaneLocal(Parse(PrologueThenBody(IfElseBody(then_block, else_block, {}))), 64u);
		Check(both.lane_local, "the then and else halves of one split cover the lanes they split");
		Words other;
		Vop1(other, 1, 14, 256u + 5u); // v_mov_b32 v14, v5: the else arm leaves v13 undefined
		const auto one = ShaderProveNggFrontLaneLocal(Parse(PrologueThenBody(IfElseBody(then_block, other, {}))), 64u);
		Check(!one.lane_local && one.reason.ContainsStr("before it is defined"), "one arm alone defines half the lanes");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, AScalarAnExecBranchMaySkipIsLaunchDependentAfterTheJoin)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words then_block;
		Vop1(then_block, 1, 13, Inline(0));                                      // v_mov_b32 v13, 0
		then_block.push_back(0xb0000000u | (24u << 16u) | 7u);                   // s_movk_i32 s24, 7
		Words else_block;
		Vop1(else_block, 1, 13, 256u + 5u);
		Words read_after;
		Vop1(read_after, 1, 13, 24u);                                            // v_mov_b32 v13, s24
		// The guest runs the then block once any lane of the wave takes it, the host once any lane of the subgroup
		// does: s24 differs between them after the join.
		const auto front = ShaderProveNggFrontLaneLocal(Parse(PrologueThenBody(IfElseBody(then_block, else_block, read_after))), 64u);
		Check(!front.lane_local && front.reason.ContainsStr("launch-dependent scalar"), "a scalar the region may skip");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, ABufferAddressNeedsOneVgprPerEnabledPart)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words body;
		Vop1(body, 1, 0, Inline(0)); // v_mov_b32 v0, 0: the only defined address word
		Exp(body, 12, 15, true, 0, 0, 0, 0);
		Sopp(body, 1);
		for (int part = 0; part < 3; ++part)
		{
			// part 0: OFFEN only; 1: IDXEN only; 2: IDXEN and OFFEN (the pair needs v1 too).
			auto code = Parse(PrologueThenBody(body));
			ShaderInstruction load {};
			load.type        = ShaderInstructionType::BufferLoadDwordx4;
			load.dst         = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 4};
			load.src[0]      = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = part == 2 ? 2 : 1};
			load.src[1]      = {.type = ShaderOperandType::Sgpr, .register_id = 16, .size = 4};
			load.src[2]      = {.type = ShaderOperandType::IntegerInlineConstant, .size = 0};
			load.src_num     = 3;
			const auto export_index = InstructionIndex(code, ShaderInstructionType::VMovB32) + 1u; // after the only address word is defined
			load.pc           = code.GetInstructions().At(export_index).pc;
			load.buffer_offen = part != 1;
			load.buffer_idxen = part != 0;
			code.GetInstructions().InsertAt(export_index, load);
			const auto front = ShaderProveNggFrontLaneLocal(code, 64u);
			Check(front.lane_local == (part != 2), part == 2 ? "the offset half of a pair must be defined" : "a single address word is enough");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, APrologueThatIsNotCountExactIsRefusedBeforeTheBody)
{
	ASSERT_EXIT(([] {
		Initialize();
		// A constant 32-bit EXEC mask is right for three vertices only.
		auto words = Fixture(3, 1, true);
		const auto code = Parse(words);
		const auto front = ShaderProveNggFrontLaneLocal(code, 32u);
		Check(!front.lane_local && front.rejection != ShaderNggPassthroughRejection::None, "count-specific EXEC is refused");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggFront, GeneratedModuleLaunchesEveryLiveLaneOfTheSubgroup)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(Fixture());
		ShaderVertexInputInfo input {};
		input.gs_prolog = true;
		input.position1_usage = ShaderVertexPosition1Usage::RenderTargetLayer;
		const auto source = SpirvGenerateSource(code, &input, nullptr, nullptr);
		Check(!source.IsEmpty(), "complete module source");
		// The prologue's EXEC = low (s3 & 0xff) bits must keep every live invocation of the subgroup.
		const char* sequence = "%ngg_wave_msb = OpGroupNonUniformBallotFindMSB %uint %uint_3 %native_initial_ballot\n"
		                       "%ngg_wave_lanes = OpIAdd %uint %ngg_wave_msb %uint_1\n"
		                       "%ngg_wave_primitives = OpShiftLeftLogical %uint %ngg_wave_lanes %uint_8\n"
		                       "%ngg_wave_info = OpBitwiseOr %uint %ngg_wave_primitives %ngg_wave_lanes\n"
		                       "OpStore %s3 %ngg_wave_info\n";
		Check(source.FindIndex(sequence) != Core::STRING8_INVALID_INDEX, "s3 counts every lane up to the highest live one");
		Check(source.FindIndex("OpStore %s3 %uint_0x00000101") == Core::STRING8_INVALID_INDEX,
		      "no one-lane wave whose EXEC would drop every invocation but subgroup lane 0");
		const auto ballot = source.FindIndex("%native_initial_ballot");
		const auto info   = source.FindIndex("OpStore %s3 %ngg_wave_info");
		Check(ballot != Core::STRING8_INVALID_INDEX && ballot < info, "s3 is derived after the live-lane ballot");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

UT_END();
