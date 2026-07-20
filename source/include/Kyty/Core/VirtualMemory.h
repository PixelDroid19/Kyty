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
	bool skip_ud2 = false;
	bool fault_log = false;
};

// Environment diagnostics are enabled by variable presence, including an
// empty value. Callers load the environment outside signal handlers.
SignalDiagnosticsConfig MakeSignalDiagnosticsConfig(const char* skip_ud2, const char* fault_log) noexcept;

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
		ExceptionType       type                   = ExceptionType::Unknown;
		AccessViolationType access_violation_type  = AccessViolationType::Unknown;
		uint64_t            access_violation_vaddr = 0;
		uint64_t            exception_address      = 0;
		uint64_t            rbp                    = 0;
		uint32_t            exception_win_code     = 0;
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
// Preserve the requested view when possible. A host may relocate only when
// its own runtime occupies the range and Kyty does not own the collision.
uint64_t MapSharedFixedOrRelocated(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size, Mode mode,
                                   uint64_t alignment);
bool           Free(uint64_t address);
bool           Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode = nullptr);
// Write-enable a page from an access-violation handler without taking Kyty's
// virtual-memory bookkeeping lock. This is intentionally narrow: callers must
// restore tracked protection with Protect() outside the handler.
bool ProtectWriteSignalSafe(uint64_t address, uint64_t size);
bool FlushInstructionCache(uint64_t address, uint64_t size);
bool PatchReplace(uint64_t vaddr, uint64_t value);

// Diagnostic single-step tracer (macOS/Rosetta): logs the next `steps` guest
// instructions on the current thread via the x86 trap flag. No-op elsewhere.
void SetGuestTrace(int steps);

// Diagnostic timer profiler (macOS/Rosetta): periodically logs the guest
// instruction pointer of the running thread; locates spinning guest loops.
void StartGuestProfiler();

// Flexible-memory demand paging (macOS): register a reserved range, then map its
// pages lazily from the fault handler on first write. TryDemandMap returns true if
// vaddr fell in a registered range and its page was mapped (retry the instruction).
void RegisterDemandRange(uint64_t addr, uint64_t size);
bool TryDemandMap(uint64_t vaddr);

// Async-signal-safe fatal fault report + terminate (no allocator use).
void FatalFault(uint64_t vaddr, uint64_t rip);

} // namespace VirtualMemory

} // namespace Kyty::Core

#endif /* INCLUDE_KYTY_CORE_VIRTUALMEMORY_H_ */
