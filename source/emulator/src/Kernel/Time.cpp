#include "Emulator/Kernel/Time.h"
#include "Emulator/Kernel/HostTime.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Timer.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/Timer.h"

#include <ctime>
#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

LIB_NAME("libkernel", "libkernel");

namespace HostTime = ::Kyty::Kernel::HostTime;

// The errno table remains part of the HLE registration namespace for now. Keep
// this dependency explicit while the time implementation moves into Kernel.
using ::Kyty::Libs::LibKernel::KERNEL_ERROR_EFAULT;
using ::Kyty::Libs::LibKernel::KERNEL_ERROR_EINVAL;

static bool TimezoneFromUtc(int64_t utc_seconds, KernelTimezone* timezone, int32_t* dst_seconds)
{
	if (timezone == nullptr)
	{
		return false;
	}

	std::tm local {};
	if (!HostTime::LocaltimeFromUtc(utc_seconds, &local))
	{
		return false;
	}

	const int64_t local_seconds = HostTime::CivilToUnixSeconds(local);
	std::tm       standard_time = local;
	standard_time.tm_isdst      = 0;
	const auto standard_utc = std::mktime(&standard_time);
	if (standard_utc == static_cast<std::time_t>(-1))
	{
		return false;
	}

	const int64_t standard_offset_seconds = local_seconds - static_cast<int64_t>(standard_utc);
	const int64_t dst_delta_64            = local_seconds - utc_seconds - standard_offset_seconds;
	if (dst_delta_64 < std::numeric_limits<int32_t>::min() || dst_delta_64 > std::numeric_limits<int32_t>::max())
	{
		return false;
	}
	const int32_t dst_delta                = static_cast<int32_t>(dst_delta_64);

	timezone->tz_minuteswest = static_cast<int32_t>(-standard_offset_seconds / 60);
	timezone->tz_dsttime     = dst_delta != 0 ? 4 : 0;
	if (dst_seconds != nullptr)
	{
		*dst_seconds = dst_delta;
	}

	return true;
}

bool KernelClockSourceFromId(KernelClockid clock_id, KernelClockSource* source)
{
	if (source == nullptr)
	{
		return false;
	}

	switch (clock_id)
	{
		case 0: *source = KernelClockSource::Realtime; return true;
		case 4:
		case 13: *source = KernelClockSource::Monotonic; return true;
		default: return false;
	}
}

static bool ToHostClock(KernelClockSource source, clockid_t* host_clock)
{
	if (host_clock == nullptr)
	{
		return false;
	}

	switch (source)
	{
		case KernelClockSource::Realtime: *host_clock = CLOCK_REALTIME; return true;
		case KernelClockSource::Monotonic: *host_clock = CLOCK_MONOTONIC; return true;
	}

	return false;
}

int KYTY_SYSV_ABI KernelClockGetres(KernelClockid clock_id, KernelTimespec* tp)
{
	PRINT_NAME();

	if (tp == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	KernelClockSource source {};
	clockid_t         pclock_id {};
	if (!KernelClockSourceFromId(clock_id, &source) || !ToHostClock(source, &pclock_id))
	{
		return KERNEL_ERROR_EINVAL;
	}

	timespec t {};
	int      result = clock_getres(pclock_id, &t);
	tp->tv_sec      = t.tv_sec;
	tp->tv_nsec     = t.tv_nsec;

	return result == 0 ? OK : KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI KernelClockGettime(KernelClockid clock_id, KernelTimespec* tp)
{
	PRINT_NAME();

	if (tp == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	KernelClockSource source {};
	clockid_t         pclock_id {};
	if (!KernelClockSourceFromId(clock_id, &source) || !ToHostClock(source, &pclock_id))
	{
		return KERNEL_ERROR_EINVAL;
	}

	timespec t {};
	int      result = clock_gettime(pclock_id, &t);
	tp->tv_sec      = t.tv_sec;
	tp->tv_nsec     = t.tv_nsec;

	return result == 0 ? OK : KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI KernelGettimeofday(KernelTimeval* tp)
{
	PRINT_NAME();

	if (tp == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	timespec t {};
	int      result = clock_gettime(CLOCK_REALTIME, &t);
	tp->tv_sec      = t.tv_sec;
	tp->tv_usec     = t.tv_nsec / 1000;

	return result == 0 ? OK : KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI KernelGettimezone(KernelTimezone* timezone)
{
	PRINT_NAME();

	if (timezone == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	const auto now = std::time(nullptr);
	return TimezoneFromUtc(static_cast<int64_t>(now), timezone, nullptr) ? OK : KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI KernelConvertLocaltimeToUtc(int64_t local_time, int64_t /*reserved*/, int64_t* utc_time,
                                               KernelTimezone* timezone, int32_t* dst_seconds)
{
	PRINT_NAME();

	if (timezone == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	std::tm local {};
	if (!HostTime::GmtimeFromUnixSeconds(local_time, &local))
	{
		return KERNEL_ERROR_EINVAL;
	}

	local.tm_isdst = -1;
	const auto converted = std::mktime(&local);
	if (converted == static_cast<std::time_t>(-1))
	{
		return KERNEL_ERROR_EINVAL;
	}

	const int64_t utc_seconds = static_cast<int64_t>(converted);
	int32_t       dst_delta   = 0;
	if (!TimezoneFromUtc(utc_seconds, timezone, &dst_delta))
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (utc_time != nullptr)
	{
		*utc_time = utc_seconds;
	}
	if (dst_seconds != nullptr)
	{
		*dst_seconds = dst_delta;
	}

	return OK;
}

int KYTY_SYSV_ABI KernelConvertUtcToLocaltime(int64_t utc_seconds, int64_t* local_time, KernelTimesec* timesec, uint64_t* dst_seconds)
{
	PRINT_NAME();

	if (local_time == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	std::tm local {};
	if (!HostTime::LocaltimeFromUtc(utc_seconds, &local))
	{
		return KERNEL_ERROR_EINVAL;
	}

	KernelTimezone timezone {};
	int32_t        dst_delta = 0;
	if (!TimezoneFromUtc(utc_seconds, &timezone, &dst_delta))
	{
		return KERNEL_ERROR_EINVAL;
	}

	const int64_t local_seconds             = HostTime::CivilToUnixSeconds(local);
	const int64_t standard_offset_seconds   = -static_cast<int64_t>(timezone.tz_minuteswest) * 60;

	*local_time = local_seconds;
	if (timesec != nullptr)
	{
		timesec->time           = utc_seconds;
		timesec->offset_seconds = static_cast<uint32_t>(static_cast<int32_t>(standard_offset_seconds));
		timesec->dst_seconds    = static_cast<uint32_t>(dst_delta);
	}
	if (dst_seconds != nullptr)
	{
		*dst_seconds = static_cast<uint32_t>(dst_delta);
	}

	return OK;
}

uint64_t KYTY_SYSV_ABI KernelGetTscFrequency()
{
	return Core::Timer::QueryPerformanceFrequency();
}

uint64_t KYTY_SYSV_ABI KernelReadTsc()
{
	return Core::Timer::QueryPerformanceCounter();
}

uint64_t KYTY_SYSV_ABI KernelGetProcessTime()
{
	return static_cast<uint64_t>(Loader::Timer::GetTimeMs() * 1000.0);
}

uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounter()
{
	return Loader::Timer::GetCounter();
}

uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounterFrequency()
{
	return Loader::Timer::GetFrequency();
}

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED
