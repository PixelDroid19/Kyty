#include "Kyty/DevTools/Telemetry/Progress.h"

#include "Kyty/DevTools/Time/MonotonicClock.h"

namespace Kyty::DevTools {
namespace {

constexpr uint64_t kInstanceMask = (uint64_t {1} << 56u) - 1u;

[[nodiscard]] uint16_t DomainOfOp(uint32_t op) noexcept
{
	return static_cast<uint16_t>((op >> 8u) & 0xffu);
}

} // namespace

bool ProgressKey::TryPack(uint64_t* out) const noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	if (domain == Domain::Unknown || domain >= Domain::Count)
	{
		return false;
	}
	if (instance == 0u || instance > kInstanceMask)
	{
		return false;
	}
	*out = (static_cast<uint64_t>(domain) << 56u) | (instance & kInstanceMask);
	return true;
}

bool ProgressKey::TryUnpack(uint64_t wire, ProgressKey* out) noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	const auto domain = static_cast<Domain>((wire >> 56u) & 0xffu);
	const uint64_t instance = wire & kInstanceMask;
	if (domain == Domain::Unknown || domain >= Domain::Count || instance == 0u)
	{
		return false;
	}
	out->domain   = domain;
	out->instance = instance;
	return true;
}

uint32_t ProgressRegistry::DomainSlotCount(Domain d) noexcept
{
	switch (d)
	{
		case Domain::GuestThread: return kProgressSlotsGuestThread;
		case Domain::Hle: return kProgressSlotsHle;
		case Domain::CommandProcessor: return kProgressSlotsCommandProcessor;
		case Domain::Renderer: return kProgressSlotsRenderer;
		case Domain::GpuQueue: return kProgressSlotsGpuQueue;
		case Domain::VideoOut: return kProgressSlotsVideoOut;
		case Domain::Presentation: return kProgressSlotsPresentation;
		case Domain::Synchronization: return kProgressSlotsSynchronization;
		default: return 0;
	}
}

uint32_t ProgressRegistry::DomainBase(Domain d) noexcept
{
	uint32_t base = 0;
	for (uint16_t i = 1; i < static_cast<uint16_t>(d); ++i)
	{
		base += DomainSlotCount(static_cast<Domain>(i));
	}
	return base;
}

void ProgressRegistry::NoteMaxLoss(uint64_t now) noexcept
{
	if (now > loss_.max_loss_monotonic_ns)
	{
		loss_.max_loss_monotonic_ns = now;
	}
}

void ProgressRegistry::NoteCapacity(Domain d, uint64_t now) noexcept
{
	const auto di = static_cast<uint32_t>(d);
	if (di < static_cast<uint32_t>(Domain::Count))
	{
		loss_.capacity[di].total += 1u;
		loss_.capacity[di].last_loss_monotonic_ns = now;
	}
	NoteMaxLoss(now);
}

void ProgressRegistry::NoteRejected(Domain d, uint64_t now) noexcept
{
	const auto di = static_cast<uint32_t>(d);
	if (di < static_cast<uint32_t>(Domain::Count))
	{
		loss_.rejected_update[di].total += 1u;
		loss_.rejected_update[di].last_loss_monotonic_ns = now;
	}
	NoteMaxLoss(now);
}

bool ProgressRegistry::ValidateUpdate(Domain domain, const ProgressUpdate& update, bool is_submit) const noexcept
{
	(void)is_submit;
	if (update.state == ProgressState::Unknown || update.state >= ProgressState::Count ||
	    update.state == ProgressState::Unavailable || update.state == ProgressState::Closed)
	{
		return false;
	}
	const uint16_t known_flags =
	    static_cast<uint16_t>(ProgressFlag::OperationValid) | static_cast<uint16_t>(ProgressFlag::CorrelationValid) |
	    static_cast<uint16_t>(ProgressFlag::Auxiliary0Valid) | static_cast<uint16_t>(ProgressFlag::Auxiliary1Valid) |
	    static_cast<uint16_t>(ProgressFlag::TimedWait);
	if ((update.flags & ~known_flags) != 0u)
	{
		return false;
	}
	if ((update.flags & static_cast<uint16_t>(ProgressFlag::OperationValid)) == 0u)
	{
		return false;
	}
	const uint32_t op = static_cast<uint32_t>(update.operation);
	if (op == 0u || DomainOfOp(op) != static_cast<uint16_t>(domain))
	{
		return false;
	}
	return true;
}

bool ProgressRegistry::TokenMatches(const ProgressToken& token) const noexcept
{
	if (token.domain == 0u || token.domain >= static_cast<uint16_t>(Domain::Count))
	{
		return false;
	}
	const auto domain = static_cast<Domain>(token.domain);
	const uint32_t count = DomainSlotCount(domain);
	if (token.slot >= count)
	{
		return false;
	}
	const uint32_t idx = DomainBase(domain) + token.slot;
	if (idx >= MaxProgressSnapshotEntries)
	{
		return false;
	}
	const Slot& slot = slots_[idx];
	return slot.live.load(std::memory_order_acquire) == 1u && slot.generation.load(std::memory_order_acquire) == token.generation &&
	       static_cast<uint16_t>(slot.key.domain) == token.domain;
}

bool ProgressRegistry::Register(ProgressKey key, ProgressToken* out) noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	uint64_t packed = 0;
	if (!key.TryPack(&packed))
	{
		return false;
	}
	const uint64_t now = MonotonicNowNs();
	const uint32_t base  = DomainBase(key.domain);
	const uint32_t count = DomainSlotCount(key.domain);

	// Reject duplicate live keys.
	for (uint32_t s = 0; s < count; ++s)
	{
		const Slot& slot = slots_[base + s];
		if (slot.live.load(std::memory_order_acquire) == 1u && slot.record.instance_key == packed)
		{
			NoteRejected(key.domain, now);
			return false;
		}
	}

	for (uint32_t s = 0; s < count; ++s)
	{
		Slot& slot = slots_[base + s];
		if (slot.retired)
		{
			continue;
		}
		uint32_t expected_live = 0;
		if (!slot.live.compare_exchange_strong(expected_live, 1u, std::memory_order_acq_rel, std::memory_order_acquire))
		{
			// Allow reuse of closed (2) slots with a new key.
			if (expected_live == 2u)
			{
				if (!slot.live.compare_exchange_strong(expected_live, 1u, std::memory_order_acq_rel, std::memory_order_acquire))
				{
					continue;
				}
			} else
			{
				continue;
			}
		}
		if (slot.generation.load(std::memory_order_relaxed) == 0xffffffffu)
		{
			slot.retired = true;
			slot.live.store(0, std::memory_order_release);
			NoteCapacity(key.domain, now);
			return false;
		}
		const uint32_t gen = slot.generation.load(std::memory_order_relaxed) + 1u;
		slot.generation.store(gen, std::memory_order_relaxed);
		slot.key                   = key;
		slot.record                = {};
		slot.record.instance_key   = packed;
		slot.record.state         = static_cast<uint16_t>(ProgressState::Idle);
		slot.record.last_change_ns = now;
		out->domain                = static_cast<uint16_t>(key.domain);
		out->slot                  = static_cast<uint16_t>(s);
		out->generation            = gen;
		inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
		return true;
	}
	NoteCapacity(key.domain, now);
	return false;
}

bool ProgressRegistry::Submit(ProgressToken token, const ProgressUpdate& update) noexcept
{
	const uint64_t now = (update.monotonic_ns != 0u) ? update.monotonic_ns : MonotonicNowNs();
	if (!TokenMatches(token))
	{
		NoteRejected(static_cast<Domain>(token.domain), now);
		return false;
	}
	const auto domain = static_cast<Domain>(token.domain);
	if (!ValidateUpdate(domain, update, true))
	{
		NoteRejected(domain, now);
		return false;
	}
	Slot& slot = slots_[DomainBase(domain) + token.slot];
	// Strictly newer epoch required (or first submit from Idle 0/0).
	if (!(update.epoch > slot.record.submitted ||
	      (slot.record.submitted == 0u && slot.record.completed == 0u && update.epoch > 0u)))
	{
		NoteRejected(domain, now);
		return false;
	}
	slot.record.submitted      = update.epoch;
	slot.record.operation      = static_cast<uint32_t>(update.operation);
	slot.record.state          = static_cast<uint16_t>(update.state);
	slot.record.flags          = update.flags;
	slot.record.correlation    = update.correlation;
	slot.record.auxiliary[0]   = update.auxiliary[0];
	slot.record.auxiliary[1]   = update.auxiliary[1];
	slot.record.last_change_ns = now;
	inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
	return true;
}

bool ProgressRegistry::Advance(ProgressToken token, const ProgressUpdate& update) noexcept
{
	const uint64_t now = (update.monotonic_ns != 0u) ? update.monotonic_ns : MonotonicNowNs();
	if (!TokenMatches(token))
	{
		NoteRejected(static_cast<Domain>(token.domain), now);
		return false;
	}
	const auto domain = static_cast<Domain>(token.domain);
	if (!ValidateUpdate(domain, update, false))
	{
		NoteRejected(domain, now);
		return false;
	}
	Slot& slot = slots_[DomainBase(domain) + token.slot];
	// Same epoch/operation; may update correlation/aux/state.
	if (update.epoch != slot.record.submitted || static_cast<uint32_t>(update.operation) != slot.record.operation)
	{
		NoteRejected(domain, now);
		return false;
	}
	slot.record.state          = static_cast<uint16_t>(update.state);
	slot.record.flags          = update.flags;
	slot.record.correlation    = update.correlation;
	slot.record.auxiliary[0]   = update.auxiliary[0];
	slot.record.auxiliary[1]   = update.auxiliary[1];
	slot.record.last_change_ns = now;
	inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
	return true;
}

bool ProgressRegistry::Complete(ProgressToken token, uint64_t epoch, uint64_t now_ns) noexcept
{
	const uint64_t now = (now_ns != 0u) ? now_ns : MonotonicNowNs();
	if (!TokenMatches(token))
	{
		NoteRejected(static_cast<Domain>(token.domain), now);
		return false;
	}
	const auto domain = static_cast<Domain>(token.domain);
	Slot&      slot   = slots_[DomainBase(domain) + token.slot];
	if (epoch != slot.record.submitted || epoch == 0u)
	{
		NoteRejected(domain, now);
		return false;
	}
	if (slot.record.completed == epoch)
	{
		NoteRejected(domain, now);
		return false;
	}
	slot.record.completed      = epoch;
	slot.record.state          = static_cast<uint16_t>(ProgressState::Idle);
	slot.record.last_change_ns = now;
	inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
	return true;
}

void ProgressRegistry::Close(ProgressToken token) noexcept
{
	if (!TokenMatches(token))
	{
		return;
	}
	const auto domain = static_cast<Domain>(token.domain);
	Slot&      slot   = slots_[DomainBase(domain) + token.slot];
	slot.record.state = static_cast<uint16_t>(ProgressState::Closed);
	slot.live.store(2u, std::memory_order_release); // closed, reusable with new key
	inventory_generation_.fetch_add(1u, std::memory_order_relaxed);
}

bool ProgressRegistry::Snapshot(ProgressToken token, ProgressRecord* out) const noexcept
{
	if (out == nullptr || !TokenMatches(token))
	{
		return false;
	}
	const auto domain = static_cast<Domain>(token.domain);
	*out              = slots_[DomainBase(domain) + token.slot].record;
	return true;
}

bool ProgressRegistry::SnapshotAll(ProgressSnapshot* out) const noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	const uint64_t gen_before = inventory_generation_.load(std::memory_order_acquire);
	out->count                = 0;
	out->unavailable_count    = 0;
	for (uint32_t i = 0; i < MaxProgressSnapshotEntries; ++i)
	{
		const Slot& slot = slots_[i];
		if (slot.live.load(std::memory_order_acquire) != 1u)
		{
			continue;
		}
		if (out->count >= MaxProgressSnapshotEntries)
		{
			out->unavailable_count += 1u;
			continue;
		}
		out->entries[out->count].key    = slot.key;
		out->entries[out->count].record = slot.record;
		out->count += 1u;
	}
	const uint64_t gen_after = inventory_generation_.load(std::memory_order_acquire);
	if (gen_before != gen_after)
	{
		out->inventory_generation = 0;
		return false;
	}
	out->inventory_generation = gen_before;
	return true;
}

ProgressLossSnapshot ProgressRegistry::SnapshotLoss() const noexcept
{
	return loss_;
}

} // namespace Kyty::DevTools
