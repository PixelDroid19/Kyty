#include "Kyty/UnitTest.h"

#include "Emulator/VideoFrameMemory.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Kyty/Core/VirtualMemory.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

// HLE bodies live in LibCString.cpp; declare the tested surface here so the
// unit test links against the emulator's own implementations.
namespace Kyty::Libs::LibC {
int c_strcasecmp(const char* a, const char* b);
int c_strncasecmp(const char* a, const char* b, size_t count);
int c_strncat_s(char* dst, size_t dst_size, const char* src, size_t count);
char* c_strnstr(const char* haystack, const char* needle, size_t count);
int c_vsnprintf(char* s, size_t n, const char* fmt, Kyty::Libs::VaList* ap);
size_t c_fread(void* ptr, size_t size, size_t count, FILE* stream);
char* c_fgets(char* buffer, int size, FILE* stream);
} // namespace Kyty::Libs::LibC

UT_BEGIN(EmulatorLibcString);

namespace {

uint32_t g_stdio_prepare_count = 0;
bool     g_stdio_made_writable = false;

void IgnoreLinearFrame(uint64_t /*base*/, size_t /*size*/, uint32_t /*row_pitch_bytes*/) {}

void IgnoreUnregisterFrame(uint64_t /*base*/) {}

void PrepareStdioDestination(uint64_t base, uint64_t /*size*/)
{
	g_stdio_prepare_count++;
	g_stdio_made_writable = Core::VirtualMemory::Protect(base, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite);
}

template <typename Function>
Function ResolveLibcExport(Kyty::Loader::SymbolDatabase* symbols, const char* name)
{
	Kyty::Loader::SymbolResolve query {};
	query.name                 = Kyty::Loader::EncodeNameAsNid(name);
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Kyty::Loader::SymbolType::Func;
	const auto* record = symbols->Find(query);
	return record != nullptr ? reinterpret_cast<Function>(record->vaddr) : nullptr;
}

template <typename Function, typename Result>
void ExpectClassicBinaryPrefix(Function convert)
{
	const char input[] = "0b101";
	for (const int base: std::array {0, 2})
	{
		char* end = nullptr;
		errno     = 0;
		EXPECT_EQ(convert(input, &end, base), Result {});
		EXPECT_EQ(end, input + 1);
		EXPECT_EQ(errno, 0);
	}

	char* end = nullptr;
	errno     = 0;
	EXPECT_EQ(convert(input, &end, 16), static_cast<Result>(45313));
	EXPECT_EQ(end, input + 5);
	EXPECT_EQ(errno, 0);

	const char signed_input[] = " \t-0B101";
	for (const int base: std::array {0, 2})
	{
		end   = nullptr;
		errno = 0;
		EXPECT_EQ(convert(signed_input, &end, base), Result {});
		EXPECT_EQ(end, signed_input + 4);
		EXPECT_EQ(errno, 0);
	}
}

} // namespace

TEST(EmulatorLibcString, ResolvedIntegerConversionsKeepClassicPrefixAndGuestLongWidth)
{
	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libc_1", &symbols));

	using SignedConversion   = int64_t(KYTY_SYSV_ABI*)(const char*, char**, int);
	using UnsignedConversion = uint64_t(KYTY_SYSV_ABI*)(const char*, char**, int);
	const auto strtol   = ResolveLibcExport<SignedConversion>(&symbols, "strtol");
	const auto strtoul  = ResolveLibcExport<UnsignedConversion>(&symbols, "strtoul");
	const auto strtoll  = ResolveLibcExport<SignedConversion>(&symbols, "strtoll");
	const auto strtoull = ResolveLibcExport<UnsignedConversion>(&symbols, "strtoull");
	ASSERT_NE(strtol, nullptr);
	ASSERT_NE(strtoul, nullptr);
	ASSERT_NE(strtoll, nullptr);
	ASSERT_NE(strtoull, nullptr);

	ExpectClassicBinaryPrefix<SignedConversion, int64_t>(strtol);
	ExpectClassicBinaryPrefix<UnsignedConversion, uint64_t>(strtoul);
	ExpectClassicBinaryPrefix<SignedConversion, int64_t>(strtoll);
	ExpectClassicBinaryPrefix<UnsignedConversion, uint64_t>(strtoull);

	const char signed_above_32_bit[] = "2147483648";
	char*     end                    = nullptr;
	errno                            = 0;
	EXPECT_EQ(strtol(signed_above_32_bit, &end, 10), INT64_C(2147483648));
	EXPECT_EQ(end, signed_above_32_bit + sizeof(signed_above_32_bit) - 1);
	EXPECT_EQ(errno, 0);
	errno = 0;
	EXPECT_EQ(strtoll(signed_above_32_bit, &end, 10), INT64_C(2147483648));
	EXPECT_EQ(end, signed_above_32_bit + sizeof(signed_above_32_bit) - 1);
	EXPECT_EQ(errno, 0);
	errno = 0;
	EXPECT_EQ(strtoul(signed_above_32_bit, &end, 10), UINT64_C(2147483648));
	EXPECT_EQ(end, signed_above_32_bit + sizeof(signed_above_32_bit) - 1);
	EXPECT_EQ(errno, 0);
	errno = 0;
	EXPECT_EQ(strtoull(signed_above_32_bit, &end, 10), UINT64_C(2147483648));
	EXPECT_EQ(end, signed_above_32_bit + sizeof(signed_above_32_bit) - 1);
	EXPECT_EQ(errno, 0);
}

