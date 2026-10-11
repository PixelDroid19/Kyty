#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdint>
#include <string>

UT_BEGIN(EmulatorGdsExec);

using namespace Libs::Graphics;

namespace {

void InitializeShaderEnvironment()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

uint32_t Sop1Move(uint32_t destination, uint32_t source)
{
	return (0x17du << 23u) | (destination << 16u) | (0x03u << 8u) | source;
}

uint32_t Vop1Move(uint32_t destination, uint32_t source)
{
	return 0x7e000000u | (destination << 17u) | (0x01u << 9u) | source;
}

bool BuildOrdinaryGdsProgram(uint32_t ds_opcode, ShaderCode* code, ShaderComputeInputInfo* input)
{
	if (code == nullptr || input == nullptr)
	{
		return false;
	}

	// v2 starts with a lane-distinct value, then CMPX leaves only lane 1 in EXEC.
	// The DS instruction therefore exercises one active and two inactive lanes.
	const std::array<uint32_t, 8> words {
	    Sop1Move(14u, 14u),                               // s_mov_b32 s14, s14 materializes the bound GDS pointer SGPR.
	    Sop1Move(124u, 128u),                             // s_mov_b32 m0, 0
	    Vop1Move(2u, 256u),                               // v_mov_b32 v2, v0
	    Vop1Move(1u, 129u),                               // v_mov_b32 v1, 1
	    0x7c000000u | (0xd2u << 17u) | (1u << 9u) | 256u, // v_cmpx_eq_u32 vcc, v0, v1
	    0xd8000000u | (ds_opcode << 18u) | (1u << 17u),   // ds_append/ds_consume v2, gds
	    2u << 24u,
	    0xbf810000u, // s_endpgm
	};
	code->SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), code))
	{
		return false;
	}

	input->dispatch_mode                    = 0x8041u;
	input->threads_num[0]                   = 32u;
	input->threads_num[1]                   = 1u;
	input->threads_num[2]                   = 1u;
	input->thread_ids_num                   = 1;
	input->wave_layout.strategy             = ShaderComputeWaveStrategy::Native;
	input->wave_layout.guest_wave_size      = 32u;
	input->wave_layout.native_subgroup_size = 32u;
	input->wave_layout.guest_local[0] = input->wave_layout.physical_local[0] = 32u;
	input->wave_layout.guest_local[1] = input->wave_layout.physical_local[1] = 1u;
	input->wave_layout.guest_local[2] = input->wave_layout.physical_local[2] = 1u;
	input->native_wave_sensitive                                             = ShaderUsesNativeWaveState(*code);
	input->required_subgroup_size                                            = input->native_wave_sensitive ? 32u : 0u;
	input->bind.gds_pointers.pointers_num                                    = 1;
	input->bind.gds_pointers.start_register[0]                               = 14;
	ShaderCalcBindingIndices(&input->bind);
	return input->bind.gds_pointers.binding_index == 0 && input->bind.push_constant_size == 16u;
}

std::string GenerateOrdinaryGdsSource(uint32_t opcode)
{
	ShaderCode             code;
	ShaderComputeInputInfo input {};
	if (!BuildOrdinaryGdsProgram(opcode, &code, &input))
	{
		return {};
	}
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	return {source.GetDataConst(), source.Size()};
}

