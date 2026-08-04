#include "Emulator/Host/Platform.h"

#include <chrono>
#include <ctime>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace Kyty::Emulator::Host {

std::string UtcTimestamp()
{
	const auto now = std::chrono::system_clock::now();
	const auto tt  = std::chrono::system_clock::to_time_t(now);

	std::tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &tt);
#else
	gmtime_r(&tt, &utc);
#endif

	char stamp[32] {};
	std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc);
	return stamp;
}

uint64_t PeakRssBytes()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS counters {};
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
	{
		return 0;
	}
	return static_cast<uint64_t>(counters.PeakWorkingSetSize);
#else
	struct rusage usage {};
	if (getrusage(RUSAGE_SELF, &usage) != 0)
	{
		return 0;
	}
#if defined(__APPLE__)
	return static_cast<uint64_t>(usage.ru_maxrss);
#else
	return static_cast<uint64_t>(usage.ru_maxrss) * 1024u;
#endif
#endif
}

} // namespace Kyty::Emulator::Host
