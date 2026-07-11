#include "Kyty/Core/VirtualMemory.h"

#include "Kyty/Sys/SysVirtual.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define KYTY_HAS_EXCEPTIONS
#elif defined(__APPLE__)
#define KYTY_HAS_SIGNAL_EXCEPTIONS
#endif

#ifdef KYTY_HAS_EXCEPTIONS
#include <windows.h> // IWYU pragma: keep
#endif

#ifdef KYTY_HAS_SIGNAL_EXCEPTIONS
#include <csignal>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/ucontext.h>
#endif

// IWYU pragma: no_include <basetsd.h>
// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <wtypes.h>

namespace Kyty::Core {

SystemInfo GetSystemInfo()
{
	SystemInfo ret {};

	sys_get_system_info(&ret);

	return ret;
}

namespace VirtualMemory {

#ifdef KYTY_HAS_EXCEPTIONS

struct JmpRax
{
	template <class Handler>
	void SetFunc(Handler func)
	{
		*reinterpret_cast<Handler*>(&code[2]) = func;
	}

	// mov rax, 0x1122334455667788
	// jmp rax
	uint8_t code[16] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xE0};
};

class ExceptionHandlerPrivate
{
public:
#pragma pack(1)

	struct UnwindInfo
	{
		uint8_t Version : 3;
		uint8_t Flags   : 5;
		uint8_t SizeOfProlog;
		uint8_t CountOfCodes;
		uint8_t FrameRegister : 4;
		uint8_t FrameOffset   : 4;
		ULONG   ExceptionHandler;

		ExceptionHandlerPrivate* ExceptionData;
	};

	struct HandlerInfo
	{
		JmpRax           code;
		RUNTIME_FUNCTION function_table = {};
		UnwindInfo       unwind_info    = {};
	};

#pragma pack()

	static EXCEPTION_DISPOSITION Handler(PEXCEPTION_RECORD   exception_record, ULONG64 /*EstablisherFrame*/, PCONTEXT /*ContextRecord*/,
	                                     PDISPATCHER_CONTEXT dispatcher_context)
	{
		ExceptionHandler::ExceptionInfo info {};

		info.exception_address = reinterpret_cast<uint64_t>(exception_record->ExceptionAddress);

		if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
		{
			info.type = ExceptionHandler::ExceptionType::AccessViolation;
			switch (exception_record->ExceptionInformation[0])
			{
				case 0: info.access_violation_type = ExceptionHandler::AccessViolationType::Read; break;
				case 1: info.access_violation_type = ExceptionHandler::AccessViolationType::Write; break;
				case 8: info.access_violation_type = ExceptionHandler::AccessViolationType::Execute; break;
				default: info.access_violation_type = ExceptionHandler::AccessViolationType::Unknown; break;
			}
			info.access_violation_vaddr = exception_record->ExceptionInformation[1];
		}

		info.rbp                = dispatcher_context->ContextRecord->Rbp;
		info.exception_win_code = exception_record->ExceptionCode;

		auto* p = *static_cast<ExceptionHandlerPrivate**>(dispatcher_context->HandlerData);
		p->func(&info);

		return ExceptionContinueExecution;
	}

	void InitHandler()
	{
		auto* h           = new (reinterpret_cast<void*>(handler_addr)) HandlerInfo;
		auto* code        = &h->code;
		auto* unwind_info = &h->unwind_info;

		function_table = &h->function_table;

		function_table->BeginAddress = 0;
		function_table->EndAddress   = image_size;
		function_table->UnwindData   = reinterpret_cast<uintptr_t>(unwind_info) - base_address;

		unwind_info->Version          = 1;
		unwind_info->Flags            = UNW_FLAG_EHANDLER;
		unwind_info->SizeOfProlog     = 0;
		unwind_info->CountOfCodes     = 0;
		unwind_info->FrameRegister    = 0;
		unwind_info->FrameOffset      = 0;
		unwind_info->ExceptionHandler = reinterpret_cast<uintptr_t>(code) - base_address;
		unwind_info->ExceptionData    = this;

		code->SetFunc(Handler);

		FlushInstructionCache(reinterpret_cast<uint64_t>(code), sizeof(h->code));
	}

	uint64_t          base_address   = 0;
	uint64_t          handler_addr   = 0;
	uint64_t          image_size     = 0;
	PRUNTIME_FUNCTION function_table = nullptr;

	ExceptionHandler::handler_func_t func = nullptr;

	static ExceptionHandler::handler_func_t g_vec_func;
};

ExceptionHandler::handler_func_t ExceptionHandlerPrivate::g_vec_func = nullptr;

#elif defined(KYTY_HAS_SIGNAL_EXCEPTIONS)

class ExceptionHandlerPrivate
{
public:
	static ExceptionHandler::handler_func_t g_vec_func;
};

ExceptionHandler::handler_func_t ExceptionHandlerPrivate::g_vec_func = nullptr;

// Single-step tracer (macOS/Rosetta): TF-based instruction trace of guest code,
// used to diagnose libc faults that lldb cannot reach under Rosetta 2. Armed by
// SetGuestTrace(); logs guest-range instruction pointers until the budget runs out.
static thread_local int g_trace_guest = 0; // remaining guest instructions to log
static thread_local int g_trace_total = 0; // hard cap on total single-steps

void SetGuestTrace(int steps)
{
	g_trace_guest = steps;
	g_trace_total = steps * 200; // allow stepping through HLE/return code in between
}

// Timer-based guest profiler (macOS/Rosetta): samples the instruction pointer of
// whatever thread is running when SIGPROF fires. Unlike TF single-step it survives
// guest popfq/pushf, so it can locate a spinning guest loop. Gated by StartGuestProfiler().
static void kyty_sigprof_handler(int /*sig*/, siginfo_t* /*info*/, void* ucontext)
{
	auto*    uc  = static_cast<ucontext_t*>(ucontext);
	uint64_t rip = uc->uc_mcontext->__ss.__rip;
	static int n = 0;
	if (n++ < 200)
	{
		const char* zone = (rip >= 0x900000000ull && rip < 0x920000000ull) ? "GUEST"
		                   : (rip >= 0x100000000ull && rip < 0x110000000ull) ? "fc_script"
		                                                                     : "other";
		printf("PROF %s %016llx\n", zone, static_cast<unsigned long long>(rip));
	}
}

void StartGuestProfiler()
{
	struct sigaction sa {};
	sa.sa_sigaction = kyty_sigprof_handler;
	sa.sa_flags     = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPROF, &sa, nullptr);
	struct itimerval t {};
	t.it_interval.tv_usec = 2000; // 500 Hz
	t.it_value.tv_usec    = 2000;
	setitimer(ITIMER_PROF, &t, nullptr);
}

static void kyty_sigtrap_handler(int /*sig*/, siginfo_t* /*info*/, void* ucontext)
{
	auto*    uc  = static_cast<ucontext_t*>(ucontext);
	auto&    s   = uc->uc_mcontext->__ss;
	uint64_t rip = s.__rip;
	if (g_trace_guest <= 0 || g_trace_total <= 0)
	{
		s.__rflags &= ~0x100ull;
		return;
	}
	g_trace_total--;
	if (rip >= 0x900000000ull && rip < 0x920000000ull) // any guest module
	{
		printf("TRACE %016llx\n", rip);
		g_trace_guest--;
	}
	s.__rflags |= 0x100ull;
}

// Demand-paged ranges (macOS): flexible-memory regions the guest reserves but that
// are backed lazily. On a write fault inside one, the touched page is mapped RW
// (zero-filled) and the faulting instruction retried — mirroring PS5 flexible memory.
struct DemandRange
{
	uint64_t addr = 0;
	uint64_t size = 0;
};
static DemandRange g_demand_ranges[64];
static int         g_demand_count = 0;

void RegisterDemandRange(uint64_t addr, uint64_t size)
{
	if (g_demand_count < 64)
	{
		g_demand_ranges[g_demand_count].addr = addr;
		g_demand_ranges[g_demand_count].size = size;
		g_demand_count++;
	}
}

