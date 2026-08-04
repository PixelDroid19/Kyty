#include "Kyty/UnitTest.h"

#include "Emulator/Kernel/HostTime.h"
#include "Emulator/Kernel/TimePort.h"

#include <ctime>

UT_BEGIN(EmulatorKernelTime);

namespace {

double TestTimeMs()
{
	return 12.5;
}

uint64_t TestCounter()
{
	return 0x1234u;
}

uint64_t TestFrequency()
{
	return 1000000u;
}

} // namespace

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

TEST(EmulatorKernelTime, TimePortUsesInstalledProviderAndRestoresFallback)
{
	Kyty::Kernel::TimePort::Install({TestTimeMs, TestCounter, TestFrequency});

	EXPECT_DOUBLE_EQ(Kyty::Kernel::TimePort::GetTimeMs(), 12.5);
	EXPECT_EQ(Kyty::Kernel::TimePort::GetCounter(), 0x1234u);
	EXPECT_EQ(Kyty::Kernel::TimePort::GetFrequency(), 1000000u);

	Kyty::Kernel::TimePort::Install({});
	EXPECT_GT(Kyty::Kernel::TimePort::GetFrequency(), 0u);
}

UT_END();
