#include "Kyty/DevTools/Telemetry/WriterRegistry.h"

#include "Kyty/DevTools/Time/MonotonicClock.h"

#include <algorithm>
#include <cstring>

namespace Kyty::DevTools {
namespace {

[[nodiscard]] bool DomainEventInRange(uint16_t domain, uint16_t event) noexcept
{
	return domain > static_cast<uint16_t>(Domain::Unknown) && domain < static_cast<uint16_t>(Domain::Count) &&
	       event > static_cast<uint16_t>(EventId::Unknown) && event < static_cast<uint16_t>(EventId::Count);
}

} // namespace

void TimelineHistory::AppendDrained(const EventRecord* records, uint32_t count) noexcept
{
	if (records == nullptr || count == 0u)
	{
		return;
	}
	for (uint32_t i = 0; i < count; ++i)
	{
		buffer_[head_] = records[i];
		head_          = (head_ + 1u) % kCapacity;
		if (size_ < kCapacity)
		{
			++size_;
		}
	}
}

uint32_t TimelineHistory::SnapshotNewest(EventRecord* out, uint32_t capacity) const noexcept
{
	if (out == nullptr || capacity == 0u || size_ == 0u)
	{
		return 0;
	}
	const uint32_t n = (capacity < size_) ? capacity : size_;
	for (uint32_t i = 0; i < n; ++i)
	{
		// Newest first: head_-1 is newest.
		const uint32_t idx = (head_ + kCapacity - 1u - i) % kCapacity;
		out[i]             = buffer_[idx];
	}
	return n;
}

void WriterRegistry::NoteMaxLossTime(uint64_t monotonic_ns) noexcept
{
	uint64_t cur = max_loss_monotonic_ns_.load(std::memory_order_relaxed);
	while (monotonic_ns > cur &&
	       !max_loss_monotonic_ns_.compare_exchange_weak(cur, monotonic_ns, std::memory_order_relaxed, std::memory_order_relaxed))
	{
	}
}

void WriterRegistry::NoteLoss(Slot& slot, uint64_t attempted_sequence, uint64_t monotonic_ns, bool ring_full) noexcept
{
	slot.loss.total += 1u;
	slot.loss.last_attempted_sequence = attempted_sequence;
	slot.loss.last_loss_monotonic_ns  = monotonic_ns;
	if (ring_full)
	{
		aggregate_ring_.total += 1u;
		aggregate_ring_.last_loss_monotonic_ns = monotonic_ns;
	}
	NoteMaxLossTime(monotonic_ns);
}

void WriterRegistry::NoteInactive(uint64_t monotonic_ns) noexcept
{
	inactive_writer_attempts_.total += 1u;
	inactive_writer_attempts_.last_loss_monotonic_ns = monotonic_ns;
	NoteMaxLossTime(monotonic_ns);
}

void WriterRegistry::NoteRegistrationCapacity(uint64_t monotonic_ns) noexcept
{
	registration_capacity_.total += 1u;
	registration_capacity_.last_loss_monotonic_ns = monotonic_ns;
	NoteMaxLossTime(monotonic_ns);
}

bool WriterRegistry::TokenMatches(const Slot& slot, TelemetryWriterToken token, WriterState expected) const noexcept
{
	return token.slot < kWriterSlots && slot.generation == token.generation &&
	       slot.state.load(std::memory_order_acquire) == static_cast<uint32_t>(expected);
}

bool WriterRegistry::Reserve(ThreadRole role, uint64_t diagnostic_thread_instance, TelemetryWriterToken* out) noexcept
{
	if (out == nullptr || role == ThreadRole::Unknown || role >= ThreadRole::Count || diagnostic_thread_instance == 0u)
	{
		return false;
	}
	const uint64_t now = MonotonicNowNs();
	for (uint32_t i = 0; i < kWriterSlots; ++i)
	{
		Slot&    slot     = slots_[i];
		uint32_t expected = static_cast<uint32_t>(WriterState::Free);
		if (slot.retired)
		{
			continue;
		}
		if (!slot.state.compare_exchange_strong(expected, static_cast<uint32_t>(WriterState::Reserved), std::memory_order_acq_rel,
		                                       std::memory_order_acquire))
		{
			continue;
		}
		// Generation starts at 1 and never wraps to zero: retire before wrap.
		if (slot.generation == 0xffffffffu)
		{
			slot.retired = true;
			slot.state.store(static_cast<uint32_t>(WriterState::Retired), std::memory_order_release);
			NoteRegistrationCapacity(now);
			return false;
		}
		slot.generation += 1u;
		slot.role                       = role;
		slot.diagnostic_thread_instance = diagnostic_thread_instance;
		// attempted_sequence and loss intentionally preserved across generations.
		// Ring indices reset for new generation storage only.
		slot.ring.Reset();
		out->slot       = i;
		out->generation = slot.generation;
		reserved_count_.fetch_add(1u, std::memory_order_relaxed);
		inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
		return true;
	}
	NoteRegistrationCapacity(now);
	return false;
}

bool WriterRegistry::Activate(TelemetryWriterToken token) noexcept
{
	if (token.slot >= kWriterSlots)
	{
		NoteInactive(MonotonicNowNs());
		return false;
	}
	Slot& slot = slots_[token.slot];
	if (!TokenMatches(slot, token, WriterState::Reserved))
	{
		NoteInactive(MonotonicNowNs());
		return false;
	}
	slot.state.store(static_cast<uint32_t>(WriterState::Active), std::memory_order_release);
	inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
	return true;
}

void WriterRegistry::Abandon(TelemetryWriterToken token) noexcept
{
	if (token.slot >= kWriterSlots)
	{
		NoteInactive(MonotonicNowNs());
		return;
	}
	Slot& slot = slots_[token.slot];
	if (!TokenMatches(slot, token, WriterState::Reserved))
	{
		NoteInactive(MonotonicNowNs());
		return;
	}
	slot.role                       = ThreadRole::Unknown;
	slot.diagnostic_thread_instance = 0;
	slot.state.store(static_cast<uint32_t>(WriterState::Free), std::memory_order_release);
	inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
}

bool WriterRegistry::TryRecord(TelemetryWriterToken token, EventRecord record) noexcept
{
	const uint64_t now = (record.monotonic_ns != 0u) ? record.monotonic_ns : MonotonicNowNs();
	if (token.slot >= kWriterSlots)
	{
		NoteInactive(now);
		return false;
	}
	Slot& slot = slots_[token.slot];
	if (!TokenMatches(slot, token, WriterState::Active))
	{
		NoteInactive(now);
		return false;
	}

	// Assign sequence and writer key; ignore caller values.
	slot.attempted_sequence += 1u;
	const uint64_t attempted = slot.attempted_sequence;
	record.sequence          = attempted;
	record.writer_key        = WriterKey {token.slot, token.generation}.Pack();
	if (record.monotonic_ns == 0u)
	{
		record.monotonic_ns = now;
	}

	if (!DomainEventInRange(record.domain, record.event) || !ValidateEventSchema(record))
	{
		NoteLoss(slot, attempted, record.monotonic_ns, false);
		return false;
	}

	if (!slot.ring.TryPush(record))
	{
		NoteLoss(slot, attempted, record.monotonic_ns, true);
		return false;
	}
	return true;
}

void WriterRegistry::Close(TelemetryWriterToken token, EventRecord exit_record) noexcept
{
	const uint64_t now = (exit_record.monotonic_ns != 0u) ? exit_record.monotonic_ns : MonotonicNowNs();
	if (token.slot >= kWriterSlots)
	{
		NoteInactive(now);
		return;
	}
	Slot& slot = slots_[token.slot];
	if (!TokenMatches(slot, token, WriterState::Active))
	{
		NoteInactive(now);
		return;
	}
	// Publish exit through same path (may drop if full — still advances attempted sequence).
	(void)TryRecord(token, exit_record);
	slot.state.store(static_cast<uint32_t>(WriterState::Closing), std::memory_order_release);
	inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
}

uint32_t WriterRegistry::Drain(EventRecord* out, uint32_t capacity) noexcept
{
	if (out == nullptr || capacity == 0u)
	{
		return 0;
	}

	// Multiway merge: pick globally earliest (monotonic_ns, writer_key, sequence) head each step.
	uint32_t written = 0;
	while (written < capacity)
	{
		EventRecord best {};
		int         best_slot = -1;
		bool        have      = false;
		for (uint32_t s = 0; s < kWriterSlots; ++s)
		{
			EventRecord head {};
			if (!slots_[s].ring.TryPeek(&head))
			{
				continue;
			}
			if (!have || head.monotonic_ns < best.monotonic_ns ||
			    (head.monotonic_ns == best.monotonic_ns && head.writer_key < best.writer_key) ||
			    (head.monotonic_ns == best.monotonic_ns && head.writer_key == best.writer_key && head.sequence < best.sequence))
			{
				best      = head;
				best_slot = static_cast<int>(s);
				have      = true;
			}
		}
		if (!have || best_slot < 0)
		{
			break;
		}
		slots_[static_cast<uint32_t>(best_slot)].ring.Pop();
		out[written++] = best;

		// Recycle Closing slots once their ring is empty.
		Slot& slot = slots_[static_cast<uint32_t>(best_slot)];
		if (slot.state.load(std::memory_order_acquire) == static_cast<uint32_t>(WriterState::Closing) &&
		    slot.ring.PublishedCount() == 0u)
		{
			slot.role                       = ThreadRole::Unknown;
			slot.diagnostic_thread_instance = 0;
			slot.state.store(static_cast<uint32_t>(WriterState::Free), std::memory_order_release);
			inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
		}
	}

	if (written > 0u)
	{
		history_.AppendDrained(out, written);
	}
	return written;
}

WriterLossSnapshot WriterRegistry::SnapshotLoss() const noexcept
{
	WriterLossSnapshot snap {};
	for (uint32_t i = 0; i < kWriterSlots; ++i)
	{
		snap.writer[i] = slots_[i].loss;
	}
	snap.aggregate_ring            = aggregate_ring_;
	snap.registration_capacity     = registration_capacity_;
	snap.inactive_writer_attempts  = inactive_writer_attempts_;
	snap.max_loss_monotonic_ns     = max_loss_monotonic_ns_.load(std::memory_order_relaxed);
	return snap;
}

bool WriterRegistry::SnapshotInventory(WriterInventorySnapshot* out) const noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	const uint64_t gen_before = inventory_generation_.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < kWriterSlots; ++i)
	{
		const Slot& slot = slots_[i];
		const auto  st   = static_cast<WriterState>(slot.state.load(std::memory_order_acquire));
		out->entries[i].state                      = st;
		out->entries[i].role                       = slot.role;
		out->entries[i].diagnostic_thread_instance = slot.diagnostic_thread_instance;
		out->entries[i].writer_key                 = WriterKey {i, slot.generation}.Pack();
	}
	const uint64_t gen_after = inventory_generation_.load(std::memory_order_acquire);
	if (gen_before != gen_after)
	{
		// Mixed snapshot: signal retry by zero generation.
		out->inventory_generation = 0;
		return false;
	}
	out->inventory_generation = gen_before;
	return true;
}

} // namespace Kyty::DevTools
