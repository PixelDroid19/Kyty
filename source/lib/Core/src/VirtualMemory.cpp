// glibc exposes REG_* gregset indices only under feature-test macros.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "Kyty/Core/VirtualMemory.h"

#include "Kyty/Sys/SysVirtual.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define KYTY_HAS_EXCEPTIONS
#elif defined(__APPLE__) || KYTY_PLATFORM == KYTY_PLATFORM_LINUX
// POSIX SIGSEGV/SIGBUS path for flexible-memory demand paging and GPU watches.
#define KYTY_HAS_SIGNAL_EXCEPTIONS
#endif

#ifdef KYTY_HAS_EXCEPTIONS
#include <windows.h> // IWYU pragma: keep
#endif

#ifdef KYTY_HAS_SIGNAL_EXCEPTIONS
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__)) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 1
#endif
#include <csignal>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ucontext.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
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

SignalDiagnosticsConfig MakeSignalDiagnosticsConfig(const char* skip_ud2, const char* fault_log) noexcept
{
	SignalDiagnosticsConfig config {};
	config.skip_ud2 = skip_ud2 != nullptr;
	config.fault_log = fault_log != nullptr;
	return config;
}

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

static volatile sig_atomic_t g_signal_skip_ud2  = 0;
static volatile sig_atomic_t g_signal_fault_log = 0;
static volatile sig_atomic_t g_signal_extrq_reported = 0;

static void LoadSignalDiagnosticsConfigFromEnvironment() noexcept
{
	const auto config = MakeSignalDiagnosticsConfig(getenv("KYTY_SKIP_UD2"), getenv("KYTY_FAULT_LOG"));
	g_signal_skip_ud2  = config.skip_ud2 ? 1 : 0;
	g_signal_fault_log = config.fault_log ? 1 : 0;
}

// Platform register accessors for POSIX ucontext. The exception contract keeps
// the historical x86 register names because the guest ABI is x86_64; native
// arm64 builds expose the corresponding program counter, frame/stack pointers,
// and first ABI argument registers for host diagnostics.
#if defined(__APPLE__)
// Signal handlers cannot call the normal thread API: it may touch TLS or take
// locks. Keep this direct syscall at the signal boundary for an async-signal-safe
// native thread identifier.
#if defined(__arm64__) || defined(__aarch64__)
static inline uint64_t uc_get_rip(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__pc); }
static inline uint64_t uc_get_rbp(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__fp); }
static inline uint64_t uc_get_rsp(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__sp); }
static inline uint64_t uc_get_rax(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[0]); }
static inline uint64_t uc_get_rbx(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[1]); }
static inline uint64_t uc_get_rcx(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[2]); }
static inline uint64_t uc_get_rdx(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[3]); }
static inline uint64_t uc_get_rsi(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[1]); }
static inline uint64_t uc_get_rdi(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[0]); }
static inline uint64_t uc_get_r8(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[4]); }
static inline uint64_t uc_get_r9(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[5]); }
static inline uint64_t uc_get_r10(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[6]); }
static inline uint64_t uc_get_r11(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[7]); }
static inline uint64_t uc_get_r12(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[8]); }
static inline uint64_t uc_get_r13(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[9]); }
static inline uint64_t uc_get_r14(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[10]); }
static inline uint64_t uc_get_r15(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__x[11]); }
// Darwin arm64 signal contexts do not expose an x86 page-fault error code.
// The signal info still supplies the fault address; classify it as a read when
// no architecture-specific access bit is available.
static inline uint64_t uc_get_err(ucontext_t* /*uc*/) { return 0; }
static inline long     host_tid() { return ::syscall(SYS_thread_selfid); }
#else
static inline uint64_t uc_get_rip(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rip); }
static inline void     uc_set_rip(ucontext_t* uc, uint64_t v) { uc->uc_mcontext->__ss.__rip = static_cast<__uint64_t>(v); }
static inline uint64_t uc_get_rbp(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rbp); }
static inline uint64_t uc_get_rsp(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rsp); }
static inline uint64_t uc_get_rax(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rax); }
static inline uint64_t uc_get_rbx(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rbx); }
static inline uint64_t uc_get_rcx(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rcx); }
static inline uint64_t uc_get_rdx(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rdx); }
static inline uint64_t uc_get_rsi(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rsi); }
static inline uint64_t uc_get_rdi(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rdi); }
static inline uint64_t uc_get_r8(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__r8); }
static inline uint64_t uc_get_r9(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__r9); }
static inline uint64_t uc_get_r10(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__r10); }
static inline uint64_t uc_get_r11(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__r11); }
static inline uint64_t uc_get_r12(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__r12); }
static inline uint64_t uc_get_r13(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__r13); }
static inline uint64_t uc_get_r14(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__r14); }
static inline uint64_t uc_get_r15(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__r15); }
static inline uint64_t uc_get_err(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__es.__err); }
static inline uint64_t uc_get_rflags(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext->__ss.__rflags); }
static inline void     uc_set_rflags(ucontext_t* uc, uint64_t v) { uc->uc_mcontext->__ss.__rflags = static_cast<__uint64_t>(v); }

