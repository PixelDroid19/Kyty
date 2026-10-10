#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDIRTYPAGETRACKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDIRTYPAGETRACKER_H_

#include "Kyty/Core/VirtualMemory.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Kyty::Libs::Graphics {

enum class GpuDirtyTrackingMode : uint32_t
{
	PageFault,
	HashFallback
};

enum class GpuDirtyProtectionState : uint32_t
{
	Writable,
	Capturing,
	Arming,
	ArmingRollback,
	Armed,
	Disarming,
	Retired
};

[[nodiscard]] constexpr size_t GpuDirtyPageTableIndex(uintptr_t page, size_t mask) noexcept
{
	page >>= 12u;
	page ^= page >> 33u;
	page *= static_cast<uintptr_t>(0xff51afd7ed558ccdULL);
	page ^= page >> 33u;
	return static_cast<size_t>(page) & mask;
}

[[nodiscard]] constexpr bool GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState state)
{
	return state != GpuDirtyProtectionState::Writable && state != GpuDirtyProtectionState::Capturing;
}

[[nodiscard]] constexpr bool GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState observed)
{
	// If a writer completes while Rearm is entering mprotect, the delayed
	// protection can still make the page read-only. A concurrent final
	// unregister can likewise publish Retired before the protection commits.
	return observed == GpuDirtyProtectionState::Writable || observed == GpuDirtyProtectionState::ArmingRollback ||
	       observed == GpuDirtyProtectionState::Retired;
}

[[nodiscard]] constexpr bool GpuDirtyTrackingEnabledForProcess(const char* disable_value, bool fault_handler_ready)
{
	return fault_handler_ready &&
	       (disable_value == nullptr || disable_value[0] == '\0' || (disable_value[0] == '0' && disable_value[1] == '\0'));
}

struct GpuDirtyReadObservation
{
	uint64_t generation = 0;
	bool     tracked    = false;
};

// Native protection operations. The tracker leases removed write access to its authority, which the
// virtual-memory layer notifies about guest protection and mapping changes over those pages.
struct GpuDirtyPageProtectionOps
{
	void* context = nullptr;
	Core::VirtualMemory::ProtectionChangeResult (*remove_write_and_capture)(
	    void* context, uintptr_t address, size_t size, Core::VirtualMemory::CapturedProtectionVisitor visitor, void* visitor_context,
	    const Core::VirtualMemory::WriteLeaseAuthority* authority) noexcept = nullptr;
	bool (*remove_write)(void* context, uintptr_t address, size_t size, uint32_t restore_token,
	                     const Core::VirtualMemory::WriteLeaseAuthority* authority) noexcept = nullptr;
	bool (*restore)(void* context, uintptr_t address, size_t size, uint32_t restore_token) noexcept = nullptr;
	bool (*restore_signal_safe)(void* context, uintptr_t address, size_t size, uint32_t restore_token) noexcept = nullptr;
	// Ends every lease of the authority (see VirtualMemory::ReleaseWriteLeases). Optional for
	// operations that never lease through the VM.
	bool (*release_leases)(void* context, const Core::VirtualMemory::WriteLeaseAuthority* authority) noexcept = nullptr;
};

// Bounded dirty-page metadata. Registration and protection changes happen on
// normal threads. HandleWriteFault and NotifyWrite touch only fixed page and
// block tables with atomics plus the raw restore VM primitive; range records
// are never visited on that path. Every write-permission restore is fenced
// against guest protection and mapping changes (see WriteLeaseAuthority) and
// against page slots changing identity, so a token the guest replaced, a
// mapping it removed, or another page's metadata is never applied.
class GpuDirtyPageTracker
{
public:
	static GpuDirtyPageTracker& Instance() noexcept;
	explicit GpuDirtyPageTracker(bool enabled = true);
	GpuDirtyPageTracker(const GpuDirtyPageProtectionOps& protection_ops, bool enabled = true);
	~GpuDirtyPageTracker();

	GpuDirtyPageTracker(const GpuDirtyPageTracker&)            = delete;
	GpuDirtyPageTracker& operator=(const GpuDirtyPageTracker&) = delete;

	[[nodiscard]] bool RegisterRange(uintptr_t address, size_t size) noexcept;
	[[nodiscard]] bool UnregisterRange(uintptr_t address, size_t size) noexcept;

