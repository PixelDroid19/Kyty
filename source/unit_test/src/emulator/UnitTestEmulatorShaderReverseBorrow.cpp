#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

UT_BEGIN(EmulatorShaderReverseBorrow);

using namespace Libs::Graphics;

namespace {

void CaptureRejectionDiagnostic()
{
	// Core EXIT writes to stdout; gtest matches the death-test child's stderr.
	std::fflush(stdout);
#if defined(_WIN32)
	const int redirected = ::_dup2(::_fileno(stderr), ::_fileno(stdout));
#else
	const int redirected = ::dup2(::fileno(stderr), ::fileno(stdout));
#endif
	if (redirected < 0)
	{
		std::perror("reverse-borrow fixture: redirect diagnostic");
		std::_Exit(6);
	}
}

std::string Instructions(const String8& source)
{
	std::istringstream stream(source.c_str());
	std::string result = "\n";
	for (std::string line; std::getline(stream, line);)
	{
		std::istringstream words(line.substr(0, line.find(';')));
		std::string instruction;
		for (std::string word; words >> word;)
		{
			if (!instruction.empty()) { instruction += ' '; }
			instruction += word;
		}
		if (!instruction.empty()) { result += instruction + '\n'; }
	}
	return result;
}

bool RequireInstructions(const std::string& source, const String8& expected, size_t* cursor, uint32_t width, uint32_t index)
{
	// Match complete, contiguous instructions in program order. Only layout and
	// comments are ignored; operand identities, types and dataflow stay exact.
	const auto block = Instructions(expected);
	const auto found = source.find(block, *cursor);
	if (found == std::string::npos)
	{
		std::fprintf(stderr, "reverse-borrow wave%u instruction[%u]: missing ordered instruction block:%s", width, index, block.c_str());
		return false;
	}
	*cursor = found + block.size() - 1;
	return true;
}

bool CheckPackedReverseBorrow(const String8& assembly, uint32_t width)
{
	const auto source = Instructions(assembly);
	size_t cursor = 0;
	for (uint32_t index = 0; index < 3; ++index)
	{
		const bool compare = index == 1;
		const char* low = compare ? "s4" : (index == 0 ? "vcc_lo" : "s8");
		const char* high = compare ? "s5" : (index == 0 ? "vcc_hi" : "s9");
		String8 expected;
		if (compare)
		{
			expected = R"(
%tt0_1 = OpLoad %float %v0
%t0_1 = OpBitcast %uint %tt0_1
%tt1_1 = OpLoad %float %v1
%t1_1 = OpBitcast %uint %tt1_1
%t2_1 = OpULessThan %bool %t0_1 %t1_1
%t3_1 = OpSelect %uint %t2_1 %uint_1 %uint_0
)";
		} else
		{
			expected = R"(
%t0_<i> = OpBitcast %uint %int_<constant>
%tt1_<i> = OpLoad %float %<data>
%t1_<i> = OpBitcast %uint %tt1_<i>
%t2_<i>_lane = OpLoad %uint %gl_SubgroupInvocationID
%t2_<i>_shift = OpBitwiseAnd %uint %t2_<i>_lane %uint_31
%t2_<i>_lo = OpLoad %uint %<mask_low>
)";
			expected += width == 64 ? R"(
%t2_<i>_hi = OpLoad %uint %<mask_high>
%t2_<i>_upper = OpUGreaterThanEqual %bool %t2_<i>_lane %uint_32
%t2_<i>_word = OpSelect %uint %t2_<i>_upper %t2_<i>_hi %t2_<i>_lo
)" : R"(
%t2_<i>_word = OpCopyObject %uint %t2_<i>_lo
)";
			expected += R"(
%t2_<i>_shifted = OpShiftRightLogical %uint %t2_<i>_word %t2_<i>_shift
%t2_<i> = OpBitwiseAnd %uint %t2_<i>_shifted %uint_1
%subrev_carry_nonzero_<i> = OpINotEqual %bool %t2_<i> %uint_0
%subrev_carry_<i> = OpSelect %uint %subrev_carry_nonzero_<i> %uint_1 %uint_0
%subrev_base_<i> = OpISubBorrow %ResTypeU %t1_<i> %t0_<i>
%subrev_value_<i> = OpCompositeExtract %uint %subrev_base_<i> 0
%subrev_base_borrow_<i> = OpCompositeExtract %uint %subrev_base_<i> 1
%subrev_adjusted_<i> = OpISubBorrow %ResTypeU %subrev_value_<i> %subrev_carry_<i>
%subrev_result_<i> = OpCompositeExtract %uint %subrev_adjusted_<i> 0
%subrev_adjusted_borrow_<i> = OpCompositeExtract %uint %subrev_adjusted_<i> 1
%subrev_borrow_<i> = OpBitwiseOr %uint %subrev_base_borrow_<i> %subrev_adjusted_borrow_<i>
%t_<i> = OpCompositeConstruct %v2uint %subrev_result_<i> %subrev_borrow_<i>
%t208_<i> = OpCompositeExtract %uint %t_<i> 1
%t209_<i> = OpCompositeExtract %uint %t_<i> 0
%t210_<i> = OpBitcast %float %t209_<i>
%exec_lo_u_<i> = OpLoad %uint %exec_lane_lo
%exec_hi_u_<i> = OpLoad %uint %exec_lane_hi
%exec_lo_b_<i> = OpINotEqual %bool %exec_lo_u_<i> %uint_0
%subrev_old_dst_<i> = OpLoad %float %<dst>
%subrev_dst_<i> = OpSelect %float %exec_lo_b_<i> %t210_<i> %subrev_old_dst_<i>
OpStore %<dst> %subrev_dst_<i>
%t213_<i> = OpSelect %uint %exec_lo_b_<i> %t208_<i> %uint_0
)";
		}
		// Both the comparison producer and the borrow outputs are architectural
		// packed words. Inactive EXEC lanes contribute zero bits to the ballot.
		expected += R"(
