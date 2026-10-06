#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderFragmentMaskFlow.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "NggPassthroughFixture.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

UT_BEGIN(EmulatorFragmentMaskFlow);

using namespace Libs::Graphics;

namespace {

using namespace NggFixture;

// One entry per source instruction so a variant can insert, drop or reorder whole
// instructions. Independently authored generic RDNA2 encodings of the pattern a
// compiler emits for "flip the result when a uniform constant says so" in a Wave64
// pixel shader; registers and values are synthetic.
enum Slot : size_t
{
	kPrefetch,
	kM0,
	kLoad,
	kSave,
	kWqm,
	kWait,
	kCompare,
	kSelect,
	kCopy,
	kBlend,
	kRestore,
	kZero,
	kExport,
	kEnd,
	kSlotCount,
};

constexpr uint32_t kVccLo   = 106u;
constexpr uint32_t kM0Reg   = 124u;
constexpr uint32_t kExecLo  = 126u;
constexpr uint32_t kVgpr    = 256u;

std::vector<Words> Baseline()
{
	std::vector<Words> slots(kSlotCount);
	Sopp(slots[kPrefetch], 32, 1);                         // s_inst_prefetch
	Sop1(slots[kM0], 3, kM0Reg, 16);                       // s_mov_b32 m0, s16
	slots[kLoad] = {0xf4201a86u, 4u};                      // s_buffer_load_dword vcc_lo, s[12:15], 4
	Sop1(slots[kSave], 4, 12, kExecLo);                    // s_mov_b64 s[12:13], exec
	Sop1(slots[kWqm], 10, kExecLo, kExecLo);               // s_wqm_b64 exec, exec
	Sopp(slots[kWait], 12, 0xc07f);                        // s_waitcnt lgkmcnt(0)
	slots[kCompare] = Sopc(6, Inline(1), kVccLo);          // s_cmp_eq_u32 1, vcc_lo
	Sop2(slots[kSelect], 11, kVccLo, kExecLo, Inline(0));  // s_cselect_b64 vcc, exec, 0
	Vop1(slots[kCopy], 1, 1, kVgpr + 0);                   // v_mov_b32 v1, v0
	Vop2(slots[kBlend], 1, 0, kVgpr + 0, 1);               // v_cndmask_b32 v0, v0, v1, vcc
	Sop1(slots[kRestore], 4, kExecLo, 12);                 // s_mov_b64 exec, s[12:13]
	Vop1(slots[kZero], 1, 2, Inline(0));                   // v_mov_b32 v2, 0
	slots[kExport] = {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)}; // exp mrt0 v2,v2,v2,v2 vm done
	Sopp(slots[kEnd], 1);                                  // s_endpgm
	return slots;
}

ShaderCode ParsePixel(const std::vector<Words>& slots)
{
	Words words;
	for (const auto& slot: slots) { words.insert(words.end(), slot.begin(), slot.end()); }
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	Check(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code), "bounded complete parse");
	for (const auto& inst: code.GetInstructions())
	{
		Check(ShaderInstructionLoweringPreconditions(inst), "every parsed instruction passes the shared preconditions");
	}
	return code;
}

uint32_t Find(const ShaderCode& code, ShaderInstructionType type)
{
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		if (code.GetInstructions().At(index).type == type) { return index; }
	}
	Check(false, "requested instruction exists");
	return 0;
}

bool Contains(const char* text, const char* fragment)
{
	return text != nullptr && std::strstr(text, fragment) != nullptr;
}

void ExpectNotLaneLocal(const ShaderCode& code, const char* why)
{
	for (uint32_t width: {32u, 64u})
	{
		const auto wave = ShaderAnalyzeNativeWave(code, width);
		Check(wave.proof != ShaderNativeWaveProof::LaneLocal, why);
	}
}

} // namespace

TEST(EmulatorFragmentMaskFlow, AdmitsTheWqmBracketedUniformSelectAsLaneLocal)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = ParsePixel(Baseline());
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(flow.lane_local && flow.reason == nullptr, "every mask observation is a lane bit or mask algebra");
		const auto wqm     = Find(code, ShaderInstructionType::SWqmB64);
		uint32_t closed = 0;
		for (uint32_t index = 0; index < flow.closed_exec_write.size(); ++index) { closed += flow.closed_exec_write[index] ? 1u : 0u; }
		Check(closed == 2u && flow.closed_exec_write[wqm], "exactly the WQM entry and its restore are closed");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ANumericObservationOfTheMaskIsRefused)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = Baseline();
		Sop2(slots[kWait], 0, 0, kExecLo, Inline(1)); // s_add_u32 s0, exec_lo, 1: the lane set as a number
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(!flow.lane_local && Contains(flow.reason, "mask observed as a number"), "refused as a numeric use");
		ExpectNotLaneLocal(code, "a numeric mask observation is never lane local");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// VOP3 v_writelane_b32 / v_readlane_b32: VDST holds the VGPR (write) or the SGPR (read).
