#include "Emulator/Graphics/GpuDirtyPageTracker.h"

#include "Kyty/Core/VirtualMemory.h"

#include <atomic>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>

namespace Kyty::Libs::Graphics {

struct GpuDirtyPageTracker::PageEntry
{
	std::atomic<uintptr_t> key {0};
	std::atomic<uint64_t>  generation {0};
	std::atomic<uint32_t>  refs {0};
	std::atomic<uint32_t>  protection_state {static_cast<uint32_t>(GpuDirtyProtectionState::Writable)};
	std::atomic<uint32_t>  original_mode {0};
	std::atomic<uint32_t>  original_mode_valid {0};
};

struct GpuDirtyPageTracker::RangeEntry
{
	uintptr_t             begin = 0;
	uintptr_t             end   = 0;
	std::atomic<uint64_t> generation {0};
	std::atomic<uint32_t> refs {0};
	std::atomic<uint32_t> active {0};
	std::atomic<uint32_t> mode {static_cast<uint32_t>(GpuDirtyTrackingMode::PageFault)};
};

namespace {

constexpr uintptr_t kTombstoneKey = 1u;
std::atomic<bool>   g_fault_handler_ready {false};

[[nodiscard]] uintptr_t HashPage(uintptr_t page) noexcept
{
	page >>= 12u;
	page ^= page >> 33u;
	page *= static_cast<uintptr_t>(0xff51afd7ed558ccdULL);
	page ^= page >> 33u;
	return page;
}

void AtomicMax(std::atomic<uint64_t>* target, uint64_t value) noexcept
{
	uint64_t current = target->load(std::memory_order_relaxed);
	while (current < value && !target->compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed))
	{
	}
}

[[nodiscard]] Core::VirtualMemory::Mode ReadOnlyMode(Core::VirtualMemory::Mode original) noexcept
{
	uint32_t bits = static_cast<uint32_t>(original);
	bits &= ~static_cast<uint32_t>(Core::VirtualMemory::Mode::Write);
	if ((bits & static_cast<uint32_t>(Core::VirtualMemory::Mode::Read)) == 0u &&
	    (bits & static_cast<uint32_t>(Core::VirtualMemory::Mode::Execute)) == 0u)
	{
		bits |= static_cast<uint32_t>(Core::VirtualMemory::Mode::Read);
	}
	return static_cast<Core::VirtualMemory::Mode>(bits);
}

bool DefaultProtect(void*, uintptr_t address, size_t size, Core::VirtualMemory::Mode mode, Core::VirtualMemory::Mode* old_mode) noexcept
{
	return Core::VirtualMemory::Protect(address, size, mode, old_mode);
}

bool DefaultProtectWriteSignalSafe(void*, uintptr_t address, size_t size) noexcept
{
	return Core::VirtualMemory::ProtectWriteSignalSafe(address, size);
}

} // namespace

GpuDirtyPageTracker::GpuDirtyPageTracker(bool enabled)
    : GpuDirtyPageTracker(GpuDirtyPageProtectionOps {nullptr, &DefaultProtect, &DefaultProtectWriteSignalSafe}, enabled)
{
}

GpuDirtyPageTracker::GpuDirtyPageTracker(const GpuDirtyPageProtectionOps& protection_ops, bool enabled)
    : m_page_size(Core::VirtualMemory::GetPageSize()), m_pages(enabled ? new PageEntry[kPageTableSize] : nullptr),
      m_ranges(enabled ? new RangeEntry[kMaxRanges] : nullptr), m_registration_mutex(enabled ? new std::mutex : nullptr),
      m_epoch(enabled ? new std::atomic<uint64_t>(0) : nullptr), m_protection_ops(protection_ops),
      m_enabled(enabled && protection_ops.protect != nullptr && protection_ops.protect_write_signal_safe != nullptr)
{
}

GpuDirtyPageTracker::~GpuDirtyPageTracker()
{
	delete m_epoch;
	delete m_registration_mutex;
}

uintptr_t GpuDirtyPageTracker::PageStart(uintptr_t address) const noexcept
{
	return m_page_size == 0 ? 0 : address - address % m_page_size;
}

uintptr_t GpuDirtyPageTracker::PageEnd(uintptr_t page) const noexcept
{
	return page > std::numeric_limits<uintptr_t>::max() - m_page_size ? std::numeric_limits<uintptr_t>::max() : page + m_page_size;
}

