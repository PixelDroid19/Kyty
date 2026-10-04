#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

UT_BEGIN(EmulatorShaderScalarCompare);

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
		std::perror("scalar-compare fixture: redirect diagnostic");
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

bool RequireInstructions(const std::string& source, const String8& expected, size_t* cursor, const char* encoding)
{
	// Complete contiguous instructions, including operand order and types;
	// comments and whitespace are the only normalized parts of the assembly.
	const auto block = Instructions(expected);
	const auto found = source.find(block, *cursor);
	if (found == std::string::npos)
	{
		std::fprintf(stderr, "VCC_HI %s: missing ordered instruction block:%s", encoding, block.c_str());
		return false;
	}
	*cursor = found + block.size() - 1;
	return true;
}

bool CheckVccHiProgram(const ShaderCode& code, bool sdwa)
{
	if (code.GetInstructions().Size() != 3u) { return false; }
	const auto& compare = code.GetInstructions().At(0);
	const auto& select = code.GetInstructions().At(1);
	const auto& end = code.GetInstructions().At(2);
	return compare.type == ShaderInstructionType::VCmpNeU32 && compare.pc == 0 && compare.src_num == 2 &&
	       compare.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1 && compare.dst.type == ShaderOperandType::VccHi &&
	       compare.dst.size == 1 && compare.vop_sdwa == sdwa && (!sdwa || compare.vop_sdwa_ctrl == 0x0686eb80u) &&
	       compare.src[0].type == ShaderOperandType::IntegerInlineConstant && compare.src[0].constant.i == 0 &&
	       compare.src[1].type == ShaderOperandType::Vgpr && compare.src[1].register_id == 0 &&
	       compare.src[0].swizzle == 6u && compare.src[1].swizzle == 6u &&
	       select.type == ShaderInstructionType::VCndmaskB32 && select.pc == 8 && select.src_num == 3 &&
	       select.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2 && select.dst.type == ShaderOperandType::Vgpr &&
	       select.dst.register_id == 0 && select.dst.size == 1 &&
	       select.src[0].type == ShaderOperandType::IntegerInlineConstant && select.src[0].constant.i == 0 &&
	       select.src[1].type == ShaderOperandType::Vgpr && select.src[1].register_id == 1 &&
	       select.src[2].type == ShaderOperandType::VccHi && select.src[2].size == 1 &&
	       end.type == ShaderInstructionType::SEndpgm && end.pc == 16;
}

bool CheckVccHiSource(const String8& assembly, const char* encoding)
{
	const auto source = Instructions(assembly);
	size_t cursor = 0;
	// In wave32, VCC_HI is a one-word mask destination: ballot word zero
	// contains all 32 guest lanes, regardless of the destination register name.
	// Dispatch shares the bitwise NE comparison with the I32 emitter.
	if (!RequireInstructions(source, R"(
%t0_0 = OpBitcast %int %int_0
%tt1_0 = OpLoad %float %v0
%t1_0 = OpBitcast %int %tt1_0
%t2_0 = OpINotEqual %bool %t0_0 %t1_0
%t3_0 = OpSelect %uint %t2_0 %uint_1 %uint_0
%mask_out_exec_0 = OpLoad %uint %exec_lane_lo
%mask_out_live_0 = OpINotEqual %bool %mask_out_exec_0 %uint_0
%mask_out_pred_0 = OpINotEqual %bool %t3_0 %uint_0
%mask_out_active_0 = OpLogicalAnd %bool %mask_out_live_0 %mask_out_pred_0
%mask_out_ballot_0 = OpGroupNonUniformBallot %v4uint %uint_3 %mask_out_active_0
%mask_out_lo_0 = OpCompositeExtract %uint %mask_out_ballot_0 0
%mask_out_hi_0 = OpCompositeExtract %uint %mask_out_ballot_0 1
OpStore %vcc_hi %mask_out_lo_0
)", &cursor, encoding)) { return false; }
	// CNDMASK extracts this invocation's bit, chooses src1 only when set,
	// and preserves the old float-typed VGPR bits for inactive EXEC lanes.
	if (!RequireInstructions(source, R"(
%t0_1 = OpBitcast %float %int_0
%t1_1 = OpLoad %float %v1
%t22_1_lane = OpLoad %uint %gl_SubgroupInvocationID
%t22_1_shift = OpBitwiseAnd %uint %t22_1_lane %uint_31
%t22_1_lo = OpLoad %uint %vcc_hi
%t22_1_word = OpCopyObject %uint %t22_1_lo
%t22_1_shifted = OpShiftRightLogical %uint %t22_1_word %t22_1_shift
%t22_1 = OpBitwiseAnd %uint %t22_1_shifted %uint_1
%tb_1 = OpBitwiseAnd %uint %t22_1 %uint_1
%t2_1 = OpINotEqual %bool %tb_1 %uint_0
%t3_1 = OpSelect %float %t2_1 %t1_1 %t0_1
%exec_lo_u_1 = OpLoad %uint %exec_lane_lo
%exec_hi_u_1 = OpLoad %uint %exec_lane_hi
%exec_lo_b_1 = OpINotEqual %bool %exec_lo_u_1 %uint_0
%tdst_1 = OpLoad %float %v0
%tval_1 = OpSelect %float %exec_lo_b_1 %t3_1 %tdst_1
OpStore %v0 %tval_1
)", &cursor, encoding)) { return false; }
	for (const char* name: {"vcc_lo", "vcc_hi"})
	{
		const std::string store = "\nOpStore %" + std::string(name) + " ";
		size_t stores = 0;
		for (size_t pos = 0; (pos = source.find(store, pos)) != std::string::npos; pos += store.size()) { ++stores; }
		const size_t expected_stores = std::string(name) == "vcc_hi" ? 1u : 0u;
		if (stores != expected_stores)
		{
			std::fprintf(stderr, "VCC_HI %s: %s writes=%zu expected=%zu\n", encoding, name, stores, expected_stores);
			return false;
		}
	}
	return true;
}

} // namespace

TEST(EmulatorShaderScalarCompare, ParsesAndLowersBothWordsOfScalarInequality)
{
	// Compare a scalar pair with zero, then two ordinary scalar pairs.
	const uint32_t shader[] = {0xbf000000u | (0x13u << 16u) | (128u << 8u) | 8u,
	                           0xbf000000u | (0x13u << 16u) | (12u << 8u) | 8u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    class ValidationConfig final: public Config::ConfigSource
		    {
		    public:
			    bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t GetInteger(const Core::String&) const override { return 0; }
			    bool GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation;
		    Config::Load(validation);
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    const auto& inst = code.GetInstructions().At(0);
		    if (inst.src_num != 2 || inst.src[0].size != 2 || inst.src[1].size != 2) { std::_Exit(2); }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    if (source.FindIndex("OpLoad %uint %s9") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpLoad %uint %s13") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpLogicalOr %bool") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpStore %scc %cmp64_result_") == Core::STRING8_INVALID_INDEX)
		    {
			    std::_Exit(3);
		    }
		    Vector<uint32_t> binary;
		    String8 error;
		    if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
		    {
			    std::fprintf(stderr, "%s\n", error.c_str());
			    std::_Exit(4);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderScalarCompare, ParsesAndLowersBothWordsOfScalarEquality)
{
	// Compare an SGPR pair with zero and another ordinary SGPR pair.
	const uint32_t shader[] = {0xbf000000u | (0x12u << 16u) | (128u << 8u) | 8u,
	                           0xbf000000u | (0x12u << 16u) | (12u << 8u) | 8u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    class ValidationConfig final: public Config::ConfigSource
		    {
		    public:
			    bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t GetInteger(const Core::String&) const override { return 0; }
			    bool GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation;
		    Config::Load(validation);
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    const auto& inst = code.GetInstructions().At(0);
		    if (inst.type != ShaderInstructionType::SCmpEqU64 || inst.src_num != 2 || inst.src[0].size != 2 ||
		        inst.src[1].size != 2)
		    {
			    std::_Exit(2);
		    }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    if (source.FindIndex("OpLoad %uint %s9") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpLoad %uint %s13") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpIEqual %bool %cmp64_word0_") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpIEqual %bool %cmp64_word1_") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpLogicalAnd %bool") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpStore %scc %cmp64_result_") == Core::STRING8_INVALID_INDEX)
		    {
			    std::_Exit(3);
		    }
		    Vector<uint32_t> binary;
		    String8 error;
		    if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
		    {
			    std::fprintf(stderr, "%s\n", error.c_str());
			    std::_Exit(4);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderScalarCompare, RejectsPairOutsideOrdinarySgprRange)
{
	const uint32_t shader[] = {0xbf000000u | (0x13u << 16u) | (128u << 8u) | 103u, 0xbf810000u};
	const auto rejection = String8::FromPrintf(
	    "shader emitter missing: stage=4 instruction=%u format=0x0000000000001013 pc=0x00000000 "
	    "reason=lowering-preconditions src_num=2 mimg_address_num=0 sopp=0xff raw=0xbf138067",
	    static_cast<unsigned>(ShaderInstructionType::SCmpLgU64));
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	ASSERT_EXIT(
	    {
		    CaptureRejectionDiagnostic();
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    if (code.GetInstructions().Size() != 2u || code.GetInstructions().At(0).type != ShaderInstructionType::SCmpLgU64 ||
		        code.GetInstructions().At(0).pc != 0 || code.GetInstructions().At(0).src_num != 2 ||
		        code.GetInstructions().At(0).src[0].type != ShaderOperandType::Sgpr ||
		        code.GetInstructions().At(0).src[0].register_id != 103 || code.GetInstructions().At(0).src[0].size != 2)
		    {
			    std::fprintf(stderr, "out-of-range scalar-pair parse: expected s_cmp_lg_u64 s[103:104], 0 at pc=0\n%s\n",
			                 code.DbgDump().c_str());
			    std::_Exit(2);
		    }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    (void)SpirvGenerateSource(code, nullptr, nullptr, &input);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(rejected_exit), rejection.c_str());
}

TEST(EmulatorShaderVectorMask, VccHiCompareFeedsCndmask)
{
	// Assembled for gfx1030: v_cmp_ne_u32_e64 vcc_hi, 0, v0;
	// v_cndmask_b32_e64 v0, 0, v1, vcc_hi.
	const uint32_t shader[] = {0xd4c5006bu, 0x00020080u, 0xd5010000u, 0x01ae0280u, 0xbf810000u};
	// gfx1030 VOPC SDWA with an explicitly encoded VCC_HI result.
	const uint32_t sdwa_shader[] = {0x7d8a00f9u, 0x0686eb80u, 0xd5010000u, 0x01ae0280u, 0xbf810000u};
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
			    bool         Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t      GetInteger(const Core::String&) const override { return 0; }
			    bool         GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation;
		    Config::Load(validation);
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    if (!CheckVccHiProgram(code, false))
		    {
			    std::fprintf(stderr, "VCC_HI VOP3: expected compare at pc=0, cndmask at pc=8, endpgm at pc=16\n%s\n", code.DbgDump().c_str());
			    std::_Exit(2);
		    }
		    if (code.DbgDump().FindIndex("vcc_hi") == Core::STRING8_INVALID_INDEX)
		    {
			    std::fprintf(stderr, "VCC_HI VOP3: debug dump lost vcc_hi operand\n%s\n", code.DbgDump().c_str());
			    std::_Exit(3);
		    }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = 32;
		    input.threads_num[1] = input.threads_num[2] = 1;
		    input.wave_layout = (ShaderComputeWaveLayout {ShaderComputeWaveStrategy::Native, {32, 1, 1}, {32, 1, 1}, 32, 32, 1, 1, 0});
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    if (!CheckVccHiSource(source, "VOP3"))
		    {
			    std::fprintf(stderr, "VCC_HI VOP3 generated source:\n%s\n", source.c_str());
			    std::_Exit(4);
		    }
		    Vector<uint32_t> binary;
		    String8          error;
		    if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
		    {
			    std::fprintf(stderr, "VCC_HI VOP3 toolchain: %s\n%s\n", error.c_str(), source.c_str());
			    std::_Exit(5);
		    }
		    ShaderCode sdwa_code;
		    sdwa_code.SetType(ShaderType::Compute);
		    ShaderParse(sdwa_shader, &sdwa_code);
		    if (!CheckVccHiProgram(sdwa_code, true))
		    {
			    std::fprintf(stderr, "VCC_HI SDWA: expected compare at pc=0, cndmask at pc=8, endpgm at pc=16\n%s\n",
			                 sdwa_code.DbgDump().c_str());
			    std::_Exit(6);
		    }
		    const auto sdwa_source = SpirvGenerateSource(sdwa_code, nullptr, nullptr, &input);
		    if (!CheckVccHiSource(sdwa_source, "SDWA"))
		    {
			    std::fprintf(stderr, "VCC_HI SDWA generated source:\n%s\n", sdwa_source.c_str());
			    std::_Exit(7);
		    }
		    binary.Clear();
		    if (!ShaderToolchain::Run(sdwa_source, &binary, &error) || binary.IsEmpty())
		    {
			    std::fprintf(stderr, "VCC_HI SDWA toolchain: %s\n%s\n", error.c_str(), sdwa_source.c_str());
			    std::_Exit(8);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderVectorCount, ComputeMbcntWritesFloatTypedVgpr)
{
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
			    bool         Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t      GetInteger(const Core::String&) const override { return 0; }
			    bool         GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation;
		    Config::Load(validation);
		    ShaderInstruction mbcnt {};
		    mbcnt.type               = ShaderInstructionType::VMbcntLoU32B32;
		    mbcnt.format             = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
		    mbcnt.dst.type           = ShaderOperandType::Vgpr;
		    mbcnt.dst.register_id    = 3;
		    mbcnt.dst.size           = 1;
		    mbcnt.src[0].type        = ShaderOperandType::Sgpr;
		    mbcnt.src[0].register_id = 0;
		    mbcnt.src[0].size        = 1;
		    mbcnt.src[1].type        = ShaderOperandType::Vgpr;
		    mbcnt.src[1].register_id = 2;
		    mbcnt.src[1].size        = 1;
		    mbcnt.src_num            = 2;
		    ShaderInstruction end {};
		    end.pc     = 4;
		    end.type   = ShaderInstructionType::SEndpgm;
		    end.format = ShaderInstructionFormat::Empty;
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    code.GetInstructions().Add(mbcnt);
		    code.GetInstructions().Add(end);
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    const auto       source                                            = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    Vector<uint32_t> binary;
		    String8          error;
		    if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
		    {
			    std::fprintf(stderr, "%s\n", error.c_str());
			    std::_Exit(2);
		    }
		    if (source.FindIndex("%mbcnt_old_float_0 = OpLoad %float %v3") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("%mbcnt_value_float_0 = OpBitcast %float %mbcnt_value_0") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpStore %v3 %mbcnt_value_float_0") == Core::STRING8_INVALID_INDEX)
		    {
			    std::_Exit(3);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
