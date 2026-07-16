#include "Emulator/Loader/RuntimeLinker.h"

#include "Kyty/UnitTest.h"

#include <cstdint>
#include <cstring>
#include <vector>

#ifdef KYTY_EMU_ENABLED

UT_BEGIN(EmulatorLoaderTls);

using Kyty::Loader::LoaderRewriteTlsGdCallRexPrefix;

// Captured guest encoding: three 0x66 prefixes + E8 rel32 (should be REX.W).
// Real image had 730 sites, all targeting the TLS handler after rewrite to 64-bit.
TEST(EmulatorLoaderTls, RewritesTriple66CallToRexW)
{
	// 66 66 66 e8 2e 3e 4d 00  + trailing marker
	std::vector<uint8_t> code = {0x66, 0x66, 0x66, 0xe8, 0x2e, 0x3e, 0x4d, 0x00, 0xcc};
	EXPECT_EQ(LoaderRewriteTlsGdCallRexPrefix(code.data(), code.size()), 1u);
	EXPECT_EQ(code[0], 0x66u);
	EXPECT_EQ(code[1], 0x66u);
	EXPECT_EQ(code[2], 0x48u); // REX.W restored
	EXPECT_EQ(code[3], 0xe8u);
	EXPECT_EQ(code[4], 0x2eu); // displacement preserved
	EXPECT_EQ(code[5], 0x3eu);
	EXPECT_EQ(code[6], 0x4du);
	EXPECT_EQ(code[7], 0x00u);
	EXPECT_EQ(code[8], 0xccu);
}

TEST(EmulatorLoaderTls, LeavesCorrectRexWSequenceUntouched)
{
	std::vector<uint8_t> code = {0x66, 0x66, 0x48, 0xe8, 0x11, 0x22, 0x33, 0x44};
	EXPECT_EQ(LoaderRewriteTlsGdCallRexPrefix(code.data(), code.size()), 0u);
	EXPECT_EQ(code[2], 0x48u);
	EXPECT_EQ(code[4], 0x11u);
}

TEST(EmulatorLoaderTls, RewritesMultipleSitesAndSkipsShortBuffers)
{
	std::vector<uint8_t> code = {
	    0x90,
	    0x66, 0x66, 0x66, 0xe8, 0x01, 0x00, 0x00, 0x00,
	    0x90,
	    0x66, 0x66, 0x66, 0xe8, 0x02, 0x00, 0x00, 0x00,
	};
	EXPECT_EQ(LoaderRewriteTlsGdCallRexPrefix(code.data(), code.size()), 2u);
	EXPECT_EQ(code[3], 0x48u);
	EXPECT_EQ(code[12], 0x48u);

	uint8_t tiny[4] = {0x66, 0x66, 0x66, 0xe8};
	EXPECT_EQ(LoaderRewriteTlsGdCallRexPrefix(tiny, sizeof(tiny)), 0u);
	EXPECT_EQ(LoaderRewriteTlsGdCallRexPrefix(nullptr, 64), 0u);
}

// Gen5 CRT: entry (_start) at 0x70 contains `call _init` targeting DT_INIT at 0x10.
// Host must not also run DT_INIT before entry (double static ctor → self-loop spin).
TEST(EmulatorLoaderTls, DetectsCrtEntryDirectCallToInit)
{
	using Kyty::Loader::LoaderCodeContainsDirectCallTo;

	// code @ 0x100: 90 e8 0b 00 00 00 c3
	// CALL at 0x101, next=0x106, rel=+0x0b → target 0x111.
	const uint8_t code[] = {0x90, 0xe8, 0x0b, 0x00, 0x00, 0x00, 0xc3};
	EXPECT_TRUE(LoaderCodeContainsDirectCallTo(code, sizeof(code), 0x100, 0x111));
	EXPECT_FALSE(LoaderCodeContainsDirectCallTo(code, sizeof(code), 0x100, 0x200));
	EXPECT_FALSE(LoaderCodeContainsDirectCallTo(nullptr, 16, 0x100, 0x111));
	EXPECT_FALSE(LoaderCodeContainsDirectCallTo(code, 4, 0x100, 0x111));
}

UT_END();

#endif // KYTY_EMU_ENABLED