%mask_out_exec_<i> = OpLoad %uint %exec_lane_lo
%mask_out_live_<i> = OpINotEqual %bool %mask_out_exec_<i> %uint_0
%mask_out_pred_<i> = OpINotEqual %bool %<predicate>_<i> %uint_0
%mask_out_active_<i> = OpLogicalAnd %bool %mask_out_live_<i> %mask_out_pred_<i>
%mask_out_ballot_<i> = OpGroupNonUniformBallot %v4uint %uint_3 %mask_out_active_<i>
%mask_out_lo_<i> = OpCompositeExtract %uint %mask_out_ballot_<i> 0
%mask_out_hi_<i> = OpCompositeExtract %uint %mask_out_ballot_<i> 1
OpStore %<low> %mask_out_lo_<i>
)";
		if (width == 64) { expected += "OpStore %<high> %mask_out_hi_<i>\n"; }
		expected = expected.ReplaceStr("<constant>", index == 0 ? "0" : "1")
		                       .ReplaceStr("<data>", index == 0 ? "v17" : "v18")
		                       .ReplaceStr("<mask_low>", index == 0 ? "vcc_lo" : "s4")
		                       .ReplaceStr("<mask_high>", index == 0 ? "vcc_hi" : "s5")
		                       .ReplaceStr("<dst>", index == 0 ? "v5" : "v6")
		                       .ReplaceStr("<predicate>", compare ? "t3" : "t213")
		                       .ReplaceStr("<low>", low).ReplaceStr("<high>", high)
		                       .ReplaceStr("<i>", String8::FromPrintf("%u", index));
		if (!RequireInstructions(source, expected, &cursor, width, index)) { return false; }
		// A packed store must not be followed by the former zero-high store;
		// wave32 must leave the adjacent word untouched altogether.
		const std::string store = "\nOpStore %" + std::string(high) + " ";
		size_t stores = 0;
		for (size_t pos = 0; (pos = source.find(store, pos)) != std::string::npos; pos += store.size()) { ++stores; }
		const size_t expected_stores = width == 64 ? 1u : 0u;
		if (stores != expected_stores)
		{
			std::fprintf(stderr, "reverse-borrow wave%u instruction[%u]: %s writes=%zu expected=%zu\n",
			             width, index, high, stores, expected_stores);
			return false;
		}
	}
	return true;
}

std::string ReverseBorrowRejection(uint32_t pc)
{
	return String8::FromPrintf("shader emitter missing: stage=4 instruction=%u format=0x000000020d040517 pc=0x%08x "
	                          "sampled=0/0/0 inst=VSubrevCoCiU32 .*sopp=0xff raw=0xd52a0806",
	                          static_cast<unsigned>(ShaderInstructionType::VSubrevCoCiU32), pc).c_str();
}

} // namespace

TEST(EmulatorShaderReverseBorrow, ParsesAndLowersGen5VSubrevCoCiU32)
{
	// Synthetic VOP2/VOP3B encodings, deliberately assembled from instruction
	// fields rather than copied workload words. Both forms compute
	// src1 - src0 - borrow_in; VOP2 uses the implicit VCC pair and VOP3B uses
	// the explicit scalar pairs below. VOP3B's S4:S5 borrow input is produced
	// by the preceding full-pair V_CMP_LT_U32.
	constexpr uint32_t vop3_cmp_lt_u32 = (0xd4u << 24u) | (0xc1u << 16u) | 4u;
	constexpr uint32_t vop3_cmp_sources = 256u | ((256u + 1u) << 9u);
	constexpr uint32_t vop2_subrev_co_ci = (0x2au << 25u) | (5u << 17u) | (17u << 9u) | 128u;
	constexpr uint32_t vop3_subrev_co_ci = (0xd4u << 24u) | (0x12au << 16u) | (8u << 8u) | 6u;
	constexpr uint32_t vop3_sources      = (129u << 0u) | ((256u + 18u) << 9u) | (4u << 18u);
	constexpr uint32_t s_endpgm          = (0xbf800000u | (0x01u << 16u));
	const uint32_t     shader[]          = {vop2_subrev_co_ci, vop3_cmp_lt_u32, vop3_cmp_sources,
	                                        vop3_subrev_co_ci, vop3_sources, s_endpgm};

	// The missing parser exits before it can emit an instruction on the old
	// behavior. A subprocess keeps that expected RED failure contained while
	// the successful path proves parser shape, generated SPIR-V, and the real
	// toolchain assembly/optimization route together.
	ASSERT_EXIT(
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
			    bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t GetInteger(const Core::String&) const override { return 0; }
			    bool GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation_config;
		    Config::Load(validation_config);

		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);

		    if (code.GetInstructions().Size() != 4u)
		    {
			    std::fprintf(stderr, "reverse-borrow parse: expected 4 instructions, got %u\n%s\n",
			                 code.GetInstructions().Size(), code.DbgDump().c_str());
			    std::_Exit(2);
		    }
		    const auto& vop2 = code.GetInstructions().At(0);
		    const auto& cmp  = code.GetInstructions().At(1);
		    const auto& vop3 = code.GetInstructions().At(2);
		    const bool cmp_contract = cmp.type == ShaderInstructionType::VCmpLtU32 && cmp.pc == 4 &&
		                             cmp.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1 && cmp.src_num == 2 &&
		                             cmp.dst.type == ShaderOperandType::Sgpr && cmp.dst.register_id == 4 && cmp.dst.size == 2 &&
		                             cmp.src[0].type == ShaderOperandType::Vgpr && cmp.src[0].register_id == 0 &&
		                             cmp.src[1].type == ShaderOperandType::Vgpr && cmp.src[1].register_id == 1;
		    const bool vop2_contract =
			    vop2.type == ShaderInstructionType::VSubrevCoCiU32 && vop2.pc == 0 &&
			    vop2.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2 && vop2.src_num == 3 &&
			    vop2.dst.type == ShaderOperandType::Vgpr && vop2.dst.register_id == 5 &&
			    vop2.src[0].type == ShaderOperandType::IntegerInlineConstant && vop2.src[0].constant.i == 0 &&
			    vop2.src[1].type == ShaderOperandType::Vgpr && vop2.src[1].register_id == 17 &&
			    vop2.src[2].type == ShaderOperandType::VccLo && vop2.src[2].size == 2 &&
			    vop2.dst2.type == ShaderOperandType::VccLo && vop2.dst2.size == 2;
		    const bool vop3_contract =
			    vop3.type == ShaderInstructionType::VSubrevCoCiU32 && vop3.pc == 12 &&
			    vop3.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2 && vop3.src_num == 3 &&
			    vop3.dst.type == ShaderOperandType::Vgpr && vop3.dst.register_id == 6 &&
			    vop3.src[0].type == ShaderOperandType::IntegerInlineConstant && vop3.src[0].constant.i == 1 &&
			    vop3.src[1].type == ShaderOperandType::Vgpr && vop3.src[1].register_id == 18 &&
			    vop3.src[2].type == ShaderOperandType::Sgpr && vop3.src[2].register_id == 4 && vop3.src[2].size == 2 &&
			    vop3.dst2.type == ShaderOperandType::Sgpr && vop3.dst2.register_id == 8 && vop3.dst2.size == 2;
		    if (!vop2_contract || !cmp_contract || !vop3_contract || code.GetInstructions().At(3).type != ShaderInstructionType::SEndpgm ||
		        code.GetInstructions().At(3).pc != 20)
		    {
			    std::fprintf(stderr, "reverse-borrow parse: vop2=%d compare=%d vop3=%d; expected endpgm at pc=0x14\n%s\n",
			                 vop2_contract, cmp_contract, vop3_contract, code.DbgDump().c_str());
			    std::_Exit(3);
		    }

		    for (uint32_t width: {64u, 32u})
		    {
			    ShaderComputeInputInfo input {};
			    input.threads_num[0] = width;
			    input.threads_num[1] = input.threads_num[2] = 1;
			    input.wave_layout = (ShaderComputeWaveLayout {ShaderComputeWaveStrategy::Native, {width, 1, 1}, {width, 1, 1},
			                                                 width, width, 1, 1, 0});
			    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
			    const bool source_contract = CheckPackedReverseBorrow(source, width);
			    Vector<uint32_t> binary;
			    Core::String8 error;
			    const bool toolchain_contract = ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty();
			    if (!source_contract || !toolchain_contract)
			    {
				    std::fprintf(stderr, "reverse-borrow wave%u: assembly=%d toolchain=%d\n%s\n%s\n",
				                 width, source_contract, toolchain_contract, error.c_str(), source.c_str());
				    std::_Exit(4);
			    }
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderReverseBorrow, RejectsUnprovenScalarBorrowMask)
{
	// A scalar copy lacks the comparison-producer proof required by the current
	// reverse-borrow admission policy, even with packed architectural storage.
	constexpr uint32_t s_mov_b64_s4_s2 = (0x17du << 23u) | (4u << 16u) | (0x04u << 8u) | 2u;
	constexpr uint32_t vop3_subrev_co_ci = (0xd4u << 24u) | (0x12au << 16u) | (8u << 8u) | 6u;
	constexpr uint32_t vop3_sources      = (129u << 0u) | ((256u + 18u) << 9u) | (4u << 18u);
	constexpr uint32_t s_endpgm          = (0xbf800000u | (0x01u << 16u));
	const uint32_t     shader[]          = {s_mov_b64_s4_s2, vop3_subrev_co_ci, vop3_sources, s_endpgm};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	ASSERT_EXIT(
	    {
		    CaptureRejectionDiagnostic();
		    if (!Config::IsInitialized())
		    {
			    Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    if (code.GetInstructions().Size() != 3u || code.GetInstructions().At(0).type != ShaderInstructionType::SMovB64 ||
		        code.GetInstructions().At(1).type != ShaderInstructionType::VSubrevCoCiU32 ||
		        code.GetInstructions().At(1).src[2].type != ShaderOperandType::Sgpr ||
		        code.GetInstructions().At(1).src[2].register_id != 4 || code.GetInstructions().At(1).src[2].size != 2)
		    {
			    std::fprintf(stderr, "unproven borrow-mask parse: expected s_mov_b64 followed by reverse-borrow from s[4:5]\n%s\n",
			                 code.DbgDump().c_str());
			    std::_Exit(2);
		    }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    (void)SpirvGenerateSource(code, nullptr, nullptr, &input);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(rejected_exit), ReverseBorrowRejection(4));
}

TEST(EmulatorShaderReverseBorrow, RejectsUnimplementedReverseBorrowModifiers)
{
	// Parsing a neutral instruction first isolates emitter modifier rejection
	// from the previously unsupported opcode. Its VCC source is the explicit
	// backend mask contract; each child changes one modifier.
	const uint32_t shader[] = {(0xd4u << 24u) | (0x12au << 16u) | (8u << 8u) | 6u,
	                           2u | ((256u + 18u) << 9u) | (106u << 18u), 0xbf810000u};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	for (uint32_t modifier = 0; modifier < 8; ++modifier)
	{
		SCOPED_TRACE(::testing::Message() << "reverse-borrow modifier=" << modifier);
		ASSERT_EXIT(
		    {
			    CaptureRejectionDiagnostic();
			    if (!Config::IsInitialized())
			    {
				    Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    }
			    Config::SetNextGen(true);
			    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    ShaderParse(shader, &code);
			    if (code.GetInstructions().Size() != 2u || code.GetInstructions().At(0).type != ShaderInstructionType::VSubrevCoCiU32 ||
			        code.GetInstructions().At(0).pc != 0)
			    {
				    std::fprintf(stderr, "reverse-borrow modifier=%u: unexpected parsed program\n%s\n", modifier, code.DbgDump().c_str());
				    std::_Exit(2);
			    }
			    auto& inst = code.GetInstructions()[0];
			    switch (modifier)
			    {
				    case 0: inst.dst.clamp = true; break;
				    case 1: inst.dst.multiplier = 2.0f; break;
				    case 2: inst.src[0].negate = true; break;
				    case 3: inst.src[0].absolute = true; break;
				    case 4: inst.src[1].negate = true; break;
				    case 5: inst.src[1].absolute = true; break;
				    case 6: inst.src[2].negate = true; break;
				    case 7: inst.src[2].absolute = true; break;
			    }
			    ShaderComputeInputInfo input {};
			    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
			    (void)SpirvGenerateSource(code, nullptr, nullptr, &input);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(rejected_exit), ReverseBorrowRejection(0));
	}
}

TEST(EmulatorShaderReverseBorrow, RejectsUnsupportedReverseBorrowSdwaSelections)
{
	constexpr uint32_t instruction = (0x2au << 25u) | (5u << 17u) | (17u << 9u) | 249u;
	constexpr uint32_t neutral = 1u | (6u << 8u) | (6u << 16u) | (6u << 24u);
	const uint32_t controls[] = {neutral & ~(7u << 8u), neutral | (1u << 11u), neutral | (1u << 19u),
	                             neutral | (1u << 27u), neutral | (1u << 16u), neutral | (1u << 24u)};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	for (const auto control: controls)
	{
		SCOPED_TRACE(String8::FromPrintf("reverse-borrow SDWA control=0x%08x", control).c_str());
		const uint32_t shader[] = {instruction, control, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    CaptureRejectionDiagnostic();
			    if (!Config::IsInitialized())
			    {
				    Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    }
			    Config::SetNextGen(true);
			    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    ShaderParse(shader, &code);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(rejected_exit), "unsupported SDWA reverse-borrow selection");
	}
}

UT_END();
