#include "Kyty/DevTools/Time/MonotonicClock.h"

#include <cerrno>
#include <ctime>
#include <limits>

namespace Kyty::DevTools {

bool MonotonicFromPosixTimespec(int64_t sec, int64_t nsec, uint64_t* out_ns) noexcept
{
	if (out_ns == nullptr || sec < 0 || nsec < 0 || nsec >= 1000000000ll)
	{
		return false;
	}
	const uint64_t sec_u = static_cast<uint64_t>(sec);
	if (sec_u > (std::numeric_limits<uint64_t>::max() / 1000000000ull))
	{
		return false;
	}
	const uint64_t base = sec_u * 1000000000ull;
	const uint64_t add  = static_cast<uint64_t>(nsec);
	if (base > std::numeric_limits<uint64_t>::max() - add)
	{
		return false;
	}
	*out_ns = base + add;
	return true;
}

bool MonotonicFromWindowsCounter(uint64_t counter, uint64_t frequency, uint64_t* out_ns) noexcept
{
	if (out_ns == nullptr || frequency == 0u)
	{
		return false;
	}
	// out = counter * 1e9 / frequency with checked remainder.
	const uint64_t q = counter / frequency;
	const uint64_t r = counter % frequency;
	if (q > (std::numeric_limits<uint64_t>::max() / 1000000000ull))
	{
		return false;
	}
	uint64_t ns = q * 1000000000ull;
	// r * 1e9 / frequency
	if (r > (std::numeric_limits<uint64_t>::max() / 1000000000ull))
	{
		return false;
	}
	const uint64_t frac = (r * 1000000000ull) / frequency;
	if (ns > std::numeric_limits<uint64_t>::max() - frac)
	{
		return false;
	}
	*out_ns = ns + frac;
	return true;
}

#if !defined(KYTY_DEVTOOLS_CLOCK_CONVERSION_ONLY)
uint64_t MonotonicNowNs() noexcept
{
	timespec ts {};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
	{
		return 0;
	}
	uint64_t out = 0;
	if (!MonotonicFromPosixTimespec(ts.tv_sec, ts.tv_nsec, &out))
	{
		return 0;
	}
	return out;
}
#endif

} // namespace Kyty::DevTools