void ExpectCountedAndBroadcastOldValue(const std::string& source, const char* atomic_opcode)
{
	const auto atomic = source.find(atomic_opcode);
	ASSERT_NE(atomic, std::string::npos);
	EXPECT_EQ(source.find(atomic_opcode, atomic + 1u), std::string::npos);

	const auto exec_lo        = source.find("%gds_exec_", 0);
	const auto exec_hi        = source.find("%gds_exec_", exec_lo + 1u);
	const auto find_exec_load = [&source](const char* word, size_t after)
	{
		const auto packed = source.find(std::string("OpLoad %uint %packed_exec_") + word, after);
		return packed != std::string::npos ? packed : source.find(std::string("OpLoad %uint %exec_") + word, after);
	};
	const auto low_mask        = find_exec_load("lo", exec_lo);
	const auto high_mask       = find_exec_load("hi", exec_hi);
	const auto low_count       = source.find("OpBitCount %uint %gds_exec_", low_mask);
	const auto high_count      = source.find("OpBitCount %uint %gds_exec_", high_mask);
	const auto elect           = source.find("OpGroupNonUniformElect %bool %uint_3");
	const auto active_guard    = source.find("_has_active = OpINotEqual %bool %gds_exec_", low_count);
	const auto atomic_guard    = source.find("OpLogicalAnd %bool %gds_exec_", elect);
	const auto conditional     = source.find("OpBranchConditional %gds_exec_", atomic_guard);
	const auto atomic_body     = source.find("_atomic_do = OpLabel", conditional);
	const auto counter_pointer = source.find("_m0 = OpLoad %uint %m0", atomic_body);
	const auto atomic_op       = source.find(atomic_opcode, counter_pointer);
	const auto atomic_merge    = source.find("_atomic_merge = OpLabel", atomic_op);
	const auto atomic_result   = source.find("OpPhi %uint %gds_exec_", atomic_merge);
	const auto broadcast       = source.find("OpGroupNonUniformBroadcastFirst %uint %uint_3");
	const auto lane_active     = source.find("OpLoad %uint %exec_lane_lo", broadcast);
	const auto active_test     = source.find("OpINotEqual %bool %gds_exec_", lane_active);
	const auto previous_bits   = source.find("_previous_bits = OpBitcast %uint", lane_active);
	const auto select          = source.find("OpSelect %uint %gds_exec_", active_test);
	const auto writeback       = source.find("OpStore %v2", select);
	ASSERT_NE(exec_lo, std::string::npos);
	ASSERT_NE(exec_hi, std::string::npos);
	ASSERT_NE(low_mask, std::string::npos);
	ASSERT_NE(high_mask, std::string::npos);
	ASSERT_NE(low_count, std::string::npos);
	ASSERT_NE(high_count, std::string::npos);
	ASSERT_NE(elect, std::string::npos);
	ASSERT_NE(active_guard, std::string::npos);
	ASSERT_NE(atomic_guard, std::string::npos);
	ASSERT_NE(conditional, std::string::npos);
	ASSERT_NE(atomic_body, std::string::npos);
	ASSERT_NE(counter_pointer, std::string::npos);
	ASSERT_NE(atomic_op, std::string::npos);
	ASSERT_NE(atomic_merge, std::string::npos);
	ASSERT_NE(atomic_result, std::string::npos);
	ASSERT_NE(broadcast, std::string::npos);
	ASSERT_NE(lane_active, std::string::npos);
	ASSERT_NE(active_test, std::string::npos);
	ASSERT_NE(previous_bits, std::string::npos);
	ASSERT_NE(select, std::string::npos);
	ASSERT_NE(writeback, std::string::npos);
	EXPECT_LT(exec_lo, low_count);
	EXPECT_LT(exec_hi, high_count);
	EXPECT_LT(low_count, atomic);
	EXPECT_LT(high_count, atomic);
	EXPECT_LT(elect, active_guard);
	EXPECT_LT(active_guard, atomic_guard);
	EXPECT_LT(atomic_guard, conditional);
	EXPECT_LT(conditional, atomic_body);
	EXPECT_LT(atomic_body, counter_pointer);
	EXPECT_LT(counter_pointer, atomic_op);
	EXPECT_LT(atomic_op, atomic_merge);
	EXPECT_LT(atomic_merge, atomic_result);
	EXPECT_EQ(atomic, atomic_op);
	EXPECT_LT(atomic, broadcast);
	EXPECT_LT(broadcast, lane_active);
	EXPECT_LT(lane_active, active_test);
	EXPECT_LT(active_test, previous_bits);
	EXPECT_LT(previous_bits, select);
	EXPECT_LT(select, writeback);
}

} // namespace

TEST(EmulatorGdsExec, OrdinaryAppendGuardsCounterAndReturnWritebackWithExec)
{
	InitializeShaderEnvironment();
	const auto source = GenerateOrdinaryGdsSource(0x3eu);
	ASSERT_FALSE(source.empty());
	ExpectCountedAndBroadcastOldValue(source, "OpAtomicIAdd");
}

TEST(EmulatorGdsExec, OrdinaryConsumeGuardsCounterAndReturnWritebackWithExec)
{
	InitializeShaderEnvironment();
	const auto source = GenerateOrdinaryGdsSource(0x3du);
	ASSERT_FALSE(source.empty());
	ExpectCountedAndBroadcastOldValue(source, "OpAtomicISub");
}

UT_END();
