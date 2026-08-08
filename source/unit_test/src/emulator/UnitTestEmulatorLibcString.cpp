#include "Kyty/UnitTest.h"

#include "Emulator/VideoFrameMemory.h"
#include "Emulator/Libs/VaContext.h"
#include "Kyty/Core/VirtualMemory.h"

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

} // namespace

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
