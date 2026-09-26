#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>

UT_BEGIN(EmulatorShaderMimg);

using namespace Libs::Graphics;

static void InitMimgParser()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

TEST(EmulatorShaderMimg, RejectsGen5MimgExtendedOpcodeInsteadOfAliasingImageLoad)
{
	// GFX10 MIMG encodes OP[7] in word zero bit zero, not beside OP[6:0].
	const uint32_t shader[] = {(0x3cu << 26u) | (0xfu << 8u) | 1u, 0u, 0xbf810000u};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(rejected_exit), "");
}

TEST(EmulatorShaderMimg, ParsesBvhNsaOperandsWithoutTextureOrPaddingRegisters)
{
	// Synthetic GFX10 BVH packet: four outputs, eleven independent address
	// registers, one 128-bit descriptor and two unused NSA padding bytes.
	const uint32_t shader[] = {(0x3cu << 26u) | (0x66u << 18u) | 0x9f07u,
	                           (6u << 16u) | (20u << 8u) | 8u,
	                           0x100e0c0au, 0x18161412u, 0xfffe1c1au, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    const auto& inst = code.GetInstructions().At(0);
		    if (inst.dst.register_id != 20 || inst.dst.size != 4 || inst.src_num != 2 ||
		        inst.src[1].register_id != 24 || inst.src[1].size != 4 || inst.mimg_address_num != 11)
		    {
			    std::_Exit(2);
		    }
		    for (int i = 0; i < 11; ++i)
		    {
			    if (inst.mimg_address[i].register_id != 8 + 2 * i) { std::_Exit(3); }
		    }
		    if (code.GetInstructions().At(1).pc != 20) { std::_Exit(4); }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, ParsesContiguousBvhSourcesThroughLastVgpr)
{
	const uint32_t shader[] = {(0x3cu << 26u) | (0x66u << 18u) | 0x9f01u,
	                           (25u << 16u) | (252u << 8u) | 245u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    const auto& inst = code.GetInstructions().At(0);
		    if (inst.type != ShaderInstructionType::ImageBvhIntersectRay || inst.mimg_address_num != 11 ||
		        inst.dst.register_id != 252 || inst.dst.size != 4 || code.GetInstructions().At(1).pc != 8)
		    {
			    std::_Exit(2);
		    }
		    for (int i = 0; i < 11; ++i)
		    {
			    if (inst.mimg_address[i].register_id != 245 + i) { std::_Exit(3); }
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