static char* uc_get_xmm_bytes(ucontext_t* uc, uint32_t index)
{
	switch (index)
	{
		case 0: return uc->uc_mcontext->__fs.__fpu_xmm0.__xmm_reg;
		case 1: return uc->uc_mcontext->__fs.__fpu_xmm1.__xmm_reg;
		case 2: return uc->uc_mcontext->__fs.__fpu_xmm2.__xmm_reg;
		case 3: return uc->uc_mcontext->__fs.__fpu_xmm3.__xmm_reg;
		case 4: return uc->uc_mcontext->__fs.__fpu_xmm4.__xmm_reg;
		case 5: return uc->uc_mcontext->__fs.__fpu_xmm5.__xmm_reg;
		case 6: return uc->uc_mcontext->__fs.__fpu_xmm6.__xmm_reg;
		case 7: return uc->uc_mcontext->__fs.__fpu_xmm7.__xmm_reg;
		default: return nullptr;
	}
}

static uint64_t load_signal_u64(const char* bytes)
{
	uint64_t value = 0;
	for (uint32_t i = 0; i < sizeof(uint64_t); i++)
	{
		value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i])) << (i * 8u);
	}
	return value;
}

static void store_signal_u64(char* bytes, uint64_t value)
{
	for (uint32_t i = 0; i < sizeof(uint64_t); i++)
	{
		bytes[i] = static_cast<char>(value >> (i * 8u));
	}
	for (uint32_t i = sizeof(uint64_t); i < 16; i++)
	{
		bytes[i] = 0;
	}
}

static bool try_emulate_guest_extrq(ucontext_t* uc)
{
	const uint64_t rip  = uc_get_rip(uc);
	const auto*    code = reinterpret_cast<const uint8_t*>(rip);

	// EXTRQ xmm, xmm, imm8, imm8: 66 0f 78 /0 ib ib. The guest reaches this
	// SSE4a instruction under Rosetta, which does not execute it.
	if (code[0] != 0x66u || code[1] != 0x0fu || code[2] != 0x78u)
	{
		return false;
	}

	const uint8_t modrm = code[3];
	if ((modrm & 0xc0u) != 0xc0u || (modrm & 0x38u) != 0)
	{
		return false;
	}

	// The immediate form is 66 0f 78 /0 ib ib: ModRM.reg is the fixed /0
	// extension and ModRM.r/m is the sole, in-place XMM operand.
	char* operand = uc_get_xmm_bytes(uc, modrm & 0x7u);
	if (operand == nullptr)
	{
		return false;
	}

	uint64_t length = code[4] & 0x3fu;
	const auto index = static_cast<uint64_t>(code[5] & 0x3fu);
	if (length == 0)
	{
		if (index != 0)
		{
			return false;
		}
		length = 64;
	}
	if (index + length > 64)
	{
		return false;
	}

	uint64_t value = load_signal_u64(operand) >> index;
	if (length < 64)
	{
		value &= (UINT64_C(1) << length) - 1;
	}
	store_signal_u64(operand, value);
	uc_set_rip(uc, rip + 6);
	return true;
}

