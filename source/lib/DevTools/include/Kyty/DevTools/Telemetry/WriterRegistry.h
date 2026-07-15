#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_WRITERREGISTRY_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_WRITERREGISTRY_H_

#include "Kyty/DevTools/Telemetry/Event.h"
#include "Kyty/DevTools/Telemetry/EventRing.h"

#include <atomic>
#include <cstdint>

namespace Kyty::DevTools {

enum class WriterState: uint32_t
{
	Free     = 0,
	Reserved = 1,
	Active   = 2,
	Closing  = 3,
	Retired  = 4
};

struct TelemetryWriterToken
{
	uint32_t slot       = 0;
	uint32_t generation = 0;
};

struct WriterLossCounter
{
	uint64_t total                    = 0;
	uint64_t last_attempted_sequence  = 0;
	uint64_t last_loss_monotonic_ns   = 0;
};

struct GlobalLossCounter
{
	uint64_t total                  = 0;
	uint64_t last_loss_monotonic_ns = 0;
};

struct WriterLossSnapshot
{
	WriterLossCounter writer[512];
	GlobalLossCounter aggregate_ring;
	GlobalLossCounter registration_capacity;
	GlobalLossCounter inactive_writer_attempts;
	uint64_t          max_loss_monotonic_ns = 0;
};

struct WriterInventoryEntry
{
	uint64_t     writer_key                = 0;
	uint64_t     diagnostic_thread_instance = 0;
	ThreadRole   role                      = ThreadRole::Unknown;
	WriterState  state                     = WriterState::Free;
};

struct WriterInventorySnapshot
{
	WriterInventoryEntry entries[512];
	uint64_t             inventory_generation = 0;
};

// Publisher-owned circular window of drained timeline events (newest-first snapshot).
class TimelineHistory
{
public:
	static constexpr uint32_t kCapacity = 4096u;

	void AppendDrained(const EventRecord* records, uint32_t count) noexcept;
	// Writes up to capacity newest records (newest first). Returns count written.
	uint32_t SnapshotNewest(EventRecord* out, uint32_t capacity) const noexcept;
	[[nodiscard]] uint32_t Size() const noexcept { return size_; }

private:
	EventRecord buffer_[kCapacity] {};
	uint32_t    head_ = 0; // next write index
	uint32_t    size_ = 0;
};

class WriterRegistry
{
public:
	static constexpr uint32_t kWriterSlots = 512u;

	WriterRegistry() noexcept = default;

	bool Reserve(ThreadRole role, uint64_t diagnostic_thread_instance, TelemetryWriterToken* out) noexcept;
	bool Activate(TelemetryWriterToken token) noexcept;
	void Abandon(TelemetryWriterToken token) noexcept;
	bool TryRecord(TelemetryWriterToken token, EventRecord record) noexcept;
	void Close(TelemetryWriterToken token, EventRecord exit_record) noexcept;
	uint32_t Drain(EventRecord* out, uint32_t capacity) noexcept;
	WriterLossSnapshot SnapshotLoss() const noexcept;
	bool SnapshotInventory(WriterInventorySnapshot* out) const noexcept;

	// Exposes drained history for tests/supervisor without a separate publisher loop.
	[[nodiscard]] const TimelineHistory& History() const noexcept { return history_; }
	TimelineHistory&                     History() noexcept { return history_; }

private:
	struct Slot
	{
		std::atomic<uint32_t> state {static_cast<uint32_t>(WriterState::Free)};
		uint32_t              generation {0};
		ThreadRole            role {ThreadRole::Unknown};
		uint64_t              diagnostic_thread_instance {0};
		uint64_t              attempted_sequence {0}; // process-lifetime, never resets
		WriterLossCounter     loss {};
		EventRing             ring {};
		bool                  retired {false};
	};

	void NoteLoss(Slot& slot, uint64_t attempted_sequence, uint64_t monotonic_ns, bool ring_full) noexcept;
	void NoteInactive(uint64_t monotonic_ns) noexcept;
	void NoteRegistrationCapacity(uint64_t monotonic_ns) noexcept;
	void NoteMaxLossTime(uint64_t monotonic_ns) noexcept;
	[[nodiscard]] bool TokenMatches(const Slot& slot, TelemetryWriterToken token, WriterState expected) const noexcept;

	Slot              slots_[kWriterSlots];
	std::atomic<uint32_t> reserved_count_ {0};
	GlobalLossCounter aggregate_ring_ {};
	GlobalLossCounter registration_capacity_ {};
	GlobalLossCounter inactive_writer_attempts_ {};
	std::atomic<uint64_t> max_loss_monotonic_ns_ {0};
	std::atomic<uint64_t> inventory_generation_ {0};
	TimelineHistory   history_ {};
};

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_WRITERREGISTRY_H_ */
