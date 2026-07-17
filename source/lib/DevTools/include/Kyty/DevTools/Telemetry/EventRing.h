#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_EVENTRING_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_EVENTRING_H_

#include "Kyty/DevTools/Telemetry/Event.h"

#include <atomic>
#include <cstdint>

namespace Kyty::DevTools {

// Per-writer SPSC ring: single producer exclusively owns write_sequence and
// plain slots; single consumer exclusively owns read_sequence. Capacity 256.
class EventRing
{
public:
	static constexpr uint32_t kCapacity = 256u;

	EventRing() noexcept = default;
	EventRing(const EventRing&)            = delete;
	EventRing& operator=(const EventRing&) = delete;

	// Reset storage indices for a new writer generation (single-threaded at Reserve).
	void Reset() noexcept
	{
		write_sequence_.store(0, std::memory_order_relaxed);
		read_sequence_.store(0, std::memory_order_relaxed);
	}

	// Returns false when full (write - read == capacity). Caller owns loss accounting.
	[[nodiscard]] bool TryPush(const EventRecord& record) noexcept;

	// Returns false when empty.
	[[nodiscard]] bool TryPeek(EventRecord* out) const noexcept;
	// Advances read_sequence after a successful TryPeek consumer decision.
	void Pop() noexcept;

	[[nodiscard]] uint64_t WriteSequence() const noexcept
	{
		return write_sequence_.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t ReadSequence() const noexcept
	{
		return read_sequence_.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t PublishedCount() const noexcept
	{
		const uint64_t w = WriteSequence();
		const uint64_t r = ReadSequence();
		return (w >= r) ? (w - r) : 0;
	}

private:
	alignas(64) std::atomic<uint64_t> write_sequence_ {0};
	alignas(64) std::atomic<uint64_t> read_sequence_ {0};
	EventRecord                       slots_[kCapacity] {};
};

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_EVENTRING_H_ */
