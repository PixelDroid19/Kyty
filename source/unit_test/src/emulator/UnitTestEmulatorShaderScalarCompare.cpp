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

UT_BEGIN(EmulatorShaderScalarCompare);

using namespace Libs::Graphics;

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
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    (void)SpirvGenerateSource(code, nullptr, nullptr, &input);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(rejected_exit), "");
}

UT_END();
