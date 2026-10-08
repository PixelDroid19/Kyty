#ifndef INCLUDE_KYTY_CORE_VIRTUALMEMORY_H_
#define INCLUDE_KYTY_CORE_VIRTUALMEMORY_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

namespace Kyty::Core {

struct SystemInfo
{
	String ProcessorName;
};

SystemInfo GetSystemInfo();

namespace VirtualMemory {

struct SignalDiagnosticsConfig
{
	bool skip_ud2     = false;
	bool fault_log    = false;
	bool crash_memory = false;
};

// Environment diagnostics are enabled by variable presence, including an
// empty value. Callers load the environment outside signal handlers.
SignalDiagnosticsConfig MakeSignalDiagnosticsConfig(const char* skip_ud2, const char* fault_log, const char* crash_memory = nullptr) noexcept;

class ExceptionHandlerPrivate;

class ExceptionHandler
{
public:
	enum class ExceptionType
	{
		Unknown,
		AccessViolation
	};

	enum class AccessViolationType
	{
		Unknown,
		Read,
		Write,
		Execute
	};

	struct ExceptionInfo
	{
		static constexpr uint32_t StackCapacity        = 128;
		static constexpr uint32_t MemoryWindowCapacity = 24;
		static constexpr uint32_t MemoryWindowSize     = 64;

		struct MemoryWindow
		{
			uint64_t address                 = 0;
			uint8_t  bytes[MemoryWindowSize] = {};
			uint32_t size                    = 0;
		};

		ExceptionType       type                   = ExceptionType::Unknown;
		AccessViolationType access_violation_type  = AccessViolationType::Unknown;
		uint64_t            access_violation_vaddr = 0;
		uint64_t            exception_address      = 0;
		uint64_t            rbp                    = 0;
		uint64_t            rsp                    = 0;
		uint64_t            rflags                 = 0;
		uint64_t            rax                    = 0;
		uint64_t            rbx                    = 0;
		uint64_t            rcx                    = 0;
		uint64_t            rdx                    = 0;
		uint64_t            rsi                    = 0;
		uint64_t            rdi                    = 0;
		uint64_t            r8                     = 0;
		uint64_t            r9                     = 0;
		uint64_t            r10                    = 0;
		uint64_t            r11                    = 0;
		uint64_t            r12                    = 0;
		uint64_t            r13                    = 0;
		uint64_t            r14                    = 0;
		uint64_t            r15                    = 0;
		uint64_t            stack[StackCapacity]   = {};
		MemoryWindow        memory_windows[MemoryWindowCapacity] = {};
		uint32_t            stack_count                         = 0;
		uint32_t            memory_window_count                 = 0;
		uint32_t            exception_win_code                  = 0;
	};

	using handler_func_t = void (*)(const ExceptionInfo*);

	ExceptionHandler();
	virtual ~ExceptionHandler();

	KYTY_CLASS_NO_COPY(ExceptionHandler);

	static uint64_t GetSize();

	bool Install(uint64_t base_address, uint64_t handler_addr, uint64_t image_size, handler_func_t func);
	bool Uninstall();

	static bool InstallVectored(handler_func_t func);

private:
	ExceptionHandlerPrivate* m_p = nullptr;
};

enum class Mode : uint32_t
{
	NoAccess         = 0,
	Read             = 1,
	Write            = 2,
	ReadWrite        = Read | Write,
	Execute          = 4,
	ExecuteRead      = Execute | Read,
	ExecuteWrite     = Execute | Write,
	ExecuteReadWrite = Execute | Read | Write,
};

class SharedBacking;

inline bool IsExecute(Mode mode)
{
	return (mode == Mode::Execute || mode == Mode::ExecuteRead || mode == Mode::ExecuteWrite || mode == Mode::ExecuteReadWrite);
}

void Init();

uint64_t GetPageSize();

uint64_t Alloc(uint64_t address, uint64_t size, Mode mode);
uint64_t AllocAligned(uint64_t address, uint64_t size, Mode mode, uint64_t alignment);
bool     AllocFixed(uint64_t address, uint64_t size, Mode mode);
// Commit private pages inside a NoAccess reservation owned by this process.
// Prefix and suffix remain reserved.
bool     AllocFixedReplacingOwnedReservation(uint64_t address, uint64_t size, Mode mode);
// Reserve guest virtual address space without committing host memory. On
// POSIX this is a PROT_NONE/no-reserve mapping; on Windows it is MEM_RESERVE.
uint64_t Reserve(uint64_t address, uint64_t size);
uint64_t ReserveAligned(uint64_t address, uint64_t size, uint64_t alignment);
bool     ReserveFixed(uint64_t address, uint64_t size);

// Sparse host backing whose views share bytes by backing_offset. Free() unmaps
// individual views without destroying the backing or other aliases.
SharedBacking* CreateSharedBacking(uint64_t size);
void           DestroySharedBacking(SharedBacking* backing);
// Reclaim host RAM for a released physical range (punch hole / discard pages).
// Only call when no live map still covers [backing_offset, backing_offset+size).
bool           DiscardSharedBackingRange(SharedBacking* backing, uint64_t backing_offset, uint64_t size);
struct SharedBackingSpan
{
	uint64_t offset      = 0;
	uint64_t size        = 0;
	bool     unpopulated = false;
};
// Snapshot query: marks each page-aligned span whose whole backing interval
// holds no populated or swapped-out page. Spans are swept in backing order, so
// one host query clears every span up to the next populated page. Unsupported
// hosts and query errors leave spans unmarked.
void           FindUnpopulatedSharedBackingSpans(SharedBacking* backing, SharedBackingSpan* spans, size_t count);
// The backing gains a page only when a view first touches it and loses pages
// only through DiscardSharedBackingRange, so an unchanged population means no
// page of any view became resident or was dropped in between.
struct SharedBackingPopulation
{
	uint64_t populated_bytes = 0;
	uint64_t discards        = 0;
	bool     operator==(const SharedBackingPopulation& other) const
	{
		return populated_bytes == other.populated_bytes && discards == other.discards;
	}
	bool operator!=(const SharedBackingPopulation& other) const { return !(*this == other); }
};
// False when the host cannot report the population.
bool           QuerySharedBackingPopulation(SharedBacking* backing, SharedBackingPopulation* population);
uint64_t       MapSharedAligned(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size, Mode mode,
                                uint64_t alignment);
bool           MapSharedFixed(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size, Mode mode);
// Replace only a NoAccess reservation owned by this process. Prefix and suffix
// remain reserved and independently releasable.
bool MapSharedFixedReplacingOwnedReservation(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
                                             Mode mode);
bool SupportsSharedFixedOwnedReservationReplacement();
// Preserve the requested view when possible. A host may relocate only when
// its own runtime occupies the range and Kyty does not own the collision.
uint64_t MapSharedFixedOrRelocated(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size, Mode mode,
                                   uint64_t alignment);
bool           Free(uint64_t address);
// Unmaps a page-aligned part of one mapping; the rest stays mapped.
bool           FreeRange(uint64_t address, uint64_t size);
bool           Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode = nullptr);
// Guest-only protection transition. Ownership validation, the host operation,
// and protection tracking are one transaction with Free() and guest copies.
bool           ProtectGuest(uint64_t address, uint64_t size, Mode mode, Mode* old_mode = nullptr);
// Applies guest protection only while the entire page-aligned range still
// belongs to the supplied mapping instance. Identity validation, protection
// tracking, and write-lease handling share one transaction with mapping changes.
// The metadata-only check has no deferred-copy byte budget; invalid input or
// an identity mismatch leaves the range and old_mode unchanged.
bool ProtectGuestIfMappingMatches(uint64_t address, uint64_t size, Mode mode, uint64_t mapping_identity,
                                 Mode* old_mode = nullptr);
// Turn a committed guest-owned interval back into a NoAccess reservation.
// Linux supports partial intervals; other hosts fail without changing state.
bool           DecommitGuestRange(uint64_t address, uint64_t size);
// Returns true only when every byte belongs to a mapping created through
// Kyty's guest virtual-memory map family. Host mappings are never guest-owned.
bool           IsRangeGuestOwned(uint64_t address, uint64_t size);
// Returns true only when every byte belongs to a committed mapping whose host
// protection permits reads. The range must also be guest-owned. It does not
// probe memory or install a fault guard.
bool           IsRangeReadable(uint64_t address, uint64_t size);
// One byte per page of [address, address + size): nonzero when the page is
// resident (populated). Platforms without a residency query report every page
// resident. address and size must be page aligned.
bool           QueryResidentPages(uint64_t address, uint64_t size, uint8_t* resident);
// Returns true only when every byte belongs to a committed mapping whose host
// protection permits writes. The range must also be guest-owned. It does not
// probe memory or install a fault guard.
bool           IsRangeWritable(uint64_t address, uint64_t size);
using ReadableGuestRangeVisitor = bool (*)(const void* data, uint64_t size, void* context);
// Validate and visit a readable guest-owned range while Free() and protection
// changes are excluded. The visitor must not call virtual-memory APIs.
bool VisitReadableGuestRange(uint64_t address, uint64_t size, ReadableGuestRangeVisitor visitor, void* context);
// Copy while holding the virtual-memory tracker lock across range validation
// and the copy. This serializes the transfer with Free() and Protect().
bool           CopyFromGuest(void* destination, uint64_t source, uint64_t size);
bool           CopyToGuest(uint64_t destination, const void* source, uint64_t size);
// The guest mapping instances that covered a guest range at one instant. Every successful map,
// shared view, commit, decommit, or reservation (re)publication gives its interval a fresh
// identity, even when it reuses the address or the backing bytes of an earlier mapping.
// Splitting a mapping (a partial unmap or replacement elsewhere in it), guest protection changes,
// and write-lease removal or rearm keep the identity; unmapping drops it. Identities are never
// reused within the process. A snapshot is a fixed-size value: it owns no host resources and
// needs no release. The byte budget covers the largest deferred host publication (the 48 KiB
// guest GDS window); kMaxSegments covers that budget at 4 KiB host pages from any start offset.
struct GuestMappingSnapshot
{
	static constexpr uint64_t kMaxBytes    = 48u * 1024u;
	static constexpr uint32_t kMaxSegments = 16;

	struct Segment
	{
		uint64_t address  = 0;
		uint64_t size     = 0;
		uint64_t identity = 0;
	};

	uint64_t address                = 0;
	uint64_t size                   = 0;
	uint32_t segment_count          = 0;
	Segment  segments[kMaxSegments] = {};
};
// Captures, in one VM transaction, the mapping identities of a range that is entirely guest owned.
// Capture is metadata only: it does not consult protection, so a read-only or NoAccess range of a
// guest mapping is captured, and a write lease's native protection is irrelevant. Fails, leaving an
// empty snapshot that every copy refuses, when any byte is not guest owned, or when the range
// exceeds the byte or segment budget.
bool CaptureGuestMappingSnapshot(uint64_t address, uint64_t size, GuestMappingSnapshot* snapshot);
// Copies `size` bytes to the snapshot's range only when `size` equals its size, every byte still
// belongs to the captured mapping instance, and the guest protection permits writes now. Identity,
// write access, and the copy are one transaction with unmap, map, and protection changes. A
// refusal copies nothing. Like CopyToGuest, it does not lift a write lease's native protection.
bool CopyToGuestIfMappingMatches(const GuestMappingSnapshot& snapshot, const void* source, uint64_t size);
enum class ProtectionChangeStatus : uint32_t
{
	Success,
	InvalidRange,
	UnmappedRange,
	UnsupportedProtection,
	ApplyFailedRolledBack,
	RollbackFailed
};
struct CapturedProtectionRun
{
	uint64_t address = 0;
	uint64_t size = 0;
	Mode mode = Mode::NoAccess;
	uint32_t restore_token = 0;
};
using CapturedProtectionVisitor = bool (*)(void* context, const CapturedProtectionRun& run) noexcept;
struct ProtectionChangeResult
{
	ProtectionChangeStatus status = ProtectionChangeStatus::InvalidRange;
	uint32_t applied_runs = 0;
	uint64_t applied_bytes = 0;
	[[nodiscard]] bool Succeeded() const noexcept { return status == ProtectionChangeStatus::Success; }
};
// Write leases. RemoveWriteAndCapture() and RemoveWriteFromProtection() lease the write permission of
// guest pages to an authority (for example a dirty-page tracker) that restores it from a fault
// handler with RestoreProtectionSignalSafe(). The guest's own protection stays authoritative: guest
// queries and copies see it, RestoreProtection() reapplies it, and the lease never outlives the
// mapping. Every later protection change or mapping removal over a leased page is reported to its
// authority inside the same VM transaction:
//  - begin_change fences the authority: it must stop starting signal-safe restores and wait for the
//    restores already in flight, so no stale token is applied after the change.
//  - decide (protection changes only) reports, for every byte of the given run in ascending order,
//    whether write stays removed under the new protection. It must not publish anything: the change
//    may still fail and be rolled back.
//  - end_change publishes the outcome and releases the fence. A committed protection change replaces
//    the run's tokens; a committed removal (kind Unmap) revokes them; an uncommitted change leaves the
//    authority's state as it was.
// All callbacks run with the VM transaction locked: they must not lock, allocate, or call
// VirtualMemory, and the fence holder never touches leased guest memory. A lease without an authority
// is dropped by the next guest protection change.
enum class WriteLeaseChangeKind : uint32_t
{
	Protect,
	Unmap
};
struct WriteLeaseChange
{
	uint64_t             address     = 0;
	uint64_t             size        = 0;
	WriteLeaseChangeKind kind        = WriteLeaseChangeKind::Protect;
	Mode                 mode        = Mode::NoAccess;
	uint32_t             guest_token = 0;
	bool                 committed   = false;
};
using WriteLeaseDecision = bool (*)(void* decision_context, uint64_t address, uint64_t size, bool remove_write) noexcept;
struct WriteLeaseAuthority
{
	void* context = nullptr;
	void (*begin_change)(void* context, uint64_t address, uint64_t size) noexcept = nullptr;
	void (*decide)(void* context, const WriteLeaseChange& change, WriteLeaseDecision decision, void* decision_context) noexcept = nullptr;
	void (*end_change)(void* context, const WriteLeaseChange& change) noexcept = nullptr;
};
// The visitor runs synchronously while the host protection transaction is locked. It must not call
// Protect(), RemoveWriteAndCapture(), or RestoreProtection(). Returning false aborts before native
// protection changes, but does not undo side effects produced by earlier visitor calls. Every run
// is visited before write access is removed so fault handlers can always restore a published token.
// The captured runs are leased to `authority` (which may be null).
ProtectionChangeResult RemoveWriteAndCapture(uint64_t address, uint64_t size, CapturedProtectionVisitor visitor, void* context,
	                                         const WriteLeaseAuthority* authority = nullptr) noexcept;
// Removes write from the current guest protection of a mapped range and leases it to `authority`. The
// guest protection is authoritative; restore_token is the holder's last known token and is not trusted.
// A page whose guest protection has no write access keeps it and stays leased.
bool RemoveWriteFromProtection(uint64_t address, uint64_t size, uint32_t restore_token,
	                           const WriteLeaseAuthority* authority = nullptr) noexcept;
// Reapplies the authoritative guest protection of a mapped range; restore_token is not trusted. The
// lease remains, so later guest changes are still reported to its authority.
bool RestoreProtection(uint64_t address, uint64_t size, uint32_t restore_token) noexcept;
// Applies restore_token natively without the VM lock. Only a lease authority that is fenced against
// guest protection changes (see above) may call it.
bool RestoreProtectionSignalSafe(uint64_t address, uint64_t size, uint32_t restore_token) noexcept;
// Ends every lease of `authority` and reapplies the guest protection of those pages. When a page's
// protection cannot be reapplied, its lease stays without an authority and the call fails; in every
// case the VM no longer refers to `authority` afterwards.
[[nodiscard]] bool ReleaseWriteLeases(const WriteLeaseAuthority* authority) noexcept;
// Write-enable a page from an access-violation handler without taking Kyty's
// virtual-memory bookkeeping lock. This is intentionally narrow: callers must
// restore tracked protection with Protect() outside the handler.
bool ProtectWriteSignalSafe(uint64_t address, uint64_t size);
bool FlushInstructionCache(uint64_t address, uint64_t size);
bool PatchReplace(uint64_t vaddr, uint64_t value);

// Returns the decoded x86-64 instruction length, or zero when the opcode is
// unsupported. The decoder does not allocate and is suitable for executable
// image transforms that first verify a complete 15-byte instruction window.

// Diagnostic single-step tracer (macOS/Rosetta): logs the next `steps` guest
// instructions on the current thread via the x86 trap flag. No-op elsewhere.
void SetGuestTrace(int steps);

// Diagnostic timer profiler (macOS/Rosetta): periodically logs the guest
// instruction pointer of the running thread; locates spinning guest loops.
void StartGuestProfiler();

// POSIX demand paging for reserved guest ranges. Consumed and released subranges
// must be unregistered before another owner can use the same virtual address.
// TryDemandMap returns true when the faulting page was materialized.
bool RegisterDemandRange(uint64_t addr, uint64_t size);
bool UnregisterDemandRange(uint64_t addr, uint64_t size);
bool TryDemandMap(uint64_t vaddr);

// Configure an optional fixed-path native crash report. The path is copied
// before guest execution; the fatal handler never allocates.
void ConfigureFatalFaultReport(const char* path) noexcept;

// Native fatal fault report + terminate. The report contains the bounded
// register and stack snapshot captured at the exception boundary.
void FatalFault(const ExceptionHandler::ExceptionInfo* info) noexcept;

// Compatibility overload for callers that only have an address and RIP.
void FatalFault(uint64_t vaddr, uint64_t rip);

} // namespace VirtualMemory

} // namespace Kyty::Core

#endif /* INCLUDE_KYTY_CORE_VIRTUALMEMORY_H_ */
