#include "Emulator/Graphics/GpuDirtyPageTracker.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/VirtualMemory.h"

#include <algorithm>
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
	// Global epochs of the last write and the last failed native transition.
	std::atomic<uint64_t>  write_epoch {0};
	std::atomic<uint64_t>  fallback_epoch {0};
	std::atomic<uint32_t>  block {kNoBlock};
	std::atomic<uint32_t>  refs {0};
	std::atomic<uint32_t>  protection_state {static_cast<uint32_t>(GpuDirtyProtectionState::Writable)};
	std::atomic<uint32_t>  original_mode {0};
	std::atomic<uint32_t>  original_token {0};
	std::atomic<uint32_t>  original_mode_valid {0};
};

// Maximum epochs of kBlockPages consecutive pages. The fault path raises them
// together with the page's own, so a fully covered block answers a range query
// without visiting its pages.
struct GpuDirtyPageTracker::BlockEntry
{
	std::atomic<uintptr_t> key {0};
	std::atomic<uint32_t>  pages {0};
	// Exact page identities, including retained late-fault metadata.
	// Identity-gate admission protects readers from block-slot reuse.
	std::atomic<uint64_t>  page_mask {0};
	std::atomic<uint64_t>  write_epoch {0};
	std::atomic<uint64_t>  fallback_epoch {0};
	// Advanced after each mark is published; edge caches compare it.
	std::atomic<uint64_t>  changes {0};
};

namespace {

constexpr size_t kLookupProbeLimit = 8u;

constexpr uintptr_t kTombstoneKey      = 1u;
constexpr uintptr_t kBlockTombstoneKey = std::numeric_limits<uintptr_t>::max();
// The process-wide tracker, published once by the startup thread after the
// fault handler is installed. The signal route only loads this pointer.
std::atomic<GpuDirtyPageTracker*> g_process_tracker {nullptr};

void AtomicMax(std::atomic<uint64_t>* target, uint64_t value) noexcept
{
	uint64_t current = target->load(std::memory_order_relaxed);
	while (current < value && !target->compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed))
	{
	}
}

bool ModeGrantsWrite(uint32_t mode) noexcept
{
	return (mode & static_cast<uint32_t>(Core::VirtualMemory::Mode::Write)) != 0u;
}

Core::VirtualMemory::ProtectionChangeResult DefaultRemoveWriteAndCapture(void*, uintptr_t address, size_t size,
                                                                         Core::VirtualMemory::CapturedProtectionVisitor visitor,
                                                                         void*                                          context,
                                                                         const Core::VirtualMemory::WriteLeaseAuthority* authority) noexcept
{
	return Core::VirtualMemory::RemoveWriteAndCapture(address, size, visitor, context, authority);
}

bool DefaultRemoveWrite(void*, uintptr_t address, size_t size, uint32_t restore_token,
                        const Core::VirtualMemory::WriteLeaseAuthority* authority) noexcept
{
	return Core::VirtualMemory::RemoveWriteFromProtection(address, size, restore_token, authority);
}

bool DefaultRestore(void*, uintptr_t address, size_t size, uint32_t restore_token) noexcept
{
	return Core::VirtualMemory::RestoreProtection(address, size, restore_token);
}

bool DefaultRestoreSignalSafe(void*, uintptr_t address, size_t size, uint32_t restore_token) noexcept
{
	return Core::VirtualMemory::RestoreProtectionSignalSafe(address, size, restore_token);
}

bool DefaultReleaseLeases(void*, const Core::VirtualMemory::WriteLeaseAuthority* authority) noexcept
{
	return Core::VirtualMemory::ReleaseWriteLeases(authority);
}

} // namespace

GpuDirtyPageTracker::GpuDirtyPageTracker(bool enabled)
    : GpuDirtyPageTracker(GpuDirtyPageProtectionOps {nullptr, &DefaultRemoveWriteAndCapture, &DefaultRemoveWrite, &DefaultRestore,
                                                     &DefaultRestoreSignalSafe, &DefaultReleaseLeases},
                          enabled)
{
}

GpuDirtyPageTracker::GpuDirtyPageTracker(const GpuDirtyPageProtectionOps& protection_ops, bool enabled)
    : m_page_size(Core::VirtualMemory::GetPageSize()), m_pages(enabled ? new PageEntry[kPageTableSize] : nullptr),
      m_blocks(enabled ? new BlockEntry[kBlockTableSize] : nullptr), m_registration_mutex(enabled ? new std::mutex : nullptr),
      m_epoch(enabled ? new std::atomic<uint64_t>(0) : nullptr), m_protection_ops(protection_ops),
      m_authority {this, &AuthorityBeginChange, &AuthorityDecide, &AuthorityEndChange},
      m_enabled(enabled && protection_ops.remove_write_and_capture != nullptr && protection_ops.remove_write != nullptr &&
                protection_ops.restore != nullptr && protection_ops.restore_signal_safe != nullptr)
{
}

GpuDirtyPageTracker::~GpuDirtyPageTracker()
{
	// Leased pages return to their guest protection, and the VM no longer
	// refers to this tracker's authority. A page left without write access
	// could never be served again: its next guest write would fault with
	// nobody to restore it.
	if (m_enabled && m_protection_ops.release_leases != nullptr &&
	    !m_protection_ops.release_leases(m_protection_ops.context, &m_authority))
	{
		EXIT("dirty-page tracker could not restore the guest protection of its leased pages\n");
	}
	delete m_epoch;
	delete m_registration_mutex;
}

void GpuDirtyPageTracker::AcquirePublisher() noexcept
{
	uint32_t state = m_identity_gate.load(std::memory_order_acquire);
	for (;;)
	{
		const uint32_t publishers     = state & kIdentityPublisherCountMask;
		const bool     writer_pending = (state & kIdentityWriterPending) != 0u;
		// Protect and BeginUnmap announce every run before ending any. Let those
		// callbacks join while the publisher count is nonzero. Per-run callback
		// pairs may drain to zero; a pending writer then owns admission.
		if (publishers == kIdentityPublisherCountMask || (writer_pending && publishers == 0u))
		{
			std::this_thread::yield();
			state = m_identity_gate.load(std::memory_order_acquire);
			continue;
		}
		if (m_identity_gate.compare_exchange_weak(state, state + 1u, std::memory_order_acq_rel, std::memory_order_acquire))
		{
			return;
		}
	}
}

void GpuDirtyPageTracker::ReleasePublisher() noexcept
{
	m_identity_gate.fetch_sub(1u, std::memory_order_release);
}

void GpuDirtyPageTracker::AcquireIdentityWriter() noexcept
{
	uint32_t state = m_identity_gate.load(std::memory_order_acquire);
	for (;;)
	{
		if ((state & kIdentityWriterPending) != 0u)
		{
			std::this_thread::yield();
			state = m_identity_gate.load(std::memory_order_acquire);
			continue;
		}
		if (m_identity_gate.compare_exchange_weak(state, state | kIdentityWriterPending, std::memory_order_acq_rel,
		                                          std::memory_order_acquire))
		{
			break;
		}
	}
	while ((m_identity_gate.load(std::memory_order_acquire) & kIdentityPublisherCountMask) != 0u)
	{
		std::this_thread::yield();
	}
}

void GpuDirtyPageTracker::ReleaseIdentityWriter() noexcept
{
	EXIT_IF(m_identity_gate.load(std::memory_order_relaxed) != kIdentityWriterPending);
	m_identity_gate.store(0u, std::memory_order_release);
}

// Restorers are the paths that apply a captured token without the VM lock.
// Each one announces itself before it resolves page metadata and leaves after
// its native restore. A fence holder (a VM change, or a page slot changing
// identity) raises the fence first and then waits for announced restorers, so
// the two never overlap. The announcement never waits: a restorer that sees a
// fence withdraws at once. Fence holders run on normal threads and never touch
// tracked guest memory while fenced.
bool GpuDirtyPageTracker::TryEnterRestorer() noexcept
{
	m_restorers.fetch_add(1, std::memory_order_seq_cst);
	if (m_authority_fences.load(std::memory_order_seq_cst) == 0u)
	{
		return true;
	}
	m_restorers.fetch_sub(1, std::memory_order_seq_cst);
	return false;
}

void GpuDirtyPageTracker::LeaveRestorer() noexcept
{
	m_restorers.fetch_sub(1, std::memory_order_release);
}

// Normal threads only: waits for the current fence to drop.
void GpuDirtyPageTracker::EnterRestorer() noexcept
{
	while (!TryEnterRestorer())
	{
		std::this_thread::yield();
	}
}