TEST(EmulatorLibcString, ResolvedIntegerConversionsPreserveErrnoAndSignedness)
{
	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libc_1", &symbols));

	using SignedConversion   = int64_t(KYTY_SYSV_ABI*)(const char*, char**, int);
	using UnsignedConversion = uint64_t(KYTY_SYSV_ABI*)(const char*, char**, int);
	const auto strtol   = ResolveLibcExport<SignedConversion>(&symbols, "strtol");
	const auto strtoul  = ResolveLibcExport<UnsignedConversion>(&symbols, "strtoul");
	const auto strtoll  = ResolveLibcExport<SignedConversion>(&symbols, "strtoll");
	const auto strtoull = ResolveLibcExport<UnsignedConversion>(&symbols, "strtoull");
	ASSERT_NE(strtol, nullptr);
	ASSERT_NE(strtoul, nullptr);
	ASSERT_NE(strtoll, nullptr);
	ASSERT_NE(strtoull, nullptr);

	const char invalid_base_input[] = "42";
	char*     end                   = nullptr;
	errno                           = 0;
	EXPECT_EQ(strtol(invalid_base_input, &end, 1), 0);
	EXPECT_EQ(errno, EINVAL);
	errno = ERANGE;
	const char classic_prefix[] = "0b101";
	EXPECT_EQ(strtol(classic_prefix, &end, 0), 0);
	EXPECT_EQ(end, classic_prefix + 1);
	EXPECT_EQ(errno, ERANGE);

	const char negative_unsigned[] = "-1";
	errno                         = 0;
	EXPECT_EQ(strtoul(negative_unsigned, &end, 10), UINT64_MAX);
	EXPECT_EQ(errno, 0);
	EXPECT_EQ(strtoull(negative_unsigned, &end, 10), UINT64_MAX);
	EXPECT_EQ(errno, 0);

	const char signed_overflow[] = "9223372036854775808";
	errno                        = 0;
	EXPECT_EQ(strtol(signed_overflow, &end, 10), INT64_MAX);
	EXPECT_EQ(end, signed_overflow + sizeof(signed_overflow) - 1);
	EXPECT_EQ(errno, ERANGE);
	errno = 0;
	EXPECT_EQ(strtoll(signed_overflow, &end, 10), INT64_MAX);
	EXPECT_EQ(end, signed_overflow + sizeof(signed_overflow) - 1);
	EXPECT_EQ(errno, ERANGE);

	const char unsigned_overflow[] = "18446744073709551616";
	errno                          = 0;
	EXPECT_EQ(strtoul(unsigned_overflow, &end, 10), UINT64_MAX);
	EXPECT_EQ(end, unsigned_overflow + sizeof(unsigned_overflow) - 1);
	EXPECT_EQ(errno, ERANGE);
	errno = 0;
	EXPECT_EQ(strtoull(unsigned_overflow, &end, 10), UINT64_MAX);
	EXPECT_EQ(end, unsigned_overflow + sizeof(unsigned_overflow) - 1);
	EXPECT_EQ(errno, ERANGE);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__GLIBC__)
TEST(EmulatorLibcString, ResolvedScanfExportsKeepClassicIntegerTokenBoundary)
{
	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libc_1", &symbols));

	using Sscanf = int(KYTY_SYSV_ABI*)(const char*, const char*, ...);
	const auto sscanf_export   = ResolveLibcExport<Sscanf>(&symbols, "sscanf");
	const auto sscanf_s_export = ResolveLibcExport<Sscanf>(&symbols, "sscanf_s");
	ASSERT_NE(sscanf_export, nullptr);
	ASSERT_NE(sscanf_s_export, nullptr);

	const char input[] = "0b101x";
	int       value    = -1;
	int       consumed = -1;
	char      next     = '\0';
	EXPECT_EQ(sscanf_export(input, "%i%n%c", &value, &consumed, &next), 2);
	EXPECT_EQ(value, 0);
	EXPECT_EQ(consumed, 1);
	EXPECT_EQ(next, 'b');

	value    = -1;
	consumed = -1;
	next     = '\0';
	EXPECT_EQ(sscanf_s_export(input, "%i%n%c", &value, &consumed, &next, sizeof(next)), 2);
	EXPECT_EQ(value, 0);
	EXPECT_EQ(consumed, 1);
	EXPECT_EQ(next, 'b');
}
#endif

