#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/Jit.h"

#include "Kyty/UnitTest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef KYTY_EMU_ENABLED

UT_BEGIN(EmulatorLoaderTls);

using Kyty::Loader::LoaderRewriteTlsGdCallRexPrefix;
using Kyty::Loader::LoaderPatchTlsFsBaseLoads;

TEST(EmulatorLoaderTls, SafeCallAlignsUnknownGuestStackBeforeHostCallback)
{
	Kyty::Loader::Jit::SafeCall call;

	EXPECT_EQ(call.code[0x0c], 0x53u);
	EXPECT_EQ(call.code[0x40], 0x48u);
	EXPECT_EQ(call.code[0x41], 0x89u);
	EXPECT_EQ(call.code[0x42], 0xe3u);
	EXPECT_EQ(call.code[0x43], 0x48u);
	EXPECT_EQ(call.code[0x44], 0x83u);
	EXPECT_EQ(call.code[0x45], 0xe4u);
	EXPECT_EQ(call.code[0x46], 0xf0u);
	EXPECT_EQ(call.code[0x47], 0x48u);
	EXPECT_EQ(call.code[0x48], 0x83u);
	EXPECT_EQ(call.code[0x49], 0xecu);
	EXPECT_EQ(call.code[0x4a], 0x20u);
	EXPECT_EQ(call.code[0x51], 0x48u);
	EXPECT_EQ(call.code[0x52], 0x89u);
	EXPECT_EQ(call.code[0x53], 0xdcu);
	EXPECT_EQ(call.code[0x6a], 0x5bu);
}

TEST(EmulatorLoaderTls, CallPltPreservesGuestArgumentsAcrossLazyResolution)
{
	alignas(Kyty::Loader::Jit::CallPlt)
	    std::array<uint8_t, Kyty::Loader::Jit::CallPlt::GetSize(1)> storage {};
	auto* call = new (storage.data()) Kyty::Loader::Jit::CallPlt(1);

	EXPECT_EQ(Kyty::Loader::Jit::CallPlt::GetSize(1), Kyty::Loader::Jit::CallPlt::kResolverSize + 16u);
	EXPECT_EQ(call->code[0], 0x49u);
	EXPECT_EQ(call->code[1], 0xbbu);
	EXPECT_EQ(call->code[19], 0x48u);
	EXPECT_EQ(call->code[20], 0x81u);
	EXPECT_EQ(call->code[21], 0xecu);
	EXPECT_EQ(call->code[22], 0x88u);
	EXPECT_EQ(call->code[73], 0x49u);
	EXPECT_EQ(call->code[74], 0x8bu);
	EXPECT_EQ(call->code[85], 0x49u);
	EXPECT_EQ(call->code[86], 0xbau);
	EXPECT_EQ(call->code[164], 0x48u);
	EXPECT_EQ(call->code[168], 0x41u);
	EXPECT_EQ(call->code[169], 0xffu);
	EXPECT_EQ(call->code[170], 0xe3u);
}

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

TEST(EmulatorLoaderTls, PatchesUnprefixedFsBaseLoad)
{
	std::vector<uint8_t> code = {
	    0x90,
	    0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00,
	    0xcc,
	};
	EXPECT_EQ(LoaderPatchTlsFsBaseLoads(code.data(), code.size(), 0x100000), 1u);
	EXPECT_EQ(code[0], 0x90u);
	EXPECT_EQ(code[1], 0xe8u);
	EXPECT_EQ(code[6], 0x48u);
	EXPECT_EQ(code[7], 0x89u);
	EXPECT_EQ(code[8], 0xc0u);
	EXPECT_EQ(code[9], 0x90u);
	EXPECT_EQ(code[10], 0xccu);
}

TEST(EmulatorLoaderTls, PatchesPrefixedFsBaseLoadFromInstructionStart)
{
	std::vector<uint8_t> code = {
	    0x90,
	    0x66, 0x66, 0x66,
	    0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00,
	    0xcc,
	};
	EXPECT_EQ(LoaderPatchTlsFsBaseLoads(code.data(), code.size(), 0x100000), 1u);
	EXPECT_EQ(code[0], 0x90u);
	EXPECT_EQ(code[1], 0xe8u);
	EXPECT_EQ(code[6], 0x48u);
	EXPECT_EQ(code[7], 0x89u);
	EXPECT_EQ(code[8], 0xc0u);
	EXPECT_EQ(code[9], 0x90u);
	EXPECT_EQ(code[10], 0x90u);
	EXPECT_EQ(code[11], 0x90u);
	EXPECT_EQ(code[12], 0x90u);
	EXPECT_EQ(code[13], 0xccu);
}

TEST(EmulatorLoaderTls, DoesNotPatchInvalidFsBaseLoadWindows)
{
	uint8_t short_code[8] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00};
	uint8_t valid_code[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00};
	EXPECT_EQ(LoaderPatchTlsFsBaseLoads(short_code, sizeof(short_code), 0x100000), 0u);
	EXPECT_EQ(LoaderPatchTlsFsBaseLoads(valid_code, sizeof(valid_code), 0), 0u);
	EXPECT_EQ(LoaderPatchTlsFsBaseLoads(nullptr, 16, 0x100000), 0u);
}