void LaneMove(Words& words, uint32_t opcode, uint32_t dst, uint32_t src0, uint32_t lane)
{
	words.push_back(0xd4000000u | (opcode << 16u) | dst);
	words.push_back(src0 | (lane << 9u));
}

// The saved EXEC spilled through two lanes of v3 and read back before the restore,
// as a compiler spills a scalar pair under register pressure.
std::vector<Words> SpilledSaveSlots(bool read_back_as_number)
{
	auto slots = Baseline();
	LaneMove(slots[kSave], 0x361, 3, 12, Inline(1)); // v_writelane_b32 v3, s12, 1
	LaneMove(slots[kSave], 0x361, 3, 13, Inline(2)); // v_writelane_b32 v3, s13, 2
	slots[kRestore].clear();
	LaneMove(slots[kRestore], 0x360, 40, kVgpr + 3, Inline(1)); // v_readlane_b32 s40, v3, 1
	LaneMove(slots[kRestore], 0x360, 41, kVgpr + 3, Inline(2)); // v_readlane_b32 s41, v3, 2
	if (read_back_as_number)
	{
		Vop1(slots[kRestore], 1, 4, 40); // v_mov_b32 v4, s40
	}
	Sop1(slots[kRestore], 4, kExecLo, 40); // s_mov_b64 exec, s[40:41]
	return slots;
}

TEST(EmulatorFragmentMaskFlow, AMaskSpilledThroughAVgprLaneStaysAMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto spilled = ShaderAnalyzeFragmentMaskFlow(ParsePixel(SpilledSaveSlots(false)));
		if (!spilled.lane_local) { std::fprintf(stderr, "refused: %s\n", spilled.reason); }
		Check(spilled.lane_local && spilled.reason == nullptr, "a static spill moves the saved EXEC like a scalar copy");

		const auto observed = ShaderAnalyzeFragmentMaskFlow(ParsePixel(SpilledSaveSlots(true)));
		Check(!observed.lane_local && Contains(observed.reason, "mask observed as a number"),
		      "the read-back word is still a mask");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ASelectUnderAMaskDerivedSccIsRefused)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = Baseline();
		slots[kCompare].clear(); // SCC still holds the S_WQM_B64 result: exec != 0
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(!flow.lane_local && Contains(flow.reason, "SCC derived from a mask"), "refused as a lane-dependent choice");
		ExpectNotLaneLocal(code, "a select under a mask-derived SCC is never lane local");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ANumericValueCannotBecomeTheExecutionMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = Baseline();
		slots[kRestore].clear();
		Sop1(slots[kRestore], 3, kExecLo, 20); // s_mov_b32 exec_lo, s20
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(!flow.lane_local && Contains(flow.reason, "EXEC assigned a numeric value"), "refused as a numeric EXEC");
		ExpectNotLaneLocal(code, "a numeric EXEC is never lane local");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// VCC enters undefined (treated as a mask) and the load replaces only its low word with a number. Scalar ALU may
// carry such a half-mask pair like a diverged word, as long as nothing observes the result.
std::vector<Words> MixedPairCarried()
{
	auto slots = Baseline();
	slots[kSelect].clear();
	Sop2(slots[kSelect], 15, kVccLo, 0, kVccLo); // s_and_b64 vcc, s[0:1], vcc
	return slots;
}

