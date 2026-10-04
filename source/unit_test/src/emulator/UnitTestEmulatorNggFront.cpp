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
Words FetchBody(bool uniform_compare = true, bool lane_data_to_vector = false, uint32_t sdwa_src1_select = 0u)
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
	Sopp(body, 7, 4);                                          // s_cbranch_vccnz over the next four dwords
	Vop1(body, 1, 13, lane_data_to_vector ? 3u : 256u + 5u);   // v_mov_b32 v13, v5 (or s3: the wave info)
	Append(body, Sopc(6, 20, Inline(0)));                      // s_cmp_eq_u32 s20, 0
	Sop2(body, 11, 106, kExecLoReg, Inline(0));                // s_cselect_b64 vcc, exec, 0
	Vop2(body, 1, 14, 256u + 5u, 8u);                          // v_cndmask_b32 v14, v5, v8, vcc
	Exp(body, 12, 15, true, 13, 13, 14, 14);                   // exp pos0 v13, v13, v14, v14 done
	Sopp(body, 1);                                             // s_endpgm
	return body;
}

uint32_t InstructionIndex(const ShaderCode& code, ShaderInstructionType type)
{
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		if (code.GetInstructions().At(index).type == type) { return index; }
	}
	Check(false, "requested instruction exists");
	return 0;
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

TEST(EmulatorNggFront, AnSdwaCompareOfWholeDwordsIsAPlainUniformCompare)
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
