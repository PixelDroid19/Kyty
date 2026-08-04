#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_TIME_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_TIME_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Namespace.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

struct KernelTimespec
{
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct KernelTimeval
{
	int64_t tv_sec;
	int64_t tv_usec;
};

struct KernelTimezone
{
	int32_t tz_minuteswest;
	int32_t tz_dsttime;
};
static_assert(sizeof(KernelTimezone) == 8);

struct KernelTimesec
{
	int64_t  time;
	uint32_t offset_seconds;
	uint32_t dst_seconds;
};
static_assert(sizeof(KernelTimesec) == 16);

using KernelClockid = int32_t;

// Guest clock IDs are normalized before a subsystem selects its native clock
// primitive. This keeps guest-to-host clock policy consistent for timers and
// synchronization objects.
enum class KernelClockSource
{
	Realtime,
	Monotonic,
};

[[nodiscard]] bool KernelClockSourceFromId(KernelClockid clock_id, KernelClockSource* source);

int KYTY_SYSV_ABI      KernelClockGetres(KernelClockid clock_id, KernelTimespec* tp);
int KYTY_SYSV_ABI      KernelClockGettime(KernelClockid clock_id, KernelTimespec* tp);
int KYTY_SYSV_ABI      KernelGettimeofday(KernelTimeval* tp);
int KYTY_SYSV_ABI      KernelGettimezone(KernelTimezone* timezone);
int KYTY_SYSV_ABI      KernelConvertLocaltimeToUtc(int64_t local_time, int64_t reserved, int64_t* utc_time,
                                                    KernelTimezone* timezone, int32_t* dst_seconds);
int KYTY_SYSV_ABI      KernelConvertUtcToLocaltime(int64_t utc_seconds, int64_t* local_time, KernelTimesec* timesec, uint64_t* dst_seconds);
uint64_t KYTY_SYSV_ABI KernelGetTscFrequency();
uint64_t KYTY_SYSV_ABI KernelReadTsc();
uint64_t KYTY_SYSV_ABI KernelGetProcessTime();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounter();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounterFrequency();

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_TIME_H_ */