TEST(EmulatorFragmentMaskFlow, AdmitsAHalfMaskPairThatScalarAluCarriesButNothingObserves)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = MixedPairCarried();
		slots[kBlend].clear();
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(flow.lane_local && flow.reason == nullptr, "an unobserved half-mask pair is not a mask observation");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AHalfMaskPairCarriedByScalarAluIsStillRefusedWhereObserved)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto lane_bit = ParsePixel(MixedPairCarried()); // v_cndmask reads the carried pair as lane bits
		const auto by_lane  = ShaderAnalyzeFragmentMaskFlow(lane_bit);
		Check(!by_lane.lane_local && Contains(by_lane.reason, "mask pair partly overwritten by a number"), "a vector read refuses");
		ExpectNotLaneLocal(lane_bit, "a lane read of a half-mask pair is never lane local");
		auto slots = MixedPairCarried();
		slots[kBlend].clear();
		Sop1(slots[kBlend], 4, kExecLo, kVccLo); // s_mov_b64 exec, vcc
		const auto exec    = ParsePixel(slots);
		const auto by_exec = ShaderAnalyzeFragmentMaskFlow(exec);
		Check(!by_exec.lane_local && Contains(by_exec.reason, "partly overwritten by a number"), "EXEC from it refuses");
		slots = MixedPairCarried();
		slots[kBlend].clear();
		Sop2(slots[kBlend], 11, 20, Inline(1), Inline(0)); // s_cselect_b64 s[20:21], 1, 0: SCC came from the pair
		const auto select    = ParsePixel(slots);
		const auto by_select = ShaderAnalyzeFragmentMaskFlow(select);
		Check(!by_select.lane_local && Contains(by_select.reason, "SCC derived from a mask"), "its SCC selects nothing");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ADescriptorReusedAsTheSaveAreaIsAMaskOnlyAfterTheSave)
{
	ASSERT_EXIT(([] {
		Initialize();
		// Save first, then load through the very registers that now hold the mask.
		auto slots = Baseline();
		std::swap(slots[kLoad], slots[kSave]);
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(!flow.lane_local && Contains(flow.reason, "mask"), "a mask used as a descriptor is refused");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AWrapperWithoutItsRestoreStaysOutsideAnyClosedRegion)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = Baseline();
		slots[kRestore].clear();
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(!flow.lane_local && Contains(flow.reason, "helper lanes"), "exporting while still widened is refused");
		ExpectNotLaneLocal(code, "an unrestored EXEC widening is never lane local");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AClobberedSaveAreaOrAnExportInsideBreaksTheWrapper)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto clobbered = Baseline();
		Sop1(clobbered[kWait], 3, 12, 20); // s_mov_b32 s12, s20 after the widening
		const auto clobbered_code = ParsePixel(clobbered);
		const auto clobbered_flow = ShaderAnalyzeFragmentMaskFlow(clobbered_code);
		Check(!clobbered_flow.lane_local && Contains(clobbered_flow.reason, "partly overwritten"),
		      "a save area half replaced by a number is not a mask");
		ExpectNotLaneLocal(clobbered_code, "an unverified restore is never lane local");

		auto exported = Baseline();
		std::swap(exported[kExport], exported[kRestore]); // export while helper lanes are still active
		const auto exported_code = ParsePixel(exported);
		const auto exported_flow = ShaderAnalyzeFragmentMaskFlow(exported_code);
		Check(!exported_flow.lane_local && Contains(exported_flow.reason, "helper lanes"),
		      "an export while EXEC includes helper lanes is refused");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AMemoryWriteMakesHelperLanesObservable)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto code = ParsePixel(Baseline());
		// A buffer store before the export: the guest performs it for live lanes only.
		ShaderInstruction store {};
		store.type = ShaderInstructionType::BufferStoreDword;
		store.pc   = code.GetInstructions().At(Find(code, ShaderInstructionType::Exp)).pc;
		code.GetInstructions().InsertAt(Find(code, ShaderInstructionType::Exp), store);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(!flow.lane_local && Contains(flow.reason, "memory write"), "a memory write is refused");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// A divergent block: lanes selected by a per-lane compare run a scalar load and a vector
// use; the others skip it. `after_join` is appended after the join.
std::vector<Words> DivergentProgram(const Words& after_join)
{
	std::vector<Words> slots;
	Words head;
	Sop1(head, 4, 4, kExecLo);                                        // s_mov_b64 s[4:5], exec
	Append(head, Vopc(0xc5, Inline(0), 0u));                          // v_cmp_ne_u32 vcc, 0, v0 (per lane)
	Sop1(head, 0x24, 2, kVccLo);                                      // s_and_saveexec_b64 s[2:3], vcc
	Sopp(head, 8, 4);                                                 // s_cbranch_execz over the next four dwords
	head.insert(head.end(), {0xf4200508u, 0xfa000004u});              // s_buffer_load_dword s20, s[16:19], 4
	Sopp(head, 12, 0xc07f);                                           // s_waitcnt lgkmcnt(0)
	Vop1(head, 1, 1, 20);                                             // v_mov_b32 v1, s20
	Sop1(head, 4, kExecLo, 2);                                        // join: s_mov_b64 exec, s[2:3]
	Append(head, after_join);
	Vop1(head, 1, 2, Inline(0));                                      // v_mov_b32 v2, 0
	head.insert(head.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(head, 1);
	slots.push_back(head);
	return slots;
}

TEST(EmulatorFragmentMaskFlow, AdmitsDivergentBlocksWhoseScalarsAreNotReadAfterTheJoin)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto program = ParsePixel(DivergentProgram({}));
		const auto flow    = ShaderAnalyzeFragmentMaskFlow(program);
		Check(flow.lane_local, "a scalar used only inside its divergent block is lane local");
		Check(flow.closed_exec_write[Find(program, ShaderInstructionType::SAndSaveexecB64)], "narrowing by a lane bit is accounted for");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(program, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		// A packed compare after control flow is still this lane's own bit.
		Words compare_after;
		Append(compare_after, Vopc(0xc5, Inline(0), 0u)); // v_cmp_ne_u32 vcc, 0, v0
		const auto after = ParsePixel(DivergentProgram(compare_after));
		const auto wave  = ShaderAnalyzeNativeWave(after, 64u);
		Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "packed compare after a branch is lane local");
		// Redefining the scalar after the join ends its divergence.
		Words reload;
		reload.insert(reload.end(), {0xf4200508u, 0xfa000004u});
		Sopp(reload, 12, 0xc07f);
		Vop1(reload, 1, 3, 20);
		Check(ShaderAnalyzeFragmentMaskFlow(ParsePixel(DivergentProgram(reload))).lane_local, "a redefined scalar is clean again");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ADivergentBlocksScalarCannotBeReadByAVectorAfterTheJoin)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words leak;
		Vop1(leak, 1, 3, 20); // v_mov_b32 v3, s20: skipped lanes still hold the old s20 on the host
		const auto flow = ShaderAnalyzeFragmentMaskFlow(ParsePixel(DivergentProgram(leak)));
		Check(!flow.lane_local && Contains(flow.reason, "divergent control flow"), "the divergent scalar is refused");
		ExpectNotLaneLocal(ParsePixel(DivergentProgram(leak)), "a divergent scalar read after the join is never lane local");
		// Carried through scalar ALU it is still divergent, and a scalar load address is a sink too.
		Words carried;
		Sop2(carried, 0, 21, 20, Inline(1));                         // s_add_u32 s21, s20, 1
		carried.insert(carried.end(), {0xf4200508u | (22u << 6u), 21u << 25u});       // s_buffer_load_dword s22, s[16:19], s21
		const auto by_address = ShaderAnalyzeFragmentMaskFlow(ParsePixel(DivergentProgram(carried)));
		Check(!by_address.lane_local && Contains(by_address.reason, "divergent control flow"), "divergence survives scalar ALU");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, UniformBranchesNeedNoDivergenceRuleAndMaskedSccIsRefused)
{
	ASSERT_EXIT(([] {
		Initialize();
		// SCC from a compare of scalar numbers: every lane takes the same path, so a
		// scalar written in the skipped block may be read afterwards.
		std::vector<Words> uniform(1);
		uniform[0].insert(uniform[0].end(), {0xf4200508u, 0xfa000004u});          // s_buffer_load_dword s20, s[16:19], 4
		Sopp(uniform[0], 12, 0xc07f);
		Append(uniform[0], Sopc(6, 20, Inline(0)));                               // s_cmp_eq_u32 s20, 0
		Sopp(uniform[0], 5, 2);                                                   // s_cbranch_scc1 over the next two dwords
		uniform[0].insert(uniform[0].end(), {0xf4200508u | (21u << 6u), 0xfa000008u}); // s_buffer_load_dword s21, s[16:19], 8
		Vop1(uniform[0], 1, 3, 21);                                               // join: v_mov_b32 v3, s21
		Vop1(uniform[0], 1, 2, Inline(0));
		uniform[0].insert(uniform[0].end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(uniform[0], 1);
		Check(ShaderAnalyzeFragmentMaskFlow(ParsePixel(uniform)).lane_local, "a uniform branch needs no divergence rule");

		// The same branch on the SCC an S_WQM_B64 left behind depends on the lane set.
		std::vector<Words> masked(1);
		Sop1(masked[0], 4, 4, kExecLo);
		Sop1(masked[0], 10, kExecLo, kExecLo);
		Sopp(masked[0], 5, 1);
		Vop1(masked[0], 1, 3, Inline(0));
		Sop1(masked[0], 4, kExecLo, 4);
		Vop1(masked[0], 1, 2, Inline(0));
		masked[0].insert(masked[0].end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(masked[0], 1);
		const auto by_scc = ShaderAnalyzeFragmentMaskFlow(ParsePixel(masked));
		Check(!by_scc.lane_local && Contains(by_scc.reason, "SCC branch"), "a branch on a mask-derived SCC is refused");

		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

std::vector<Words> AsSlots(const Words& words)
{
	return {words};
}

// A pixel loop as the guest compiler emits it for "iterate while a per-lane condition
// holds": lanes leave by narrowing EXEC, the wave leaves when none is left, EXEC comes
// back from its saved copy after the exit.
//   save; head: v_cmpx; s_cbranch_execz exit; body; s_branch head; exit: restore; after; export
Words LoopProgram(const Words& body, const Words& after)
{
	Words words;
	Sop1(words, 4, 4, kExecLo);                                  // s_mov_b64 s[4:5], exec
	const size_t head = words.size();
	Append(words, Vopc(0xd5, Inline(0), 0u));                    // v_cmpx_ne_u32 0, v0: lanes drop out
	const size_t test = words.size();
	Sopp(words, 8, 0);                                           // s_cbranch_execz exit (patched below)
	Append(words, body);
	const size_t back = words.size();
	Sopp(words, 2, static_cast<uint32_t>(head - (back + 1u)) & 0xffffu); // s_branch head
	words[test] |= static_cast<uint32_t>(words.size() - (test + 1u));
	Sop1(words, 4, kExecLo, 4);                                  // exit: s_mov_b64 exec, s[4:5]
	Append(words, after);
	Vop1(words, 1, 2, Inline(0));
	words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(words, 1);
	return words;
}

// The bottom-tested form: the body runs once per iteration and S_CBRANCH_EXECNZ repeats it.
Words BottomTestedLoop(const Words& body, const Words& after)
{
	Words words;
	Sop1(words, 4, 4, kExecLo);                                  // s_mov_b64 s[4:5], exec
	const size_t head = words.size();
	Append(words, Vopc(0xd5, Inline(0), 0u));                    // v_cmpx_ne_u32 0, v0
	Append(words, body);
	const size_t test = words.size();
	Sopp(words, 9, static_cast<uint32_t>(head - (test + 1u)) & 0xffffu); // s_cbranch_execnz head
	Sop1(words, 4, kExecLo, 4);                                  // s_mov_b64 exec, s[4:5]
	Append(words, after);
	Vop1(words, 1, 2, Inline(0));
	words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(words, 1);
	return words;
}

Words ScalarLoadOfS20()
{
	Words words;
	words.insert(words.end(), {0xf4200508u, 0xfa000004u});       // s_buffer_load_dword s20, s[16:19], 4
	Sopp(words, 12, 0xc07f);                                     // s_waitcnt lgkmcnt(0)
	return words;
}

ShaderFragmentMaskFlow FlowOf(const Words& words)
{
	return ShaderAnalyzeFragmentMaskFlow(ParsePixel(AsSlots(words)));
}

TEST(EmulatorFragmentMaskFlow, AdmitsALoopWhoseLanesLeaveByNarrowingTheMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words body;
		Vop1(body, 1, 1, 256u + 0u);                             // v_mov_b32 v1, v0
		for (const auto& words: {LoopProgram(body, {}), BottomTestedLoop(body, {})})
		{
			const auto code = ParsePixel(AsSlots(words));
			const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
			Check(flow.lane_local && flow.reason == nullptr, "narrowing EXEC inside a loop is mask algebra");
			Check(flow.closed_exec_write[Find(code, ShaderInstructionType::VCmpxNeU32)], "the narrowing compare is accounted for");
			for (uint32_t width: {32u, 64u})
			{
				const auto wave = ShaderAnalyzeNativeWave(code, width);
				Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
			}
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AScalarWrittenInADivergentLoopIsDivergentAfterTheExit)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words inside = ScalarLoadOfS20();
		Vop1(inside, 1, 1, 20);                                  // v_mov_b32 v1, s20: a live lane sees its own iterations
		Check(FlowOf(LoopProgram(inside, {})).lane_local, "a scalar used only inside the loop is lane local");
		Check(FlowOf(BottomTestedLoop(inside, {})).lane_local, "so is the bottom-tested form");

		// Lanes that left early ran a different number of iterations on the host than on the guest.
		Words leak;
		Vop1(leak, 1, 3, 20);                                    // v_mov_b32 v3, s20 after the exit
		for (const auto& words: {LoopProgram(inside, leak), BottomTestedLoop(inside, leak)})
		{
			const auto flow = FlowOf(words);
			Check(!flow.lane_local && Contains(flow.reason, "divergent control flow"), "the loop's scalar is divergent after the exit");
		}
		// Redefined after the exit it is clean again.
		Words reload = ScalarLoadOfS20();
		Vop1(reload, 1, 3, 20);
		Check(FlowOf(LoopProgram(inside, reload)).lane_local, "a redefined scalar is clean again");

		// A counter that advances per iteration is coherent for the lanes still running.
		Words counter;
		Sop2(counter, 0, 21, 21, Inline(1));                     // s_add_u32 s21, s21, 1
		Vop1(counter, 1, 1, 21);                                 // v_mov_b32 v1, s21
		Check(FlowOf(LoopProgram(counter, {})).lane_local, "a loop-carried scalar is read by the lanes that produced it");
		Words after_counter;
		Vop1(after_counter, 1, 3, 21);
		const auto flow = FlowOf(LoopProgram(counter, after_counter));
		Check(!flow.lane_local && Contains(flow.reason, "divergent control flow"), "the counter is divergent after the exit");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AdmitsThePlainWqmBracketWhenTheWholeProgramPassAlreadyAcceptsIt)
{
	ASSERT_EXIT(([] {
		Initialize();
		// s_mov_b64 vcc, exec; s_wqm_b64 exec, exec; <derivative work>; s_mov_b64 exec, vcc; export.
		// No SCC, no select: the whole-program pass accepts it without the flow proof, and the
		// widening write must still be accounted for by the flow proof.
		Words words;
		Sop1(words, 4, kVccLo, kExecLo);
		Sop1(words, 10, kExecLo, kExecLo);
		Vop1(words, 1, 1, 256u + 0u);
		Sop1(words, 4, kExecLo, kVccLo);
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
		const auto code = ParsePixel(AsSlots(words));
		Check(ShaderAnalyzeFragmentMaskFlow(code).lane_local, "the bracket is a closed widen/restore of the entry mask");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		// The restore is what makes the export narrow: without it the export sees helper lanes.
		Words open_bracket;
		Sop1(open_bracket, 4, kVccLo, kExecLo);
		Sop1(open_bracket, 10, kExecLo, kExecLo);
		Vop1(open_bracket, 1, 2, Inline(0));
		open_bracket.insert(open_bracket.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(open_bracket, 1);
		const auto open = ParsePixel(AsSlots(open_bracket));
		Check(!ShaderAnalyzeFragmentMaskFlow(open).lane_local, "an export under the widened mask is refused");
		ExpectNotLaneLocal(open, "and so is the stage");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AUniformLoopNeedsNoDivergenceRule)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words words = ScalarLoadOfS20();                         // the trip count is a scalar number
		const size_t head = words.size();
		Sop2(words, 0, 21, 21, Inline(1));                       // head: s_add_u32 s21, s21, 1
		Append(words, Sopc(0x4, 21, 20));                        // s_cmp_lg_u32 s21, s20
		Sopp(words, 5, static_cast<uint32_t>(head - (words.size() + 1u)) & 0xffffu); // s_cbranch_scc1 head
		Vop1(words, 1, 3, 21);                                   // the counter is the same on every lane
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
		Check(FlowOf(words).lane_local, "every lane runs the same iterations, so the counter needs no divergence rule");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ABackEdgeCarriesWhatTheLoopBodyDoesToTheLoopHead)
{
	ASSERT_EXIT(([] {
		Initialize();
		// Iteration one reads s20 as a number; iteration two reads the mask the body saved into it.
		Words words = ScalarLoadOfS20();
		const size_t head = words.size();
		Vop1(words, 1, 1, 20);                                   // head: v_mov_b32 v1, s20
		Sop1(words, 4, 20, kExecLo);                             // s_mov_b64 s[20:21], exec
		Append(words, Sopc(0x6, 22, Inline(0)));                 // s_cmp_eq_u32 s22, 0 (a clean number)
		Sopp(words, 5, static_cast<uint32_t>(head - (words.size() + 1u)) & 0xffffu); // s_cbranch_scc1 head
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
		const auto flow = FlowOf(words);
		Check(!flow.lane_local && Contains(flow.reason, "mask observed as a number"), "the second iteration observes the mask");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ALoopOnAMaskDerivedSccIsRefusedButADerivativeFetchInsideIsNot)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words masked;
		Sop1(masked, 4, 4, kExecLo);
		Sop1(masked, 10, kExecLo, kExecLo);                      // s_wqm_b64 leaves SCC = "any lane"
		const size_t head = masked.size();
		Vop1(masked, 1, 3, Inline(0));
		Sopp(masked, 5, static_cast<uint32_t>(head - (masked.size() + 1u)) & 0xffffu); // s_cbranch_scc1 head
		Sop1(masked, 4, kExecLo, 4);
		Vop1(masked, 1, 2, Inline(0));
		masked.insert(masked.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(masked, 1);
		const auto by_scc = FlowOf(masked);
		Check(!by_scc.lane_local && Contains(by_scc.reason, "SCC branch"), "the trip count would depend on the lane set");

		Words body;
		Vop1(body, 1, 1, 256u + 0u);
		// The bottom-tested loop opens no branch region: only the loop itself is divergent. Its lane-dependent
		// branches take their quad's vote, so the quad runs every iteration as one.
		for (const auto& words: {LoopProgram(body, {}), BottomTestedLoop(body, {})})
		{
			auto inside = ParsePixel(AsSlots(words));
			ShaderInstruction sample {};
			sample.type = ShaderInstructionType::ImageSample;
			const auto mov = Find(inside, ShaderInstructionType::VMovB32);
			sample.pc = inside.GetInstructions().At(mov).pc - 1u;
			inside.GetInstructions().InsertAt(mov, sample);
			const auto derivative = ShaderAnalyzeFragmentMaskFlow(inside);
			Check(derivative.lane_local, "implicit derivatives inside a divergent loop see their whole quad");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}


// Discard: widen to quads, kill lanes by a per-lane compare, early-out to an inert tail
// when no lane survives, re-widen from the survivors, narrow again before the export.
Words DiscardProgram(bool inert_tail, bool restore_before_export, bool wide_survivors)
{
	Words words;
	Sop1(words, 4, 12, kExecLo);                                 // s_mov_b64 s[12:13], exec (live lanes)
	Sop1(words, 10, kExecLo, kExecLo);                           // s_wqm_b64 exec, exec
	Append(words, Vopc(0xc5, Inline(0), 0u));                    // v_cmp_ne_u32 vcc, 0, v0 (kill condition)
	if (wide_survivors)
	{
		Sop1(words, 8, 12, kVccLo);                              // s_not_b64 s[12:13], vcc: a complement includes helpers
	} else
	{
		Sop2(words, 0x15, 12, 12, kVccLo);                       // s_andn2_b64 s[12:13], s[12:13], vcc
	}
	Sopp(words, 4, restore_before_export ? 7 : 6);               // s_cbranch_scc0 to the tail
	Sop1(words, 10, kExecLo, 12);                                // s_wqm_b64 exec, s[12:13]
	Vop1(words, 1, 1, 256u + 0u);                                // v_mov_b32 v1, v0 (derivative work)
	if (restore_before_export) { Sop1(words, 4, kExecLo, 12); }  // s_mov_b64 exec, s[12:13]
	Vop1(words, 1, 2, Inline(0));
	words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(words, 1);
	if (inert_tail)
	{
		Sop1(words, 4, kExecLo, Inline(0));                      // s_mov_b64 exec, 0
		words.insert(words.end(), {0xf8001c00u, 0u});            // exp null mrt0, vm, done
		Sopp(words, 1);
	} else
	{
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
	}
	return words;
}

TEST(EmulatorFragmentMaskFlow, AdmitsTheDiscardPatternWithWideningAnInertTailAndANarrowExport)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = ParsePixel(AsSlots(DiscardProgram(true, true, false)));
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(flow.lane_local, "kill, early-out and re-widening are lane local");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// A depth/stencil-only pass kills lanes with the valid-mask export of the dedicated null target
// (`exp null, vm, done`): survivors are exported as EXEC, and a wave with none skips to a tail that
// exports an empty mask. Both tails kill the same lanes, so the wave-level shortcut is unobservable.
Words NullTargetKillProgram(bool exporting_tail)
{
	Words words;
	Append(words, Vopc(0xc5, Inline(0), 0u));                    // v_cmp_ne_u32 vcc, 0, v0 (kill condition)
	Sop2(words, 0x15, kVccLo, kExecLo, kVccLo);                  // s_andn2_b64 vcc, exec, vcc (survivors)
	Sopp(words, 4, 4);                                           // s_cbranch_scc0 to the tail
	Sop1(words, 4, kExecLo, kVccLo);                             // s_mov_b64 exec, vcc
	words.insert(words.end(), {0xf8001890u, 0u});                // exp null, vm, done
	Sopp(words, 1);
	Sop1(words, 4, kExecLo, Inline(0));                          // tail: s_mov_b64 exec, 0
	if (exporting_tail)
	{
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)}); // exp mrt0 ... done
	} else
	{
		words.insert(words.end(), {0xf8001890u, 0u});            // exp null, vm, done
	}
	Sopp(words, 1);
	return words;
}

TEST(EmulatorFragmentMaskFlow, AdmitsTheNullTargetKillPassWithItsInertTail)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = ParsePixel(AsSlots(NullTargetKillProgram(false)));
		Check(Find(code, ShaderInstructionType::Exp) < code.GetInstructions().Size(), "the program has its exports");
		Check(code.GetInstructions().At(Find(code, ShaderInstructionType::Exp)).format == ShaderInstructionFormat::NullVmDone,
		      "the valid-mask export decodes as the dedicated null target");
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
		Check(flow.lane_local, "survivors exported as EXEC and an empty-mask tail are lane local");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		// A tail that exports colour is not inert: skipping to it changes what the wave writes.
		const auto exporting = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AsSlots(NullTargetKillProgram(true))));
		Check(!exporting.lane_local && Contains(exporting.reason, "SCC branch on a mask"), "a tail with a colour export is observable");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, TheDiscardPatternNeedsItsInertTailItsNarrowingAndNarrowSurvivors)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto refused = [](const Words& words, const char* reason, const char* why) {
			const auto code = ParsePixel(AsSlots(words));
			const auto flow = ShaderAnalyzeFragmentMaskFlow(code);
			Check(!flow.lane_local && Contains(flow.reason, reason), why);
			ExpectNotLaneLocal(code, why);
		};
		refused(DiscardProgram(false, true, false), "SCC branch on a mask", "an early-out to a tail that exports is not unobservable");
		refused(DiscardProgram(true, false, false), "helper lanes", "exporting while widened");
		refused(DiscardProgram(true, true, true), "SCC branch on a mask", "survivors that may include helpers do not prove an empty wave");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// A kill inside a divergent block: lanes selected by a per-lane compare test a second condition and drop out of
// the live mask, ending in the inert tail once none survives. `kill_op` is the SOP2 opcode combining the live mask
// with that compare: S_ANDN2_B64 (0x15) clears only lanes inside the block, S_AND_B64 (0x0f) clears every other lane.
Words DivergentKillProgram(uint32_t kill_op)
{
	Words words;
	Sop1(words, 4, 12, kExecLo);                       // s_mov_b64 s[12:13], exec (live lanes)
	Sop1(words, 4, 14, kExecLo);                       // s_mov_b64 s[14:15], exec
	Append(words, Vopc(0xd5, Inline(0), 0u));          // v_cmpx_ne_u32 0, v0: the block's lanes
	Sopp(words, 8, 3);                                 // s_cbranch_execz over the block
	Append(words, Vopc(0xc5, Inline(1), 1u));          // v_cmp_ne_u32 vcc, 1, v1 (kill condition, block lanes only)
	Sop2(words, kill_op, 12, 12, kVccLo);              // s_andn2_b64 / s_and_b64 s[12:13], s[12:13], vcc
	Sopp(words, 4, 6);                                 // s_cbranch_scc0 to the inert tail
	Sop1(words, 4, kExecLo, 14);                       // join: s_mov_b64 exec, s[14:15]
	Vop1(words, 1, 2, Inline(0));
	Sop1(words, 4, kExecLo, 12);                       // s_mov_b64 exec, s[12:13]: the survivors export
	words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(words, 1);
	Sop1(words, 4, kExecLo, Inline(0));                // s_mov_b64 exec, 0
	words.insert(words.end(), {0xf8001c00u, 0u});      // exp null mrt0, vm, done
	Sopp(words, 1);
	return words;
}

TEST(EmulatorFragmentMaskFlow, AKillInsideADivergentBlockKeepsEveryOtherLanesLiveBit)
{
	ASSERT_EXIT(([] {
		Initialize();
		// The host skips the block for a subgroup with no lane in it, the guest runs it for the wave; ANDN2 with a
		// compare evaluated inside clears no bit outside, so the live mask is the same per lane either way.
		const auto kept = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AsSlots(DivergentKillProgram(0x15))));
		Check(kept.lane_local, "a kill confined to the block's lanes");
		// AND with the same compare clears every lane outside the block when the block runs: a diverged mask.
		const auto cleared = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AsSlots(DivergentKillProgram(0x0f))));
		Check(!cleared.lane_local, "a mask the block may clear outside its lanes is diverged after the join");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, DerivativeFetchesAreAdmittedInsideQuadUniformBlocks)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto outside = ParsePixel(DivergentProgram({}));
		auto inside  = ParsePixel(DivergentProgram({}));
		ShaderInstruction sample {};
		sample.type = ShaderInstructionType::ImageSample;
		sample.pc = inside.GetInstructions().At(Find(inside, ShaderInstructionType::VMovB32)).pc + 1u;
		inside.GetInstructions().InsertAt(Find(inside, ShaderInstructionType::VMovB32), sample);
		// The EXEC branch around the block is emitted quad-uniform, so the fetch sees its whole quad.
		Check(ShaderAnalyzeFragmentMaskFlow(inside).lane_local, "implicit derivatives inside a quad-uniform block");
		sample.pc = outside.GetInstructions().At(Find(outside, ShaderInstructionType::Exp)).pc + 1u;
		outside.GetInstructions().InsertAt(Find(outside, ShaderInstructionType::Exp), sample);
		Check(ShaderAnalyzeFragmentMaskFlow(outside).lane_local, "the same fetch outside any divergent block is fine");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, PixelExecBranchesTakeTheVoteOfARealFragmentOfTheQuad)
{
	ASSERT_EXIT(([] {
		Initialize();
		// s_mov_b64 s[4:5], exec; v_cmp_ne_u32 vcc, 0, v0; s_and_saveexec_b64 s[2:3], vcc; s_cbranch_execz over one
		// dword; v_mov_b32 v1, 0; s_mov_b64 exec, s[2:3]; v_mov_b32 v2, 0; export; s_endpgm.
		Words words;
		Sop1(words, 4, 4, kExecLo);
		Append(words, Vopc(0xc5, Inline(0), 0u));
		Sop1(words, 0x24, 2, kVccLo);
		Sopp(words, 8, 1);
		Vop1(words, 1, 1, Inline(0));
		Sop1(words, 4, kExecLo, 2);
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
		const auto           code = ParsePixel(std::vector<Words> {words});
		ShaderPixelInputInfo input {};
		const auto           source = SpirvGenerateSource(code, nullptr, &input, nullptr);
		Check(!source.IsEmpty(), "complete module");
		// The packed-EXEC vote goes to cc_vote; every lane, helpers included, then reads cc_any from a real
		// fragment of its quad, so the quad takes the branch as one.
		Check(source.FindIndex("%cc_vote_") != Core::STRING8_INVALID_INDEX &&
		          source.FindIndex("_helper = OpLoad %bool %gl_HelperInvocation") != Core::STRING8_INVALID_INDEX &&
		          source.FindIndex("OpGroupNonUniformQuadBroadcast %bool %uint_3 %cc_vote_") != Core::STRING8_INVALID_INDEX,
		      "quad-uniform branch vote");
		Vector<uint32_t> binary;
		String8          error;
		Check(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty(), error.c_str());
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

UT_END();