// Normal threads only.
void GpuDirtyPageTracker::RaiseFence() noexcept
{
	m_authority_fences.fetch_add(1, std::memory_order_seq_cst);
	while (m_restorers.load(std::memory_order_seq_cst) != 0u)
	{
		std::this_thread::yield();
	}
}

void GpuDirtyPageTracker::LowerFence() noexcept
{
	m_authority_fences.fetch_sub(1, std::memory_order_release);
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
	const size_t start = GpuDirtyPageTableIndex(page, kPageTableSize - 1u);
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

GpuDirtyPageTracker::PageEntry* GpuDirtyPageTracker::FindWritePage(uintptr_t page) noexcept
{
	if (page == 0 || m_pages == nullptr)
	{
		return nullptr;
	}
	const size_t start = GpuDirtyPageTableIndex(page, kPageTableSize - 1u);
	// Preserve shallow hits without admission. A saturated table otherwise
	// makes every notification for an untracked page walk the entire table.
	for (size_t i = 0; i < kLookupProbeLimit; ++i)
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

	// Normal-thread lookups try admission once and never join a pending
	// identity writer. Contention retains the ordinary exact lookup. The
	// signal handler uses FindPage directly and never takes this gate.
	uint32_t state = m_identity_gate.load(std::memory_order_acquire);
	if ((state & kIdentityWriterPending) != 0u || (state & kIdentityPublisherCountMask) == kIdentityPublisherCountMask ||
	    !m_identity_gate.compare_exchange_strong(state, state + 1u, std::memory_order_acq_rel, std::memory_order_acquire))
	{
		return FindPage(page);
	}
	const auto may_contain_page = [this, page]() noexcept
	{
		const uintptr_t key   = BlockKey(page);
		const size_t    start = GpuDirtyPageTableIndex(key << 12u, kBlockTableSize - 1u);
		const uint64_t  bit   = uint64_t {1} << ((page / m_page_size) % kBlockPages);
		for (size_t i = 0; i < kLookupProbeLimit; ++i)
		{
			const auto&     block   = m_blocks[(start + i) & (kBlockTableSize - 1u)];
			const uintptr_t current = block.key.load(std::memory_order_acquire);
			if (current == key)
			{
				return (block.page_mask.load(std::memory_order_acquire) & bit) != 0u;
			}
			if (current == 0)
			{
				return false;
			}
		}
		// A deep block collision is inconclusive. Never turn a short page
		// miss into a full secondary-table scan, or skip a hidden identity.
		return true;
	};
	const bool exists = may_contain_page();
	ReleasePublisher();
	// Release admission before a notification can wait on a page transition.
	// Restart a positive lookup: an identity writer may now reuse any slot.
	return exists ? FindPage(page) : nullptr;
}

void GpuDirtyPageTracker::ReactivateRetiredPage(PageEntry* entry) noexcept
{
	if (entry->refs.load(std::memory_order_acquire) != 0u ||
	    entry->protection_state.load(std::memory_order_acquire) != static_cast<uint32_t>(GpuDirtyProtectionState::Retired))
	{
		return;
	}
	AcquireIdentityWriter();
	// A late fault may still be resolving the retired protection token.
	RaiseFence();
	if (entry->refs.load(std::memory_order_acquire) == 0u &&
	    entry->protection_state.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyProtectionState::Retired))
	{
		entry->generation.store(0, std::memory_order_relaxed);
		entry->write_epoch.store(0, std::memory_order_relaxed);
		entry->fallback_epoch.store(0, std::memory_order_relaxed);
		entry->original_mode.store(0, std::memory_order_relaxed);
		entry->original_token.store(0, std::memory_order_relaxed);
		entry->original_mode_valid.store(0, std::memory_order_relaxed);
		entry->protection_state.store(static_cast<uint32_t>(GpuDirtyProtectionState::Writable), std::memory_order_release);
	}
	LowerFence();
	ReleaseIdentityWriter();
}

GpuDirtyPageTracker::PageEntry* GpuDirtyPageTracker::FindOrCreatePage(uintptr_t page) noexcept
{
	const size_t start = GpuDirtyPageTableIndex(page, kPageTableSize - 1u);
	PageEntry*   first_tombstone = nullptr;
	auto         initialize      = [this, page](PageEntry* entry) noexcept -> PageEntry*
	{
		AcquireIdentityWriter();
		const uint32_t block = FindOrCreateBlock(page);
		if (block == kNoBlock)
		{
			ReleaseIdentityWriter();
			return nullptr;
		}
		m_blocks[block].pages.fetch_add(1, std::memory_order_relaxed);
		entry->generation.store(0, std::memory_order_relaxed);
		entry->write_epoch.store(0, std::memory_order_relaxed);
		entry->fallback_epoch.store(0, std::memory_order_relaxed);
		entry->block.store(block, std::memory_order_relaxed);
		entry->refs.store(0, std::memory_order_relaxed);
		entry->protection_state.store(static_cast<uint32_t>(GpuDirtyProtectionState::Writable), std::memory_order_relaxed);
		entry->original_mode.store(0, std::memory_order_relaxed);
		entry->original_token.store(0, std::memory_order_relaxed);
		entry->original_mode_valid.store(0, std::memory_order_relaxed);
		m_blocks[block].page_mask.fetch_or(uint64_t {1} << ((page / m_page_size) % kBlockPages), std::memory_order_relaxed);
		entry->key.store(page, std::memory_order_release);
		++m_page_identity_count;
		ReleaseIdentityWriter();
		return entry;
	};
	for (size_t i = 0; i < kPageTableSize; i++)
	{
		auto&           entry = m_pages[(start + i) & (kPageTableSize - 1u)];
		const uintptr_t key   = entry.key.load(std::memory_order_relaxed);
		if (key == page)
		{
			ReactivateRetiredPage(&entry);
			return &entry;
		}
		if (key == kTombstoneKey)
		{
			if (first_tombstone == nullptr)
			{
				first_tombstone = &entry;
			}
			continue;
		}
		if (key == 0)
		{
			return initialize(first_tombstone != nullptr ? first_tombstone : &entry);
		}
	}
	return first_tombstone != nullptr ? initialize(first_tombstone) : nullptr;
}

// A slot changes identity only while publishers and restorers cannot hold it.
// Later fault lookups skip the tombstone and will resolve any reused key anew.
void GpuDirtyPageTracker::ReleasePageSlot(PageEntry* entry) noexcept
{
	AcquireIdentityWriter();
	RaiseFence();
	const uintptr_t page  = entry->key.load(std::memory_order_relaxed);
	const uint32_t block = entry->block.exchange(kNoBlock, std::memory_order_acq_rel);
	entry->key.store(kTombstoneKey, std::memory_order_release);
	--m_page_identity_count;
	if (block < kBlockTableSize)
	{
		m_blocks[block].page_mask.fetch_and(~(uint64_t {1} << ((page / m_page_size) % kBlockPages)), std::memory_order_relaxed);
		if (m_blocks[block].pages.fetch_sub(1, std::memory_order_acq_rel) == 1u)
		{
			m_blocks[block].key.store(kBlockTombstoneKey, std::memory_order_release);
		}
	}
	LowerFence();
	ReleaseIdentityWriter();
}

uintptr_t GpuDirtyPageTracker::BlockKey(uintptr_t page) const noexcept
{
	return page / m_page_size / kBlockPages + 1u;
}

const GpuDirtyPageTracker::BlockEntry* GpuDirtyPageTracker::FindBlock(uintptr_t page) const noexcept
{
	if (m_blocks == nullptr)
	{
		return nullptr;
	}
	const uintptr_t key   = BlockKey(page);
	const size_t    start = GpuDirtyPageTableIndex(key << 12u, kBlockTableSize - 1u);
	for (size_t i = 0; i < kBlockTableSize; i++)
	{
		const auto&     entry   = m_blocks[(start + i) & (kBlockTableSize - 1u)];
		const uintptr_t current = entry.key.load(std::memory_order_acquire);
		if (current == key)
		{
			return &entry;
		}
		if (current == 0)
		{
			return nullptr;
		}
	}
	return nullptr;
}

