#include "Kyty/DevTools/Telemetry/ThreadInstanceAllocator.h"

namespace Kyty::DevTools {

ThreadInstanceAllocator::ThreadInstanceAllocator(uint64_t next_instance) noexcept : next_instance_(next_instance) {}

bool ThreadInstanceAllocator::Allocate(uint64_t* out) noexcept
{
	if (out == nullptr)
	{
		return false;
	}

	uint64_t candidate = next_instance_.load(std::memory_order_relaxed);
	for (;;)
	{
		if (candidate == 0u || candidate > kMaxInstance)
		{
			return false;
		}

		if (next_instance_.compare_exchange_weak(candidate, candidate + 1u, std::memory_order_relaxed,
		                                          std::memory_order_relaxed))
		{
			*out = candidate;
			return true;
		}
	}
}

} // namespace Kyty::DevTools
