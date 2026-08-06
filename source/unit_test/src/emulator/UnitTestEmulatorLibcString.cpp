#include "Kyty/UnitTest.h"

#include "Emulator/Libs/VaContext.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// HLE bodies live in LibCString.cpp; declare the tested surface here so the
// unit test links against the emulator's own implementations.
namespace Kyty::Libs::LibC {
int c_strcasecmp(const char* a, const char* b);
int c_strncasecmp(const char* a, const char* b, size_t count);
int c_vsnprintf(char* s, size_t n, const char* fmt, Kyty::Libs::VaList* ap);
} // namespace Kyty::Libs::LibC

UT_BEGIN(EmulatorLibcString);

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
