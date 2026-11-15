#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <cstdint>
#include <cstring>

UT_BEGIN(EmulatorRtc);

namespace {

using namespace Kyty;

using FormatRfc3339 = int (*)(char*, const uint64_t*, int);
using ParseRfc3339  = int (*)(uint64_t*, const char*);

constexpr int      RTC_ERROR_INVALID_POINTER  = static_cast<int>(0x80b50002);
constexpr int      RTC_ERROR_INVALID_ARGUMENT = static_cast<int>(0x80b50003);
constexpr int      RTC_ERROR_BAD_PARSE        = static_cast<int>(0x80b50007);
constexpr int      RTC_ERROR_INVALID_YEAR     = static_cast<int>(0x80b50008);
constexpr int      RTC_ERROR_INVALID_DAY      = static_cast<int>(0x80b5000a);
constexpr uint64_t UNIX_EPOCH_TICK            = 62'135'596'800'000'000ULL;
// 2024-02-29T12:34:56.789000Z, in microseconds from 0001-01-01.
constexpr uint64_t LEAP_DAY_TICK = UNIX_EPOCH_TICK + 1'709'210'096'789'000ULL;

void EnsureLog()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

template <typename Function>
Function ResolveRtcFunction(Loader::SymbolDatabase* symbols, const char32_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"Rtc";
	query.library_version      = 1;
	query.module               = U"Rtc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* rec            = symbols->Find(query);
	return rec != nullptr ? reinterpret_cast<Function>(rec->vaddr) : nullptr;
}

} // namespace

TEST(EmulatorRtc, FormatRfc3339WritesCentisecondsAndTheZone)
{
	EnsureLog();
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSceRtc_1", &symbols));
	auto* format = ResolveRtcFunction<FormatRfc3339>(&symbols, U"WJ3rqFwymew");
	ASSERT_NE(format, nullptr);

	char           text[32] {};
	const uint64_t tick = LEAP_DAY_TICK;
	ASSERT_EQ(format(text, &tick, 0), 0);
	EXPECT_STREQ(text, "2024-02-29T12:34:56.78Z");
	ASSERT_EQ(format(text, &tick, 90), 0);
	EXPECT_STREQ(text, "2024-02-29T14:04:56.78+01:30");
	ASSERT_EQ(format(text, &tick, -1439), 0);
	EXPECT_STREQ(text, "2024-02-28T12:35:56.78-23:59");

	std::memset(text, 'x', sizeof(text));
	EXPECT_EQ(format(text, &tick, 1440), RTC_ERROR_INVALID_ARGUMENT);
	EXPECT_EQ(text[0], 'x');
	const uint64_t year_one = 0;
	EXPECT_EQ(format(text, &year_one, -1), RTC_ERROR_INVALID_ARGUMENT);
	const uint64_t past_9999 = 3'652'059ULL * 86'400'000'000ULL;
	EXPECT_EQ(format(text, &past_9999, 0), RTC_ERROR_INVALID_YEAR);
	EXPECT_EQ(format(nullptr, &tick, 0), RTC_ERROR_INVALID_POINTER);
}

TEST(EmulatorRtc, ParseRfc3339ReadsTheZoneAndRequiresIt)
{
	EnsureLog();
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSceRtc_1", &symbols));
	auto* parse  = ResolveRtcFunction<ParseRfc3339>(&symbols, U"99bMGglFW3I");
	auto* format = ResolveRtcFunction<FormatRfc3339>(&symbols, U"WJ3rqFwymew");
	ASSERT_NE(parse, nullptr);
	ASSERT_NE(format, nullptr);

	uint64_t tick = 0;
	ASSERT_EQ(parse(&tick, "2024-02-29T14:04:56.789+01:30"), 0);
	EXPECT_EQ(tick, LEAP_DAY_TICK);
	ASSERT_EQ(parse(&tick, "2024-02-29t12:34:56.789z"), 0);
	EXPECT_EQ(tick, LEAP_DAY_TICK);
	ASSERT_EQ(parse(&tick, "1970-01-01T00:00:00Z"), 0);
	EXPECT_EQ(tick, UNIX_EPOCH_TICK);
	ASSERT_EQ(parse(&tick, "2024-02-29T12:34:56.1234567Z trailing"), 0);
	EXPECT_EQ(tick, LEAP_DAY_TICK - 789'000ULL + 123'456ULL);

	tick = 7;
	EXPECT_EQ(parse(&tick, "2024-02-29T12:34:56"), RTC_ERROR_BAD_PARSE);
	EXPECT_EQ(parse(&tick, "2024-2-29T12:34:56Z"), RTC_ERROR_BAD_PARSE);
	EXPECT_EQ(parse(&tick, "2023-02-29T00:00:00Z"), RTC_ERROR_INVALID_DAY);
	EXPECT_EQ(tick, 7u);
	EXPECT_EQ(parse(nullptr, "1970-01-01T00:00:00Z"), RTC_ERROR_INVALID_POINTER);

	// Formatting keeps centiseconds, so a round trip drops the rest of the fraction.
	char text[32] {};
	for (const int zone: {0, 1, -1, 59, -60, 90, -300, 1439, -1439})
	{
		const uint64_t source = LEAP_DAY_TICK;
		ASSERT_EQ(format(text, &source, zone), 0);
		ASSERT_EQ(parse(&tick, text), 0);
		EXPECT_EQ(tick, LEAP_DAY_TICK - 9'000ULL);
	}
}

UT_END();