uintptr_t GpuDirtyPageTracker::RangeEnd(uintptr_t address, size_t size) const noexcept
{
	if (size == 0 || address > std::numeric_limits<uintptr_t>::max() - size)
	{
		return 0;
	}
	return address + size;
}

GpuDirtyPageTracker::PageEntry* GpuDirtyPageTracker::FindPage(uintptr_t page) noexcept
{
	if (page == 0 || m_pages == nullptr)
	{
		return nullptr;
	}
	const size_t start = HashPage(page) & (kPageTableSize - 1u);
	for (size_t i = 0; i < kPageTableSize; i++)
	{
		auto&           entry = m_pages[(start + i) & (kPageTableSize - 1u)];
		const uintptr_t key   = entry.key.load(std::memory_order_acquire);
		if (key == page)
		{
			return &entry;
		}
		if (key == 0)
		{
			return nullptr;
		}
	}
	return nullptr;
}

const GpuDirtyPageTracker::PageEntry* GpuDirtyPageTracker::FindPage(uintptr_t page) const noexcept
{
	return const_cast<GpuDirtyPageTracker*>(this)->FindPage(page);
}

GpuDirtyPageTracker::PageEntry* GpuDirtyPageTracker::FindOrCreatePage(uintptr_t page) noexcept
{
	const size_t start = HashPage(page) & (kPageTableSize - 1u);
	for (size_t i = 0; i < kPageTableSize; i++)
	{
		auto&           entry = m_pages[(start + i) & (kPageTableSize - 1u)];
		const uintptr_t key   = entry.key.load(std::memory_order_relaxed);
		if (key == page)
		{
			if (entry.refs.load(std::memory_order_acquire) == 0u &&
			    entry.protection_state.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyProtectionState::Retired))
			{
				entry.generation.store(0, std::memory_order_relaxed);
				entry.protection_state.store(static_cast<uint32_t>(GpuDirtyProtectionState::Writable), std::memory_order_release);
			}
			return &entry;
		}
		if (key == 0 || key == kTombstoneKey)
		{
			entry.generation.store(0, std::memory_order_relaxed);
			entry.refs.store(0, std::memory_order_relaxed);
			entry.protection_state.store(static_cast<uint32_t>(GpuDirtyProtectionState::Writable), std::memory_order_relaxed);
			entry.original_mode_valid.store(0, std::memory_order_relaxed);
			entry.key.store(page, std::memory_order_release);
			return &entry;
		}
	}
	return nullptr;
}

GpuDirtyPageTracker::RangeEntry* GpuDirtyPageTracker::FindRange(uintptr_t address, size_t size) noexcept
{
	const uintptr_t end = RangeEnd(address, size);
	if (end == 0)
	{
		return nullptr;
	}
	for (size_t i = 0; i < kMaxRanges; i++)
	{
		auto& range = m_ranges[i];
		if (range.active.load(std::memory_order_acquire) != 0u && range.begin == address && range.end == end)
		{
			return &range;
		}
	}
	return nullptr;
}

const GpuDirtyPageTracker::RangeEntry* GpuDirtyPageTracker::FindRange(uintptr_t address, size_t size) const noexcept
{
	return const_cast<GpuDirtyPageTracker*>(this)->FindRange(address, size);
}