uint32_t GpuDirtyPageTracker::FindOrCreateBlock(uintptr_t page) noexcept
{
	const uintptr_t key             = BlockKey(page);
	const size_t    start           = GpuDirtyPageTableIndex(key << 12u, kBlockTableSize - 1u);
	size_t          first_tombstone = kBlockTableSize;
	auto initialize = [this, key](size_t index) noexcept
	{
		auto& entry = m_blocks[index];
		entry.pages.store(0, std::memory_order_relaxed);
		entry.page_mask.store(0, std::memory_order_relaxed);
		entry.write_epoch.store(0, std::memory_order_relaxed);
		entry.fallback_epoch.store(0, std::memory_order_relaxed);
		entry.changes.fetch_add(1, std::memory_order_relaxed);
		entry.key.store(key, std::memory_order_release);
		return static_cast<uint32_t>(index);
	};
	for (size_t i = 0; i < kBlockTableSize; i++)
	{
		const size_t    index   = (start + i) & (kBlockTableSize - 1u);
		const uintptr_t current = m_blocks[index].key.load(std::memory_order_relaxed);
		if (current == key)
		{
			return static_cast<uint32_t>(index);
		}
		if (current == kBlockTombstoneKey)
		{
			if (first_tombstone == kBlockTableSize)
			{
				first_tombstone = index;
			}
			continue;
		}
		if (current == 0)
		{
			return initialize(first_tombstone != kBlockTableSize ? first_tombstone : index);
		}
	}
	return first_tombstone != kBlockTableSize ? initialize(first_tombstone) : kNoBlock;
}

GpuDirtyPageTracker::BlockEntry* GpuDirtyPageTracker::BlockOf(const PageEntry* page) noexcept
{
	const uint32_t block = page->block.load(std::memory_order_acquire);
	return block < kBlockTableSize ? &m_blocks[block] : nullptr;
}

GpuDirtyPageTracker::RangeEvidence GpuDirtyPageTracker::SegmentEvidence(uintptr_t first_index, uintptr_t last_index) const noexcept
{
	RangeEvidence evidence;
	for (uintptr_t index = first_index;; index++)
	{
		if (const auto* entry = FindPage(index * m_page_size); entry != nullptr)
		{
			evidence.write_epoch    = std::max(evidence.write_epoch, entry->write_epoch.load(std::memory_order_acquire));
			evidence.fallback_epoch = std::max(evidence.fallback_epoch, entry->fallback_epoch.load(std::memory_order_acquire));
		} else
		{
			evidence.complete = false;
		}
		if (index == last_index)
		{
			break;
		}
	}
	return evidence;
}

// Fully covered blocks answer from their summaries. A partially covered block
// also summarizes neighbour pages, so the range's pages there are read one by
// one, and read again only after the block recorded another write or fallback.
GpuDirtyPageTracker::RangeEvidence GpuDirtyPageTracker::RangeEvidenceOf(const RangeKey& range, const RangeRecord& record) const noexcept
{
	RangeEvidence evidence;
	auto          merge = [&evidence](const RangeEvidence& part)
	{
		evidence.write_epoch    = std::max(evidence.write_epoch, part.write_epoch);
		evidence.fallback_epoch = std::max(evidence.fallback_epoch, part.fallback_epoch);
		evidence.complete       = evidence.complete && part.complete;
	};
	const uintptr_t first_index = PageStart(range.first) / m_page_size;
	const uintptr_t last_index  = PageStart(range.second - 1u) / m_page_size;
	for (uintptr_t index = first_index;;)
	{
		const uintptr_t block_first  = index - index % kBlockPages;
		const uintptr_t block_last   = block_first + (kBlockPages - 1u);
		const uintptr_t segment_last = std::min(block_last, last_index);
		const auto*     block        = FindBlock(index * m_page_size);
		if (block == nullptr)
		{
			merge(SegmentEvidence(index, segment_last));
		} else if (index == block_first && block_last <= last_index)
		{
			merge({block->write_epoch.load(std::memory_order_acquire), block->fallback_epoch.load(std::memory_order_acquire), true});
		} else
		{
			auto&          cache   = index == first_index ? record.head : record.tail;
			const uint64_t changes = block->changes.load(std::memory_order_acquire);
			if (!cache.valid || cache.changes != changes)
			{
				cache = {changes, SegmentEvidence(index, segment_last), true};
			}
			merge(cache.evidence);
		}
		if (segment_last == last_index)
		{
			break;
		}
		index = segment_last + 1u;
	}
	return evidence;
}

bool GpuDirtyPageTracker::RangeIsFallback(const RangeKey& range, const RangeRecord& record) const noexcept
{
	const auto evidence = RangeEvidenceOf(range, record);
	return !evidence.complete || evidence.fallback_epoch > record.registered_epoch;
}

template <typename Visitor>
void GpuDirtyPageTracker::VisitOverlappingRangesLocked(uintptr_t begin, uintptr_t end, const Visitor& visitor) const
{
	// No record starts more than m_max_range_bytes before a range it overlaps.
	const uintptr_t from = begin > m_max_range_bytes ? begin - m_max_range_bytes : 0u;
	for (auto it = m_ranges.lower_bound(RangeKey {from, 0u}); it != m_ranges.end() && it->first.first < end; ++it)
	{
		if (it->first.second > begin)
		{
			visitor(it->first, it->second);
		}
	}
}

template <typename Visitor>
void GpuDirtyPageTracker::VisitWritePages(uintptr_t first, uintptr_t last, const Visitor& visitor) noexcept
{
	if ((last - first) / m_page_size < kDirectWritePages)
	{
		for (uintptr_t page = first;; page += m_page_size)
		{
			if (auto* entry = FindWritePage(page); entry != nullptr)
			{
				visitor(entry, page);
			}
			if (page == last) { break; }
		}
		return;
	}

	// NotifyWrite takes no registration lock, allocation, mutable range snapshot
	// or walk across the caller's potentially enormous untracked address space.
	// Empty and retired slots still cost only one pass.
	for (size_t i = 0; i < kPageTableSize; ++i)
	{
		auto& entry = m_pages[i];
		const uintptr_t page = entry.key.load(std::memory_order_acquire);
		if (page > kTombstoneKey && page >= first && page <= last) { visitor(&entry, page); }
	}
}

void GpuDirtyPageTracker::ClaimPageForRetirement(PageEntry* entry) noexcept
{
	uint32_t state = entry->protection_state.load(std::memory_order_acquire);
	for (;;)
	{
		if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Capturing) ||
		    state == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
		{
			std::this_thread::yield();
			state = entry->protection_state.load(std::memory_order_acquire);
			continue;
		}
		if (entry->protection_state.compare_exchange_weak(state, static_cast<uint32_t>(GpuDirtyProtectionState::Disarming),
		                                                   std::memory_order_acq_rel, std::memory_order_acquire))
		{
			return;
		}
	}
}

bool GpuDirtyPageTracker::RestoreRetirementRun(uintptr_t first, uintptr_t last, uint32_t token) noexcept
{
	const size_t size = last - first + m_page_size;
	const bool batch_restored = m_protection_ops.restore(m_protection_ops.context, first, size, token);
	if (!batch_restored)
	{
		for (uintptr_t page = first;; page += m_page_size)
		{
			(void)m_protection_ops.restore(m_protection_ops.context, page, m_page_size, token);
			if (page == last || page > last - m_page_size)
			{
				break;
			}
		}
	}
	for (uintptr_t page = first;; page += m_page_size)
	{
		if (auto* entry = FindPage(page); entry != nullptr)
		{
			entry->protection_state.store(static_cast<uint32_t>(GpuDirtyProtectionState::Retired), std::memory_order_release);
		}
		if (page == last || page > last - m_page_size)
		{
			break;
		}
	}
	return batch_restored;
}

// Preflight the complete cover before changing refs or retired protection. A
// refused optimization must not discard a token still needed by a late fault.
bool GpuDirtyPageTracker::HasPageCapacity(uintptr_t first, uintptr_t last) const noexcept
{
	size_t remaining = kMaxPages - m_page_identity_count;
	for (uintptr_t page = first;; page += m_page_size)
	{
		if (FindPage(page) == nullptr)
		{
			if (remaining == 0u)
			{
				return false;
			}
			--remaining;
		}
		if (page == last || page > last - m_page_size)
		{
			break;
		}
	}
	return true;
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
	const RangeKey              key {address, end};
	if (auto existing = m_ranges.find(key); existing != m_ranges.end())
	{
		if (existing->second.refs == std::numeric_limits<uint32_t>::max())
		{
			return false;
		}
		existing->second.refs++;
		return true;
	}
	if (m_ranges.size() >= kMaxRanges)
	{
		return false;
	}
	if (!HasPageCapacity(first, last))
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
						ReleasePageSlot(old);
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

	// Fallback evidence marked before this epoch belongs to earlier owners.
	m_ranges.emplace(key, RangeRecord {1u, m_epoch->load(std::memory_order_acquire)});
	m_max_range_bytes = std::max(m_max_range_bytes, end - address);
	return true;
}