static inline long     host_tid() { return ::syscall(SYS_thread_selfid); }
#endif
#elif defined(__linux__)
static inline uint64_t uc_get_rip(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]); }
static inline void     uc_set_rip(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(v); }
static inline uint64_t uc_get_rbp(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBP]); }
static inline uint64_t uc_get_rsp(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSP]); }
static inline uint64_t uc_get_rax(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RAX]); }
static inline uint64_t uc_get_rbx(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBX]); }
static inline uint64_t uc_get_rcx(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RCX]); }
static inline uint64_t uc_get_rdx(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RDX]); }
static inline uint64_t uc_get_rsi(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSI]); }
static inline uint64_t uc_get_rdi(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RDI]); }
static inline uint64_t uc_get_r8(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R8]); }
static inline uint64_t uc_get_r9(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R9]); }
static inline uint64_t uc_get_r10(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R10]); }
static inline uint64_t uc_get_r11(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R11]); }
static inline uint64_t uc_get_r12(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R12]); }
static inline uint64_t uc_get_r13(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R13]); }
static inline uint64_t uc_get_r14(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R14]); }
static inline uint64_t uc_get_r15(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R15]); }
static inline uint64_t uc_get_err(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_ERR]); }
static inline uint64_t uc_get_rflags(ucontext_t* uc) { return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_EFL]); }
static inline void     uc_set_rflags(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_EFL] = static_cast<greg_t>(v); }
static inline long     host_tid() { return ::syscall(SYS_gettid); }
#else
#error "KYTY_HAS_SIGNAL_EXCEPTIONS requires Apple or Linux ucontext accessors"
#endif

// Single-step tracer: TF-based instruction trace of guest code. Armed by
// SetGuestTrace(); logs guest-range instruction pointers until the budget runs out.
#if defined(__x86_64__) || defined(__i386__)
static thread_local int g_trace_guest = 0; // remaining guest instructions to log
static thread_local int g_trace_total = 0; // hard cap on total single-steps
#endif

// Async-signal-safe fatal report used by all POSIX diagnostic handlers.
static void sigsafe_fault(const char* tag, uint64_t a, uint64_t b);

void SetGuestTrace(int steps)
{
#if defined(__x86_64__) || defined(__i386__)
	g_trace_guest = steps;
	g_trace_total = steps * 200; // allow stepping through HLE/return code in between
#else
	(void)steps;
#endif
}

