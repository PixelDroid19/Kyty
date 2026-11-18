#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GpuClockMark.h"

#include <cstdint>

UT_BEGIN(EmulatorGraphicsGpuClockMark);

using namespace Libs::Graphics;

// 52.083 ns ticks (19.2 MHz) against a nanosecond host counter.
constexpr double   kPeriodNs = 1e9 / 19200000.0;
constexpr uint64_t kHostHz   = 1000000000u;

TEST(EmulatorGraphicsGpuClockMark, ConvertsMarksAfterAndBeforeTheCalibration)
{
	const GpuClockCalibration calibration {1000000, 5000000000u};
	// 19,200 ticks is exactly 1 ms.
	EXPECT_EQ(GpuClockMarkHostCounter(calibration, 1000000 + 19200, 36, kPeriodNs, kHostHz), 5001000000u);
	EXPECT_EQ(GpuClockMarkHostCounter(calibration, 1000000 - 19200, 36, kPeriodNs, kHostHz), 4999000000u);
	EXPECT_EQ(GpuClockMarkHostCounter(calibration, 1000000, 36, kPeriodNs, kHostHz), 5000000000u);
}

TEST(EmulatorGraphicsGpuClockMark, FollowsTheTimestampAcrossItsWrap)
{
	constexpr uint64_t kTop = (uint64_t {1} << 36) - 9600;
	const GpuClockCalibration calibration {kTop, 7000000000u};
	// 9,600 ticks to the wrap and 9,600 past it: 1 ms after the calibration.
	EXPECT_EQ(GpuClockMarkHostCounter(calibration, 9600, 36, kPeriodNs, kHostHz), 7001000000u);
	const GpuClockCalibration after_wrap {9600, 7001000000u};
	EXPECT_EQ(GpuClockMarkHostCounter(after_wrap, kTop, 36, kPeriodNs, kHostHz), 7000000000u);
}

TEST(EmulatorGraphicsGpuClockMark, ScalesToTheHostCounterRateAndStopsAtItsStart)
{
	// A 10 MHz host counter: 1 ms is 10,000 counts.
	const GpuClockCalibration calibration {500000, 20000};
	EXPECT_EQ(GpuClockMarkHostCounter(calibration, 500000 + 19200, 64, kPeriodNs, 10000000u), 30000u);
	EXPECT_EQ(GpuClockMarkHostCounter(calibration, 500000 - 2 * 19200, 64, kPeriodNs, 10000000u), 0u);
}

UT_END();