bool GpuDirtyPageTracker::UnregisterRange(uintptr_t address, size_t size) noexcept
{
	if (!m_enabled || m_registration_mutex == nullptr)
	{
		return false;
	}
	const uintptr_t end = RangeEnd(address, size);
	if (end == 0)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(*m_registration_mutex);
	const auto                  range = m_ranges.find(RangeKey {address, end});
	if (range == m_ranges.end())
	{
		return false;
	}
	if (range->second.refs > 1u)
	{
		range->second.refs--;
		return true;
	}
	const uintptr_t first = PageStart(address);
	const uintptr_t last  = PageStart(end - 1u);
	m_ranges.erase(range);
	bool result = true;
	for (uintptr_t page = first; page <= last;)
	{
		PageEntry* entry = FindPage(page);
		if (entry == nullptr)
		{
			if (page == last || page > last - m_page_size) { break; }
			page += m_page_size;
			continue;
		}
		const uint32_t refs = entry->refs.load(std::memory_order_acquire);
		if (refs > 1u)
		{
			entry->refs.fetch_sub(1, std::memory_order_acq_rel);
			if (page == last || page > last - m_page_size) { break; }
			page += m_page_size;
			continue;
		}
		if (refs == 0u)
		{
			if (page == last || page > last - m_page_size) { break; }
			page += m_page_size;
			continue;
		}

		ClaimPageForRetirement(entry);
		entry->refs.store(0, std::memory_order_release);
		if (entry->original_mode_valid.load(std::memory_order_acquire) == 0u)
		{
			entry->protection_state.store(static_cast<uint32_t>(GpuDirtyProtectionState::Retired), std::memory_order_release);
			ReleasePageSlot(entry);
			if (page == last || page > last - m_page_size) { break; }
			page += m_page_size;
			continue;
		}

		const uint32_t token     = entry->original_token.load(std::memory_order_relaxed);
		const uintptr_t run_first = page;
		uintptr_t       run_last  = page;
		while (run_last != last && run_last <= last - m_page_size)
		{
			const uintptr_t next_page = run_last + m_page_size;
			auto*           next      = FindPage(next_page);
			if (next == nullptr || next->refs.load(std::memory_order_acquire) != 1u ||
			    next->original_mode_valid.load(std::memory_order_acquire) == 0u ||
			    next->original_token.load(std::memory_order_relaxed) != token)
			{
				break;
			}
			ClaimPageForRetirement(next);
			next->refs.store(0, std::memory_order_release);
			run_last = next_page;
		}
		result = RestoreRetirementRun(run_first, run_last, token) && result;
		if (run_last == last || run_last > last - m_page_size) { break; }
		page = run_last + m_page_size;
	}
	return result;
}

void GpuDirtyPageTracker::MarkFallback(uintptr_t page, uintptr_t end) noexcept
{
	if (end <= page)
	{
		return;
	}
	VisitWritePages(PageStart(page), PageStart(end - 1u), [this](PageEntry* entry, uintptr_t /*address*/) noexcept { MarkPageFallback(entry); });
}

// Signal-safe: page and block metadata only. Every range over this page reads
// the evidence later; none is visited here.
void GpuDirtyPageTracker::MarkPageFallback(PageEntry* page) noexcept
{
	if (page == nullptr)
	{
		return;
	}
	const uint64_t epoch = m_epoch->fetch_add(1, std::memory_order_relaxed) + 1u;
	AtomicMax(&page->fallback_epoch, epoch);
	if (auto* block = BlockOf(page); block != nullptr)
	{
		AtomicMax(&block->fallback_epoch, epoch);
		block->changes.fetch_add(1, std::memory_order_release);
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
	AtomicMax(&page->write_epoch, epoch);
	if (auto* block = BlockOf(page); block != nullptr)
	{
		AtomicMax(&block->write_epoch, epoch);
		block->changes.fetch_add(1, std::memory_order_release);
	}
}

GpuDirtyReadObservation GpuDirtyPageTracker::BeginRead(uintptr_t address, size_t size) noexcept
{
	GpuDirtyReadObservation observation;
	const uint64_t          generation_before = SnapshotGeneration(address, size);
	if (!PrepareForRead(address, size))
	{
		return observation;
	}
	const uint64_t generation_after = SnapshotGeneration(address, size);
	if (generation_after != generation_before)
	{
		return observation;
	}
	observation.generation = generation_before;
	observation.tracked    = true;
	return observation;
}

bool GpuDirtyPageTracker::PrepareForRead(uintptr_t address, size_t size) noexcept
{
	if (!m_enabled || m_page_size == 0 || address == 0 || size == 0)
	{
		return false;
	}
	const uintptr_t end   = RangeEnd(address, size);
	const uintptr_t first = PageStart(address);
	const uintptr_t last  = end == 0 ? 0 : PageStart(end - 1u);
	if (end == 0 || last < first)
	{
		return false;
	}
	bool fallback = false;
	{
		std::lock_guard<std::mutex> lock(*m_registration_mutex);
		const RangeKey              key {address, end};
		if (const auto exact = m_ranges.find(key); exact != m_ranges.end())
		{
			if (RangeIsFallback(key, exact->second))
			{
				return false;
			}
		} else
		{
			std::vector<RangeKey> covers;
			VisitOverlappingRangesLocked(address, end,
			                             [&](const RangeKey& range, const RangeRecord& record)
			                             {
				                             covers.push_back(range);
				                             fallback = fallback || RangeIsFallback(range, record);
			                             });
			for (uintptr_t page = first;; page += m_page_size)
			{
				const uintptr_t page_end = PageEnd(page);
				const bool      covered  = FindPage(page) != nullptr &&
				                     std::any_of(covers.begin(), covers.end(),
				                                 [page, page_end](const RangeKey& range)
				                                 { return range.second > page && range.first < page_end; });
				fallback = fallback || !covered;
				if (page == last || page > last - m_page_size)
				{
					break;
				}
			}
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
	if (HasHostWriteLocked(first, last))
	{
		// This is temporary ownership, not a tracking failure. Do not make the
		// resource permanently hash-only when an unrelated I/O byte range shares
		// its protection page.
		return false;
	}
	auto finalize_arming = [this](uintptr_t page, PageEntry* entry) noexcept
	{
		for (;;)
		{
			uint32_t state = entry->protection_state.load(std::memory_order_acquire);
			if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Arming))
			{
				if (entry->protection_state.compare_exchange_weak(state, static_cast<uint32_t>(GpuDirtyProtectionState::Armed),
				                                                  std::memory_order_release, std::memory_order_acquire))
				{
					return;
				}
				continue;
			}
			if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
			{
				std::this_thread::yield();
				continue;
			}
			// A writer can finish Disarming before this thread enters mprotect.
			// Keep that transaction distinguishable until the delayed protection
			// has either committed or been rolled back.
			if (!GpuDirtyProtectionStateNeedsArmingRollback(static_cast<GpuDirtyProtectionState>(state)))
			{
				return;
			}
			const uint32_t rollback_state = state;
			if (!entry->protection_state.compare_exchange_weak(state, static_cast<uint32_t>(GpuDirtyProtectionState::Disarming),
			                                                   std::memory_order_acq_rel, std::memory_order_acquire))
			{
				continue;
			}
			EnterRestorer();
			const auto result = RestoreFromAuthority(entry, page);
			LeaveRestorer();
			if (result == RestoreResult::Denied)
			{
				// The guest protection or mapping changed while arming. Reapply the
				// authoritative guest protection instead of a stale token.
				(void)m_protection_ops.restore(m_protection_ops.context, page, m_page_size,
				                               entry->original_token.load(std::memory_order_relaxed));
			}
			const bool     settled        = result != RestoreResult::Failed;
			const uint32_t restored_state = rollback_state == static_cast<uint32_t>(GpuDirtyProtectionState::Retired)
			                                    ? rollback_state
			                                    : static_cast<uint32_t>(GpuDirtyProtectionState::Writable);
			entry->protection_state.store(settled ? restored_state
			                                      : (rollback_state == static_cast<uint32_t>(GpuDirtyProtectionState::Retired)
			                                             ? rollback_state
			                                             : static_cast<uint32_t>(GpuDirtyProtectionState::Armed)),
			                              std::memory_order_release);
			if (!settled)
			{
				MarkPageFallback(entry);
			}
			return;
		}
	};
	auto capture_pages = [](void* context, const Core::VirtualMemory::CapturedProtectionRun& run) noexcept
	{
		auto* self = static_cast<GpuDirtyPageTracker*>(context);
		if (run.mode == Core::VirtualMemory::Mode::NoAccess || run.size == 0)
		{
			return false;
		}
		const uintptr_t run_first = self->PageStart(run.address);
		const uintptr_t run_last  = self->PageStart(run.address + run.size - 1u);
		for (uintptr_t page = run_first;; page += self->m_page_size)
		{
			auto* entry = self->FindPage(page);
			if (entry == nullptr ||
			    entry->protection_state.load(std::memory_order_acquire) != static_cast<uint32_t>(GpuDirtyProtectionState::Capturing))
			{
				return false;
			}
			entry->original_mode.store(static_cast<uint32_t>(run.mode), std::memory_order_relaxed);
			entry->original_token.store(run.restore_token, std::memory_order_relaxed);
			entry->original_mode_valid.store(1, std::memory_order_release);
			entry->protection_state.store(static_cast<uint32_t>(GpuDirtyProtectionState::Arming), std::memory_order_release);
			if (page == run_last || page > run_last - self->m_page_size)
			{
				break;
			}
		}
		return true;
	};
	auto discard_captured_metadata = [this](uintptr_t begin, uintptr_t finish) noexcept
	{
		for (uintptr_t page = begin;; page += m_page_size)
		{
			if (auto* entry = FindPage(page); entry != nullptr)
			{
				entry->original_mode_valid.store(0, std::memory_order_release);
				uint32_t state = entry->protection_state.load(std::memory_order_acquire);
				while ((state == static_cast<uint32_t>(GpuDirtyProtectionState::Capturing) ||
				        state == static_cast<uint32_t>(GpuDirtyProtectionState::Arming) ||
				        state == static_cast<uint32_t>(GpuDirtyProtectionState::ArmingRollback)) &&
				       !entry->protection_state.compare_exchange_weak(
				           state, static_cast<uint32_t>(GpuDirtyProtectionState::Writable), std::memory_order_release,
				           std::memory_order_acquire))
				{
				}
			}
			if (page == finish || page > finish - m_page_size)
			{
				break;
			}
		}
	};

	bool first_arm = true;
	for (uintptr_t page = first;; page += m_page_size)
	{
		auto* entry = FindPage(page);
		if (entry == nullptr || entry->refs.load(std::memory_order_acquire) == 0u ||
		    entry->protection_state.load(std::memory_order_acquire) != static_cast<uint32_t>(GpuDirtyProtectionState::Writable) ||
		    entry->original_mode_valid.load(std::memory_order_acquire) != 0u)
		{
			first_arm = false;
			break;
		}
		if (page == last || page > last - m_page_size)
		{
			break;
		}
	}
	if (first_arm)
	{
		uintptr_t claimed_last = first;
		for (uintptr_t page = first;; page += m_page_size)
		{
			auto* entry = FindPage(page);
			uint32_t expected = static_cast<uint32_t>(GpuDirtyProtectionState::Writable);
			if (!entry->protection_state.compare_exchange_strong(expected, static_cast<uint32_t>(GpuDirtyProtectionState::Capturing),
			                                                     std::memory_order_acq_rel, std::memory_order_acquire))
			{
				for (uintptr_t rollback = first;; rollback += m_page_size)
				{
					if (auto* old = FindPage(rollback); old != nullptr)
					{
						uint32_t arming = static_cast<uint32_t>(GpuDirtyProtectionState::Capturing);
						(void)old->protection_state.compare_exchange_strong(
						    arming, static_cast<uint32_t>(GpuDirtyProtectionState::Writable), std::memory_order_release,
						    std::memory_order_acquire);
					}
					if (rollback == claimed_last || rollback > claimed_last - m_page_size) { break; }
				}
				first_arm = false;
				break;
			}
			claimed_last = page;
			if (page == last || page > last - m_page_size) { break; }
		}
		if (first_arm)
		{
			const auto change = m_protection_ops.remove_write_and_capture(m_protection_ops.context, first, end - first, capture_pages,
			                                                              this, &m_authority);
			if (!change.Succeeded())
			{
				EXIT_IF(change.status == Core::VirtualMemory::ProtectionChangeStatus::RollbackFailed);
				discard_captured_metadata(first, last);
				for (uintptr_t page = first;; page += m_page_size)
				{
					if (auto* entry = FindPage(page); entry != nullptr)
					{
						uint32_t arming = static_cast<uint32_t>(GpuDirtyProtectionState::Arming);
						(void)entry->protection_state.compare_exchange_strong(
						    arming, static_cast<uint32_t>(GpuDirtyProtectionState::Writable), std::memory_order_release,
						    std::memory_order_acquire);
					}
					if (page == last || page > last - m_page_size) { break; }
				}
				MarkFallback(first, end);
				return false;
			}
			for (uintptr_t page = first;; page += m_page_size)
			{
				finalize_arming(page, FindPage(page));
				if (page == last || page > last - m_page_size) { break; }
			}
			return true;
		}
	}

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
		const bool needs_capture = entry->original_mode_valid.load(std::memory_order_acquire) == 0u;
		uint32_t expected = static_cast<uint32_t>(GpuDirtyProtectionState::Writable);
		if (!entry->protection_state.compare_exchange_strong(
		        expected, static_cast<uint32_t>(needs_capture ? GpuDirtyProtectionState::Capturing : GpuDirtyProtectionState::Arming),
		                                                     std::memory_order_acq_rel, std::memory_order_acquire))
		{
			if (page == last || page > last - m_page_size)
			{
				break;
			}
			page += m_page_size;
			continue;
		}

		if (needs_capture)
		{
			const uintptr_t run_start = page;
			uintptr_t run_last = page;
			while (run_last != last && run_last <= last - m_page_size)
			{
				const uintptr_t next_page = run_last + m_page_size;
				auto* next = FindPage(next_page);
				if (next == nullptr || next->refs.load(std::memory_order_acquire) == 0u ||
				    next->original_mode_valid.load(std::memory_order_acquire) != 0u)
				{
					break;
				}
				uint32_t next_expected = static_cast<uint32_t>(GpuDirtyProtectionState::Writable);
				if (!next->protection_state.compare_exchange_strong(next_expected,
				                                                    static_cast<uint32_t>(GpuDirtyProtectionState::Capturing),
				                                                    std::memory_order_acq_rel, std::memory_order_acquire))
				{
					break;
				}
				run_last = next_page;
			}
			const size_t run_size = run_last - run_start + m_page_size;
			const auto change = m_protection_ops.remove_write_and_capture(m_protection_ops.context, run_start, run_size,
			                                                                capture_pages, this, &m_authority);
			if (!change.Succeeded())
			{
				EXIT_IF(change.status == Core::VirtualMemory::ProtectionChangeStatus::RollbackFailed);
				discard_captured_metadata(run_start, run_last);
				for (uintptr_t rollback = run_start;; rollback += m_page_size)
				{
					if (auto* claimed = FindPage(rollback); claimed != nullptr)
					{
						uint32_t arming = static_cast<uint32_t>(GpuDirtyProtectionState::Arming);
						(void)claimed->protection_state.compare_exchange_strong(
						    arming, static_cast<uint32_t>(GpuDirtyProtectionState::Writable), std::memory_order_release,
						    std::memory_order_acquire);
					}
					if (rollback == run_last || rollback > run_last - m_page_size) { break; }
				}
				MarkFallback(first, end);
				return false;
			}
			for (uintptr_t armed = run_start;; armed += m_page_size)
			{
				finalize_arming(armed, FindPage(armed));
				if (armed == run_last || armed > run_last - m_page_size) { break; }
			}
			if (run_last == last || run_last > last - m_page_size) { break; }
			page = run_last + m_page_size;
			continue;
		}

		const uint32_t token = entry->original_token.load(std::memory_order_relaxed);
		const uintptr_t run_start = page;
		uintptr_t       run_last  = page;
		while (run_last != last && run_last <= last - m_page_size)
		{
			const uintptr_t next_page = run_last + m_page_size;
			auto*           next      = FindPage(next_page);
			if (next == nullptr || next->refs.load(std::memory_order_acquire) == 0u ||
			    next->protection_state.load(std::memory_order_acquire) == static_cast<uint32_t>(GpuDirtyProtectionState::Retired) ||
			    next->original_mode_valid.load(std::memory_order_acquire) == 0u ||
			    next->original_token.load(std::memory_order_relaxed) != token)
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

		if (!m_protection_ops.remove_write(m_protection_ops.context, run_start, run_size, token, &m_authority))
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
					       state == static_cast<uint32_t>(GpuDirtyProtectionState::ArmingRollback) ||
					       state == static_cast<uint32_t>(GpuDirtyProtectionState::Writable))
					{
						if (claimed->protection_state.compare_exchange_strong(state,
						                                                      static_cast<uint32_t>(GpuDirtyProtectionState::Disarming),
						                                                      std::memory_order_acq_rel, std::memory_order_acquire))
						{
							const uint32_t token    = claimed->original_token.load(std::memory_order_relaxed);
							bool           restored = m_protection_ops.restore(m_protection_ops.context, rollback, m_page_size, token);
							if (!restored)
							{
								EnterRestorer();
								restored = RestoreFromAuthority(claimed, rollback) != RestoreResult::Failed;
								LeaveRestorer();
							}
							claimed->protection_state.store(restored ? static_cast<uint32_t>(GpuDirtyProtectionState::Writable)
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

// Applies the page's authoritative guest token without the VM lock. The caller
// is an announced restorer, so neither a guest protection or mapping change nor
// a slot identity change can overlap the token read and the native restore.
GpuDirtyPageTracker::RestoreResult GpuDirtyPageTracker::RestoreFromAuthority(PageEntry* entry, uintptr_t page) noexcept
{
	if (entry->key.load(std::memory_order_acquire) != page || entry->original_mode_valid.load(std::memory_order_acquire) == 0u ||
	    !ModeGrantsWrite(entry->original_mode.load(std::memory_order_acquire)))
	{
		return RestoreResult::Denied;
	}
	const uint32_t token = entry->original_token.load(std::memory_order_acquire);
	return m_protection_ops.restore_signal_safe(m_protection_ops.context, page, m_page_size, token) ? RestoreResult::Restored
	                                                                                                 : RestoreResult::Failed;
}

bool GpuDirtyPageTracker::HandleWriteFault(uintptr_t address) noexcept
{
	if (!m_enabled)
	{
		return false;
	}
	const uintptr_t page_address = PageStart(address);
	// A page without metadata was never armed. Registration precedes arming.
	const PageEntry* entry = FindPage(page_address);
	if (entry == nullptr)
	{
		return false;
	}
	// The handler never waits. While a VM change or a slot identity change holds
	// the fence, a fault the tracker owns is reported handled without any
	// change, so the access is executed again; the fence holder progresses on
	// its own thread and never touches tracked guest memory while fenced. Any
	// other fault stays the guest's.
	if (!TryEnterRestorer())
	{
		return FaultMayBeTracked(entry, page_address);
	}
	const bool handled = HandleWriteFaultEntered(page_address);
	LeaveRestorer();
	return handled;
}

// Lock-free ownership test for a fault that arrives while the fence is held.
// Slots are never freed, so reading one is safe, but its identity and tokens
// may be changing: the key is checked again and every field is read once. The
// answer describes the page as it was before the fenced change publishes. A
// write that races a guest protection change on the same page can therefore
// be retried once more, or reported as the guest's fault, which matches the
// write landing on either side of that change.
bool GpuDirtyPageTracker::FaultMayBeTracked(const PageEntry* entry, uintptr_t page_address) const noexcept
{
	const uint32_t state = entry->protection_state.load(std::memory_order_acquire);
	const uint32_t refs  = entry->refs.load(std::memory_order_acquire);
	const bool     captured_writable =
	    entry->original_mode_valid.load(std::memory_order_acquire) != 0u && ModeGrantsWrite(entry->original_mode.load(std::memory_order_acquire));
	if (entry->key.load(std::memory_order_acquire) != page_address || !captured_writable)
	{
		return false;
	}
	if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Retired) ||
	    state == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
	{
		return true;
	}
	return refs != 0u && (GpuDirtyProtectionStateHandlesFault(static_cast<GpuDirtyProtectionState>(state)) ||
	                      state == static_cast<uint32_t>(GpuDirtyProtectionState::Writable));
}

bool GpuDirtyPageTracker::HandleWriteFaultEntered(uintptr_t page_address) noexcept
{
	// Resolved while announced: the slot keeps this identity until we leave.
	PageEntry* entry = FindPage(page_address);
	if (entry == nullptr)
	{
		return false;
	}

	uint32_t state                     = entry->protection_state.load(std::memory_order_acquire);
	auto     captured_mode_is_writable = [entry]() noexcept
	{
		return entry->original_mode_valid.load(std::memory_order_acquire) != 0u &&
		       ModeGrantsWrite(entry->original_mode.load(std::memory_order_acquire));
	};
	const uint32_t refs = entry->refs.load(std::memory_order_acquire);
	const bool     queued_writable_fault =
	    state == static_cast<uint32_t>(GpuDirtyProtectionState::Writable) && refs != 0u && captured_mode_is_writable();
	if ((refs == 0u && state != static_cast<uint32_t>(GpuDirtyProtectionState::Retired) &&
	     state != static_cast<uint32_t>(GpuDirtyProtectionState::Disarming)) ||
	    (!GpuDirtyProtectionStateHandlesFault(static_cast<GpuDirtyProtectionState>(state)) && !queued_writable_fault))
	{
		return false;
	}
	// The tracker only ever removed a write the guest granted. When the guest
	// protection no longer grants it, or the mapping was replaced, this is a
	// real guest fault.
	if (!captured_mode_is_writable())
	{
		return false;
	}
	if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Retired))
	{
		return RestoreFromAuthority(entry, page_address) == RestoreResult::Restored;
	}
	uint32_t restored_state = static_cast<uint32_t>(GpuDirtyProtectionState::Writable);
	uint32_t claimed_state  = state;
	for (;;)
	{
		if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Writable))
		{
			// Multiple writers can fault while the page is still read-only. A
			// sibling delivery may enter after the first handler restored the page
			// and published Writable. Active coverage plus the captured writable
			// mode proves this is still a tracker-owned protection fault.
			if (entry->refs.load(std::memory_order_acquire) == 0u || !captured_mode_is_writable())
			{
				return false;
			}
		}
		if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Capturing))
		{
			return false;
		}
		if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
		{
			// Another fault handler owns the permission restore. This fault is
			// still tracker-induced; retrying after that handler completes is safe.
			return true;
		}
		claimed_state = state;
		if (entry->protection_state.compare_exchange_weak(state, static_cast<uint32_t>(GpuDirtyProtectionState::Disarming),
		                                                  std::memory_order_acq_rel, std::memory_order_acquire))
		{
			if (claimed_state == static_cast<uint32_t>(GpuDirtyProtectionState::Arming) ||
			    claimed_state == static_cast<uint32_t>(GpuDirtyProtectionState::ArmingRollback))
			{
				restored_state = static_cast<uint32_t>(GpuDirtyProtectionState::ArmingRollback);
			}
			break;
		}
	}
	// The claim must still describe this page; never dirty or restore another
	// page's metadata at this address.
	if (entry->key.load(std::memory_order_acquire) != page_address)
	{
		entry->protection_state.store(claimed_state, std::memory_order_release);
		return false;
	}

	MarkPageWrite(entry);
	const auto result = RestoreFromAuthority(entry, page_address);
	if (result == RestoreResult::Denied)
	{
		// A concurrent guest change won. A revoked mapping is no longer armed; a
		// denied write leaves the page in the state this handler claimed.
		entry->protection_state.store(entry->original_mode_valid.load(std::memory_order_acquire) == 0u ? restored_state : claimed_state,
		                              std::memory_order_release);
		return false;
	}
	entry->protection_state.store(result == RestoreResult::Restored ? restored_state
	                                                                : static_cast<uint32_t>(GpuDirtyProtectionState::Armed),
	                              std::memory_order_release);
	if (result == RestoreResult::Failed)
	{
		MarkPageFallback(entry);
		return false;
	}
	return true;
}

bool GpuDirtyPageTracker::HandleAccessFault(uintptr_t address, Core::VirtualMemory::ExceptionHandler::AccessViolationType access) noexcept
{
	using Access = Core::VirtualMemory::ExceptionHandler::AccessViolationType;
	// Tracking only removes write access; armed pages keep the guest's read and
	// execute permission, so those faults are never the tracker's.
	return (access == Access::Write || access == Access::Unknown) && HandleWriteFault(address);
}

// Host writers run on normal threads, so they wait for a fence instead of
// retrying an access.
bool GpuDirtyPageTracker::NotifyPageWrite(PageEntry* entry, uintptr_t page_address) noexcept
{
	bool handled = false;
	for (;;)
	{
		EnterRestorer();
		const bool settled = NotifyPageWriteEntered(entry, page_address, &handled);
		LeaveRestorer();
		if (settled)
		{
			return handled;
		}
		std::this_thread::yield();
	}
}

// One attempt as an announced restorer; false when another transition owns the
// page and the attempt must be repeated.
bool GpuDirtyPageTracker::NotifyPageWriteEntered(PageEntry* entry, uintptr_t page_address, bool* handled) noexcept
{
	if (entry->key.load(std::memory_order_acquire) != page_address) { return true; }
	uint32_t       state = entry->protection_state.load(std::memory_order_acquire);
	const uint32_t refs  = entry->refs.load(std::memory_order_acquire);
	if (refs == 0u && state != static_cast<uint32_t>(GpuDirtyProtectionState::Capturing) &&
	    state != static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
	{
		return true;
	}
	*handled = true;
	if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Capturing) ||
	    state == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
	{
		return false;
	}
	if (refs == 0u) { return true; }
	if (state == static_cast<uint32_t>(GpuDirtyProtectionState::Writable))
	{
		MarkPageWrite(entry);
		return true;
	}
	const uint32_t claimed_state = state;
	if (!entry->protection_state.compare_exchange_weak(state, static_cast<uint32_t>(GpuDirtyProtectionState::Disarming),
	                                                   std::memory_order_acq_rel, std::memory_order_acquire))
	{
		return false;
	}
	// The walk resolved this slot before announcing itself, so a registration
	// may have recycled it since. Never apply another page's restore token to
	// the address sampled by the walk.
	if (entry->key.load(std::memory_order_acquire) != page_address)
	{
		entry->protection_state.store(claimed_state, std::memory_order_release);
		return true;
	}
	MarkPageWrite(entry);
	const auto     result           = RestoreFromAuthority(entry, page_address);
	const bool     arming_in_flight = claimed_state == static_cast<uint32_t>(GpuDirtyProtectionState::Arming) ||
	                                  claimed_state == static_cast<uint32_t>(GpuDirtyProtectionState::ArmingRollback);
	const uint32_t released_state   = static_cast<uint32_t>(arming_in_flight ? GpuDirtyProtectionState::ArmingRollback
	                                                                         : GpuDirtyProtectionState::Writable);
	if (result == RestoreResult::Denied)
	{
		// The guest protection now denies writes, or the mapping was replaced:
		// the writer meets the guest's own protection natively.
		entry->protection_state.store(entry->original_mode_valid.load(std::memory_order_acquire) == 0u ? released_state : claimed_state,
		                              std::memory_order_release);
		return true;
	}
	entry->protection_state.store(result == RestoreResult::Restored ? released_state
	                                                                : static_cast<uint32_t>(GpuDirtyProtectionState::Armed),
	                              std::memory_order_release);
	if (result == RestoreResult::Failed) { MarkPageFallback(entry); }
	return true;
}

bool GpuDirtyPageTracker::NotifyWritePages(uintptr_t first, uintptr_t last) noexcept
{
	bool handled = false;
	VisitWritePages(first, last, [this, &handled](PageEntry* entry, uintptr_t page) noexcept
	{
		const bool page_handled = NotifyPageWrite(entry, page);
		handled = handled || page_handled;
	});
	return handled;
}

bool GpuDirtyPageTracker::NotifyWrite(uintptr_t address, size_t size) noexcept
{
	if (!m_enabled || m_page_size == 0 || address == 0 || size == 0) { return false; }
	const uintptr_t end = RangeEnd(address, size);
	if (end == 0) { return false; }
	return NotifyWritePages(PageStart(address), PageStart(end - 1u));
}

bool GpuDirtyPageTracker::HasHostWriteLocked(uintptr_t first, uintptr_t last) const noexcept
{
	for (const auto& write: m_host_writes)
	{
		if (write.first <= last && first <= write.last)
		{
			return true;
		}
	}
	return false;
}

void GpuDirtyPageTracker::PrepareHostWriteLocked(uintptr_t address, size_t size) noexcept
{
	VisitWritePages(PageStart(address), PageStart(RangeEnd(address, size) - 1u), [this](PageEntry* entry, uintptr_t page) noexcept
	{
		(void)NotifyPageWrite(entry, page);
		if (entry->refs.load(std::memory_order_acquire) != 0u)
		{
			uint32_t state = entry->protection_state.load(std::memory_order_acquire);
			while (state == static_cast<uint32_t>(GpuDirtyProtectionState::Disarming))
			{
				std::this_thread::yield();
				state = entry->protection_state.load(std::memory_order_acquire);
			}
			// Native rearming cannot be in flight while we hold the registration
			// mutex. A failed permission restore is an internal VM failure, never
			// permission to start I/O into an artificially read-only destination.
			// A guest protection without write access stays and fails natively.
			const bool guest_writable = entry->original_mode_valid.load(std::memory_order_acquire) != 0u &&
			                            ModeGrantsWrite(entry->original_mode.load(std::memory_order_acquire));
			EXIT_IF(state != static_cast<uint32_t>(GpuDirtyProtectionState::Writable) && guest_writable);
		}
	});
}

uint64_t GpuDirtyPageTracker::BeginHostWrite(uintptr_t address, size_t size) noexcept
{
	if (!Enabled() || address == 0 || RangeEnd(address, size) == 0)
	{
		return 0;
	}
	std::lock_guard<std::mutex> lock(*m_registration_mutex);
	const uintptr_t first = PageStart(address);
	const uintptr_t last = PageStart(RangeEnd(address, size) - 1u);
	uint64_t token = 0;
	for (auto& write: m_host_writes)
	{
		if (write.first == first && write.last == last)
		{
			EXIT_IF(write.refs == UINT64_MAX);
			write.refs++;
			token = write.token;
			break;
		}
	}
	if (token == 0)
	{
		EXIT_IF(m_next_host_write == 0);
		token = m_next_host_write++;
		m_host_writes.push_back({token, first, last, 1});
	}
	PrepareHostWriteLocked(address, size);
	return token;
}

void GpuDirtyPageTracker::EndHostWrite(uint64_t token) noexcept
{
	if (token == 0) { return; }
	EXIT_IF(!Enabled());
	std::lock_guard<std::mutex> lock(*m_registration_mutex);
	for (auto it = m_host_writes.begin(); it != m_host_writes.end(); ++it)
	{
		if (it->token != token) { continue; }
		// A reader starting after Begin must also see the completion generation.
		// This covers a newly registered resource and short/error exits alike.
		(void)NotifyWritePages(it->first, it->last);
		if (--it->refs == 0) { m_host_writes.erase(it); }
		return;
	}
	EXIT("Unknown host-write lease\n");
}

uint64_t GpuDirtyPageTracker::SnapshotGeneration(uintptr_t address, size_t size) const noexcept
{
	const uintptr_t end = RangeEnd(address, size);
	if (!Enabled() || end == 0)
	{
		return 0;
	}
	std::lock_guard<std::mutex> lock(*m_registration_mutex);
	const RangeKey              key {address, end};
	if (const auto exact = m_ranges.find(key); exact != m_ranges.end())
	{
		return RangeEvidenceOf(key, exact->second).write_epoch;
	}
	uint64_t snapshot = 0;
	VisitOverlappingRangesLocked(address, end, [&](const RangeKey& range, const RangeRecord& record)
	                             { snapshot = std::max(snapshot, RangeEvidenceOf(range, record).write_epoch); });
	return snapshot;
}

bool GpuDirtyPageTracker::ChangedSince(uintptr_t address, size_t size, uint64_t snapshot) const noexcept
{
	if (!Enabled())
	{
		return true;
	}
	std::lock_guard<std::mutex> lock(*m_registration_mutex);
	const uintptr_t end = RangeEnd(address, size);
	if (end == 0 || HasHostWriteLocked(PageStart(address), PageStart(end - 1u)))
	{
		return true;
	}
	auto changed = [&](const RangeKey& range, const RangeRecord& record)
	{
		const auto evidence = RangeEvidenceOf(range, record);
		return !evidence.complete || evidence.fallback_epoch > record.registered_epoch || evidence.write_epoch > snapshot;
	};
	const RangeKey key {address, end};
	if (const auto exact = m_ranges.find(key); exact != m_ranges.end())
	{
		return changed(key, exact->second);
	}
	bool found  = false;
	bool result = false;
	VisitOverlappingRangesLocked(address, end,
	                             [&](const RangeKey& range, const RangeRecord& record)
	                             {
		                             found  = true;
		                             result = result || changed(range, record);
	                             });
	return result || !found;
}