TEST(EmulatorLibcString, StdioReadsPrepareProtectedGuestDestinations)
{
	using namespace Kyty::Libs::LibC;

	FILE* file = std::tmpfile();
	ASSERT_NE(file, nullptr);
	const char source[] = "abc\n";
	ASSERT_EQ(std::fwrite(source, 1, sizeof(source) - 1u, file), sizeof(source) - 1u);
	std::rewind(file);

	const uint64_t page_size  = Core::VirtualMemory::GetPageSize();
	const uint64_t destination = Core::VirtualMemory::Alloc(0, page_size, Core::VirtualMemory::Mode::ReadWrite);
	ASSERT_NE(destination, 0u);
	ASSERT_TRUE(Core::VirtualMemory::Protect(destination, page_size, Core::VirtualMemory::Mode::Read));

	g_stdio_prepare_count = 0;
	g_stdio_made_writable = false;
	const Emulator::VideoFrameMemory::Callbacks callbacks {&IgnoreLinearFrame, &IgnoreUnregisterFrame, &PrepareStdioDestination};
	ASSERT_TRUE(Emulator::VideoFrameMemory::InstallCallbacks(callbacks));
	EXPECT_EQ(c_fread(reinterpret_cast<void*>(destination), 1, sizeof(source) - 1u, file), sizeof(source) - 1u);
	EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(destination), source, sizeof(source) - 1u), 0);
	EXPECT_EQ(g_stdio_prepare_count, 1u);
	EXPECT_TRUE(g_stdio_made_writable);

	std::rewind(file);
	ASSERT_TRUE(Core::VirtualMemory::Protect(destination, page_size, Core::VirtualMemory::Mode::Read));
	g_stdio_made_writable = false;
	EXPECT_NE(c_fgets(reinterpret_cast<char*>(destination), static_cast<int>(sizeof(source)), file), nullptr);
	EXPECT_STREQ(reinterpret_cast<const char*>(destination), source);
	EXPECT_EQ(g_stdio_prepare_count, 2u);
	EXPECT_TRUE(g_stdio_made_writable);

	EXPECT_EQ(c_fread(reinterpret_cast<void*>(destination), std::numeric_limits<size_t>::max(), 2u, file), 0u);
	EXPECT_EQ(g_stdio_prepare_count, 2u);
	EXPECT_TRUE(Emulator::VideoFrameMemory::InstallCallbacks({}));
	EXPECT_TRUE(Core::VirtualMemory::Free(destination));
	EXPECT_EQ(std::fclose(file), 0);
}

// strcasecmp (NID AV6ipCNa4Rw): ASCII case-insensitive comparison with the C
// contract (negative/zero/positive on the first differing folded byte).
TEST(EmulatorLibcString, StrCaseCmpFoldsAsciiAndOrders)
{
	using namespace Kyty::Libs::LibC;

	EXPECT_EQ(c_strcasecmp("", ""), 0);
	EXPECT_EQ(c_strcasecmp("abc", "abc"), 0);
	EXPECT_EQ(c_strcasecmp("ABC", "abc"), 0);
	EXPECT_EQ(c_strcasecmp("AbC", "aBc"), 0);
	EXPECT_EQ(c_strcasecmp("casefold", "CASEFOLD"), 0);
	// First differing byte decides, case-folded.
	EXPECT_LT(c_strcasecmp("abc", "abd"), 0);
	EXPECT_GT(c_strcasecmp("abd", "abc"), 0);
	EXPECT_LT(c_strcasecmp("ABC", "abd"), 0);
	EXPECT_GT(c_strcasecmp("aBd", "abc"), 0);
	// A longer prefix compares greater than the shorter one.
	EXPECT_GT(c_strcasecmp("abcd", "abc"), 0);
	EXPECT_LT(c_strcasecmp("abc", "abcd"), 0);
	// Non-alpha bytes compare by raw value and terminate the scan normally.
	EXPECT_EQ(c_strcasecmp("a1b", "A1B"), 0);
	EXPECT_LT(c_strcasecmp("a1b", "a2b"), 0);
	EXPECT_EQ(c_strcasecmp("Save01", "sAvE01"), 0);
}