bool GpuDirtyPageTracker::RegisterRange(uintptr_t address, size_t size) noexcept
{
	if (!m_enabled || m_page_size == 0 || address == 0 || size == 0)
	{
		return false;
	}
	const uintptr_t end   = RangeEnd(address, size);
	const uintptr_t first = PageStart(address);
	const uintptr_t last  = PageStart(end - 1u);
	if (end == 0 || last < first || last - first > m_page_size * kMaxPages)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(*m_registration_mutex);
	if (FindRange(address, size) != nullptr)
	{
		FindRange(address, size)->refs.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	RangeEntry* range = nullptr;
	for (size_t i = 0; i < kMaxRanges; i++)
	{
		if (m_ranges[i].active.load(std::memory_order_relaxed) == 0u)
		{
			range = &m_ranges[i];
			break;
		}
	}
	if (range == nullptr)
	{
		return false;
	}

	for (uintptr_t page = first;; page += m_page_size)
	{
		PageEntry* entry = FindOrCreatePage(page);
		if (entry == nullptr)
		{
			for (uintptr_t rollback = first;; rollback += m_page_size)
			{
				if (PageEntry* old = FindPage(rollback); old != nullptr)
				{
					const uint32_t refs = old->refs.load(std::memory_order_relaxed);
					if (refs > 0u)
					{
						old->refs.fetch_sub(1, std::memory_order_relaxed);
					}
					if (old->refs.load(std::memory_order_relaxed) == 0u)
					{
						old->key.store(kTombstoneKey, std::memory_order_release);
					}
				}
				if (rollback == page || rollback > last - m_page_size)
				{
					break;
				}
			}
			return false;
		}
		entry->refs.fetch_add(1, std::memory_order_relaxed);
		if (page == last || page > last - m_page_size)
		{
			break;
		}
	}

	range->begin = address;
	range->end   = end;
	range->generation.store(0, std::memory_order_relaxed);
	range->refs.store(1, std::memory_order_relaxed);
	range->mode.store(static_cast<uint32_t>(GpuDirtyTrackingMode::PageFault), std::memory_order_relaxed);
	range->active.store(1, std::memory_order_release);
	return true;
}

bool GpuDirtyPageTracker::UnregisterRange(uintptr_t address, size_t size) noexcept
{
	if (!m_enabled || m_registration_mutex == nullptr)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(*m_registration_mutex);
	RangeEntry*                 range = FindRange(address, size);
	if (range == nullptr)
	{
		return false;
	}
	const uint32_t old_range_refs = range->refs.load(std::memory_order_relaxed);
	if (old_range_refs > 1u)
	{
		range->refs.store(old_range_refs - 1u, std::memory_order_release);
		return true;
	}
	const uintptr_t first = PageStart(range->begin);
	const uintptr_t last  = PageStart(range->end - 1u);
	range->active.store(0, std::memory_order_release);
	bool result = true;
	for (uintptr_t page = first;; page += m_page_size)
	{
		if (PageEntry* entry = FindPage(page); entry != nullptr)
		{
			const uint32_t refs = entry->refs.load(std::memory_order_acquire);
			if (refs == 1u)
			{
				// Claim the protection transition before publishing the last
				// reference removal. Fault handlers then keep treating a write
				// during restore as tracker-owned instead of a guest violation.
				entry->protection_state.exchange(static_cast<uint32_t>(GpuDirtyProtectionState::Disarming), std::memory_order_acq_rel);
				entry->refs.store(0, std::memory_order_release);
				if (entry->original_mode_valid.load(std::memory_order_acquire) != 0u)
				{
					const auto mode = static_cast<Core::VirtualMemory::Mode>(entry->original_mode.load(std::memory_order_relaxed));
					result          = m_protection_ops.protect(m_protection_ops.context, page, m_page_size, mode, nullptr) && result;
				}
				entry->protection_state.store(static_cast<uint32_t>(GpuDirtyProtectionState::Retired), std::memory_order_release);
			} else if (refs > 1u)
			{
				entry->refs.fetch_sub(1, std::memory_order_acq_rel);
			}
		}
		if (page == last || page > last - m_page_size)
		{
			break;
		}
	}
	return result;
}

bool GpuDirtyPageTracker::HasCover(uintptr_t page, uintptr_t end, bool* fallback) const noexcept
{
	bool covered = false;
	for (size_t i = 0; i < kMaxRanges; i++)
	{
		const auto& range = m_ranges[i];
		if (range.active.load(std::memory_order_acquire) == 0u || range.end <= page || range.begin >= end)
		{
			continue;
		}
		covered = true;
		if (range.mode.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyTrackingMode::HashFallback) && fallback != nullptr)
		{
			*fallback = true;
		}
	}
	return covered;
}

void GpuDirtyPageTracker::MarkFallback(uintptr_t page, uintptr_t end) noexcept
{
	for (size_t i = 0; i < kMaxRanges; i++)
	{
		auto& range = m_ranges[i];
		if (range.active.load(std::memory_order_acquire) != 0u && range.end > page && range.begin < end)
		{
			range.mode.store(static_cast<uint32_t>(GpuDirtyTrackingMode::HashFallback), std::memory_order_release);
		}
	}
}