bool GpuDirtyPageTracker::ReadObservationIsStable(uintptr_t address, size_t size,
	                                               const GpuDirtyReadObservation& observation) const noexcept
{
	return observation.tracked && !ChangedSince(address, size, observation.generation);
}

size_t GpuDirtyPageTracker::PageCount(uintptr_t address, size_t size) const noexcept
{
	const uintptr_t end = m_page_size == 0 || address == 0 || size == 0 ? 0 : RangeEnd(address, size);
	if (end == 0)
	{
		return 0;
	}
	return static_cast<size_t>((PageStart(end - 1u) - PageStart(address)) / m_page_size) + 1u;
}

bool GpuDirtyPageTracker::PageGenerations(uintptr_t address, size_t size, uint64_t* generations, size_t count) const noexcept
{
	if (!Enabled() || generations == nullptr || count == 0 || PageCount(address, size) != count)
	{
		return false;
	}
	const uintptr_t first = PageStart(address);
	for (size_t i = 0; i < count; i++)
	{
		const PageEntry* entry = FindPage(first + i * m_page_size);
		if (entry == nullptr)
		{
			return false;
		}
		generations[i] = entry->generation.load(std::memory_order_acquire);
	}
	return true;
}

bool GpuDirtyPageTracker::Enabled() const noexcept
{
	return m_enabled && m_page_size != 0 && m_pages != nullptr && m_blocks != nullptr;
}

GpuDirtyTrackingMode GpuDirtyPageTracker::Mode(uintptr_t address, size_t size) const noexcept
{
	const uintptr_t end = RangeEnd(address, size);
	if (!Enabled() || end == 0)
	{
		return GpuDirtyTrackingMode::HashFallback;
	}
	std::lock_guard<std::mutex> lock(*m_registration_mutex);
	const RangeKey              key {address, end};
	if (const auto exact = m_ranges.find(key); exact != m_ranges.end())
	{
		return RangeIsFallback(key, exact->second) ? GpuDirtyTrackingMode::HashFallback : GpuDirtyTrackingMode::PageFault;
	}
	bool found    = false;
	bool fallback = false;
	VisitOverlappingRangesLocked(address, end,
	                             [&](const RangeKey& range, const RangeRecord& record)
	                             {
		                             found    = true;
		                             fallback = fallback || RangeIsFallback(range, record);
	                             });
	return found && !fallback ? GpuDirtyTrackingMode::PageFault : GpuDirtyTrackingMode::HashFallback;
}

void GpuDirtyPageTracker::AuthorityBeginChange(void* context, uint64_t /*address*/, uint64_t /*size*/) noexcept
{
	auto* self = static_cast<GpuDirtyPageTracker*>(context);
	self->AcquirePublisher();
	self->RaiseFence();
}

// Reports which pages keep write removed under the new protection: the pages
// this tracker has armed or is arming. Nothing is published; the change may
// still be rolled back.
void GpuDirtyPageTracker::AuthorityDecide(void* context, const Core::VirtualMemory::WriteLeaseChange& change,
                                          Core::VirtualMemory::WriteLeaseDecision decision, void* decision_context) noexcept
{
	auto* self = static_cast<GpuDirtyPageTracker*>(context);
	if (self->m_page_size == 0 || change.size == 0 || decision == nullptr)
	{
		return;
	}
	const uintptr_t first      = self->PageStart(change.address);
	const uintptr_t last       = self->PageStart(change.address + change.size - 1u);
	uintptr_t       run_start  = first;
	bool            run_remove = false;
	for (uintptr_t page = first;; page += self->m_page_size)
	{
		bool remove_write = false;
		if (const PageEntry* entry = self->FindPage(page); entry != nullptr)
		{
			const uint32_t state = entry->protection_state.load(std::memory_order_acquire);
			remove_write         = state == static_cast<uint32_t>(GpuDirtyProtectionState::Arming) ||
			               state == static_cast<uint32_t>(GpuDirtyProtectionState::Armed);
		}
		if (page == run_start)
		{
			run_remove = remove_write;
		} else if (remove_write != run_remove)
		{
			(void)decision(decision_context, run_start, page - run_start, run_remove);
			run_start  = page;
			run_remove = remove_write;
		}
		if (page == last || page > last - self->m_page_size)
		{
			break;
		}
	}
	(void)decision(decision_context, run_start, last - run_start + self->m_page_size, run_remove);
}

void GpuDirtyPageTracker::AuthorityEndChange(void* context, const Core::VirtualMemory::WriteLeaseChange& change) noexcept
{
	auto* self = static_cast<GpuDirtyPageTracker*>(context);
	if (change.committed)
	{
		self->PublishAuthorityChange(change);
	}
	self->ReleasePublisher();
	self->LowerFence();
}

// Runs inside the VM transaction while this tracker is fenced, after the change
// was applied natively. A protection change replaces every captured token of
// the run. A removed mapping revokes the tokens, so neither a late fault nor
// unregistration can restore them over a new mapping.
void GpuDirtyPageTracker::PublishAuthorityChange(const Core::VirtualMemory::WriteLeaseChange& change) noexcept
{
	if (m_page_size == 0 || change.size == 0)
	{
		return;
	}
	const bool      unmap = change.kind == Core::VirtualMemory::WriteLeaseChangeKind::Unmap;
	const uintptr_t first = PageStart(change.address);
	const uintptr_t last  = PageStart(change.address + change.size - 1u);
	for (uintptr_t page = first;; page += m_page_size)
	{
		if (PageEntry* entry = FindPage(page); entry != nullptr && unmap)
		{
			entry->original_mode_valid.store(0, std::memory_order_release);
			entry->original_token.store(0, std::memory_order_relaxed);
			entry->original_mode.store(static_cast<uint32_t>(Core::VirtualMemory::Mode::NoAccess), std::memory_order_relaxed);
			uint32_t state = entry->protection_state.load(std::memory_order_acquire);
			while ((state == static_cast<uint32_t>(GpuDirtyProtectionState::Armed) ||
			        state == static_cast<uint32_t>(GpuDirtyProtectionState::Arming)) &&
			       !entry->protection_state.compare_exchange_weak(state, static_cast<uint32_t>(GpuDirtyProtectionState::Writable),
			                                                      std::memory_order_acq_rel, std::memory_order_acquire))
			{
			}
		} else if (entry != nullptr && entry->original_mode_valid.load(std::memory_order_acquire) != 0u)
		{
			entry->original_mode.store(static_cast<uint32_t>(change.mode), std::memory_order_relaxed);
			entry->original_token.store(change.guest_token, std::memory_order_release);
		}
		if (page == last || page > last - m_page_size)
		{
			break;
		}
	}
}

GpuDirtyPageTracker& GetGpuDirtyPageTracker() noexcept
{
	if (auto* tracker = g_process_tracker.load(std::memory_order_acquire); tracker != nullptr)
	{
		return *tracker;
	}
	// Before the fault handler exists nothing can be tracked. Early callers get
	// a disabled tracker without metadata (their ranges use the hash fallback);
	// it is never published, so it cannot disable the real tracker later.
	static auto* untracked = new GpuDirtyPageTracker(false);
	return *untracked;
}

// Runs once on the normal startup thread, after the process fault handler is
// installed. The tracker is created here, never in the signal route, and is
// published only once fully constructed. Like the handler it serves, it lives
// for the rest of the process and is intentionally never destroyed: a fault or
// a late runtime cleanup can still reach it after static destructors began.
void GpuDirtyPageTrackerNotifyFaultHandlerInstalled() noexcept
{
	if (g_process_tracker.load(std::memory_order_acquire) != nullptr)
	{
		return;
	}
	auto* tracker = new GpuDirtyPageTracker(GpuDirtyTrackingEnabledForProcess(std::getenv("KYTY_DISABLE_GPU_DIRTY_TRACKING"), true));
	GpuDirtyPageTracker* expected = nullptr;
	if (!g_process_tracker.compare_exchange_strong(expected, tracker, std::memory_order_acq_rel, std::memory_order_acquire))
	{
		delete tracker;
	}
}

bool GpuDirtyPageTrackerHandleAccessFault(uintptr_t address, Core::VirtualMemory::ExceptionHandler::AccessViolationType access) noexcept
{
	auto* tracker = g_process_tracker.load(std::memory_order_acquire);
	return tracker != nullptr && tracker->HandleAccessFault(address, access);
}

GpuDirtyPageTracker& GpuDirtyPageTracker::Instance() noexcept
{
	return GetGpuDirtyPageTracker();
}

} // namespace Kyty::Libs::Graphics
