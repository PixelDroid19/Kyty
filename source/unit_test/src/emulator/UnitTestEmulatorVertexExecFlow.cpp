#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderVertexExecFlow.h"
#include "NggPassthroughFixture.h"

#include <cstdint>
#include <vector>

UT_BEGIN(EmulatorVertexExecFlow);

using namespace Libs::Graphics;

namespace {

using namespace NggFixture;

constexpr uint32_t kVcc   = 106;
constexpr uint32_t kExec  = 126;
constexpr uint32_t kVgpr  = 256;

// VOP3 v_writelane_b32 / v_readlane_b32: VDST holds the VGPR (write) or the SGPR (read).
void LaneMove(Words& words, uint32_t opcode, uint32_t vdst, uint32_t src0, uint32_t src1)
{
	words.push_back(0xd4000000u | (opcode << 16u) | vdst);
	words.push_back(src0 | (src1 << 9u));
}

Words Front()
{
	Words words;
	Sop2(words, 3, kVcc, Inline(0), 3);       // s_sub_i32 vcc_lo, 0, s3
	Sop2(words, 33, kExec, Inline(-1), kVcc); // s_lshr_b64 exec, -1, vcc_lo
	return words;
}

bool FullAtEnd(const Words& words)
{
	const auto code = Parse(words);
	const auto full = ShaderAnalyzeVertexExecFull(code, false);
	return !full.empty() && full[code.GetInstructions().Size() - 1u];
}

void Narrow(Words& words)
{
	Sop2(words, 15, kExec, kExec, 30); // s_and_b64 exec, exec, s[30:31]
}

} // namespace

TEST(EmulatorVertexExecFlow, FrontLaneCountsFillExec)
{
	Initialize();
	auto words = Front();
	Sopp(words, 1);
	EXPECT_TRUE(FullAtEnd(words));
	EXPECT_TRUE(FullAtEnd(Fixture()));
}

TEST(EmulatorVertexExecFlow, ShiftByAnotherScalarIsNotFull)
{
	Initialize();
	Words words;
	Sop2(words, 3, kVcc, Inline(0), 5);       // s_sub_i32 vcc_lo, 0, s5
	Sop2(words, 33, kExec, Inline(-1), kVcc); // s_lshr_b64 exec, -1, vcc_lo
	Sopp(words, 1);
	EXPECT_FALSE(FullAtEnd(words));

	Words rewritten;
	Sop1(rewritten, 3, 3, 5); // s_mov_b32 s3, s5
	Sop2(rewritten, 3, kVcc, Inline(0), 3);
	Sop2(rewritten, 33, kExec, Inline(-1), kVcc);
	Sopp(rewritten, 1);
	EXPECT_FALSE(FullAtEnd(rewritten));
}

TEST(EmulatorVertexExecFlow, RestoredSaveIsFullOnlyWhileItsWordsSurvive)
{
	Initialize();
	auto saved = Front();
	Sop1(saved, 4, 20, kExec); // s_mov_b64 s[20:21], exec
	Narrow(saved);
	Sopp(saved, 1);
	EXPECT_FALSE(FullAtEnd(saved));

	auto restored = Front();
	Sop1(restored, 4, 20, kExec);
	Narrow(restored);
	Sop1(restored, 4, kExec, 20); // s_mov_b64 exec, s[20:21]
	Sopp(restored, 1);
	EXPECT_TRUE(FullAtEnd(restored));

	auto overwritten = Front();
	Sop1(overwritten, 4, 20, kExec);
	Narrow(overwritten);
	Sop1(overwritten, 3, 21, 5); // s_mov_b32 s21, s5
	Sop1(overwritten, 4, kExec, 20);
	Sopp(overwritten, 1);
	EXPECT_FALSE(FullAtEnd(overwritten));

	auto saved_narrow = Front();
	Narrow(saved_narrow);
	Sop1(saved_narrow, 4, 20, kExec);
	Sop1(saved_narrow, 4, kExec, 20);
	Sopp(saved_narrow, 1);
	EXPECT_FALSE(FullAtEnd(saved_narrow));
}

TEST(EmulatorVertexExecFlow, SaveexecKeepsTheOldMaskInItsDestination)
{
	Initialize();
	auto words = Front();
	Sop1(words, 36, 20, 30); // s_and_saveexec_b64 s[20:21], s[30:31]
	Sop1(words, 4, kExec, 20);
	Sopp(words, 1);
	EXPECT_TRUE(FullAtEnd(words));
}

TEST(EmulatorVertexExecFlow, SpilledMaskRestoresUnlessTheVgprIsRewritten)
{
	Initialize();
	const auto program = [](bool rewrite) {
		auto words = Front();
		LaneMove(words, 0x361, 3, kExec, Inline(1));     // v_writelane_b32 v3, exec_lo, 1
		LaneMove(words, 0x361, 3, kExec + 1, Inline(2)); // v_writelane_b32 v3, exec_hi, 2
		Narrow(words);
		if (rewrite) { Vop1(words, 1, 3, kVgpr + 5); } // v_mov_b32 v3, v5
		LaneMove(words, 0x360, 40, kVgpr + 3, Inline(1)); // v_readlane_b32 s40, v3, 1
		LaneMove(words, 0x360, 41, kVgpr + 3, Inline(2)); // v_readlane_b32 s41, v3, 2
		Sop1(words, 4, kExec, 40);                        // s_mov_b64 exec, s[40:41]
		Sopp(words, 1);
		return words;
	};
	EXPECT_TRUE(FullAtEnd(program(false)));
	EXPECT_FALSE(FullAtEnd(program(true)));
}

TEST(EmulatorVertexExecFlow, JoinNeedsEveryPathFull)
{
	Initialize();
	auto one_path = Front();
	Sopp(one_path, 4, 1); // s_cbranch_scc0 over the narrowing
	Narrow(one_path);
	Sopp(one_path, 1);
	EXPECT_FALSE(FullAtEnd(one_path));

	auto back_edge = Front();
	Sop1(back_edge, 4, 20, kExec);
	Narrow(back_edge);       // loop header: narrowed on every pass
	Sopp(back_edge, 4, 0xfffe); // s_cbranch_scc0 back to the narrowing
	Sop1(back_edge, 4, kExec, 20);
	Sopp(back_edge, 1);
	EXPECT_TRUE(FullAtEnd(back_edge));
}

UT_END();