	// Protect the covered pages read-only before a CPU->GPU read. BeginRead
	// publishes an observation only when the generation stays unchanged while
	// PrepareForRead arms protection. This does not make the following copy
	// atomic: callers must validate the same observation again after reading.
	[[nodiscard]] GpuDirtyReadObservation BeginRead(uintptr_t address, size_t size) noexcept;
	[[nodiscard]] bool                    PrepareForRead(uintptr_t address, size_t size) noexcept;
	[[nodiscard]] bool                    Rearm(uintptr_t address, size_t size) noexcept;

	// Called by the exception handler for a write fault. It is async-signal-safe
	// and never waits: while a fence is held it reports the fault handled without
	// any change, so the access is executed again. It returns false for
	// untracked/unarmed addresses and for pages whose guest protection denies
	// the write.
	[[nodiscard]] bool HandleWriteFault(uintptr_t address) noexcept;
	// Access-classified entry from the exception boundary. Write-only tracking
	// never claims a read or execute fault; Unknown is treated as a write and is
	// meant only for hosts whose access classification is unreliable.
	[[nodiscard]] bool HandleAccessFault(uintptr_t                                                 address,
	                                     Core::VirtualMemory::ExceptionHandler::AccessViolationType access) noexcept;

	// Host/HLE writers call this before writing a protected destination. It is
	// lock-free but waits for a held fence, so it runs on normal threads only.
	// Work for wide spans is bounded by tracked metadata, not the requested byte
	// count.
	[[nodiscard]] bool NotifyWrite(uintptr_t address, size_t size) noexcept;

	// Normal-thread host I/O ownership. A token excludes rearming on every
	// overlapping host page, including ranges registered after acquisition.
	// Zero is the no-op token (disabled tracking or an empty/invalid range).
	[[nodiscard]] uint64_t BeginHostWrite(uintptr_t address, size_t size) noexcept;
	void EndHostWrite(uint64_t token) noexcept;

	[[nodiscard]] uint64_t             SnapshotGeneration(uintptr_t address, size_t size) const noexcept;
	[[nodiscard]] bool                 ChangedSince(uintptr_t address, size_t size, uint64_t snapshot) const noexcept;
	[[nodiscard]] bool                 ReadObservationIsStable(uintptr_t address, size_t size,
	                                                          const GpuDirtyReadObservation& observation) const noexcept;
	[[nodiscard]] bool                 Enabled() const noexcept;
	[[nodiscard]] GpuDirtyTrackingMode Mode(uintptr_t address, size_t size) const noexcept;
	// Tracker pages spanned by [address, address + size); 0 for an invalid range.
	[[nodiscard]] size_t               PageCount(uintptr_t address, size_t size) const noexcept;
	// Write generation of each tracker page of [address, address + size). A
	// page's generation advances on every write notification after it was
	// armed, so a page whose generation is unchanged held its bytes. False when
	// tracking is off, count is not the page count, or a page has no metadata.
	[[nodiscard]] bool PageGenerations(uintptr_t address, size_t size, uint64_t* generations, size_t count) const noexcept;

private:
	friend struct GpuDirtyPageTrackerTestAccess;

	struct PageEntry;
	struct BlockEntry;

	struct RangeEvidence
	{
		uint64_t write_epoch    = 0;
		uint64_t fallback_epoch = 0;
		bool     complete       = true;
	};

	// Evidence of the range's pages in a partially covered block. It stays
	// valid while the block's change count is unchanged.
	struct EdgeEvidence
	{
		uint64_t      changes = 0;
		RangeEvidence evidence {};
		bool          valid = false;
	};

	// Exact registered range [begin, end). Write and fallback evidence lives in
	// page and block metadata; a range is HashFallback once any of its pages was
	// marked after the range registered. The edge caches are query state, used
	// only under the registration mutex.
	struct RangeRecord
	{
		uint32_t             refs             = 0;
		uint64_t             registered_epoch = 0;
		mutable EdgeEvidence head {};
		mutable EdgeEvidence tail {};
	};
	using RangeKey = std::pair<uintptr_t, uintptr_t>;

	enum class RestoreResult : uint32_t
	{
		Restored,
		Failed,
		// The guest protection grants no write, or the mapping was replaced.
		Denied
	};