void GpuDirtyPageTracker::MarkPageWrite(PageEntry* page) noexcept
{
	if (page == nullptr)
	{
		return;
	}
	const uint64_t epoch = m_epoch->fetch_add(1, std::memory_order_relaxed) + 1u;
	page->generation.fetch_add(1, std::memory_order_release);
	const uintptr_t address = page->key.load(std::memory_order_acquire);
	for (size_t i = 0; i < kMaxRanges; i++)
	{
		auto& range = m_ranges[i];
		if (range.active.load(std::memory_order_acquire) == 0u)
		{
			continue;
		}
		const bool overlaps_page = range.end > address && (range.begin <= address || range.begin - address < m_page_size);
		if (overlaps_page)
		{
			AtomicMax(&range.generation, epoch);
		}
	}
}

GpuDirtyReadObservation GpuDirtyPageTracker::BeginRead(uintptr_t address, size_t size) noexcept
{
	GpuDirtyReadObservation observation;
	if (!PrepareForRead(address, size))
	{
		return observation;
	}
	observation.generation = SnapshotGeneration(address, size);
	observation.tracked    = true;
	return observation;
}

bool GpuDirtyPageTracker::PrepareForRead(uintptr_t address, size_t size) noexcept
{
	if (!m_enabled || m_page_size == 0 || address == 0 || size == 0)
	{
		return false;
	}
	if (auto* range = FindRange(address, size); range != nullptr)
	{
		if (range->mode.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyTrackingMode::HashFallback))
		{
			return false;
		}
		return Rearm(address, size);
	}
	const uintptr_t end   = RangeEnd(address, size);
	const uintptr_t first = PageStart(address);
	const uintptr_t last  = end == 0 ? 0 : PageStart(end - 1u);
	if (end == 0 || last < first)
	{
		return false;
	}
	bool fallback = false;
	for (uintptr_t page = first;; page += m_page_size)
	{
		if (FindPage(page) == nullptr || !HasCover(page, page + m_page_size, &fallback))
		{
			fallback = true;
		}
		if (page == last || page > last - m_page_size)
		{
			break;
		}
	}
	if (fallback)
	{
		MarkFallback(first, end);
		return false;
	}
	return Rearm(address, size);
}

bool GpuDirtyPageTracker::Rearm(uintptr_t address, size_t size) noexcept
{
	if (!m_enabled || m_page_size == 0 || address == 0 || size == 0)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(*m_registration_mutex);
	const uintptr_t             end   = RangeEnd(address, size);
	const uintptr_t             first = PageStart(address);
	const uintptr_t             last  = end == 0 ? 0 : PageStart(end - 1u);
	if (end == 0 || last < first)
	{
		return false;
	}
	auto finalize_arming = [this](uintptr_t page, PageEntry* entry) noexcept
	{
		uint32_t expected = static_cast<uint32_t>(GpuDirtyProtectionState::Arming);
		if (!entry->protection_state.compare_exchange_strong(expected, static_cast<uint32_t>(GpuDirtyProtectionState::Armed),
		                                                     std::memory_order_release, std::memory_order_acquire))
		{
			while (expected == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
			{
				std::this_thread::yield();
				expected = entry->protection_state.load(std::memory_order_acquire);
			}
			// A writer can finish Disarming before this thread enters mprotect.
			// In that ordering mprotect makes the page read-only after the
			// writer published Writable. Claim a rollback only if no newer
			// arming transaction has started.
			if (GpuDirtyProtectionStateNeedsArmingRollback(static_cast<GpuDirtyProtectionState>(expected)))
			{
				const uint32_t rollback_state = expected;
				if (entry->protection_state.compare_exchange_strong(expected, static_cast<uint32_t>(GpuDirtyProtectionState::Disarming),
				                                                    std::memory_order_acq_rel, std::memory_order_acquire))
				{
					const bool restored = m_protection_ops.protect_write_signal_safe(m_protection_ops.context, page, m_page_size);
					entry->protection_state.store(restored ? rollback_state
					                                       : (rollback_state == static_cast<uint32_t>(GpuDirtyProtectionState::Retired)
					                                              ? rollback_state
					                                              : static_cast<uint32_t>(GpuDirtyProtectionState::Armed)),
					                              std::memory_order_release);
					if (!restored)
					{
						MarkFallback(page, PageEnd(page));
					}
				}
			}
		}
	};

	for (uintptr_t page = first; page <= last;)
	{
		auto* entry = FindPage(page);
		if (entry == nullptr)
		{
			MarkFallback(first, end);
			return false;
		}
		if (entry->refs.load(std::memory_order_acquire) == 0u ||
		    entry->protection_state.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyProtectionState::Retired))
		{
			MarkFallback(first, end);
			return false;
		}
		uint32_t expected = static_cast<uint32_t>(GpuDirtyProtectionState::Writable);
		if (!entry->protection_state.compare_exchange_strong(expected, static_cast<uint32_t>(GpuDirtyProtectionState::Arming),
		                                                     std::memory_order_acq_rel, std::memory_order_acquire))
		{
			if (page == last || page > last - m_page_size)
			{
				break;
			}
			page += m_page_size;
			continue;
		}

		if (entry->original_mode_valid.load(std::memory_order_acquire) == 0u)
		{
			Core::VirtualMemory::Mode original = Core::VirtualMemory::Mode::ReadWrite;
			if (!m_protection_ops.protect(m_protection_ops.context, page, m_page_size, Core::VirtualMemory::Mode::Read, &original) ||
			    original == Core::VirtualMemory::Mode::NoAccess)
			{
				entry->protection_state.store(static_cast<uint32_t>(GpuDirtyProtectionState::Writable), std::memory_order_release);
				MarkFallback(first, end);
				return false;
			}
			entry->original_mode.store(static_cast<uint32_t>(original), std::memory_order_relaxed);
			entry->original_mode_valid.store(1, std::memory_order_release);
			finalize_arming(page, entry);
			if (page == last || page > last - m_page_size)
			{
				break;
			}
			page += m_page_size;
			continue;
		}

		const auto target_mode = ReadOnlyMode(static_cast<Core::VirtualMemory::Mode>(entry->original_mode.load(std::memory_order_relaxed)));
		const uintptr_t run_start = page;
		uintptr_t       run_last  = page;
		while (run_last != last && run_last <= last - m_page_size)
		{
			const uintptr_t next_page = run_last + m_page_size;
			auto*           next      = FindPage(next_page);
			if (next == nullptr || next->refs.load(std::memory_order_acquire) == 0u ||
			    next->protection_state.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyProtectionState::Retired) ||
			    next->original_mode_valid.load(std::memory_order_acquire) == 0u ||
			    ReadOnlyMode(static_cast<Core::VirtualMemory::Mode>(next->original_mode.load(std::memory_order_relaxed))) != target_mode)
			{
				break;
			}
			uint32_t next_expected = static_cast<uint32_t>(GpuDirtyProtectionState::Writable);
			if (!next->protection_state.compare_exchange_strong(next_expected, static_cast<uint32_t>(GpuDirtyProtectionState::Arming),
			                                                    std::memory_order_acq_rel, std::memory_order_acquire))
			{
				break;
			}
			run_last = next_page;
		}
		const size_t run_size = run_last - run_start + m_page_size;

		if (!m_protection_ops.protect(m_protection_ops.context, run_start, run_size, target_mode, nullptr))
		{
			for (uintptr_t rollback = run_start;; rollback += m_page_size)
			{
				if (auto* claimed = FindPage(rollback); claimed != nullptr)
				{
					uint32_t state = claimed->protection_state.load(std::memory_order_acquire);
					while (state == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
					{
						std::this_thread::yield();
						state = claimed->protection_state.load(std::memory_order_acquire);
					}
					while (state == static_cast<uint32_t>(GpuDirtyProtectionState::Arming) ||
					       state == static_cast<uint32_t>(GpuDirtyProtectionState::Writable))
					{
						const uint32_t restore_state = state;
						if (claimed->protection_state.compare_exchange_strong(state,
						                                                      static_cast<uint32_t>(GpuDirtyProtectionState::Disarming),
						                                                      std::memory_order_acq_rel, std::memory_order_acquire))
						{
							const auto original =
							    static_cast<Core::VirtualMemory::Mode>(claimed->original_mode.load(std::memory_order_relaxed));
							bool restored = m_protection_ops.protect(m_protection_ops.context, rollback, m_page_size, original, nullptr);
							if (!restored)
							{
								restored = m_protection_ops.protect_write_signal_safe(m_protection_ops.context, rollback, m_page_size);
							}
							claimed->protection_state.store(restored
							                                    ? (restore_state == static_cast<uint32_t>(GpuDirtyProtectionState::Arming)
							                                           ? static_cast<uint32_t>(GpuDirtyProtectionState::Writable)
							                                           : restore_state)
							                                    : static_cast<uint32_t>(GpuDirtyProtectionState::Armed),
							                                std::memory_order_release);
							break;
						}
						while (state == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
						{
							std::this_thread::yield();
							state = claimed->protection_state.load(std::memory_order_acquire);
						}
					}
				}
				if (rollback == run_last || rollback > run_last - m_page_size)
				{
					break;
				}
			}
			MarkFallback(first, end);
			return false;
		}
		for (uintptr_t armed = run_start;; armed += m_page_size)
		{
			finalize_arming(armed, FindPage(armed));
			if (armed == run_last || armed > run_last - m_page_size)
			{
				break;
			}
		}
		if (run_last == last || run_last > last - m_page_size)
		{
			break;
		}
		page = run_last + m_page_size;
	}
	return true;
}

bool GpuDirtyPageTracker::HandleWriteFault(uintptr_t address) noexcept
{
	if (!m_enabled)
	{
		return false;
	}
	const uintptr_t page_address = PageStart(address);
	PageEntry*      entry        = FindPage(page_address);
	if (entry == nullptr)
	{
		return false;
	}

	uint32_t state = entry->protection_state.load(std::memory_order_acquire);
	if ((entry->refs.load(std::memory_order_acquire) == 0u && state != static_cast<uint32_t>(GpuDirtyProtectionState::Retired) &&
	     state != static_cast<uint32_t>(GpuDirtyProtectionState::Disarming)) ||
	    !GpuDirtyProtectionStateHandlesFault(static_cast<GpuDirtyProtectionState>(state)))
	{
		return false;
	}
	if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Retired))
	{
		if (entry->original_mode_valid.load(std::memory_order_acquire) == 0u)
		{
			return false;
		}
		const auto original = static_cast<Core::VirtualMemory::Mode>(entry->original_mode.load(std::memory_order_relaxed));
		const bool writable = (static_cast<uint32_t>(original) & static_cast<uint32_t>(Core::VirtualMemory::Mode::Write)) != 0u;
		return writable && m_protection_ops.protect_write_signal_safe(m_protection_ops.context, page_address, m_page_size);
	}
	for (;;)
	{
		if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Writable))
		{
			return false;
		}
		if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
		{
			// Another fault handler owns the permission restore. This fault is
			// still tracker-induced; retrying after that handler completes is safe.
			return true;
		}
		if (entry->protection_state.compare_exchange_weak(state, static_cast<uint32_t>(GpuDirtyProtectionState::Disarming),
		                                                  std::memory_order_acq_rel, std::memory_order_acquire))
		{
			break;
		}
	}

	MarkPageWrite(entry);
	const bool restored = m_protection_ops.protect_write_signal_safe(m_protection_ops.context, page_address, m_page_size);
	entry->protection_state.store(static_cast<uint32_t>(restored ? GpuDirtyProtectionState::Writable : GpuDirtyProtectionState::Armed),
	                              std::memory_order_release);
	if (!restored)
	{
		MarkFallback(page_address, PageEnd(page_address));
		return false;
	}
	return true;
}

bool GpuDirtyPageTracker::NotifyWrite(uintptr_t address, size_t size) noexcept
{
	if (!m_enabled || m_page_size == 0 || address == 0 || size == 0)
	{
		return false;
	}
	const uintptr_t end = RangeEnd(address, size);
	if (end == 0)
	{
		return false;
	}
	bool            handled = false;
	const uintptr_t first   = PageStart(address);
	const uintptr_t last    = PageStart(end - 1u);
	for (uintptr_t page_address = first;; page_address += m_page_size)
	{
		if (auto* entry = FindPage(page_address); entry != nullptr && entry->refs.load(std::memory_order_acquire) != 0u)
		{
			handled        = true;
			uint32_t state = entry->protection_state.load(std::memory_order_acquire);
			while (state != static_cast<uint32_t>(GpuDirtyProtectionState::Writable) &&
			       state != static_cast<uint32_t>(GpuDirtyProtectionState::Disarming) &&
			       !entry->protection_state.compare_exchange_weak(state, static_cast<uint32_t>(GpuDirtyProtectionState::Disarming),
			                                                      std::memory_order_acq_rel, std::memory_order_acquire))
			{
			}
			if (state != static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
			{
				MarkPageWrite(entry);
			}
			if (state != static_cast<uint32_t>(GpuDirtyProtectionState::Writable) &&
			    state != static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
			{
				const bool restored = m_protection_ops.protect_write_signal_safe(m_protection_ops.context, page_address, m_page_size);
				entry->protection_state.store(
				    static_cast<uint32_t>(restored ? GpuDirtyProtectionState::Writable : GpuDirtyProtectionState::Armed),
				    std::memory_order_release);
				if (!restored)
				{
					MarkFallback(page_address, PageEnd(page_address));
				}
			}
		}
		if (page_address == last || page_address > last - m_page_size)
		{
			break;
		}
	}
	return handled;
}

uint64_t GpuDirtyPageTracker::SnapshotGeneration(uintptr_t address, size_t size) const noexcept
{
	if (!m_enabled || m_ranges == nullptr)
	{
		return 0;
	}
	if (const auto* range = FindRange(address, size); range != nullptr)
	{
		return range->generation.load(std::memory_order_acquire);
	}
	const uintptr_t end = RangeEnd(address, size);
	if (end == 0)
	{
		return 0;
	}
	uint64_t snapshot = 0;
	for (size_t i = 0; i < kMaxRanges; i++)
	{
		const auto& range = m_ranges[i];
		if (range.active.load(std::memory_order_acquire) != 0u && range.end > address && range.begin < end)
		{
			const uint64_t generation = range.generation.load(std::memory_order_acquire);
			if (generation > snapshot)
			{
				snapshot = generation;
			}
		}
	}
	return snapshot;
}

bool GpuDirtyPageTracker::ChangedSince(uintptr_t address, size_t size, uint64_t snapshot) const noexcept
{
	if (!m_enabled || m_ranges == nullptr)
	{
		return true;
	}
	if (const auto* range = FindRange(address, size); range != nullptr)
	{
		return range->mode.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyTrackingMode::HashFallback) ||
		       range->generation.load(std::memory_order_acquire) > snapshot;
	}
	const uintptr_t end = RangeEnd(address, size);
	if (end == 0)
	{
		return true;
	}
	bool found = false;
	for (size_t i = 0; i < kMaxRanges; i++)
	{
		const auto& range = m_ranges[i];
		if (range.active.load(std::memory_order_acquire) == 0u || range.end <= address || range.begin >= end)
		{
			continue;
		}
		found = true;
		if (range.mode.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyTrackingMode::HashFallback) ||
		    range.generation.load(std::memory_order_acquire) > snapshot)
		{
			return true;
		}
	}
	return !found;
}

bool GpuDirtyPageTracker::Enabled() const noexcept
{
	return m_enabled && m_page_size != 0 && m_pages != nullptr && m_ranges != nullptr;
}

GpuDirtyTrackingMode GpuDirtyPageTracker::Mode(uintptr_t address, size_t size) const noexcept
{
	if (!m_enabled || m_ranges == nullptr)
	{
		return GpuDirtyTrackingMode::HashFallback;
	}
	if (const auto* range = FindRange(address, size); range != nullptr)
	{
		return static_cast<GpuDirtyTrackingMode>(range->mode.load(std::memory_order_acquire));
	}
	const uintptr_t end = RangeEnd(address, size);
	if (end == 0)
	{
		return GpuDirtyTrackingMode::HashFallback;
	}
	bool found = false;
	for (size_t i = 0; i < kMaxRanges; i++)
	{
		const auto& range = m_ranges[i];
		if (range.active.load(std::memory_order_acquire) != 0u && range.end > address && range.begin < end)
		{
			found = true;
			if (range.mode.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyTrackingMode::HashFallback))
			{
				return GpuDirtyTrackingMode::HashFallback;
			}
		}
	}
	return found ? GpuDirtyTrackingMode::PageFault : GpuDirtyTrackingMode::HashFallback;
}

GpuDirtyPageTracker& GetGpuDirtyPageTracker() noexcept
{
	static GpuDirtyPageTracker tracker(GpuDirtyTrackingEnabledForProcess(std::getenv("KYTY_DISABLE_GPU_DIRTY_TRACKING"),
	                                                                     g_fault_handler_ready.load(std::memory_order_acquire)));
	return tracker;
}

void GpuDirtyPageTrackerNotifyFaultHandlerInstalled() noexcept
{
	g_fault_handler_ready.store(true, std::memory_order_release);
}

GpuDirtyPageTracker& GpuDirtyPageTracker::Instance() noexcept
{
	return GetGpuDirtyPageTracker();
}

} // namespace Kyty::Libs::Graphics
