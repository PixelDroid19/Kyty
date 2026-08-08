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
bool           Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode = nullptr);
// Returns true only when every byte belongs to a committed mapping whose host
// protection permits reads. It does not probe memory or install a fault guard.
bool           IsRangeReadable(uint64_t address, uint64_t size);
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
// The visitor runs synchronously while the host protection transaction is locked. It must not call
// Protect(), RemoveWriteAndCapture(), or RestoreProtection(). Returning false aborts before native
// protection changes, but does not undo side effects produced by earlier visitor calls. Every run
// is visited before write access is removed so fault handlers can always restore a published token.
ProtectionChangeResult RemoveWriteAndCapture(uint64_t address, uint64_t size, CapturedProtectionVisitor visitor,
	                                         void* context) noexcept;
bool RemoveWriteFromProtection(uint64_t address, uint64_t size, uint32_t restore_token) noexcept;
bool RestoreProtection(uint64_t address, uint64_t size, uint32_t restore_token) noexcept;
bool RestoreProtectionSignalSafe(uint64_t address, uint64_t size, uint32_t restore_token) noexcept;
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
