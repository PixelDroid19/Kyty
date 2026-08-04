#include "Emulator/Libs/LibCTime.h"

#include "Kyty/Core/DbgAssert.h"

#include <ctime>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibC::Time {

void HostToGuestTm(const std::tm& host, GuestTm* guest)
{
	EXIT_IF(guest == nullptr);
	guest->sec   = host.tm_sec;
	guest->min   = host.tm_min;
	guest->hour  = host.tm_hour;
	guest->mday  = host.tm_mday;
	guest->mon   = host.tm_mon;
	guest->year  = host.tm_year;
	guest->wday  = host.tm_wday;
	guest->yday  = host.tm_yday;
	guest->isdst = host.tm_isdst;
}

std::tm GuestToHostTm(const GuestTm& guest)
{
	std::tm host {};
	host.tm_sec   = guest.sec;
	host.tm_min   = guest.min;
	host.tm_hour  = guest.hour;
	host.tm_mday  = guest.mday;
	host.tm_mon   = guest.mon;
	host.tm_year  = guest.year;
	host.tm_wday  = guest.wday;
	host.tm_yday  = guest.yday;
	host.tm_isdst = guest.isdst;
	return host;
}

namespace {

bool HostGmtime(time_t value, std::tm* output)
{
	EXIT_IF(output == nullptr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return ::gmtime_s(output, &value) == 0;
#else
	return ::gmtime_r(&value, output) != nullptr;
#endif
}

bool HostLocaltime(time_t value, std::tm* output)
{
	EXIT_IF(output == nullptr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return ::localtime_s(output, &value) == 0;
#else
	return ::localtime_r(&value, output) != nullptr;
#endif
}

GuestTm* ConvertTime(const int64_t* value, GuestTm* output, bool utc)
{
	if (value == nullptr || output == nullptr)
	{
		return nullptr;
	}
	const time_t host_value = static_cast<time_t>(*value);
	std::tm    host {};
	if (!(utc ? HostGmtime(host_value, &host) : HostLocaltime(host_value, &host)))
	{
		return nullptr;
	}
	HostToGuestTm(host, output);
	return output;
}

} // namespace

KYTY_SYSV_ABI int64_t c_time(int64_t* t)
{
	time_t r = ::time(nullptr);
	if (t) *t = r;
	return r;
}

KYTY_SYSV_ABI int64_t c_mktime(GuestTm* tmv)
{
	if (tmv == nullptr)
	{
		return -1;
	}
	std::tm       host  = GuestToHostTm(*tmv);
	const auto value = ::mktime(&host);
	if (value != static_cast<time_t>(-1))
	{
		HostToGuestTm(host, tmv);
	}
	return static_cast<int64_t>(value);
}

KYTY_SYSV_ABI GuestTm* c_gmtime(const int64_t* t)
{
	static thread_local GuestTm result {};
	return ConvertTime(t, &result, true);
}

KYTY_SYSV_ABI GuestTm* c_gmtime_s(const int64_t* t, GuestTm* result)
{
	return ConvertTime(t, result, true);
}

KYTY_SYSV_ABI GuestTm* c_localtime(const int64_t* t)
{
	static thread_local GuestTm result {};
	return ConvertTime(t, &result, false);
}

KYTY_SYSV_ABI GuestTm* c_localtime_s(const int64_t* t, GuestTm* result)
{
	return ConvertTime(t, result, false);
}

KYTY_SYSV_ABI size_t c_strftime(char* s, size_t n, const char* f, const GuestTm* tmv)
{
	if (tmv == nullptr)
	{
		return 0;
	}
	const std::tm host = GuestToHostTm(*tmv);
	return ::strftime(s, n, f, &host);
}

KYTY_SYSV_ABI char* c_asctime(const GuestTm* tmv)
{
	if (tmv == nullptr)
	{
		return nullptr;
	}
	const std::tm host = GuestToHostTm(*tmv);
	return ::asctime(&host);
}

} // namespace Kyty::Libs::LibC::Time

#endif // KYTY_EMU_ENABLED