	// Fixed, bounded page metadata with 262,144 slots. Active and retired
	// identities share a 131,072-page admission budget. Tombstones retain probe
	// history; the normal-write hint bounds conclusive missing-page lookups.
	// Each range has the same page bound (512 MiB on a 4 KiB host).
	static constexpr size_t kPageTableSize = 1u << 18u;
	static constexpr size_t kMaxPages      = kPageTableSize / 2u;
	// Summaries of kBlockPages consecutive pages answer queries over fully
	// covered blocks; edge pages are always checked individually.
	static constexpr size_t   kBlockPages     = 64u;
	static constexpr size_t   kBlockTableSize = 1u << 17u;
	static constexpr uint32_t kNoBlock        = UINT32_MAX;
	// The high bit reserves the gate for a page/block identity writer after the
	// current VM publisher transaction drains the low-bit callback count.
	static constexpr uint32_t kIdentityWriterPending      = 0x80000000u;
	static constexpr uint32_t kIdentityPublisherCountMask = kIdentityWriterPending - 1u;
	// Range records are ordinary heap metadata, used only under the
	// registration mutex. Each registration needs at least one page entry.
	static constexpr size_t kMaxRanges = kMaxPages;
	// Keep tiny writes on the hash lookup path. Larger spans scan the fixed
	// table once, including sparse spans extending over unmapped guest memory.
	static constexpr size_t kDirectWritePages = 64u;

	[[nodiscard]] uintptr_t         PageStart(uintptr_t address) const noexcept;
	[[nodiscard]] uintptr_t         PageEnd(uintptr_t page) const noexcept;
	[[nodiscard]] uintptr_t         RangeEnd(uintptr_t address, size_t size) const noexcept;
	[[nodiscard]] PageEntry*        FindPage(uintptr_t page) noexcept;
	[[nodiscard]] const PageEntry*  FindPage(uintptr_t page) const noexcept;
	[[nodiscard]] PageEntry*         FindWritePage(uintptr_t page) noexcept;
	[[nodiscard]] PageEntry*        FindOrCreatePage(uintptr_t page) noexcept;
	[[nodiscard]] bool               HasPageCapacity(uintptr_t first, uintptr_t last) const noexcept;
	void                             ReactivateRetiredPage(PageEntry* entry) noexcept;
	void                            ReleasePageSlot(PageEntry* entry) noexcept;
	[[nodiscard]] uintptr_t         BlockKey(uintptr_t page) const noexcept;
	[[nodiscard]] const BlockEntry* FindBlock(uintptr_t page) const noexcept;
	[[nodiscard]] uint32_t          FindOrCreateBlock(uintptr_t page) noexcept;
	[[nodiscard]] BlockEntry*       BlockOf(const PageEntry* page) noexcept;
	[[nodiscard]] RangeEvidence     SegmentEvidence(uintptr_t first_index, uintptr_t last_index) const noexcept;
	[[nodiscard]] RangeEvidence     RangeEvidenceOf(const RangeKey& range, const RangeRecord& record) const noexcept;
	[[nodiscard]] bool              RangeIsFallback(const RangeKey& range, const RangeRecord& record) const noexcept;
	template <typename Visitor> void VisitOverlappingRangesLocked(uintptr_t begin, uintptr_t end, const Visitor& visitor) const;
	void                            ClaimPageForRetirement(PageEntry* entry) noexcept;
	[[nodiscard]] bool              RestoreRetirementRun(uintptr_t first, uintptr_t last, uint32_t token) noexcept;
	void                            MarkFallback(uintptr_t page, uintptr_t end) noexcept;
	void                            MarkPageFallback(PageEntry* page) noexcept;
	void                            MarkPageWrite(PageEntry* page) noexcept;
	// Normal-thread admission uses the same yield-based wait as the restorer
	// fence and adds no mutex acquisition to VM callbacks.
	void                             AcquirePublisher() noexcept;
	void                             ReleasePublisher() noexcept;
	void                             AcquireIdentityWriter() noexcept;
	void                             ReleaseIdentityWriter() noexcept;
	[[nodiscard]] bool              TryEnterRestorer() noexcept;
	void                            LeaveRestorer() noexcept;
	void                            EnterRestorer() noexcept;
	void                            RaiseFence() noexcept;
	void                            LowerFence() noexcept;
	[[nodiscard]] RestoreResult     RestoreFromAuthority(PageEntry* entry, uintptr_t page) noexcept;
	[[nodiscard]] bool              FaultMayBeTracked(const PageEntry* entry, uintptr_t page_address) const noexcept;
	[[nodiscard]] bool              HandleWriteFaultEntered(uintptr_t page_address) noexcept;
	[[nodiscard]] bool              NotifyPageWriteEntered(PageEntry* entry, uintptr_t page_address, bool* handled) noexcept;
	void                            PublishAuthorityChange(const Core::VirtualMemory::WriteLeaseChange& change) noexcept;
	static void AuthorityBeginChange(void* context, uint64_t address, uint64_t size) noexcept;
	static void AuthorityDecide(void* context, const Core::VirtualMemory::WriteLeaseChange& change,
	                            Core::VirtualMemory::WriteLeaseDecision decision, void* decision_context) noexcept;
	static void AuthorityEndChange(void* context, const Core::VirtualMemory::WriteLeaseChange& change) noexcept;
	template <typename Visitor> void VisitWritePages(uintptr_t first, uintptr_t last, const Visitor& visitor) noexcept;
	[[nodiscard]] bool              NotifyPageWrite(PageEntry* entry, uintptr_t page_address) noexcept;
	[[nodiscard]] bool              NotifyWritePages(uintptr_t first, uintptr_t last) noexcept;
	[[nodiscard]] bool              HasHostWriteLocked(uintptr_t first, uintptr_t last) const noexcept;
	void                            PrepareHostWriteLocked(uintptr_t address, size_t size) noexcept;

