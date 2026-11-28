#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderProgramAddress.h"

#include <array>

UT_BEGIN(EmulatorShaderProgramAddress);

using namespace Libs::Graphics;

TEST(EmulatorShaderProgramAddress, PacksCurrentAddressWithoutTouchingEarlierMetadata)
{
	ShaderBindResources bind {};
	bind.program_base_used      = true;
	bind.program_base_offset_dw = 4;
	bind.push_constant_size     = 32;
	bind.program_base           = 0x12345678fffffffcull;
	std::array<uint32_t, 8> words {7, 8, 9, 10, 11, 12, 13, 14};
	ASSERT_TRUE(ShaderWriteProgramBaseMetadata(bind, 0x0000abcd00001000ull, words.data(), words.size()));
	const std::array<uint32_t, 8> expected {7, 8, 9, 10, 0xfffffffcu, 0x12345678u, 0x1000u, 0xabcdu};
	EXPECT_EQ(words, expected);
	bind.program_base = 0x200000010ull;
	ASSERT_TRUE(ShaderWriteProgramBaseMetadata(bind, 0, words.data(), words.size()));
	EXPECT_EQ(words[4], 0x10u);
	EXPECT_EQ(words[5], 2u);
	EXPECT_EQ(words[6], 0u);
	EXPECT_EQ(words[7], 0u);
}

TEST(EmulatorShaderProgramAddress, RejectsInvalidSpansWithoutPartialWrites)
{
	ShaderBindResources bind {};
	bind.program_base_used      = true;
	bind.program_base_offset_dw = 4;
	bind.push_constant_size     = 32;
	std::array<uint32_t, 8> words {1, 2, 3, 4, 5, 6, 7, 8};
	const auto original = words;
	EXPECT_FALSE(ShaderWriteProgramBaseMetadata(bind, 0, words.data(), 7));
	EXPECT_FALSE(ShaderWriteProgramBaseMetadata(bind, 0, nullptr, 8));
	for (const uint32_t offset: {1u, 5u, 8u, 0xfffffffcu})
	{
		bind.program_base_offset_dw = offset;
		EXPECT_FALSE(ShaderWriteProgramBaseMetadata(bind, 0, words.data(), 8));
	}
	bind.program_base_offset_dw = 4;
	bind.push_constant_size = 28;
	EXPECT_FALSE(ShaderWriteProgramBaseMetadata(bind, 0, words.data(), 8));
	EXPECT_EQ(words, original);
	bind.program_base_used = false;
	EXPECT_TRUE(ShaderWriteProgramBaseMetadata(bind, 0, nullptr, 0));
}

UT_END();
