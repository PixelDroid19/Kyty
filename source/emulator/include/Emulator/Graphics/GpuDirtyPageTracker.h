#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDIRTYPAGETRACKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDIRTYPAGETRACKER_H_

#include "Kyty/Core/VirtualMemory.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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

struct GpuDirtyPageProtectionOps
{
	void* context = nullptr;
	Core::VirtualMemory::ProtectionChangeResult (*remove_write_and_capture)(
	    void* context, uintptr_t address, size_t size, Core::VirtualMemory::CapturedProtectionVisitor visitor,
	    void* visitor_context) noexcept = nullptr;
	bool (*remove_write)(void* context, uintptr_t address, size_t size, uint32_t restore_token) noexcept = nullptr;
	bool (*restore)(void* context, uintptr_t address, size_t size, uint32_t restore_token) noexcept = nullptr;
	bool (*restore_signal_safe)(void* context, uintptr_t address, size_t size, uint32_t restore_token) noexcept = nullptr;
};

// Fixed-capacity, signal-safe dirty-page metadata. Registration and protection
// changes happen on normal threads; HandleWriteFault and NotifyWrite only use
// atomics, bounded table scans, and the raw write-enable VM primitive.
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

	// Called by the exception handler for a write fault. It must remain
	// async-signal-safe and returns false for untracked/unarmed addresses.
	[[nodiscard]] bool HandleWriteFault(uintptr_t address) noexcept;

	// Host/HLE writers call this before writing a protected destination. Work
	// for wide spans is bounded by tracked metadata, not the requested byte count.
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

private:
	struct PageEntry;
	struct RangeEntry;

	// Fixed, bounded metadata with 262,144 slots and a 131,072-page limit per
	// registered range (512 MiB on a 4 KiB host). Large texture atlases
	// otherwise exhaust the old cover and force stable full-range hashes.
	static constexpr size_t kPageTableSize = 1u << 18u;
	static constexpr size_t kMaxPages      = kPageTableSize / 2u;
	static constexpr size_t kMaxRanges     = 512u;
	// Keep tiny writes on the hash lookup path. Larger spans scan the fixed
	// table once, including sparse spans extending over unmapped guest memory.
	static constexpr size_t kDirectWritePages = 64u;

	[[nodiscard]] uintptr_t         PageStart(uintptr_t address) const noexcept;
	[[nodiscard]] uintptr_t         PageEnd(uintptr_t page) const noexcept;
	[[nodiscard]] uintptr_t         RangeEnd(uintptr_t address, size_t size) const noexcept;
	[[nodiscard]] PageEntry*        FindPage(uintptr_t page) noexcept;
	[[nodiscard]] const PageEntry*  FindPage(uintptr_t page) const noexcept;
	[[nodiscard]] PageEntry*        FindOrCreatePage(uintptr_t page) noexcept;
	[[nodiscard]] RangeEntry*       FindRange(uintptr_t address, size_t size) noexcept;
	[[nodiscard]] const RangeEntry* FindRange(uintptr_t address, size_t size) const noexcept;
	void                            ClaimPageForRetirement(PageEntry* entry) noexcept;
	[[nodiscard]] bool              RestoreRetirementRun(uintptr_t first, uintptr_t last, uint32_t token) noexcept;
	[[nodiscard]] bool              HasCover(uintptr_t page, uintptr_t end, bool* fallback) const noexcept;
	void                            MarkFallback(uintptr_t page, uintptr_t end) noexcept;
	void                            MarkPageWrite(PageEntry* page) noexcept;
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
	std::unique_ptr<RangeEntry[]> m_ranges;
	mutable std::mutex*           m_registration_mutex = nullptr;
	std::atomic<uint64_t>*        m_epoch              = nullptr;
	GpuDirtyPageProtectionOps     m_protection_ops {};
	bool                          m_enabled = true;
};

// Process-wide tracker used by the exception/HLE seams. Its fixed metadata is
// allocated once outside signal context. Tracking is enabled by default and
// can be disabled for diagnosis with KYTY_DISABLE_GPU_DIRTY_TRACKING=1.
// The runtime must publish its fault handler before first use.
void                 GpuDirtyPageTrackerNotifyFaultHandlerInstalled() noexcept;
GpuDirtyPageTracker& GetGpuDirtyPageTracker() noexcept;

} // namespace Kyty::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDIRTYPAGETRACKER_H_
