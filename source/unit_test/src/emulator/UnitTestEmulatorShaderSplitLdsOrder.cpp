#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSplitLdsOrder.h"

#include <algorithm>

UT_BEGIN(EmulatorShaderSplitLdsOrder);

using namespace Libs::Graphics;

namespace {

constexpr uint32_t kEndpgm      = 0xbf810000u;
constexpr uint32_t kBarrier     = 0xbf8a0000u;
constexpr uint32_t kWaitcntDep  = 0xbfa30000u;
constexpr uint32_t kOneLane[3]  = {64u, 1u, 1u};
// v_lshlrev_b32 v3, 2, v0: four bytes per thread along x.
constexpr uint32_t kSlotAddress = (0x1au << 25u) | (3u << 17u) | (0u << 9u) | 130u;
// v_mov_b32 v3, 0.
constexpr uint32_t kZeroAddress = (0x3fu << 25u) | (3u << 17u) | (1u << 9u) | 128u;
// v_mul_u32_u24 v3, 8, v0: whatever v0 holds, every lane's address is a multiple of 8.
constexpr uint32_t kTimesEight  = (0x0bu << 25u) | (3u << 17u) | (0u << 9u) | 136u;
// v_mul_u32_u24 v3, literal, v0.
constexpr uint32_t kTimesLiteral = (0x0bu << 25u) | (3u << 17u) | (0u << 9u) | 255u;
// v_mov_b32 v3, v0.
constexpr uint32_t kCopyAddress = (0x3fu << 25u) | (3u << 17u) | (1u << 9u) | 256u;
// s_mov_b64 exec, -1.
constexpr uint32_t kEveryLane   = (0x17du << 23u) | (126u << 16u) | (4u << 8u) | 193u;
// s_cbranch_scc1 to the instruction `words` before the one after the branch.
constexpr uint32_t BranchBack(uint32_t words)
{
	return (0x17fu << 23u) | (5u << 16u) | ((0x10000u - words) & 0xffffu);
}
// ds_write_b32 v<address>, v1, offset.
constexpr uint32_t WriteWord(uint32_t offset)
{
	return (0x36u << 26u) | (0x0du << 18u) | offset;
}
constexpr uint32_t WriteRegisters(uint32_t address)
{
	return (1u << 8u) | address;
}
// ds_read_b32 v4, v<address>, offset.
constexpr uint32_t ReadWord(uint32_t offset)
{
	return (0x36u << 26u) | (0x36u << 18u) | offset;
}
constexpr uint32_t ReadRegisters(uint32_t address)
{
	return (4u << 24u) | address;
}

ShaderCode ParseShader(const uint32_t* words)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(words, &code);
	return code;
}

size_t Orders(const ShaderSplitLdsOrder& order)
{
	return static_cast<size_t>(std::count(order.before.begin(), order.before.end(), true) +
	                           std::count(order.after.begin(), order.after.end(), true));
}

} // namespace

TEST(EmulatorShaderSplitLdsOrder, EachLanesOwnSlotNeedsNoOrder)
{
	const uint32_t own[]       = {kSlotAddress, WriteWord(0u), WriteRegisters(3u), ReadWord(0u), ReadRegisters(3u), kEndpgm};
	const uint32_t neighbour[] = {kSlotAddress, WriteWord(0u), WriteRegisters(3u), ReadWord(4u), ReadRegisters(3u), kEndpgm};
	const auto     code        = ParseShader(own);
	ASSERT_EQ(code.GetInstructions().Size(), 4u);
	ASSERT_EQ(code.GetInstructions().At(1).type, ShaderInstructionType::DsWriteB32);
	ASSERT_EQ(code.GetInstructions().At(2).type, ShaderInstructionType::DsReadB32);

	EXPECT_EQ(Orders(ShaderSplitLdsOrderOf(code, kOneLane, 1)), 0u);

	// The read of the next thread's slot meets another lane's write.
	const auto crossed = ShaderSplitLdsOrderOf(ParseShader(neighbour), kOneLane, 1);
	EXPECT_EQ(Orders(crossed), 1u);
	EXPECT_TRUE(crossed.before[2]);

	// v0 is no thread coordinate, or the wave has lanes outside the group: the address is not known per lane.
	EXPECT_TRUE(ShaderSplitLdsOrderOf(code, kOneLane, 0).before[2]);
	const uint32_t partial_wave[3] = {48u, 1u, 1u};
	EXPECT_TRUE(ShaderSplitLdsOrderOf(code, partial_wave, 1).before[2]);
}

TEST(EmulatorShaderSplitLdsOrder, SharedBytesAreOrderedOnlyAgainstTheirWriter)
{
	// Write dword 0, read dword 2, read dword 0.
	const uint32_t shader[] = {kZeroAddress, WriteWord(0u), WriteRegisters(3u), ReadWord(8u), ReadRegisters(3u),
	                           ReadWord(0u), ReadRegisters(3u), kEndpgm};
	const auto     order    = ShaderSplitLdsOrderOf(ParseShader(shader), kOneLane, 1);

	ASSERT_EQ(order.before.size(), 5u);
	EXPECT_EQ(Orders(order), 1u);
	EXPECT_TRUE(order.before[3]);
}

