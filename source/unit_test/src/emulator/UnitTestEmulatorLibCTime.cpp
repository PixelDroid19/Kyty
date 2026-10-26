#include "Kyty/UnitTest.h"

#include "Emulator/Libs/LibCTime.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

UT_BEGIN(EmulatorLibCTime);

TEST(EmulatorLibCTime, GuestTmRoundTripsHostFields)
{
	std::tm host {};
	host.tm_sec   = 1;
	host.tm_min   = 2;
	host.tm_hour  = 3;
	host.tm_mday  = 4;
	host.tm_mon   = 5;
	host.tm_year  = 124;
	host.tm_wday  = 6;
	host.tm_yday  = 123;
	host.tm_isdst = -1;

	Kyty::Libs::LibC::Time::GuestTm guest {};
	Kyty::Libs::LibC::Time::HostToGuestTm(host, &guest);
	const auto roundtrip = Kyty::Libs::LibC::Time::GuestToHostTm(guest);

	EXPECT_EQ(roundtrip.tm_sec, host.tm_sec);
	EXPECT_EQ(roundtrip.tm_min, host.tm_min);
	EXPECT_EQ(roundtrip.tm_hour, host.tm_hour);
	EXPECT_EQ(roundtrip.tm_mday, host.tm_mday);
	EXPECT_EQ(roundtrip.tm_mon, host.tm_mon);
	EXPECT_EQ(roundtrip.tm_year, host.tm_year);
	EXPECT_EQ(roundtrip.tm_wday, host.tm_wday);
	EXPECT_EQ(roundtrip.tm_yday, host.tm_yday);
	EXPECT_EQ(roundtrip.tm_isdst, host.tm_isdst);
}

TEST(EmulatorLibCTime, RejectsNullTimeInputs)
{
	EXPECT_EQ(Kyty::Libs::LibC::Time::c_gmtime_s(nullptr, nullptr), nullptr);
	EXPECT_EQ(Kyty::Libs::LibC::Time::c_localtime_s(nullptr, nullptr), nullptr);
	EXPECT_EQ(Kyty::Libs::LibC::Time::c_mktime(nullptr), -1);
	EXPECT_EQ(Kyty::Libs::LibC::Time::c_strftime(nullptr, 0, nullptr, nullptr), 0u);
	EXPECT_EQ(Kyty::Libs::LibC::Time::c_asctime(nullptr), nullptr);
}

TEST(EmulatorLibCTime, AsctimeSFormatsGuestTmAndValidatesArguments)
{
	Kyty::Libs::LibC::Time::GuestTm guest {};
	guest.sec   = 5;
	guest.min   = 4;
	guest.hour  = 3;
	guest.mday  = 2;
	guest.mon   = 0;
	guest.year  = 124;
	guest.wday  = 2;
	guest.yday  = 1;
	guest.isdst = 0;

	char buffer[32] = {};
	EXPECT_EQ(Kyty::Libs::LibC::Time::c_asctime_s(buffer, sizeof(buffer), &guest), 0);
	EXPECT_STREQ(buffer, "Tue Jan  2 03:04:05 2024\n");
	EXPECT_EQ(Kyty::Libs::LibC::Time::c_asctime_s(nullptr, sizeof(buffer), &guest), 22);
	EXPECT_EQ(Kyty::Libs::LibC::Time::c_asctime_s(buffer, 25, &guest), 22);
	EXPECT_EQ(Kyty::Libs::LibC::Time::c_asctime_s(buffer, sizeof(buffer), nullptr), 22);

	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libc_1", &symbols));
	Kyty::Loader::SymbolResolve query {};
	query.name                 = Kyty::Loader::EncodeNameAsNid("asctime_s");
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Kyty::Loader::SymbolType::Func;
	EXPECT_NE(symbols.Find(query), nullptr);
}

UT_END();
