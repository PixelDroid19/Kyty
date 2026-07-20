#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDIRTYPAGETRACKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDIRTYPAGETRACKER_H_

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>

namespace Kyty::Libs::Graphics {

enum class GpuDirtyTrackingMode : uint32_t
{
	PageFault,
	HashFallback
};

enum class GpuDirtyProtectionState : uint32_t
{
	Writable,
	Arming,
	Armed,
	Disarming,
	Retired
};

[[nodiscard]] constexpr bool GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState state)
{
	return state != GpuDirtyProtectionState::Writable;
}

[[nodiscard]] constexpr bool GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState observed)
{
	// If a writer completed while Rearm was entering mprotect, the page may
	// have become read-only after that writer published Writable. A concurrent
	// final unregister can likewise publish Retired after restoring the page.
	return observed == GpuDirtyProtectionState::Writable || observed == GpuDirtyProtectionState::Retired;
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

// Fixed-capacity, signal-safe dirty-page metadata. Registration and protection
// changes happen on normal threads; HandleWriteFault and NotifyWrite only use
// atomics, bounded table scans, and the raw write-enable VM primitive.
class GpuDirtyPageTracker
{
public:
	static GpuDirtyPageTracker& Instance() noexcept;
	explicit GpuDirtyPageTracker(bool enabled = true);
	~GpuDirtyPageTracker();

	GpuDirtyPageTracker(const GpuDirtyPageTracker&) = delete;
	GpuDirtyPageTracker& operator=(const GpuDirtyPageTracker&) = delete;

	[[nodiscard]] bool RegisterRange(uintptr_t address, size_t size) noexcept;
	[[nodiscard]] bool UnregisterRange(uintptr_t address, size_t size) noexcept;

	// Protect the covered pages read-only before a CPU->GPU read. BeginRead
	// captures the generation owned by that read transaction; callers must not
	// replace it with a later snapshot after hashing or uploading.
	[[nodiscard]] GpuDirtyReadObservation BeginRead(uintptr_t address, size_t size) noexcept;
	[[nodiscard]] bool PrepareForRead(uintptr_t address, size_t size) noexcept;
	[[nodiscard]] bool Rearm(uintptr_t address, size_t size) noexcept;

	// Called by the exception handler for a write fault. It must remain
	// async-signal-safe and returns false for untracked/unarmed addresses.
	[[nodiscard]] bool HandleWriteFault(uintptr_t address) noexcept;

	// Host/HLE writers call this before writing a protected destination.
	[[nodiscard]] bool NotifyWrite(uintptr_t address, size_t size) noexcept;

	[[nodiscard]] uint64_t SnapshotGeneration(uintptr_t address, size_t size) const noexcept;
	[[nodiscard]] bool ChangedSince(uintptr_t address, size_t size, uint64_t snapshot) const noexcept;
	[[nodiscard]] bool Enabled() const noexcept;
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

	[[nodiscard]] uintptr_t PageStart(uintptr_t address) const noexcept;
	[[nodiscard]] uintptr_t RangeEnd(uintptr_t address, size_t size) const noexcept;
	[[nodiscard]] PageEntry* FindPage(uintptr_t page) noexcept;
	[[nodiscard]] const PageEntry* FindPage(uintptr_t page) const noexcept;
	[[nodiscard]] PageEntry* FindOrCreatePage(uintptr_t page) noexcept;
	[[nodiscard]] RangeEntry* FindRange(uintptr_t address, size_t size) noexcept;
	[[nodiscard]] const RangeEntry* FindRange(uintptr_t address, size_t size) const noexcept;
	[[nodiscard]] bool HasCover(uintptr_t page, uintptr_t end, bool* fallback) const noexcept;
	void MarkFallback(uintptr_t page, uintptr_t end) noexcept;
	void MarkPageWrite(PageEntry* page) noexcept;

	uint64_t m_page_size = 0;
	std::unique_ptr<PageEntry[]> m_pages;
	std::unique_ptr<RangeEntry[]> m_ranges;
	mutable std::mutex* m_registration_mutex = nullptr;
	std::atomic<uint64_t>* m_epoch = nullptr;
	bool m_enabled = true;
};

// Process-wide tracker used by the exception/HLE seams. Its fixed metadata is
// allocated once outside signal context. Tracking is enabled by default and
// can be disabled for diagnosis with KYTY_DISABLE_GPU_DIRTY_TRACKING=1.
// The runtime must publish its fault handler before first use.
void GpuDirtyPageTrackerNotifyFaultHandlerInstalled() noexcept;
GpuDirtyPageTracker& GetGpuDirtyPageTracker() noexcept;

} // namespace Kyty::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDIRTYPAGETRACKER_H_
