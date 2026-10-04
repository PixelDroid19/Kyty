#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdlib>

UT_BEGIN(EmulatorShaderLaneExec);

using namespace Libs::Graphics;

namespace {

// Architectural EXEC is a numeric packed pair. Vector write gates use derived
// lane predicates; explicit EXEC operands and READFIRSTLANE use the packed pair.
// Value-level producer/consumer controls live in EmulatorShaderMaskValues.
constexpr uint32_t kSEndpgm = 0xbf810000u;

void InitializeLaneExecTest()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool         Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t      GetInteger(const Core::String&) const override { return 0; }
		bool         GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation_config;
	Config::Load(validation_config);
}

String8 GenerateComputeSource(const uint32_t* words, uint32_t expected_instructions)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(words, &code);
	if (code.GetInstructions().Size() != expected_instructions)
	{
		std::_Exit(2);
	}
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = 1;
	input.threads_num[2] = 1;
	input.wave_layout.guest_wave_size = 64;
	input.wave_layout.native_subgroup_size = 64;
	return SpirvGenerateSource(code, nullptr, nullptr, &input);
}

bool Contains(const String8& source, const char* text)
{
	return source.FindIndex(text) != Core::STRING8_INVALID_INDEX;
}

bool Assembles(const String8& source)
{
	Vector<uint32_t> binary;
	Core::String8    error;
	return ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty();
}

// v_mbcnt_lo_u32_b32 v2, <mask>, 0 (VOP3 opcode 869).
constexpr uint32_t MbcntLo(uint32_t mask_operand)
{
	return mask_operand | (128u << 9u);
}

} // namespace

TEST(EmulatorShaderLaneExec, ReadfirstlaneSelectsFirstSetArchitecturalExecBit)
{
	// v_readfirstlane_b32 s0, v1 must find the first packed EXEC bit, including
	// the upper half, with defined FindILsb operands for an empty mask.
	constexpr uint32_t v_readfirstlane = (0x3fu << 25u) | (0u << 17u) | (0x02u << 9u) | (256u + 1u);
	const uint32_t     shader[]        = {v_readfirstlane, kSEndpgm};
	ASSERT_EXIT(
	    {
		    InitializeLaneExecTest();
		    const auto source = GenerateComputeSource(shader, 2u);
		    const bool packed = Contains(source, "%rfl_exec_lo_0 = OpLoad %uint %exec_lo") &&
		                        Contains(source, "%rfl_exec_hi_0 = OpLoad %uint %exec_hi") &&
		                        Contains(source, "FindILsb %rfl_safe_low_0") &&
		                        Contains(source, "FindILsb %rfl_safe_high_0") &&
		                        Contains(source, "%rfl_high_or_zero_0 = OpSelect %uint %rfl_has_high_0 %rfl_high_lane_0 %uint_0") &&
		                        !Contains(source, "%rfl_ballot_0") &&
		                        Contains(source, "OpGroupNonUniformBroadcast %uint %uint_3 %t0_0 %rfl_lane_sel_0");
		    std::_Exit(packed ? (Assembles(source) ? 0 : 4) : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLaneExec, MbcntCountsNumericExecAndGatesWithDerivedLanePredicate)
{
	// v_mbcnt_lo_u32_b32 v2, exec_lo, 0: source bits are numeric; only the
	// destination write gate consumes the derived per-lane predicate.
	constexpr uint32_t vop3_mbcnt_lo = 0xd4000000u | (0x365u << 16u) | 2u;
	const uint32_t     shader[]      = {vop3_mbcnt_lo, MbcntLo(126u), kSEndpgm};
	ASSERT_EXIT(
	    {
		    InitializeLaneExecTest();
		    const auto source = GenerateComputeSource(shader, 2u);
		    const bool numeric =
		        Contains(source, "%mbcnt_mask_0 = OpLoad %uint %exec_lo") &&
		        Contains(source, "%mbcnt_exec_word_lo_0 = OpLoad %uint %exec_lane_lo") &&
		        Contains(source, "%mbcnt_exec_active_0 = OpINotEqual %bool %mbcnt_exec_word_lo_0 %uint_0") &&
		        Contains(source, "%mbcnt_masked_0 = OpBitwiseAnd %uint %mbcnt_mask_0 %mbcnt_prefix_0") &&
		        Contains(source, "%mbcnt_popcount_0 = OpBitCount %uint %mbcnt_masked_0") &&
		        Contains(source, "%mbcnt_value_0 = OpSelect %uint %mbcnt_exec_active_0 %mbcnt_result_0 %mbcnt_old_0") &&
		        !Contains(source, "ExclusiveScan");
		    std::_Exit(numeric ? (Assembles(source) ? 0 : 4) : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLaneExec, MbcntKeepsLiteralOperandAsWaveMask)
{
	// v_mbcnt_lo_u32_b32 v2, -1, 0: an inline constant remains a numeric
	// word intersected with all low-half bits below the current lane.
	constexpr uint32_t vop3_mbcnt_lo = 0xd4000000u | (0x365u << 16u) | 2u;
	const uint32_t     shader[]      = {vop3_mbcnt_lo, MbcntLo(193u), kSEndpgm};
	ASSERT_EXIT(
	    {
		    InitializeLaneExecTest();
		    const auto source = GenerateComputeSource(shader, 2u);
		    const bool wave_mask = Contains(source, "%mbcnt_masked_0 = OpBitwiseAnd %uint %mbcnt_mask_0 %mbcnt_prefix_0") &&
		                           Contains(source, "%mbcnt_popcount_0 = OpBitCount %uint %mbcnt_masked_0") &&
		                           Contains(source, "%mbcnt_exec_word_lo_0 = OpLoad %uint %exec_lane_lo") &&
		                           Contains(source, "%mbcnt_exec_active_0 = OpINotEqual %bool %mbcnt_exec_word_lo_0 %uint_0") &&
		                           !Contains(source, "ExclusiveScan");
		    std::_Exit(wave_mask ? (Assembles(source) ? 0 : 4) : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLaneExec, MbcntHighHalfDefinesLaneBeforeOffset)
{
	// v_mbcnt_hi_u32_b32 v2, exec_hi, 0 (VOP3 opcode 870): the high-half lane
	// offset is derived from the subgroup lane and must follow its definition.
	constexpr uint32_t vop3_mbcnt_hi = 0xd4000000u | (0x366u << 16u) | 2u;
	const uint32_t     shader[]      = {vop3_mbcnt_hi, MbcntLo(127u), kSEndpgm};
	ASSERT_EXIT(
	    {
		    InitializeLaneExecTest();
		    const auto source = GenerateComputeSource(shader, 2u);
		    const auto lane   = source.FindIndex("%mbcnt_lane_0 = OpLoad %uint %gl_SubgroupInvocationID");
		    const auto offset = source.FindIndex("%mbcnt_lane_offset_0 = OpISub %uint %mbcnt_lane_0 %uint_32");
		    const bool ordered = lane != Core::STRING8_INVALID_INDEX && offset != Core::STRING8_INVALID_INDEX && lane < offset;
		    std::_Exit(ordered ? (Assembles(source) ? 0 : 4) : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
