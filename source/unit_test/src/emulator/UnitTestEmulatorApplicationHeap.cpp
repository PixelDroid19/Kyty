#include "Kyty/UnitTest.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/ApplicationHeap.h"

UT_BEGIN(EmulatorApplicationHeap);

using Kyty::Libs::LibKernel::ApplicationHeap::ApiV2;
using Kyty::Libs::LibKernel::ApplicationHeap::IsApiV2Header;
using Kyty::Libs::LibKernel::ApplicationHeap::IsGuestCodePointer;
using Kyty::Libs::LibKernel::ApplicationHeap::IsValidApiV2Table;
using Kyty::Libs::LibKernel::ApplicationHeap::kApiV2Size;
using Kyty::Libs::LibKernel::ApplicationHeap::kApiV2Version;

// Captured Astro Bot application heap table header at guest 0x9088e0148.
TEST(EmulatorApplicationHeap, RecognizesCapturedV2Header)
{
	EXPECT_EQ(kApiV2Size, 0x78u);
	EXPECT_EQ(kApiV2Version, 2u);
	EXPECT_TRUE(IsApiV2Header(kApiV2Size, kApiV2Version));
	EXPECT_FALSE(IsApiV2Header(kApiV2Size, 1u));
	EXPECT_FALSE(IsApiV2Header(0x70, kApiV2Version));
}

TEST(EmulatorApplicationHeap, CreateSlotMustPointIntoExecutableImage)
{
	constexpr uint64_t text_begin = 0x900000000ull;
	constexpr uint64_t text_end   = 0x908000000ull;

	EXPECT_TRUE(IsGuestCodePointer(0x90027cea0, text_begin, text_end));
	EXPECT_FALSE(IsGuestCodePointer(0x2, text_begin, text_end));
	EXPECT_FALSE(IsGuestCodePointer(0x9088e0158, text_begin, text_end));
}

// Captured Astro Bot v2 table at 0x9088e0148 (create/destroy/malloc/free in text).
TEST(EmulatorApplicationHeap, CapturedV2TablePassesFullValidation)
{
	constexpr uint64_t text_begin = 0x900000000ull;
	constexpr uint64_t text_end   = 0x908000000ull;

	ApiV2 table {};
	table.size    = kApiV2Size;
	table.version = kApiV2Version;
	table.create  = reinterpret_cast<void(KYTY_SYSV_ABI*)()>(0x90027cea0);
	table.destroy = reinterpret_cast<void(KYTY_SYSV_ABI*)()>(0x90027ced0);
	table.malloc  = reinterpret_cast<void*(KYTY_SYSV_ABI*)(size_t)>(0x90027cf00);
	table.free    = reinterpret_cast<void(KYTY_SYSV_ABI*)(void*)>(0x90027cf30);

	EXPECT_TRUE(IsValidApiV2Table(&table, text_begin, text_end));
}

TEST(EmulatorApplicationHeap, RejectsHeaderOnlyFalsePositive)
{
	constexpr uint64_t text_begin = 0x900000000ull;
	constexpr uint64_t text_end   = 0x908000000ull;

	ApiV2 table {};
	table.size    = kApiV2Size;
	table.version = kApiV2Version;
	table.create  = reinterpret_cast<void(KYTY_SYSV_ABI*)()>(0x90027cea0);
	// destroy/malloc/free left null — must not qualify for create.

	EXPECT_FALSE(IsValidApiV2Table(&table, text_begin, text_end));
}

UT_END();
