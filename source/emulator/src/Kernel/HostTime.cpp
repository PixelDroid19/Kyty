#include "Emulator/Kernel/HostTime.h"

#include "Kyty/Core/Common.h"

namespace Kyty::Kernel::HostTime {

namespace {

int64_t FloorDiv(int64_t numerator, int64_t denominator)
{
	const int64_t quotient  = numerator / denominator;
	const int64_t remainder = numerator % denominator;
	return (remainder != 0 && ((remainder > 0) != (denominator > 0))) ? quotient - 1 : quotient;
}

int64_t DaysFromCivil(int64_t year, uint32_t month, uint32_t day)
{
	year -= month <= 2 ? 1 : 0;
	const int64_t era = FloorDiv(year, 400);
	const uint32_t year_of_era = static_cast<uint32_t>(year - era * 400);
	const uint32_t day_of_year = (153 * (month + (month > 2 ? static_cast<uint32_t>(-3) : 9)) + 2) / 5 + day - 1;
	const uint32_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
	return era * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

} // namespace

int64_t CivilToUnixSeconds(const std::tm& value)
{
	const int64_t days = DaysFromCivil(static_cast<int64_t>(value.tm_year) + 1900, static_cast<uint32_t>(value.tm_mon) + 1,
	                                   static_cast<uint32_t>(value.tm_mday));
	return days * 86400 + static_cast<int64_t>(value.tm_hour) * 3600 + static_cast<int64_t>(value.tm_min) * 60 +
	       static_cast<int64_t>(value.tm_sec);
}

bool LocaltimeFromUtc(int64_t utc_seconds, std::tm* out)
{
	if (out == nullptr)
	{
		return false;
	}

	const auto raw_time = static_cast<std::time_t>(utc_seconds);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return localtime_s(out, &raw_time) == 0;
#else
	return localtime_r(&raw_time, out) != nullptr;
#endif
}

bool GmtimeFromUnixSeconds(int64_t seconds, std::tm* out)
{
	if (out == nullptr)
	{
		return false;
	}

	const auto raw_time = static_cast<std::time_t>(seconds);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return gmtime_s(out, &raw_time) == 0;
#else
	return gmtime_r(&raw_time, out) != nullptr;
#endif
}

} // namespace Kyty::Kernel::HostTime
