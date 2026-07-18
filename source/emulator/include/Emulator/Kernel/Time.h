#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_TIME_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_TIME_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel {

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

struct KernelTimesec
{
	int64_t  time;
	uint32_t offset_seconds;
	uint32_t dst_seconds;
};
static_assert(sizeof(KernelTimesec) == 16);

using KernelClockid = int32_t;

int KYTY_SYSV_ABI      KernelClockGetres(KernelClockid clock_id, KernelTimespec* tp);
int KYTY_SYSV_ABI      KernelClockGettime(KernelClockid clock_id, KernelTimespec* tp);
int KYTY_SYSV_ABI      KernelGettimeofday(KernelTimeval* tp);
int KYTY_SYSV_ABI      KernelConvertUtcToLocaltime(int64_t utc_seconds, int64_t* local_time, KernelTimesec* timesec, uint64_t* dst_seconds);
uint64_t KYTY_SYSV_ABI KernelGetTscFrequency();
uint64_t KYTY_SYSV_ABI KernelReadTsc();
uint64_t KYTY_SYSV_ABI KernelGetProcessTime();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounter();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounterFrequency();

} // namespace Kyty::Libs::LibKernel

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_TIME_H_ */
