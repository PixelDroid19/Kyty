#include "Kyty/Core/Common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
#else

#include "Kyty/Sys/SysProcess.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task_info.h>
#include <sys/resource.h>
#endif

namespace Kyty {

namespace {

struct ProcessBaseline
{
	bool     seeded      = false;
	uint64_t cpu_ticks   = 0; // process user+system in CLK_TCK units (Linux) or microseconds (Apple)
	double   wall_seconds = 0.0;
};

ProcessBaseline g_baseline;

[[nodiscard]] double WallSecondsNow()
{
	timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

#ifdef __APPLE__

[[nodiscard]] bool ReadProcessCpuUs(uint64_t* out_us)
{
	rusage usage {};
	if (getrusage(RUSAGE_SELF, &usage) != 0)
	{
		return false;
	}
	const uint64_t user_us = static_cast<uint64_t>(usage.ru_utime.tv_sec) * 1000000ull +
	                         static_cast<uint64_t>(usage.ru_utime.tv_usec);
	const uint64_t sys_us = static_cast<uint64_t>(usage.ru_stime.tv_sec) * 1000000ull +
	                        static_cast<uint64_t>(usage.ru_stime.tv_usec);
	*out_us = user_us + sys_us;
	return true;
}

[[nodiscard]] bool ReadResidentBytes(uint64_t* out_bytes)
{
	task_basic_info_data_t info {};
	mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
	const kern_return_t    kr =
	    task_info(mach_task_self(), TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count);
	if (kr != KERN_SUCCESS)
	{
		return false;
	}
	*out_bytes = static_cast<uint64_t>(info.resident_size);
	return true;
}

#else // Linux

[[nodiscard]] bool ReadProcessCpuTicks(uint64_t* out_ticks)
{
	FILE* f = fopen("/proc/self/stat", "r");
	if (f == nullptr)
	{
		return false;
	}

	// Fields 14 (utime) and 15 (stime) are after the comm field in parentheses.
	char buf[1024];
	if (fgets(buf, sizeof(buf), f) == nullptr)
	{
		fclose(f);
		return false;
	}
	fclose(f);

	char* close = strrchr(buf, ')');
	if (close == nullptr)
	{
		return false;
	}
	close++;

	unsigned long utime = 0;
	unsigned long stime = 0;
	// After ')': state(1) ppid(2) ... utime(12) stime(13) relative to post-comm tokens.
	if (sscanf(close, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) != 2)
	{
		return false;
	}
	*out_ticks = static_cast<uint64_t>(utime) + static_cast<uint64_t>(stime);
	return true;
}

[[nodiscard]] bool ReadResidentBytes(uint64_t* out_bytes)
{
	FILE* f = fopen("/proc/self/status", "r");
	if (f == nullptr)
	{
		return false;
	}

	char     line[256];
	uint64_t kb = 0;
	bool     ok = false;
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		if (strncmp(line, "VmRSS:", 6) == 0)
		{
			if (sscanf(line + 6, " %" SCNu64, &kb) == 1)
			{
				ok = true;
			}
			break;
		}
	}
	fclose(f);
	if (!ok)
	{
		return false;
	}
	*out_bytes = kb * 1024ull;
	return true;
}

#endif

} // namespace

void SysProcessSampleReset()
{
	g_baseline = {};
}

SysProcessSample SysProcessSampleNow()
{
	SysProcessSample sample {};

	uint64_t cpu_now = 0;
#ifdef __APPLE__
	if (!ReadProcessCpuUs(&cpu_now) || !ReadResidentBytes(&sample.heap_bytes))
#else
	if (!ReadProcessCpuTicks(&cpu_now) || !ReadResidentBytes(&sample.heap_bytes))
#endif
	{
		return sample;
	}

	const double wall_now = WallSecondsNow();
	if (!g_baseline.seeded)
	{
		g_baseline.seeded       = true;
		g_baseline.cpu_ticks    = cpu_now;
		g_baseline.wall_seconds = wall_now;
		return sample;
	}

	const double wall_delta = wall_now - g_baseline.wall_seconds;
	if (wall_delta <= 0.0)
	{
		return sample;
	}

#ifdef __APPLE__
	const double cpu_delta_seconds = static_cast<double>(cpu_now - g_baseline.cpu_ticks) * 1e-6;
#else
	const long   clk               = sysconf(_SC_CLK_TCK);
	const double ticks_per_sec     = (clk > 0) ? static_cast<double>(clk) : 100.0;
	const double cpu_delta_seconds = static_cast<double>(cpu_now - g_baseline.cpu_ticks) / ticks_per_sec;
#endif

	sample.cpu_percent          = (cpu_delta_seconds / wall_delta) * 100.0;
	sample.valid                = true;
	g_baseline.cpu_ticks        = cpu_now;
	g_baseline.wall_seconds     = wall_now;
	return sample;
}

} // namespace Kyty

#endif
