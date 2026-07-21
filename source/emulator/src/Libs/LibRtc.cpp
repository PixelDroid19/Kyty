#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Time.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <ctime>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Rtc", 1, "Rtc", 1, 1);

namespace Rtc {

constexpr int RTC_ERROR_INVALID_POINTER   = static_cast<int>(0x80b50002);
constexpr int RTC_ERROR_INVALID_ARGUMENT  = static_cast<int>(0x80b50003);
constexpr int RTC_ERROR_INVALID_DATE      = static_cast<int>(0x80b50004);
constexpr int RTC_ERROR_INVALID_YEAR      = static_cast<int>(0x80b50008);
constexpr int RTC_ERROR_INVALID_MONTH     = static_cast<int>(0x80b50009);
constexpr int RTC_ERROR_INVALID_DAY       = static_cast<int>(0x80b5000a);
constexpr int RTC_ERROR_INVALID_HOUR      = static_cast<int>(0x80b5000b);
constexpr int RTC_ERROR_INVALID_MINUTE    = static_cast<int>(0x80b5000c);
constexpr int RTC_ERROR_INVALID_SECOND    = static_cast<int>(0x80b5000d);
constexpr int RTC_ERROR_INVALID_MICROSECOND = static_cast<int>(0x80b5000e);

constexpr uint64_t MICROSECONDS_PER_SECOND = 1'000'000ULL;
constexpr uint64_t MICROSECONDS_PER_MINUTE = 60'000'000ULL;
constexpr uint64_t MICROSECONDS_PER_HOUR   = 3'600'000'000ULL;
constexpr uint64_t MICROSECONDS_PER_DAY    = 86'400'000'000ULL;
constexpr uint64_t MICROSECONDS_PER_WEEK   = 604'800'000'000ULL;
constexpr uint64_t UNIX_EPOCH_TICKS        = 62'135'596'800'000'000ULL;
constexpr uint64_t WIN32_FILETIME_EPOCH_TICKS = 50'491'123'200'000'000ULL;

struct RtcDateTime
{
	uint16_t year;
	uint16_t month;
	uint16_t day;
	uint16_t hour;
	uint16_t minute;
	uint16_t second;
	uint32_t microsecond;
};

static_assert(sizeof(RtcDateTime) == 16);

static void WriteRtcDateTime(RtcDateTime* out, const std::tm& tm, uint32_t microsecond)
{
	out->year        = static_cast<uint16_t>(tm.tm_year + 1900);
	out->month       = static_cast<uint16_t>(tm.tm_mon + 1);
	out->day         = static_cast<uint16_t>(tm.tm_mday);
	out->hour        = static_cast<uint16_t>(tm.tm_hour);
	out->minute      = static_cast<uint16_t>(tm.tm_min);
	out->second      = static_cast<uint16_t>(tm.tm_sec);
	out->microsecond = microsecond;
}

static std::time_t Timegm(std::tm* tm)
{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return _mkgmtime(tm);
#else
	return timegm(tm);
#endif
}

static bool LocaltimeFromUtc(std::time_t utc_seconds, std::tm* out)
{
	if (out == nullptr)
	{
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return localtime_s(out, &utc_seconds) == 0;
#else
	return localtime_r(&utc_seconds, out) != nullptr;
#endif
}

static bool GmtimeFromUtc(std::time_t utc_seconds, std::tm* out)
{
	if (out == nullptr)
	{
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return gmtime_s(out, &utc_seconds) == 0;
#else
	return gmtime_r(&utc_seconds, out) != nullptr;
#endif
}

static int DayOfWeek(int year, int month, int day)
{
	static constexpr int month_adjust[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
	if (month < 3)
	{
		--year;
	}
	return (year + year / 4 - year / 100 + year / 400 + month_adjust[month - 1] + day) % 7;
}

static int GetDaysInMonth(int year, int month)
{
	static constexpr int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month < 1 || month > 12)
	{
		return 0;
	}
	int days = days_in_month[month - 1];
	if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
	{
		++days;
	}
	return days;
}

static bool CalendarDateInRange(int year, int month, int day)
{
	if (year < 1 || year > 9999)
	{
		return false;
	}
	if (month < 1 || month > 12)
	{
		return false;
	}
	const int last_day = GetDaysInMonth(year, month);
	return day >= 1 && day <= last_day;
}

static int ValidateRtcDateTime(const RtcDateTime& time)
{
	if (time.year < 1 || time.year > 9999)
	{
		return RTC_ERROR_INVALID_YEAR;
	}
	if (time.month < 1 || time.month > 12)
	{
		return RTC_ERROR_INVALID_MONTH;
	}
	if (time.day < 1 || time.day > GetDaysInMonth(time.year, time.month))
	{
		return RTC_ERROR_INVALID_DAY;
	}
	if (time.hour > 23)
	{
		return RTC_ERROR_INVALID_HOUR;
	}
	if (time.minute > 59)
	{
		return RTC_ERROR_INVALID_MINUTE;
	}
	if (time.second > 59)
	{
		return RTC_ERROR_INVALID_SECOND;
	}
	if (time.microsecond > 999'999)
	{
		return RTC_ERROR_INVALID_MICROSECOND;
	}
	return OK;
}

static bool RtcDateTimeToTick(const RtcDateTime& time, uint64_t* tick_out)
{
	if (tick_out == nullptr)
	{
		return false;
	}

	uint32_t year  = time.year;
	uint32_t month = time.month;
	if (month > 2)
	{
		month -= 3;
	} else
	{
		month += 9;
		year -= 1;
	}

	const uint32_t century         = year / 100;
	const uint32_t year_of_century = year - (100 * century);
	uint64_t       days            = ((146097ULL * century) >> 2) + ((1461ULL * year_of_century) >> 2) +
	                      ((153ULL * month + 2ULL) / 5ULL) + time.day;
	days -= 307ULL;
	days *= MICROSECONDS_PER_DAY;

	const uint64_t time_of_day = static_cast<uint64_t>(time.hour) * MICROSECONDS_PER_HOUR +
	                             static_cast<uint64_t>(time.minute) * MICROSECONDS_PER_MINUTE +
	                             static_cast<uint64_t>(time.second) * MICROSECONDS_PER_SECOND + time.microsecond;
	*tick_out = days + time_of_day;
	return true;
}

static bool TickToRtcDateTime(uint64_t tick, RtcDateTime* out)
{
	if (out == nullptr)
	{
		return false;
	}

	const uint64_t time_of_day = tick % MICROSECONDS_PER_DAY;
	uint64_t       z           = tick / MICROSECONDS_PER_DAY + 307ULL;

	const int64_t  era = static_cast<int64_t>((z >= 0 ? z : z - 146096ULL) / 146097ULL);
	const uint32_t doe = static_cast<uint32_t>(z - static_cast<uint64_t>(era) * 146097ULL);
	const uint32_t yoe = (doe - doe / 1460ULL + doe / 36524ULL - doe / 146096ULL) / 365ULL;
	uint32_t       y   = yoe + static_cast<uint32_t>(era) * 400U;
	const uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
	const uint32_t mp  = (5U * doy + 2U) / 153U;
	const uint32_t d   = doy - (153U * mp + 2U) / 5U + 1U;
	uint32_t       m   = mp < 10U ? mp + 3U : mp - 9U;
	if (mp >= 10U)
	{
		++y;
	}

	out->year        = static_cast<uint16_t>(y);
	out->month       = static_cast<uint16_t>(m);
	out->day         = static_cast<uint16_t>(d);
	out->hour        = static_cast<uint16_t>(time_of_day / MICROSECONDS_PER_HOUR);
	const uint64_t remainder_hour = time_of_day % MICROSECONDS_PER_HOUR;
	out->minute = static_cast<uint16_t>(remainder_hour / MICROSECONDS_PER_MINUTE);
	const uint64_t remainder_minute = remainder_hour % MICROSECONDS_PER_MINUTE;
	out->second      = static_cast<uint16_t>(remainder_minute / MICROSECONDS_PER_SECOND);
	out->microsecond = static_cast<uint32_t>(remainder_minute % MICROSECONDS_PER_SECOND);
	return true;
}

static std::tm RtcDateTimeToTmUtc(const RtcDateTime& time)
{
	std::tm tm {};
	tm.tm_year = static_cast<int>(time.year) - 1900;
	tm.tm_mon  = static_cast<int>(time.month) - 1;
	tm.tm_mday = static_cast<int>(time.day);
	tm.tm_hour = static_cast<int>(time.hour);
	tm.tm_min  = static_cast<int>(time.minute);
	tm.tm_sec  = static_cast<int>(time.second);
	tm.tm_isdst = 0;
	return tm;
}

static bool UtcTimeToRtcDateTime(std::time_t utc_seconds, uint32_t microsecond, RtcDateTime* out)
{
	if (out == nullptr)
	{
		return false;
	}
	std::tm tm {};
	if (!GmtimeFromUtc(utc_seconds, &tm))
	{
		return false;
	}
	WriteRtcDateTime(out, tm, microsecond);
	return true;
}

static bool UtcRtcDateTimeToLocalRtcDateTime(const RtcDateTime& utc, RtcDateTime* local_out)
{
	std::tm utc_tm = RtcDateTimeToTmUtc(utc);
	const std::time_t seconds = Timegm(&utc_tm);
	if (seconds == static_cast<std::time_t>(-1))
	{
		return false;
	}

	std::tm local_tm {};
	if (!LocaltimeFromUtc(seconds, &local_tm))
	{
		return false;
	}

	WriteRtcDateTime(local_out, local_tm, utc.microsecond);
	return true;
}

static bool LocalRtcDateTimeToUtcRtcDateTime(const RtcDateTime& local, RtcDateTime* utc_out)
{
	std::tm local_tm = RtcDateTimeToTmUtc(local);
	local_tm.tm_isdst = -1;
	const std::time_t seconds = std::mktime(&local_tm);
	if (seconds == static_cast<std::time_t>(-1))
	{
		return false;
	}

	std::tm utc_tm {};
	if (!GmtimeFromUtc(seconds, &utc_tm))
	{
		return false;
	}

	WriteRtcDateTime(utc_out, utc_tm, local.microsecond);
	return true;
}

static bool AddMonthsToRtcDateTime(RtcDateTime& time, int delta_months)
{
	const int total_months = static_cast<int>(time.year) * 12 + static_cast<int>(time.month) - 1 + delta_months;
	int       year         = total_months / 12;
	int       month        = total_months % 12;
	if (month < 0)
	{
		month += 12;
		--year;
	}
	++month;

	if (year < 1 || year > 9999)
	{
		return false;
	}

	const int max_day = GetDaysInMonth(year, month);
	if (static_cast<int>(time.day) > max_day)
	{
		time.day = static_cast<uint16_t>(max_day);
	}
	time.year  = static_cast<uint16_t>(year);
	time.month = static_cast<uint16_t>(month);
	return true;
}

static bool AddYearsToRtcDateTime(RtcDateTime& time, int delta_years)
{
	const int year = static_cast<int>(time.year) + delta_years;
	if (year < 1 || year > 9999)
	{
		return false;
	}

	const int max_day = GetDaysInMonth(year, static_cast<int>(time.month));
	if (static_cast<int>(time.day) > max_day)
	{
		time.day = static_cast<uint16_t>(max_day);
	}
	time.year = static_cast<uint16_t>(year);
	return true;
}

static bool CheckedTickDelta(uint64_t source_tick, int64_t delta, uint64_t microseconds_per_unit, uint64_t* result_out)
{
	if (delta == 0)
	{
		*result_out = source_tick;
		return true;
	}

	if (delta > 0)
	{
		if (microseconds_per_unit != 0 && static_cast<uint64_t>(delta) > ~0ULL / microseconds_per_unit)
		{
			return false;
		}
		const uint64_t addend = static_cast<uint64_t>(delta) * microseconds_per_unit;
		if (source_tick > ~0ULL - addend)
		{
			return false;
		}
		*result_out = source_tick + addend;
		return true;
	}

	const uint64_t abs_delta = static_cast<uint64_t>(-delta);
	if (microseconds_per_unit != 0 && abs_delta > ~0ULL / microseconds_per_unit)
	{
		return false;
	}
	const uint64_t subtrahend = abs_delta * microseconds_per_unit;
	if (source_tick < subtrahend)
	{
		return false;
	}
	*result_out = source_tick - subtrahend;
	return true;
}

static int AddTickDelta(uint64_t* destination, const uint64_t* source, int64_t delta, uint64_t microseconds_per_unit)
{
	if (destination == nullptr || source == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	uint64_t result_tick = 0;
	if (!CheckedTickDelta(*source, delta, microseconds_per_unit, &result_tick))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	*destination = result_tick;
	return OK;
}

static int AddCalendarDelta(uint64_t* destination, const uint64_t* source, int delta, bool months)
{
	if (destination == nullptr || source == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	RtcDateTime date_time {};
	if (!TickToRtcDateTime(*source, &date_time))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	const bool ok = months ? AddMonthsToRtcDateTime(date_time, delta) : AddYearsToRtcDateTime(date_time, delta);
	if (!ok)
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	uint64_t result_tick = 0;
	if (!RtcDateTimeToTick(date_time, &result_tick))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	*destination = result_tick;
	return OK;
}

static int KYTY_SYSV_ABI RtcInit()
{
	PRINT_NAME();
	return OK;
}

static int KYTY_SYSV_ABI RtcEnd()
{
	PRINT_NAME();
	return OK;
}

static int KYTY_SYSV_ABI RtcGetCurrentTick(uint64_t* tick)
{
	PRINT_NAME();
	if (tick == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	LibKernel::KernelTimeval tv {};
	if (LibKernel::KernelGettimeofday(&tv) != OK)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	RtcDateTime date_time {};
	if (!UtcTimeToRtcDateTime(static_cast<std::time_t>(tv.tv_sec), static_cast<uint32_t>(tv.tv_usec), &date_time))
	{
		return RTC_ERROR_INVALID_POINTER;
	}
	if (!RtcDateTimeToTick(date_time, tick))
	{
		return RTC_ERROR_INVALID_POINTER;
	}
	return OK;
}

static int KYTY_SYSV_ABI RtcGetCurrentAdNetworkTick(uint64_t* tick)
{
	return RtcGetCurrentTick(tick);
}

static int KYTY_SYSV_ABI RtcGetCurrentDebugNetworkTick(uint64_t* tick)
{
	return RtcGetCurrentTick(tick);
}

static int KYTY_SYSV_ABI RtcGetCurrentNetworkTick(uint64_t* tick)
{
	return RtcGetCurrentTick(tick);
}

static int KYTY_SYSV_ABI RtcGetCurrentRawNetworkTick(uint64_t* tick)
{
	return RtcGetCurrentTick(tick);
}

static int KYTY_SYSV_ABI RtcGetCurrentClock(RtcDateTime* time, int time_zone_minutes)
{
	PRINT_NAME();
	if (time == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	LibKernel::KernelTimeval tv {};
	if (LibKernel::KernelGettimeofday(&tv) != OK)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	const int64_t adjusted_seconds = static_cast<int64_t>(tv.tv_sec) + static_cast<int64_t>(time_zone_minutes) * 60;
	return UtcTimeToRtcDateTime(static_cast<std::time_t>(adjusted_seconds), static_cast<uint32_t>(tv.tv_usec), time)
	           ? OK
	           : RTC_ERROR_INVALID_ARGUMENT;
}

static int KYTY_SYSV_ABI RtcGetCurrentClockLocalTime(RtcDateTime* time)
{
	PRINT_NAME();
	if (time == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	LibKernel::KernelTimeval tv {};
	if (LibKernel::KernelGettimeofday(&tv) != OK)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	std::tm local_tm {};
	if (!LocaltimeFromUtc(static_cast<std::time_t>(tv.tv_sec), &local_tm))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	WriteRtcDateTime(time, local_tm, static_cast<uint32_t>(tv.tv_usec));
	return OK;
}

static int KYTY_SYSV_ABI RtcCheckValid(const RtcDateTime* time)
{
	PRINT_NAME();
	if (time == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}
	return ValidateRtcDateTime(*time);
}

static int KYTY_SYSV_ABI RtcCompareTick(const uint64_t* tick1, const uint64_t* tick2)
{
	PRINT_NAME();
	if (tick1 == nullptr || tick2 == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	const uint64_t left  = *tick1;
	const uint64_t right = *tick2;
	if (left < right)
	{
		return -1;
	}
	if (left > right)
	{
		return 1;
	}
	return 0;
}

static int KYTY_SYSV_ABI RtcGetTick(const RtcDateTime* time, uint64_t* tick)
{
	PRINT_NAME();
	if (time == nullptr || tick == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	const int validation = ValidateRtcDateTime(*time);
	if (validation != OK)
	{
		return validation;
	}

	return RtcDateTimeToTick(*time, tick) ? OK : RTC_ERROR_INVALID_ARGUMENT;
}

static int KYTY_SYSV_ABI RtcSetTick(RtcDateTime* time, const uint64_t* tick)
{
	PRINT_NAME();
	if (time == nullptr || tick == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	if (!TickToRtcDateTime(*tick, time))
	{
		return RTC_ERROR_INVALID_DATE;
	}
	return OK;
}

static int KYTY_SYSV_ABI RtcConvertLocalTimeToUtc(const uint64_t* tick_local, uint64_t* tick_utc)
{
	PRINT_NAME();
	if (tick_local == nullptr || tick_utc == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	RtcDateTime local_time {};
	if (!TickToRtcDateTime(*tick_local, &local_time))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	RtcDateTime utc_time {};
	if (!LocalRtcDateTimeToUtcRtcDateTime(local_time, &utc_time))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	return RtcDateTimeToTick(utc_time, tick_utc) ? OK : RTC_ERROR_INVALID_ARGUMENT;
}

static int KYTY_SYSV_ABI RtcConvertUtcToLocalTime(const uint64_t* tick_utc, uint64_t* tick_local)
{
	PRINT_NAME();
	if (tick_utc == nullptr || tick_local == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	RtcDateTime utc_time {};
	if (!TickToRtcDateTime(*tick_utc, &utc_time))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	RtcDateTime local_time {};
	if (!UtcRtcDateTimeToLocalRtcDateTime(utc_time, &local_time))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	return RtcDateTimeToTick(local_time, tick_local) ? OK : RTC_ERROR_INVALID_ARGUMENT;
}

static int KYTY_SYSV_ABI RtcGetDayOfWeek(int year, int month, int day)
{
	PRINT_NAME();
	if (!CalendarDateInRange(year, month, day))
	{
		return RTC_ERROR_INVALID_DATE;
	}

	return DayOfWeek(year, month, day);
}

static int KYTY_SYSV_ABI RtcGetDaysInMonth(int year, int month)
{
	PRINT_NAME();
	if (year < 1 || year > 9999)
	{
		return RTC_ERROR_INVALID_YEAR;
	}
	if (month < 1 || month > 12)
	{
		return RTC_ERROR_INVALID_MONTH;
	}
	return GetDaysInMonth(year, month);
}

static int KYTY_SYSV_ABI RtcIsLeapYear(int year)
{
	PRINT_NAME();
	if (year < 1 || year > 9999)
	{
		return RTC_ERROR_INVALID_YEAR;
	}
	return ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 1 : 0;
}

static int KYTY_SYSV_ABI RtcGetTickResolution()
{
	PRINT_NAME();
	return static_cast<int>(MICROSECONDS_PER_SECOND);
}

static int KYTY_SYSV_ABI RtcGetDosTime(const RtcDateTime* time, uint32_t* dos_time)
{
	PRINT_NAME();
	if (time == nullptr || dos_time == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	const int validation = ValidateRtcDateTime(*time);
	if (validation != OK)
	{
		return validation;
	}

	uint32_t packed = 0;
	packed |= static_cast<uint32_t>((time->second / 2) & 0x1f);
	packed |= static_cast<uint32_t>(time->minute & 0x3f) << 5;
	packed |= static_cast<uint32_t>(time->hour & 0x1f) << 11;
	packed |= static_cast<uint32_t>(time->day & 0x1f) << 16;
	packed |= static_cast<uint32_t>(time->month & 0x0f) << 21;
	packed |= static_cast<uint32_t>((time->year - 1980) & 0x7f) << 25;
	*dos_time = packed;
	return OK;
}

static int KYTY_SYSV_ABI RtcSetDosTime(RtcDateTime* time, uint32_t dos_time)
{
	PRINT_NAME();
	if (time == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	time->year        = static_cast<uint16_t>(1980 + ((dos_time >> 25) & 0x7f));
	time->month       = static_cast<uint16_t>((dos_time >> 21) & 0x0f);
	time->day         = static_cast<uint16_t>((dos_time >> 16) & 0x1f);
	time->hour        = static_cast<uint16_t>((dos_time >> 11) & 0x1f);
	time->minute      = static_cast<uint16_t>((dos_time >> 5) & 0x3f);
	time->second      = static_cast<uint16_t>((dos_time & 0x1f) * 2);
	time->microsecond = 0;
	return OK;
}

static int KYTY_SYSV_ABI RtcGetTimeT(const RtcDateTime* time, uint64_t* time_t_out)
{
	PRINT_NAME();
	if (time == nullptr || time_t_out == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	const int validation = ValidateRtcDateTime(*time);
	if (validation != OK)
	{
		return validation;
	}

	uint64_t tick = 0;
	if (!RtcDateTimeToTick(*time, &tick))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	*time_t_out = tick < UNIX_EPOCH_TICKS ? 0ULL : (tick - UNIX_EPOCH_TICKS) / MICROSECONDS_PER_SECOND;
	return OK;
}

static int KYTY_SYSV_ABI RtcSetTimeT(RtcDateTime* time, int64_t time_seconds)
{
	PRINT_NAME();
	if (time == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}
	if (time_seconds < 0)
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	const uint64_t tick = UNIX_EPOCH_TICKS + static_cast<uint64_t>(time_seconds) * MICROSECONDS_PER_SECOND;
	return TickToRtcDateTime(tick, time) ? OK : RTC_ERROR_INVALID_ARGUMENT;
}

static int KYTY_SYSV_ABI RtcGetWin32FileTime(const RtcDateTime* time, uint64_t* file_time)
{
	PRINT_NAME();
	if (time == nullptr || file_time == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}

	const int validation = ValidateRtcDateTime(*time);
	if (validation != OK)
	{
		return validation;
	}

	uint64_t tick = 0;
	if (!RtcDateTimeToTick(*time, &tick))
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	*file_time = tick < WIN32_FILETIME_EPOCH_TICKS ? 0ULL : (tick - WIN32_FILETIME_EPOCH_TICKS) * 10ULL;
	return OK;
}

static int KYTY_SYSV_ABI RtcSetWin32FileTime(RtcDateTime* time, int64_t file_time)
{
	PRINT_NAME();
	if (time == nullptr)
	{
		return RTC_ERROR_INVALID_POINTER;
	}
	if (file_time < 0)
	{
		return RTC_ERROR_INVALID_ARGUMENT;
	}

	const uint64_t tick = WIN32_FILETIME_EPOCH_TICKS + static_cast<uint64_t>(file_time) / 10ULL;
	return TickToRtcDateTime(tick, time) ? OK : RTC_ERROR_INVALID_ARGUMENT;
}

static int KYTY_SYSV_ABI RtcTickAddDays(uint64_t* destination, const uint64_t* source, int64_t delta)
{
	PRINT_NAME();
	return AddTickDelta(destination, source, delta, MICROSECONDS_PER_DAY);
}

static int KYTY_SYSV_ABI RtcTickAddHours(uint64_t* destination, const uint64_t* source, int64_t delta)
{
	PRINT_NAME();
	return AddTickDelta(destination, source, delta, MICROSECONDS_PER_HOUR);
}

static int KYTY_SYSV_ABI RtcTickAddMinutes(uint64_t* destination, const uint64_t* source, int64_t delta)
{
	PRINT_NAME();
	return AddTickDelta(destination, source, delta, MICROSECONDS_PER_MINUTE);
}

static int KYTY_SYSV_ABI RtcTickAddSeconds(uint64_t* destination, const uint64_t* source, int64_t delta)
{
	PRINT_NAME();
	return AddTickDelta(destination, source, delta, MICROSECONDS_PER_SECOND);
}

static int KYTY_SYSV_ABI RtcTickAddMicroseconds(uint64_t* destination, const uint64_t* source, int64_t delta)
{
	PRINT_NAME();
	return AddTickDelta(destination, source, delta, 1ULL);
}

static int KYTY_SYSV_ABI RtcTickAddTicks(uint64_t* destination, const uint64_t* source, int64_t delta)
{
	PRINT_NAME();
	return AddTickDelta(destination, source, delta, 1ULL);
}

static int KYTY_SYSV_ABI RtcTickAddWeeks(uint64_t* destination, const uint64_t* source, int64_t delta)
{
	PRINT_NAME();
	return AddTickDelta(destination, source, delta, MICROSECONDS_PER_WEEK);
}

static int KYTY_SYSV_ABI RtcTickAddMonths(uint64_t* destination, const uint64_t* source, int delta)
{
	PRINT_NAME();
	return AddCalendarDelta(destination, source, delta, true);
}

static int KYTY_SYSV_ABI RtcTickAddYears(uint64_t* destination, const uint64_t* source, int delta)
{
	PRINT_NAME();
	return AddCalendarDelta(destination, source, delta, false);
}

} // namespace Rtc

LIB_DEFINE(InitRtc_1)
{
	LIB_FUNC("LlodCMDbk3o", Rtc::RtcInit);
	LIB_FUNC("8SljQx6pDP8", Rtc::RtcEnd);
	LIB_FUNC("18B2NS1y9UU", Rtc::RtcGetCurrentTick);
	LIB_FUNC("8lfvnRMqwEM", Rtc::RtcGetCurrentClock);
	LIB_FUNC("lPEBYdVX0XQ", Rtc::RtcCheckValid);
	LIB_FUNC("8w-H19ip48I", Rtc::RtcGetTick);
	LIB_FUNC("ueega6v3GUw", Rtc::RtcSetTick);
	LIB_FUNC("ZPD1YOKI+Kw", Rtc::RtcGetCurrentClockLocalTime);
	LIB_FUNC("fNaZ4DbzHAE", Rtc::RtcCompareTick);
	LIB_FUNC("8Yr143yEnRo", Rtc::RtcConvertLocalTimeToUtc);
	LIB_FUNC("M1TvFst-jrM", Rtc::RtcConvertUtcToLocalTime);
	LIB_FUNC("LN3Zcb72Q0c", Rtc::RtcGetCurrentAdNetworkTick);
	LIB_FUNC("Ot1DE3gif84", Rtc::RtcGetCurrentDebugNetworkTick);
	LIB_FUNC("zO9UL3qIINQ", Rtc::RtcGetCurrentNetworkTick);
	LIB_FUNC("HWxHOdbM-Pg", Rtc::RtcGetCurrentRawNetworkTick);
	LIB_FUNC("CyIK-i4XdgQ", Rtc::RtcGetDayOfWeek);
	LIB_FUNC("3O7Ln8AqJ1o", Rtc::RtcGetDaysInMonth);
	LIB_FUNC("E7AR4o7Ny7E", Rtc::RtcGetDosTime);
	LIB_FUNC("jMNwqYr4R-k", Rtc::RtcGetTickResolution);
	LIB_FUNC("BtqmpTRXHgk", Rtc::RtcGetTimeT);
	LIB_FUNC("jfRO0uTjtzA", Rtc::RtcGetWin32FileTime);
	LIB_FUNC("Ug8pCwQvh0c", Rtc::RtcIsLeapYear);
	LIB_FUNC("NR1J0N7L2xY", Rtc::RtcTickAddDays);
	LIB_FUNC("MDc5cd8HfCA", Rtc::RtcTickAddHours);
	LIB_FUNC("XPIiw58C+GM", Rtc::RtcTickAddMicroseconds);
	LIB_FUNC("mn-tf4QiFzk", Rtc::RtcTickAddMinutes);
	LIB_FUNC("CL6y9q-XbuQ", Rtc::RtcTickAddMonths);
	LIB_FUNC("07O525HgICs", Rtc::RtcTickAddSeconds);
	LIB_FUNC("AqVMssr52Rc", Rtc::RtcTickAddTicks);
	LIB_FUNC("gI4t194c2W8", Rtc::RtcTickAddWeeks);
	LIB_FUNC("-5y2uJ62qS8", Rtc::RtcTickAddYears);
	LIB_FUNC("aYPCd1cChyg", Rtc::RtcSetDosTime);
	LIB_FUNC("bDEVVP4bTjQ", Rtc::RtcSetTimeT);
	LIB_FUNC("n5JiAJXsbcs", Rtc::RtcSetWin32FileTime);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
