#include "Kyty/Core/Common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#else

#include "Kyty/Sys/SysProcess.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

namespace Kyty {

namespace {

struct ProcessBaseline
{
	bool      seeded       = false;
	ULONGLONG cpu_100ns    = 0;
	ULONGLONG wall_100ns   = 0;
};

ProcessBaseline g_baseline;

[[nodiscard]] ULONGLONG FileTimeToU64(const FILETIME& ft)
{
	ULARGE_INTEGER u;
	u.LowPart  = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return u.QuadPart;
}

} // namespace

void SysProcessSampleReset()
{
	g_baseline = {};
}

SysProcessSample SysProcessSampleNow()
{
	SysProcessSample sample {};

	FILETIME create {}, exit_t {}, kernel {}, user {};
	if (!GetProcessTimes(GetCurrentProcess(), &create, &exit_t, &kernel, &user))
	{
		return sample;
	}

	PROCESS_MEMORY_COUNTERS pmc {};
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
	{
		return sample;
	}
	sample.heap_bytes = static_cast<uint64_t>(pmc.WorkingSetSize);

	FILETIME now_ft {};
	GetSystemTimeAsFileTime(&now_ft);
	const ULONGLONG cpu_now  = FileTimeToU64(kernel) + FileTimeToU64(user);
	const ULONGLONG wall_now = FileTimeToU64(now_ft);

	if (!g_baseline.seeded)
	{
		g_baseline.seeded     = true;
		g_baseline.cpu_100ns  = cpu_now;
		g_baseline.wall_100ns = wall_now;
		return sample;
	}

	const ULONGLONG wall_delta = wall_now - g_baseline.wall_100ns;
	if (wall_delta == 0)
	{
		return sample;
	}

	const ULONGLONG cpu_delta = cpu_now - g_baseline.cpu_100ns;
	sample.cpu_percent        = (static_cast<double>(cpu_delta) / static_cast<double>(wall_delta)) * 100.0;
	sample.valid              = true;
	g_baseline.cpu_100ns      = cpu_now;
	g_baseline.wall_100ns     = wall_now;
	return sample;
}

} // namespace Kyty

#endif