// strncasecmp (NID pXvbDfchu6k) honors the count bound even when the folded
// prefix matches; comparison beyond the bound must not read further.
TEST(EmulatorLibcString, StrNCaseCmpHonorsCount)
{
	using namespace Kyty::Libs::LibC;

	EXPECT_EQ(c_strncasecmp("abc", "abc", 3), 0);
	EXPECT_EQ(c_strncasecmp("ABC", "abc", 3), 0);
	// Differ at byte 3, but count=3 stops before it.
	EXPECT_EQ(c_strncasecmp("abcd", "abce", 3), 0);
	EXPECT_LT(c_strncasecmp("abcd", "abce", 4), 0);
	EXPECT_EQ(c_strncasecmp("", "anything", 0), 0);
	EXPECT_EQ(c_strncasecmp("a", "A", 0), 0);
}

TEST(EmulatorLibcString, StrncatSAppendsWithoutLosingExistingPath)
{
	using Kyty::Libs::LibC::c_strncat_s;

	char path[32] = "assets/";
	EXPECT_EQ(c_strncat_s(path, sizeof(path), "scene", 5), 0);
	EXPECT_STREQ(path, "assets/scene");
	EXPECT_EQ(c_strncat_s(path, sizeof(path), ".bin", 4), 0);
	EXPECT_STREQ(path, "assets/scene.bin");
	EXPECT_EQ(c_strncat_s(path, sizeof(path), "ignored", 0), 0);
	EXPECT_STREQ(path, "assets/scene.bin");

	char bounded[8] = "xy";
	EXPECT_EQ(c_strncat_s(bounded, sizeof(bounded), "z123", 1), 0);
	EXPECT_STREQ(bounded, "xyz");
	EXPECT_NE(c_strncat_s(bounded, sizeof(bounded), "oversized", 9), 0);
	EXPECT_STREQ(bounded, "xyz");
}

TEST(EmulatorLibcString, StrnstrRespectsHaystackBound)
{
	using Kyty::Libs::LibC::c_strnstr;

	const char value[] = "data/scene.bin";
	EXPECT_EQ(c_strnstr(value, "scene", sizeof(value) - 1), value + 5);
	EXPECT_EQ(c_strnstr(value, "data", 4), value);
	EXPECT_EQ(c_strnstr(value, "scene", 9), nullptr);
	EXPECT_EQ(c_strnstr(value, "missing", sizeof(value) - 1), nullptr);
	EXPECT_EQ(c_strnstr(value, "", 0), value);
	EXPECT_EQ(c_strnstr(value, "data", 0), nullptr);
}

// vsnprintf (NID Q2V+iqvjgC0): s/n/format arrive as direct SysV parameters;
// the guest VaList carries only the variadic conversion arguments. Buffer
// bounds truncate while the return value stays the full length per the C
// contract.
static int GuestVsnprintf(char* s, size_t n, const char* format, const uint64_t* gp_args, size_t gp_count)
{
	alignas(16) Kyty::Libs::VaRegSave reg_save {};
	for (size_t i = 0; i < gp_count && i < 6; i++)
	{
		reg_save.gp[i] = gp_args[i];
	}
	uint64_t overflow_area[8] = {};
	Kyty::Libs::VaList va_list {};
	va_list.gp_offset         = offsetof(Kyty::Libs::VaRegSave, gp);
	va_list.fp_offset         = offsetof(Kyty::Libs::VaRegSave, fp);
	va_list.reg_save_area     = &reg_save;
	va_list.overflow_arg_area = overflow_area;
	return Kyty::Libs::LibC::c_vsnprintf(s, n, format, &va_list);
}

TEST(EmulatorLibcString, VsnprintfTruncatesAndReportsFullLength)
{
	char        destination[16] = {};
	const char* format          = "value=%08x";

	const uint64_t args[] = {0x00001234u};
	const int written = GuestVsnprintf(destination, sizeof(destination), format, args, 1);
	EXPECT_STREQ(destination, "value=00001234");
	EXPECT_EQ(written, 14);
}

TEST(EmulatorLibcString, VsnprintfHonorsSmallBuffer)
{
	char        destination[8] = {};
	const char* format          = "%i-%i";

	const uint64_t args[] = {1234u, 5678u};
	const int written = GuestVsnprintf(destination, sizeof(destination), format, args, 2);
	// "1234-5678" is 9 chars; the buffer holds 7 + terminator.
	EXPECT_STREQ(destination, "1234-56");
	EXPECT_EQ(written, 9);
}

TEST(EmulatorLibcString, VsnprintfNullBufferOrFormat)
{
	const char* format = "x=%i";

	// A null format is rejected with the C error return.
	const uint64_t args_one[] = {1u};
	EXPECT_LT(GuestVsnprintf(nullptr, 0, nullptr, args_one, 1), 0);

	// vsnprintf(nullptr, 0, ...) is valid C: it measures the would-be length
	// without writing. "x=42" is 4 characters.
	const uint64_t args_measure[] = {42u};
	EXPECT_EQ(GuestVsnprintf(nullptr, 0, format, args_measure, 1), 4);
}

UT_END();