// Timer-based guest profiler: samples the instruction pointer of
// whatever thread is running when SIGPROF fires. Unlike TF single-step it survives
// guest popfq/pushf, so it can locate a spinning guest loop. Gated by StartGuestProfiler().
static void kyty_sigprof_handler(int /*sig*/, siginfo_t* /*info*/, void* ucontext)
{
	auto*    uc  = static_cast<ucontext_t*>(ucontext);
	uint64_t rip = uc_get_rip(uc);
	static volatile sig_atomic_t n = 0;
	if (n++ < 200)
	{
		const char* tag = (rip >= 0x900000000ull && rip < 0x920000000ull) ? "PROF-GUEST"
		                  : (rip >= 0x100000000ull && rip < 0x110000000ull) ? "PROF-FC"
		                                                                    : "PROF-OTHER";
		sigsafe_fault(tag, rip, 0);
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

#if defined(__x86_64__) || defined(__i386__)
static void kyty_sigtrap_handler(int /*sig*/, siginfo_t* /*info*/, void* ucontext)
{
	auto*    uc  = static_cast<ucontext_t*>(ucontext);
	uint64_t rip = uc_get_rip(uc);
	if (g_trace_guest <= 0 || g_trace_total <= 0)
	{
		uc_set_rflags(uc, uc_get_rflags(uc) & ~0x100ull);
		return;
	}
	g_trace_total--;
	if (rip >= 0x900000000ull && rip < 0x920000000ull) // any guest module
	{
		sigsafe_fault("TRACE", rip, 0);
		g_trace_guest--;
	}
	uc_set_rflags(uc, uc_get_rflags(uc) | 0x100ull);
}
#endif

// Demand-paged ranges: flexible-memory regions the guest reserves but that
// are backed lazily. On a write fault inside one, the touched page is mapped RW
// (zero-filled) and the faulting instruction retried — mirroring PS5 flexible memory.
struct DemandRange
{
	uint64_t addr = 0;
	uint64_t size = 0;
};
static DemandRange g_demand_ranges[64];
static int         g_demand_count = 0;
// The demand mapper runs from a signal handler, so it cannot query the host
// page size there. Populate this before the first guest write fault.
static uint64_t g_demand_page_size = 0;

static void EnsureDemandPageSize() noexcept
{
	if (g_demand_page_size == 0u)
	{
		const uint64_t page_size = sys_virtual_get_page_size();
		if (page_size != 0u && (page_size & (page_size - 1u)) == 0u)
		{
			g_demand_page_size = page_size;
		}
	}
}

void RegisterDemandRange(uint64_t addr, uint64_t size)
{
	EnsureDemandPageSize();
	if (g_demand_count < 64)
	{
		g_demand_ranges[g_demand_count].addr = addr;
		g_demand_ranges[g_demand_count].size = size;
		g_demand_count++;
	}
}

static bool try_demand_map(uint64_t vaddr)
{
	const uint64_t page_size = g_demand_page_size;
	if (page_size == 0u)
	{
		return false;
	}

	for (int i = 0; i < g_demand_count; i++)
	{
		if (vaddr >= g_demand_ranges[i].addr && vaddr < g_demand_ranges[i].addr + g_demand_ranges[i].size)
		{
			// Raw mmap only: this runs in a signal handler, so it must avoid the
			// allocator bookkeeping (std::map insert -> malloc) that would dead-lock.
			const uint64_t page = vaddr - (vaddr % page_size);
			void*         p    = mmap(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_WRITE,
			                           MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
			return p == reinterpret_cast<void*>(page);
		}
	}
	return false;
}

bool TryDemandMap(uint64_t vaddr)
{
	return try_demand_map(vaddr);
}

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
// Guest soft-debug `int $0x41` is NOP'd at load time (LoaderPatchGuestSoftDebugInterrupts);
// do not peek guest RIP here — LOAD code can be X-only and re-fault in the handler.
static void kyty_posix_signal_handler(int sig, siginfo_t* info, void* ucontext)
{
	auto* uc = static_cast<ucontext_t*>(ucontext);

	if (sig == SIGILL)
	{
		uint64_t rip = uc_get_rip(uc);
	#if defined(__x86_64__) || defined(__i386__)
		if (rip >= 0x900000000ull && rip < 0x920000000ull)
		{
		#if defined(__APPLE__)
			if (try_emulate_guest_extrq(uc))
			{
				if (g_signal_extrq_reported == 0)
				{
					g_signal_extrq_reported = 1;
					sigsafe_fault("EMULATE-EXTRQ", rip, 0);
				}
				return;
			}
		#endif
			const auto* code = reinterpret_cast<const uint8_t*>(rip);
			sigsafe_fault("ILL-CODE", static_cast<uint64_t>(code[0]), static_cast<uint64_t>(code[1]));
			sigsafe_fault("ILL-CODE2", static_cast<uint64_t>(code[2]), static_cast<uint64_t>(code[3]));
			sigsafe_fault("ILL-CODE3", static_cast<uint64_t>(code[4]), static_cast<uint64_t>(code[5]));
			sigsafe_fault("ILL-CODE4", static_cast<uint64_t>(code[6]), static_cast<uint64_t>(code[7]));
		}
		// A guest ud2 (0F 0B) is the trap the compiler emits after a call it believes
		// is noreturn — here, sceKernelDebugRaiseException. On real hardware certain
		// debug-raise codes are soft: the kernel logs and RESUMES past the trap. When
		// KYTY_SKIP_UD2 is set, emulate that: skip the ud2 in guest code and continue,
		// to determine whether the raised condition is actually recoverable.
		if (g_signal_skip_ud2 != 0 && rip >= 0x900000000ull && rip < 0x920000000ull)
		{
			const auto* code = reinterpret_cast<const uint8_t*>(rip);
			if (code[0] == 0x0F && code[1] == 0x0B)
			{
				sigsafe_fault("SKIP-UD2 @rip", rip, 0);
				uc_set_rip(uc, rip + 2);
				return;
			}
		}
	#endif
		sigsafe_fault("SIGILL", rip, 0);
		sigsafe_fault("ILL-RAXRBX", uc_get_rax(uc), uc_get_rbx(uc));
		sigsafe_fault("ILL-RCXRDX", uc_get_rcx(uc), uc_get_rdx(uc));
		sigsafe_fault("ILL-RSIRDI", uc_get_rsi(uc), uc_get_rdi(uc));
		sigsafe_fault("ILL-RBPRSP", uc_get_rbp(uc), uc_get_rsp(uc));
		sigsafe_fault("ILL-R8R9", uc_get_r8(uc), uc_get_r9(uc));
		sigsafe_fault("ILL-R10R11", uc_get_r10(uc), uc_get_r11(uc));
		sigsafe_fault("ILL-R12R13", uc_get_r12(uc), uc_get_r13(uc));
		sigsafe_fault("ILL-R14R15", uc_get_r14(uc), uc_get_r15(uc));
		// Returning would retry the same unsupported guest instruction forever.
		// Keep strict runs bounded and preserve the first-failure evidence.
		::_Exit(132);
	}

	ExceptionHandler::ExceptionInfo einfo {};

	einfo.type              = ExceptionHandler::ExceptionType::AccessViolation;
	einfo.exception_address = uc_get_rip(uc);
	einfo.rbp               = uc_get_rbp(uc);

	einfo.access_violation_vaddr = reinterpret_cast<uint64_t>(info->si_addr);

	uint64_t err = uc_get_err(uc);
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

	if (g_signal_fault_log != 0)
	{
		static volatile sig_atomic_t n = 0;
		if (n++ < 60)
		{
			sigsafe_fault(einfo.access_violation_type == ExceptionHandler::AccessViolationType::Write ? "FAULTW" : "FAULTR",
			              einfo.access_violation_vaddr, uc_get_rip(uc));
			sigsafe_fault("  rdi/rsi", uc_get_rdi(uc), uc_get_rsi(uc));
			// Null-page faults (addr < 0x1000) are almost always base-register + small
			// displacement. Dump rax/rbx so producers like *(null+8) are identifiable
			// without a debugger. Diagnostic only (KYTY_FAULT_LOG=1).
			sigsafe_fault("  rax/rbx", uc_get_rax(uc), uc_get_rbx(uc));
			sigsafe_fault("  rcx/rdx", uc_get_rcx(uc), uc_get_rdx(uc));
			sigsafe_fault("  rbp/rsp", uc_get_rbp(uc), uc_get_rsp(uc));
			sigsafe_fault("  r8/r9", uc_get_r8(uc), uc_get_r9(uc));
			sigsafe_fault("  r10/r11", uc_get_r10(uc), uc_get_r11(uc));
			sigsafe_fault("  r12/r13", uc_get_r12(uc), uc_get_r13(uc));
			sigsafe_fault("  r14/r15", uc_get_r14(uc), uc_get_r15(uc));
			const uint64_t rbp = uc_get_rbp(uc);
			if (rbp >= 0x1000ull)
			{
				const auto* frame = reinterpret_cast<const uint64_t*>(rbp);
				for (int depth = 0; depth < 4; depth++)
				{
					const uint64_t next = frame[0];
					sigsafe_fault("  frame", next, frame[1]);
					if (next <= reinterpret_cast<uint64_t>(frame) || next - reinterpret_cast<uint64_t>(frame) > 0x10000ull)
					{
						break;
					}
					frame = reinterpret_cast<const uint64_t*>(next);
				}
			}
			sigsafe_fault("  tid", static_cast<uint64_t>(host_tid()), 0);
			sigsafe_fault("  anchor", reinterpret_cast<uint64_t>(&kyty_posix_signal_handler), 0);
			// Captured Gen5 RBP walkers do mov (%rdx),%rdx; mov 0x8(%rdx),… and
			// FAULTR at 0x8 when the parent frame pointer is already null. Tag only;
			// does not change handling.
			if (einfo.access_violation_vaddr < 0x1000ull && uc_get_rdx(uc) == 0)
			{
				sigsafe_fault("  class", 0x8, uc_get_rbp(uc));
			}
		}
	}

	if (ExceptionHandlerPrivate::g_vec_func != nullptr)
	{
		ExceptionHandlerPrivate::g_vec_func(&einfo);
		return;
	}

	// No handler installed: report and terminate explicitly. Calling signal() here
	// would invoke a non-async-signal-safe libc path while the faulting thread is
	// already inside the signal machinery.
	sigsafe_fault("NO-HANDLER", einfo.access_violation_vaddr, einfo.exception_address);
	::_Exit(139);
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
		LoadSignalDiagnosticsConfigFromEnvironment();
		ExceptionHandlerPrivate::g_vec_func = func;

		struct sigaction sa {};
		sa.sa_sigaction = kyty_posix_signal_handler;
		sa.sa_flags     = SA_SIGINFO;
		sigemptyset(&sa.sa_mask);

		struct sigaction sigill = sa;
		sigill.sa_flags |= SA_RESETHAND;

		if (sigaction(SIGSEGV, &sa, nullptr) != 0 || sigaction(SIGBUS, &sa, nullptr) != 0 || sigaction(SIGILL, &sigill, nullptr) != 0)
		{
			printf("sigaction() failed\n");
			return false;
		}

#if defined(__x86_64__) || defined(__i386__)
		struct sigaction sat {};
		sat.sa_sigaction = kyty_sigtrap_handler;
		sat.sa_flags     = SA_SIGINFO;
		sigemptyset(&sat.sa_mask);
		sigaction(SIGTRAP, &sat, nullptr);
#endif

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
#ifdef KYTY_HAS_SIGNAL_EXCEPTIONS
	LoadSignalDiagnosticsConfigFromEnvironment();
#endif
	sys_virtual_init();
	EnsureDemandPageSize();
}

uint64_t GetPageSize()
{
	return sys_virtual_get_page_size();
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

class SharedBacking
{
public:
	void*    handle = nullptr;
	uint64_t size   = 0;
};

SharedBacking* CreateSharedBacking(uint64_t size)
{
	if (size == 0)
	{
		return nullptr;
	}

	auto* backing  = new SharedBacking;
	backing->handle = sys_virtual_create_shared_backing(size);
	backing->size   = size;
	if (backing->handle == nullptr)
	{
		delete backing;
		return nullptr;
	}
	return backing;
}

void DestroySharedBacking(SharedBacking* backing)
{
	if (backing != nullptr)
	{
		sys_virtual_destroy_shared_backing(backing->handle);
		delete backing;
	}
}

static bool shared_range_is_valid(const SharedBacking* backing, uint64_t backing_offset, uint64_t size)
{
	return backing != nullptr && backing->handle != nullptr && size != 0 && backing_offset <= backing->size &&
	       size <= backing->size - backing_offset;
}

bool DiscardSharedBackingRange(SharedBacking* backing, uint64_t backing_offset, uint64_t size)
{
	if (!shared_range_is_valid(backing, backing_offset, size))
	{
		return false;
	}
	return sys_virtual_discard_shared_backing_range(backing->handle, backing_offset, size);
}

uint64_t MapSharedAligned(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size, Mode mode,
                          uint64_t alignment)
{
	if (!shared_range_is_valid(backing, backing_offset, size) || alignment == 0 || (alignment & (alignment - 1)) != 0)
	{
		return 0;
	}
	return sys_virtual_map_shared_aligned(backing->handle, address, backing_offset, size, mode, alignment);
}

bool MapSharedFixed(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size, Mode mode)
{
	if (address == 0 || !shared_range_is_valid(backing, backing_offset, size))
	{
		return false;
	}
	return sys_virtual_map_shared_fixed(backing->handle, address, backing_offset, size, mode);
}

uint64_t MapSharedFixedOrRelocated(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size, Mode mode,
                                   uint64_t alignment)
{
	if (address == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
	    !shared_range_is_valid(backing, backing_offset, size))
	{
		return 0;
	}
	return sys_virtual_map_shared_fixed_or_relocated(backing->handle, address, backing_offset, size, mode, alignment);
}

bool Free(uint64_t address)
{
	return sys_virtual_free(address);
}

bool Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode)
{
	return sys_virtual_protect(address, size, mode, old_mode);
}

bool ProtectWriteSignalSafe(uint64_t address, uint64_t size)
{
	return sys_virtual_protect_write_signal_safe(address, size);
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