	struct HostWriteRange
	{
		uint64_t token = 0;
		uintptr_t first = 0;
		uintptr_t last = 0;
		uint64_t refs = 0;
	};
	// Counted page spans, protected by the same mutex as native rearming. Only
	// live leases occupy entries; no page-table capacity is needed for I/O into
	// memory which has not been registered as a graphics resource yet.
	std::vector<HostWriteRange> m_host_writes;
	uint64_t m_next_host_write = 1;

	uint64_t                      m_page_size = 0;
	std::unique_ptr<PageEntry[]>  m_pages;
	std::unique_ptr<BlockEntry[]> m_blocks;
	std::map<RangeKey, RangeRecord> m_ranges;
	// Counts every keyed identity, including retained late-fault evidence.
	// Read and changed only under m_registration_mutex.
	size_t                        m_page_identity_count = 0;
	uintptr_t                     m_max_range_bytes    = 0;
	mutable std::mutex*           m_registration_mutex = nullptr;
	std::atomic<uint64_t>*        m_epoch              = nullptr;
	GpuDirtyPageProtectionOps     m_protection_ops {};
	// Guest protection authority. Restorers announce themselves in m_restorers
	// and back off while m_authority_fences is nonzero; a VM change or a slot
	// identity change raises the fence and waits for announced restorers.
	Core::VirtualMemory::WriteLeaseAuthority m_authority {};
	// Native VM publishers share the page table. The backend serializes
	// Protect/BeginUnmap transactions; they can announce several runs before
	// ending any. Release pairs callbacks per run, so its count may drain between
	// runs; owner lifetime excludes concurrent slot users during destruction.
	// An identity writer drains active callbacks, then uses the restorer fence.
	std::atomic<uint32_t>         m_identity_gate {0};
	std::atomic<uint32_t>         m_restorers {0};
	std::atomic<uint32_t>         m_authority_fences {0};
	bool                          m_enabled = true;
};

// Process-wide tracker used by the exception/HLE seams. The startup thread
// creates it once the process fault handler is installed, and it then lives,
// like that handler, for the rest of the process. Tracking is enabled by
// default and can be disabled for diagnosis with
// KYTY_DISABLE_GPU_DIRTY_TRACKING=1. Before installation GetGpuDirtyPageTracker
// returns a disabled tracker that is never published.
void                 GpuDirtyPageTrackerNotifyFaultHandlerInstalled() noexcept;
GpuDirtyPageTracker& GetGpuDirtyPageTracker() noexcept;
// Signal route: looks the published tracker up without initializing anything
// and returns false while none is published.
[[nodiscard]] bool GpuDirtyPageTrackerHandleAccessFault(uintptr_t                                                 address,
                                                        Core::VirtualMemory::ExceptionHandler::AccessViolationType access) noexcept;

} // namespace Kyty::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDIRTYPAGETRACKER_H_
