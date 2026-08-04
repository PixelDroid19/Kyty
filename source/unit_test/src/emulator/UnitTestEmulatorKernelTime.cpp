#include "Kyty/UnitTest.h"

#include "Emulator/Kernel/HostTime.h"

#include <ctime>

UT_BEGIN(EmulatorKernelTime);

TEST(EmulatorKernelTime, ConvertsCivilEpochAndLeapDayWithoutGuestState)
{
	std::tm epoch {};
	epoch.tm_year = 70;
	epoch.tm_mon  = 0;
	epoch.tm_mday = 1;
	EXPECT_EQ(Kyty::Kernel::HostTime::CivilToUnixSeconds(epoch), 0);

	std::tm leap_day {};
	leap_day.tm_year = 120;
	leap_day.tm_mon  = 1;
	leap_day.tm_mday = 29;
	EXPECT_EQ(Kyty::Kernel::HostTime::CivilToUnixSeconds(leap_day), 1582934400);
}

TEST(EmulatorKernelTime, RejectsNullCalendarOutputs)
{
	EXPECT_FALSE(Kyty::Kernel::HostTime::LocaltimeFromUtc(0, nullptr));
	EXPECT_FALSE(Kyty::Kernel::HostTime::GmtimeFromUnixSeconds(0, nullptr));
}

UT_END();
