#ifndef SYS_LINUX_INCLUDE_KYTY_SYSTIMER_H_
#define SYS_LINUX_INCLUDE_KYTY_SYSTIMER_H_

// IWYU pragma: private

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
//#error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include <cpuid.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <x86intrin.h>

namespace Kyty {

struct SysTimeStruct
{
	uint16_t Year;
	uint16_t Month;
	uint16_t Day;
	uint16_t Hour;
	uint16_t Minute;
	uint16_t Second;
	uint16_t Milliseconds;
	bool     is_invalid;
};

struct SysFileTimeStruct
{
	time_t time;
	bool   is_invalid;
};

inline void sys_file_to_system_time_utc(const SysFileTimeStruct& f, SysTimeStruct& t)
{
	struct tm i
	{
	};

	if (f.is_invalid || gmtime_r(&f.time, &i) == nullptr)
	{
		t.is_invalid = true;
		return;
	}

	t.is_invalid   = false;
	t.Year         = i.tm_year + 1900;
	t.Month        = i.tm_mon + 1;
	t.Day          = i.tm_mday;
	t.Hour         = i.tm_hour;
	t.Minute       = i.tm_min;
	t.Second       = (i.tm_sec == 60 ? 59 : i.tm_sec);
	t.Milliseconds = 0;
}

inline void sys_time_t_to_system(time_t t, SysTimeStruct& s)
{
	SysFileTimeStruct ft {};
	ft.time       = t;
	ft.is_invalid = false;
	sys_file_to_system_time_utc(ft, s);
}

inline time_t sys_timegm(struct tm* tm)
{
	return timegm(tm);
	// time_t t = mktime(tm);
	// return t == (time_t)-1 ? (time_t)-1 : t + localtime(&t)->tm_gmtoff;
}

inline void sys_system_to_file_time_utc(const SysTimeStruct& f, SysFileTimeStruct& t)
{
	struct tm i
	{
	};

	i.tm_year = f.Year - 1900;
	i.tm_mon  = f.Month - 1;
	i.tm_mday = f.Day;
	i.tm_hour = f.Hour;
	i.tm_min  = f.Minute;
	i.tm_sec  = f.Second;

	t.is_invalid = (f.is_invalid || (t.time = sys_timegm(&i)) == static_cast<time_t>(-1));
}

// Retrieves the current local date and time
inline void sys_get_system_time(SysTimeStruct& t)
{
	time_t    st {};
	struct tm i
	{
	};

	if (time(&st) == static_cast<time_t>(-1) || localtime_r(&st, &i) == nullptr)
	{
		t.is_invalid = true;
		return;
	}

	t.is_invalid   = false;
	t.Year         = i.tm_year + 1900;
	t.Month        = i.tm_mon + 1;
	t.Day          = i.tm_mday;
	t.Hour         = i.tm_hour;
	t.Minute       = i.tm_min;
	t.Second       = (i.tm_sec == 60 ? 59 : i.tm_sec);
	t.Milliseconds = 0;
}

// Retrieves the current system date and time in Coordinated Universal Time (UTC).
inline void sys_get_system_time_utc(SysTimeStruct& t)
{
	time_t    st {};
	struct tm i
	{
	};

	if (time(&st) == static_cast<time_t>(-1) || gmtime_r(&st, &i) == nullptr)
	{
		t.is_invalid = true;
		return;
	}

	t.is_invalid   = false;
	t.Year         = i.tm_year + 1900;
	t.Month        = i.tm_mon + 1;
	t.Day          = i.tm_mday;
	t.Hour         = i.tm_hour;
	t.Minute       = i.tm_min;
	t.Second       = (i.tm_sec == 60 ? 59 : i.tm_sec);
	t.Milliseconds = 0;
}

inline void sys_query_performance_frequency(uint64_t* freq)
{
	*freq = 1000000000LL;
}

inline void sys_query_performance_counter(uint64_t* counter)
{
	struct timespec now
	{
	};
	clock_gettime(CLOCK_MONOTONIC, &now);
	*counter = now.tv_sec * 1000000000LL + now.tv_nsec;
}

// Reads the processor's time-stamp counter.
inline uint64_t sys_read_cycle_counter()
{
	return __rdtsc();
}

// True when the time-stamp counter ticks at a constant rate in every power state
// and the kernel keeps it as its clock source, which it does only while the
// counter stays synchronized across processors.
inline bool sys_cycle_counter_invariant()
{
	unsigned int eax = 0;
	unsigned int ebx = 0;
	unsigned int ecx = 0;
	unsigned int edx = 0;
	if (__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx) == 0 || (edx & (1u << 8u)) == 0)
	{
		return false;
	}
	FILE* f = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
	if (f == nullptr)
	{
		return false;
	}
	char       source[16] = {};
	const bool read       = fgets(source, sizeof(source), f) != nullptr;
	fclose(f);
	return read && strcmp(source, "tsc\n") == 0;
}

} // namespace Kyty

#endif

#endif /* SYS_LINUX_INCLUDE_KYTY_SYSTIMER_H_ */
