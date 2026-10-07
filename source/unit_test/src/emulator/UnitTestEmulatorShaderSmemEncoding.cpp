#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>

UT_BEGIN(EmulatorShaderSmemEncoding);

using namespace Libs::Graphics;

// s_load_dwordx4 s[16:19], s[12:13], 0x50 (checked with LLVM gfx1030).
static constexpr uint32_t kLoadWord0 = 0xf4080406u;
static constexpr uint32_t kLoadWord1 = 0xfa000050u;
static constexpr uint32_t kEnd       = 0xbf810000u;

static void InitializeConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

TEST(EmulatorShaderSmemEncoding, UndefinedEncodingBitsRemainVisibleToExactAdmission)
{
	// RDNA2 ISA table 73 defines no SMEM field at word0 bits 13, 15, 17 or
	// word1 bits 21..24. The decoded operands must not change either way.
	struct Case
	{
		uint32_t word0;
		uint32_t word1;
		bool     undefined;
	};
	const Case cases[] = {
	    {kLoadWord0, kLoadWord1, false},
	    {kLoadWord0 | (1u << 13u), kLoadWord1, true},
	    {kLoadWord0 | (1u << 15u), kLoadWord1, true},
	    {kLoadWord0 | (1u << 17u), kLoadWord1, true},
	    {kLoadWord0, kLoadWord1 | (1u << 21u), true},
	    {kLoadWord0, kLoadWord1 | (1u << 24u), true},
	};
	for (const auto& test: cases)
	{
		const uint32_t words[] = {test.word0, test.word1, kEnd};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().Size() != 2u)
			    {
				    std::_Exit(2);
			    }
			    const auto& load     = code.GetInstructions().At(0);
			    const bool  operands = load.type == ShaderInstructionType::SLoadDwordx4 && load.dst.register_id == 16 &&
			                          load.src[0].register_id == 12 && load.src[1].constant.u == 0x50u && load.smem_imm_offset == 0;
			    const bool  flagged  = (load.smem_flags & 0x80u) != 0u;
			    std::_Exit(operands && flagged == test.undefined && (load.smem_flags & 0x7fu) == 0u ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

UT_END();
