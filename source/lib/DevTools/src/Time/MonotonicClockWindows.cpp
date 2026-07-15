#include "Kyty/DevTools/Time/MonotonicClock.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Kyty::DevTools {

uint64_t MonotonicNowNs() noexcept
{
	LARGE_INTEGER freq {};
	LARGE_INTEGER counter {};
	if (QueryPerformanceFrequency(&freq) == 0 || QueryPerformanceCounter(&counter) == 0 || freq.QuadPart <= 0)
	{
		return 0;
	}
	uint64_t out = 0;
	if (!MonotonicFromWindowsCounter(static_cast<uint64_t>(counter.QuadPart), static_cast<uint64_t>(freq.QuadPart), &out))
	{
		return 0;
	}
	return out;
}

} // namespace Kyty::DevTools

#endif // _WIN32
