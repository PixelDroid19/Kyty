#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/Jit.h"

#include "Kyty/UnitTest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#endif

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
	EXPECT_EQ(call.code[0x44], 0xb8u);
	EXPECT_EQ(call.code[0x4d], 0x48u);
	EXPECT_EQ(call.code[0x4e], 0x89u);
	EXPECT_EQ(call.code[0x4f], 0xc4u);
	EXPECT_EQ(call.code[0x50], 0x48u);
	EXPECT_EQ(call.code[0x51], 0x83u);
	EXPECT_EQ(call.code[0x52], 0xe4u);
	EXPECT_EQ(call.code[0x53], 0xf0u);
	EXPECT_EQ(call.code[0x54], 0x48u);
	EXPECT_EQ(call.code[0x55], 0x83u);
	EXPECT_EQ(call.code[0x56], 0xecu);
	EXPECT_EQ(call.code[0x57], 0x20u);
	EXPECT_EQ(call.code[0x58], 0xffu);
	EXPECT_EQ(call.code[0x59], 0xd1u);
	EXPECT_EQ(call.code[0x5a], 0x48u);
	EXPECT_EQ(call.code[0x5b], 0x89u);
	EXPECT_EQ(call.code[0x5c], 0xdcu);
	EXPECT_EQ(call.code[0x73], 0x5bu);
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

TEST(EmulatorLoaderTls, StaticOffsetFollowsModuleAlignment)
{
	using Kyty::Loader::LoaderStaticTlsOffset;

	// Captured: 0x2d1 bytes at alignment 8, whose guest code reads its pointer cell at TP-0x2d0.
	EXPECT_EQ(LoaderStaticTlsOffset(0, 0x2d1, 8), 0x2d8u);
	EXPECT_EQ(LoaderStaticTlsOffset(0x2d8, 0x468, 16), 0x740u);
	EXPECT_EQ(LoaderStaticTlsOffset(0, 8, 1), 8u);
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

#if defined(__linux__) && defined(__x86_64__)
namespace {

thread_local uint64_t g_test_guest_thread_pointer = 0;

int64_t TestGuestThreadPointerCacheOffset()
{
	uint64_t host_thread_pointer = 0;
	asm volatile("mov %%fs:0, %0" : "=r"(host_thread_pointer));
	return static_cast<int64_t>(reinterpret_cast<uint64_t>(&g_test_guest_thread_pointer) - host_thread_pointer);
}

uint64_t TestTlsMissHandler()
{
	return 0x5afec411u;
}

struct TlsStubResult
{
	uint64_t rax   = 0;
	uint64_t rcx   = 0;
	uint8_t  carry = 0;
};

// Calls the stub like a patched guest site, with CF set and a known rcx.
TlsStubResult CallTlsStub(const void* stub)
{
	TlsStubResult result;
	result.rcx = 0x1234;
	asm volatile("mov %%rsp, %%rbx\n\t"
	             "lea -128(%%rsp), %%rsp\n\t"
	             "and $-16, %%rsp\n\t"
	             "stc\n\t"
	             "call *%[stub]\n\t"
	             "setc %[carry]\n\t"
	             "mov %%rbx, %%rsp\n\t"
	             : "=a"(result.rax), "+c"(result.rcx), [carry] "=q"(result.carry)
	             : [stub] "r"(stub)
	             : "rbx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
	               "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
	return result;
}

} // namespace

TEST(EmulatorLoaderTls, CachedCallReturnsThreadPointerOrTakesMissPath)
{
	const int64_t offset = TestGuestThreadPointerCacheOffset();
	ASSERT_GE(offset, static_cast<int64_t>(INT32_MIN));
	ASSERT_LE(offset, static_cast<int64_t>(INT32_MAX));

	void* page = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(page, MAP_FAILED);
	auto* bytes = static_cast<uint8_t*>(page);

	// Miss target in the same page: movabs rax, <handler>; jmp rax.
	const uint8_t trampoline[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
	std::memcpy(bytes + 0x40, trampoline, sizeof(trampoline));
	const auto handler = reinterpret_cast<uint64_t>(&TestTlsMissHandler);
	std::memcpy(bytes + 0x42, &handler, sizeof(handler));

	auto* stub = new (bytes) Kyty::Loader::Jit::TlsCachedCall;
	stub->SetCacheOffset(static_cast<int32_t>(offset));
	stub->SetMissHandler(bytes + 0x40);

	g_test_guest_thread_pointer = 0x7f00feedf00dULL;
	const auto hit              = CallTlsStub(stub);
	EXPECT_EQ(hit.rax, 0x7f00feedf00dULL);
	EXPECT_EQ(hit.rcx, 0x1234u);
	EXPECT_EQ(hit.carry, 1u);

	g_test_guest_thread_pointer = 0;
	EXPECT_EQ(CallTlsStub(stub).rax, 0x5afec411u);

	munmap(page, 0x1000);
}
#endif

UT_END();

#endif // KYTY_EMU_ENABLED
