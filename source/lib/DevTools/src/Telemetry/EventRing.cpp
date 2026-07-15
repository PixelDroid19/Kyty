#include "Kyty/DevTools/Telemetry/EventRing.h"

namespace Kyty::DevTools {

bool EventRing::TryPush(const EventRecord& record) noexcept
{
	const uint64_t write = write_sequence_.load(std::memory_order_relaxed);
	const uint64_t read  = read_sequence_.load(std::memory_order_acquire);
	if ((write - read) >= kCapacity)
	{
		return false;
	}
	slots_[static_cast<uint32_t>(write % kCapacity)] = record;
	write_sequence_.store(write + 1u, std::memory_order_release);
	return true;
}

bool EventRing::TryPeek(EventRecord* out) const noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	const uint64_t read  = read_sequence_.load(std::memory_order_relaxed);
	const uint64_t write = write_sequence_.load(std::memory_order_acquire);
	if (read >= write)
	{
		return false;
	}
	*out = slots_[static_cast<uint32_t>(read % kCapacity)];
	return true;
}

void EventRing::Pop() noexcept
{
	const uint64_t read = read_sequence_.load(std::memory_order_relaxed);
	read_sequence_.store(read + 1u, std::memory_order_release);
}

} // namespace Kyty::DevTools