TEST(EmulatorShaderSplitLdsOrder, OrderStaysOutOfTheLoopOfTheLaterAccess)
{
	// A write at an unknown address, then a loop that reads another unknown address.
	const uint32_t shader[] = {WriteWord(0u), WriteRegisters(3u), ReadWord(0u), ReadRegisters(5u), BranchBack(3u), kEndpgm};
	const auto     code     = ParseShader(shader);
	ASSERT_EQ(code.GetInstructions().Size(), 4u);
	const auto order = ShaderSplitLdsOrderOf(code, kOneLane, 1);

	EXPECT_EQ(Orders(order), 1u);
	EXPECT_TRUE(order.after[0]);

	// A write in the loop conflicts with itself across iterations.
	const uint32_t writer[] = {WriteWord(0u), WriteRegisters(3u), BranchBack(3u), kEndpgm};
	const auto     looped   = ShaderSplitLdsOrderOf(ParseShader(writer), kOneLane, 1);
	EXPECT_EQ(Orders(looped), 1u);
	EXPECT_TRUE(looped.before[0]);
}

TEST(EmulatorShaderSplitLdsOrder, BarrierOrdersTheAccessesAcrossIt)
{
	const uint32_t barrier[] = {WriteWord(0u), WriteRegisters(3u), kBarrier, ReadWord(0u), ReadRegisters(5u), kEndpgm};
	// s_waitcnt_depctr parses as a barrier record and orders nothing.
	const uint32_t hint[]    = {WriteWord(0u), WriteRegisters(3u), kWaitcntDep, ReadWord(0u), ReadRegisters(5u), kEndpgm};

	EXPECT_EQ(Orders(ShaderSplitLdsOrderOf(ParseShader(barrier), kOneLane, 1)), 0u);
	EXPECT_EQ(Orders(ShaderSplitLdsOrderOf(ParseShader(hint), kOneLane, 1)), 1u);
}

TEST(EmulatorShaderSplitLdsOrder, DisjointFieldsOfOneSlotNeedNoOrder)
{
	const uint32_t fields[] = {kTimesEight, WriteWord(0u), WriteRegisters(3u), WriteWord(4u), WriteRegisters(3u), kEndpgm};
	const auto     code     = ParseShader(fields);
	ASSERT_EQ(code.GetInstructions().Size(), 4u);
	ASSERT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::VMulU32U24);
	EXPECT_EQ(Orders(ShaderSplitLdsOrderOf(code, kOneLane, 0)), 0u);

	// The same field, or fields wider than the slot, may meet another lane's.
	const uint32_t same[] = {kTimesEight, WriteWord(0u), WriteRegisters(3u), WriteWord(0u), WriteRegisters(3u), kEndpgm};
	const uint32_t wide[] = {kTimesEight, WriteWord(0u), WriteRegisters(3u), WriteWord(8u), WriteRegisters(3u), kEndpgm};
	EXPECT_EQ(Orders(ShaderSplitLdsOrderOf(ParseShader(same), kOneLane, 0)), 1u);
	EXPECT_EQ(Orders(ShaderSplitLdsOrderOf(ParseShader(wide), kOneLane, 0)), 1u);

	// The address rewritten, or more lanes enabled, after the multiply: the later access's address is unknown.
	const uint32_t copied[]  = {kTimesEight, WriteWord(0u), WriteRegisters(3u), kCopyAddress, WriteWord(4u), WriteRegisters(3u), kEndpgm};
	const uint32_t widened[] = {kTimesEight, WriteWord(0u), WriteRegisters(3u), kEveryLane, WriteWord(4u), WriteRegisters(3u), kEndpgm};
	EXPECT_TRUE(ShaderSplitLdsOrderOf(ParseShader(copied), kOneLane, 0).before[3]);
	EXPECT_TRUE(ShaderSplitLdsOrderOf(ParseShader(widened), kOneLane, 0).before[3]);
}

TEST(EmulatorShaderSplitLdsOrder, FactorThatMayWrapKeepsItsPowerOfTwo)
{
	// 384 times a 24-bit value may wrap 32 bits: only 128 divides every product.
	const uint32_t inside[] = {kTimesLiteral, 384u, WriteWord(0u), WriteRegisters(3u), WriteWord(124u), WriteRegisters(3u), kEndpgm};
	const uint32_t across[] = {kTimesLiteral, 384u, WriteWord(0u), WriteRegisters(3u), WriteWord(132u), WriteRegisters(3u), kEndpgm};
	const auto     code     = ParseShader(inside);
	ASSERT_EQ(code.GetInstructions().Size(), 4u);
	EXPECT_EQ(Orders(ShaderSplitLdsOrderOf(code, kOneLane, 0)), 0u);
	EXPECT_EQ(Orders(ShaderSplitLdsOrderOf(ParseShader(across), kOneLane, 0)), 1u);
}

UT_END();
