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

// Synthetic guest "program" image: unconstructed Context at 0x200 (word0=0,
// buffer@+0x3e0=0) and a constructed one at 0x800 (word0 non-zero).
struct GuestMem
{
	std::vector<uint8_t> bytes;
	uint64_t             base = 0;
};

static bool TestGuestRead64(uint64_t addr, uint64_t* out, void* ctx)
{
	auto* g = static_cast<GuestMem*>(ctx);
	if (g == nullptr || out == nullptr || addr < g->base)
	{
		return false;
	}
	const uint64_t off = addr - g->base;
	if (off + sizeof(uint64_t) > g->bytes.size())
	{
		return false;
	}
	std::memcpy(out, g->bytes.data() + off, sizeof(uint64_t));
	return true;
}

TEST(EmulatorLoaderTls, PreparesThreadTlsRelocatesSelfAndClearsUnconstructedContext)
{
	using Kyty::Loader::LoaderPrepareThreadTlsImage;

	GuestMem guest;
	guest.base = 0x1000;
	guest.bytes.assign(0x1000, 0);
	// Unconstructed context at guest VA 0x1200 (offset 0x200): word0=0, +0x3e0=0.
	// Constructed context at 0x1800: word0=1, +0x3e0=0xdead.
	const uint64_t unconstructed = guest.base + 0x200;
	const uint64_t constructed   = guest.base + 0x800;
	*reinterpret_cast<uint64_t*>(guest.bytes.data() + 0x800)        = 1;
	*reinterpret_cast<uint64_t*>(guest.bytes.data() + 0x800 + 0x3e0) = 0xdead;

	constexpr uint64_t kTmpl     = 0x9000;
	constexpr uint64_t kImgSize  = 0xa0;
	std::vector<uint8_t> tls(kImgSize, 0);
	// Self-pointer at +0x70 into template (+0x40).
	*reinterpret_cast<uint64_t*>(tls.data() + 0x70) = kTmpl + 0x40;
	// Stale absolute unconstructed context (Gen5 pattern).
	*reinterpret_cast<uint64_t*>(tls.data() + 0x50) = unconstructed;
	// Live absolute constructed context — must stay.
	*reinterpret_cast<uint64_t*>(tls.data() + 0x58) = constructed;
	// Unrelated non-pointer value.
	*reinterpret_cast<uint64_t*>(tls.data() + 0x00) = 0xffffffffffffffffULL;

	const uint64_t n =
	    LoaderPrepareThreadTlsImage(tls.data(), kImgSize, kTmpl, guest.base, guest.bytes.size(), TestGuestRead64, &guest);
	EXPECT_GE(n, 2u);
	const uint64_t tls_base = reinterpret_cast<uint64_t>(tls.data());
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(tls.data() + 0x70), tls_base + 0x40);
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(tls.data() + 0x50), 0u);
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(tls.data() + 0x58), constructed);
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(tls.data() + 0x00), 0xffffffffffffffffULL);
}

TEST(EmulatorLoaderTls, PrepareThreadTlsHandlesNullAndShort)
{
	using Kyty::Loader::LoaderPrepareThreadTlsImage;
	EXPECT_EQ(LoaderPrepareThreadTlsImage(nullptr, 64, 0x1000, 0, 0, nullptr, nullptr), 0u);
	uint8_t tiny[4] = {};
	EXPECT_EQ(LoaderPrepareThreadTlsImage(tiny, sizeof(tiny), 0x1000, 0, 0, nullptr, nullptr), 0u);
}

UT_END();

#endif // KYTY_EMU_ENABLED