static bool try_demand_map(uint64_t vaddr)
{
	for (int i = 0; i < g_demand_count; i++)
	{
		if (vaddr >= g_demand_ranges[i].addr && vaddr < g_demand_ranges[i].addr + g_demand_ranges[i].size)
		{
			// Raw mmap only: this runs in a signal handler, so it must avoid the
			// allocator bookkeeping (std::map insert -> malloc) that would dead-lock.
			uint64_t page = vaddr & ~static_cast<uint64_t>(0xFFF);
			void*    p    = mmap(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
			return p == reinterpret_cast<void*>(page);
		}
	}
	return false;
}

bool TryDemandMap(uint64_t vaddr)
{
	return try_demand_map(vaddr);
}

// forward decl (defined below, near the signal handler)
static void sigsafe_fault(const char* tag, uint64_t a, uint64_t b);

void FatalFault(uint64_t vaddr, uint64_t rip)
{
	// Async-signal-safe fatal exit: report and terminate without touching the
	// allocator (which the faulting thread may hold locked, dead-locking printf).
	sigsafe_fault("FATAL-ACCESS-VIOLATION", vaddr, rip);
	::_Exit(139);
}

// Async-signal-safe: write "<tag> <vaddr> <rip>\n" to stderr without any malloc.
static void sigsafe_fault(const char* tag, uint64_t a, uint64_t b)
{
	char     buf[64];
	int      p        = 0;
	auto     hex      = [&](uint64_t v) {
        buf[p++] = ' ';
        buf[p++] = '0';
        buf[p++] = 'x';
        for (int i = 60; i >= 0; i -= 4)
        {
            buf[p++] = "0123456789abcdef"[(v >> i) & 0xf];
        }
	};
	while (*tag != 0)
	{
		buf[p++] = *tag++;
	}
	hex(a);
	hex(b);
	buf[p++] = '\n';
	(void)!write(2, buf, p);
}

// SIGSEGV/SIGBUS handler: translates the Mach fault into Kyty's ExceptionInfo and
// dispatches to the installed handler. If the handler returns (e.g. a GPU memory
// watchpoint unprotected the page), the faulting instruction is retried.
static void kyty_posix_signal_handler(int sig, siginfo_t* info, void* ucontext)
{
	auto* uc = static_cast<ucontext_t*>(ucontext);

	if (sig == SIGILL)
	{
		auto&    s   = uc->uc_mcontext->__ss;
		uint64_t rip = s.__rip;
		// A guest ud2 (0F 0B) is the trap the compiler emits after a call it believes
		// is noreturn — here, sceKernelDebugRaiseException. On real hardware certain
		// debug-raise codes are soft: the kernel logs and RESUMES past the trap. When
		// KYTY_SKIP_UD2 is set, emulate that: skip the ud2 in guest code and continue,
		// to determine whether the raised condition is actually recoverable.
		if (getenv("KYTY_SKIP_UD2") != nullptr && rip >= 0x900000000ull && rip < 0x920000000ull)
		{
			const auto* code = reinterpret_cast<const uint8_t*>(rip);
			if (code[0] == 0x0F && code[1] == 0x0B)
			{
				sigsafe_fault("SKIP-UD2 @rip", rip, 0);
				s.__rip = rip + 2;
				return;
			}
		}
		printf("\n=== SIGILL @ rip=0x%016llx ===\n", s.__rip);
		printf("rax=0x%016llx rbx=0x%016llx rcx=0x%016llx rdx=0x%016llx\n", s.__rax, s.__rbx, s.__rcx, s.__rdx);
		printf("rsi=0x%016llx rdi=0x%016llx rbp=0x%016llx rsp=0x%016llx\n", s.__rsi, s.__rdi, s.__rbp, s.__rsp);
		printf("r8 =0x%016llx r9 =0x%016llx r10=0x%016llx r11=0x%016llx\n", s.__r8, s.__r9, s.__r10, s.__r11);
		printf("r12=0x%016llx r13=0x%016llx r14=0x%016llx r15=0x%016llx\n", s.__r12, s.__r13, s.__r14, s.__r15);
		::fflush(nullptr);
		signal(SIGILL, SIG_DFL);
		return;
	}

	ExceptionHandler::ExceptionInfo einfo {};

	einfo.type              = ExceptionHandler::ExceptionType::AccessViolation;
	einfo.exception_address = static_cast<uint64_t>(uc->uc_mcontext->__ss.__rip);
	einfo.rbp               = static_cast<uint64_t>(uc->uc_mcontext->__ss.__rbp);

	einfo.access_violation_vaddr = reinterpret_cast<uint64_t>(info->si_addr);

	uint64_t err = uc->uc_mcontext->__es.__err;
	if ((err & 0x10u) != 0)
	{
		einfo.access_violation_type = ExceptionHandler::AccessViolationType::Execute;
	} else if ((err & 0x2u) != 0)
	{
		einfo.access_violation_type = ExceptionHandler::AccessViolationType::Write;
	} else
	{
		einfo.access_violation_type = ExceptionHandler::AccessViolationType::Read;
	}

	if (getenv("KYTY_FAULT_LOG") != nullptr)
	{
		static int n = 0;
		if (n++ < 60)
		{
			auto& ss = uc->uc_mcontext->__ss;
			sigsafe_fault(einfo.access_violation_type == ExceptionHandler::AccessViolationType::Write ? "FAULTW" : "FAULTR",
			              einfo.access_violation_vaddr, ss.__rip);
			sigsafe_fault("  rdi/rsi", ss.__rdi, ss.__rsi);
			sigsafe_fault("  tid", static_cast<uint64_t>(::syscall(SYS_thread_selfid)), 0);
			// Scan the stack for the nearest fc_script (Kyty HLE) and guest return
			// addresses, to identify which HLE call and which guest instruction led here.
			auto* sp     = reinterpret_cast<uint64_t*>(ss.__rsp);
			int   n_hle  = 0;
			for (int i = 0; i < 512 && n_hle < 5; i++)
			{
				uint64_t v = sp[i];
				if (v >= 0x100000000ull && v < 0x110000000ull)
				{
					sigsafe_fault("  hle-frame", v, static_cast<uint64_t>(i));
					n_hle++;
				}
			}
			sigsafe_fault("  anchor", reinterpret_cast<uint64_t>(&kyty_posix_signal_handler), 0);
		}
	}

	if (ExceptionHandlerPrivate::g_vec_func != nullptr)
	{
		ExceptionHandlerPrivate::g_vec_func(&einfo);
		return;
	}

	// No handler installed: restore default action and re-raise
	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
}

#else
class ExceptionHandlerPrivate
{
};
#endif

ExceptionHandler::ExceptionHandler(): m_p(new ExceptionHandlerPrivate) {}

ExceptionHandler::~ExceptionHandler()
{
#ifdef KYTY_HAS_EXCEPTIONS
	Uninstall();
#endif
	delete m_p;
}

uint64_t ExceptionHandler::GetSize()
{
#ifdef KYTY_HAS_EXCEPTIONS
	return (sizeof(ExceptionHandlerPrivate::HandlerInfo) & ~(static_cast<uint64_t>(0x1000) - 1)) + 0x1000;
#else
	return 0x1000;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static, misc-unused-parameters)
bool ExceptionHandler::Install(uint64_t base_address, uint64_t handler_addr, uint64_t image_size, handler_func_t func)
{
#ifdef KYTY_HAS_EXCEPTIONS
	if (m_p->function_table == nullptr)
	{
		m_p->base_address = base_address;
		m_p->handler_addr = handler_addr;
		m_p->image_size   = image_size;
		m_p->func         = func;

		m_p->InitHandler();

		if (RtlAddFunctionTable(m_p->function_table, 1, base_address) == FALSE)
		{
			printf("RtlAddFunctionTable() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
			return false;
		}

		return true;
	}

	return false;
#else
	return true;
#endif
}

#ifdef KYTY_HAS_EXCEPTIONS
static LONG WINAPI ExceptionFilter(PEXCEPTION_POINTERS exception)
{
	PEXCEPTION_RECORD exception_record = exception->ExceptionRecord;

	ExceptionHandler::ExceptionInfo info {};

	info.exception_address = reinterpret_cast<uint64_t>(exception_record->ExceptionAddress);

	// printf("exception_record->ExceptionCode = %u\n", static_cast<uint32_t>(exception_record->ExceptionCode));

	if (exception_record->ExceptionCode == DBG_PRINTEXCEPTION_C || exception_record->ExceptionCode == DBG_PRINTEXCEPTION_WIDE_C)
	{
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	if (exception_record->ExceptionCode == 0x406D1388)
	{
		// Set a thread name
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
	{
		info.type = ExceptionHandler::ExceptionType::AccessViolation;
		switch (exception_record->ExceptionInformation[0])
		{
			case 0: info.access_violation_type = ExceptionHandler::AccessViolationType::Read; break;
			case 1: info.access_violation_type = ExceptionHandler::AccessViolationType::Write; break;
			case 8: info.access_violation_type = ExceptionHandler::AccessViolationType::Execute; break;
			default: info.access_violation_type = ExceptionHandler::AccessViolationType::Unknown; break;
		}
		info.access_violation_vaddr = exception_record->ExceptionInformation[1];
	}

	info.rbp                = exception->ContextRecord->Rbp;
	info.exception_win_code = exception_record->ExceptionCode;

	ExceptionHandlerPrivate::g_vec_func(&info);

	return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

// NOLINTNEXTLINE(readability-convert-member-functions-to-static, misc-unused-parameters)
bool ExceptionHandler::InstallVectored(handler_func_t func)
{
#ifdef KYTY_HAS_EXCEPTIONS
	if (ExceptionHandlerPrivate::g_vec_func == nullptr)
	{
		ExceptionHandlerPrivate::g_vec_func = func;

		if (AddVectoredExceptionHandler(1, ExceptionFilter) == nullptr)
		{
			printf("AddVectoredExceptionHandler() failed\n");
			return false;
		}

		return true;
	}
	return false;
#elif defined(KYTY_HAS_SIGNAL_EXCEPTIONS)
	if (ExceptionHandlerPrivate::g_vec_func == nullptr)
	{
		ExceptionHandlerPrivate::g_vec_func = func;

		struct sigaction sa {};
		sa.sa_sigaction = kyty_posix_signal_handler;
		sa.sa_flags     = SA_SIGINFO;
		sigemptyset(&sa.sa_mask);

		if (sigaction(SIGSEGV, &sa, nullptr) != 0 || sigaction(SIGBUS, &sa, nullptr) != 0 || sigaction(SIGILL, &sa, nullptr) != 0)
		{
			printf("sigaction() failed\n");
			return false;
		}

		struct sigaction sat {};
		sat.sa_sigaction = kyty_sigtrap_handler;
		sat.sa_flags     = SA_SIGINFO;
		sigemptyset(&sat.sa_mask);
		sigaction(SIGTRAP, &sat, nullptr);

		return true;
	}
	return false;
#else
	return true;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static, misc-unused-parameters)
bool ExceptionHandler::Uninstall()
{
#ifdef KYTY_HAS_EXCEPTIONS
	if (m_p->function_table != nullptr)
	{
		if (RtlDeleteFunctionTable(m_p->function_table) == FALSE)
		{
			printf("RtlDeleteFunctionTable() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
			return false;
		}
		m_p->function_table = nullptr;
		return true;
	}

	return false;
#else
	return true;
#endif
}

void Init()
{
	sys_virtual_init();
}

uint64_t Alloc(uint64_t address, uint64_t size, Mode mode)
{
	return sys_virtual_alloc(address, size, mode);
}

uint64_t AllocAligned(uint64_t address, uint64_t size, Mode mode, uint64_t alignment)
{
	// An alignment of zero means that the caller does not request an additional
	// constraint. Keep that contract portable instead of passing zero to the
	// platform-specific aligned allocator, where it is invalid.
	if (alignment == 0)
	{
		return Alloc(address, size, mode);
	}
	return sys_virtual_alloc_aligned(address, size, mode, alignment);
}

bool AllocFixed(uint64_t address, uint64_t size, Mode mode)
{
	return sys_virtual_alloc_fixed(address, size, mode);
}

bool Free(uint64_t address)
{
	return sys_virtual_free(address);
}

bool Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode)
{
	return sys_virtual_protect(address, size, mode, old_mode);
}

bool FlushInstructionCache(uint64_t address, uint64_t size)
{
	return sys_virtual_flush_instruction_cache(address, size);
}

bool PatchReplace(uint64_t vaddr, uint64_t value)
{
	return sys_virtual_patch_replace(vaddr, value);
}

} // namespace VirtualMemory

} // namespace Kyty::Core