TEST(EmulatorLoaderTls, CalculatesGuestTlsRelocationValues)
{
	using Kyty::Loader::LoaderTlsRelocationValue;

	constexpr uint64_t module_id       = 7;
	constexpr uint64_t static_tls_size = 0x1868;
	constexpr uint64_t symbol_offset   = 0x40;
	constexpr int64_t  addend          = 3;

	EXPECT_EQ(LoaderTlsRelocationValue(16, module_id, symbol_offset, addend, static_tls_size), module_id);
	EXPECT_EQ(LoaderTlsRelocationValue(17, module_id, symbol_offset, addend, static_tls_size), symbol_offset + addend);
	EXPECT_EQ(LoaderTlsRelocationValue(18, module_id, symbol_offset, addend, static_tls_size),
	          symbol_offset + addend - static_tls_size);
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

TEST(EmulatorLoaderTls, InitializesOnlyFileBackedTlsPrefix)
{
	using Kyty::Loader::LoaderInitializeThreadTlsImage;

	constexpr uint64_t kImageSize = 32;
	constexpr uint64_t kInitSize  = 12;
	std::array<uint8_t, kImageSize> source {};
	std::array<uint8_t, kImageSize> tls {};
	source.fill(0xa5);
	tls.fill(0xcc);

	LoaderInitializeThreadTlsImage(tls.data(), tls.size(), source.data(), kInitSize);

	EXPECT_TRUE(std::equal(tls.begin(), tls.begin() + kInitSize, source.begin()));
	EXPECT_TRUE(std::all_of(tls.begin() + kInitSize, tls.end(), [](uint8_t value) { return value == 0; }));
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

// Gen5 TLS template can hold absolute pointers to static descriptor blobs
// (size/refcount + function pointers) at the compute-context slots. Those
// must clear so s_pTlsComputeContext starts null for the guest SET path.
TEST(EmulatorLoaderTls, PreparesThreadTlsClearsStaticDescriptorContextSlots)
{
	using Kyty::Loader::LoaderPrepareThreadTlsImage;

	GuestMem guest;
	guest.base = 0x900000000ull;
	guest.bytes.assign(0x1000, 0);
	// Descriptor at +0x148: size=0x78, refcount=2, fn ptr into program.
	const uint64_t desc_off = 0x148;
	const uint64_t desc_va  = guest.base + desc_off;
	*reinterpret_cast<uint64_t*>(guest.bytes.data() + desc_off)        = 0x78;
	*reinterpret_cast<uint64_t*>(guest.bytes.data() + desc_off + 0x08) = 2;
	*reinterpret_cast<uint64_t*>(guest.bytes.data() + desc_off + 0x10) = guest.base + 0x200; // "code"

	// Live object with vtable-like word0 (high pointer) must stay.
	const uint64_t live_off = 0x400;
	const uint64_t live_va  = guest.base + live_off;
	*reinterpret_cast<uint64_t*>(guest.bytes.data() + live_off) = guest.base + 0x300; // vtable

	constexpr uint64_t kTmpl    = 0x9088e0000ull;
	constexpr uint64_t kImgSize = 0xa0;
	std::vector<uint8_t> tls(kImgSize, 0);
	*reinterpret_cast<uint64_t*>(tls.data() + 0x70) = desc_va;
	*reinterpret_cast<uint64_t*>(tls.data() + 0x58) = live_va;

	const uint64_t n =
	    LoaderPrepareThreadTlsImage(tls.data(), kImgSize, kTmpl, guest.base, guest.bytes.size(), TestGuestRead64, &guest);
	EXPECT_GE(n, 1u);
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(tls.data() + 0x70), 0u);
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(tls.data() + 0x58), live_va);
}

// Freelist control lives at TP-0x158 (TLS offset image_size-0x158). Prepare must
// not rewrite non-program sentinel words there; corrupting them produces the
// recycle crash at guest object-init (begin patterns like 0x1fffffffe).
TEST(EmulatorLoaderTls, PrepareThreadTlsPreservesFreelistSentinelWords)
{
	using Kyty::Loader::LoaderPrepareThreadTlsImage;

	constexpr uint64_t kImgSize = 0xa80;
	constexpr uint64_t kFlOff   = kImgSize - 0x158; // 0x928
	constexpr uint64_t kTmpl    = 0x900000000ull;
	std::vector<uint8_t> tls(kImgSize, 0);
	*reinterpret_cast<uint64_t*>(tls.data() + kFlOff)     = 0x00000001fffffffeULL;
	*reinterpret_cast<uint64_t*>(tls.data() + kFlOff + 8) = 0x00000003ffffffffULL;

	GuestMem guest;
	guest.base = 0x1000;
	guest.bytes.assign(0x100, 0);

	EXPECT_EQ(LoaderPrepareThreadTlsImage(tls.data(), kImgSize, kTmpl, guest.base, guest.bytes.size(), TestGuestRead64, &guest), 0u);
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(tls.data() + kFlOff), 0x00000001fffffffeULL);
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(tls.data() + kFlOff + 8), 0x00000003ffffffffULL);
}

UT_END();

#endif // KYTY_EMU_ENABLED
