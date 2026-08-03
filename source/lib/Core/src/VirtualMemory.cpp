// glibc exposes REG_* gregset indices only under feature-test macros.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

// macOS hides the deprecated ucontext API unless this feature-test macro is
// visible before any system header is included.
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 1
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

#include <cstdlib>

#ifdef KYTY_HAS_SIGNAL_EXCEPTIONS
#include <csignal>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
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

namespace {

constexpr size_t kCrashReportPathMax = 512;
char             g_crash_report_path[kCrashReportPathMax] = {};
size_t           g_crash_report_path_size                = 0;

void AppendText(char* buffer, size_t capacity, size_t* offset, const char* text) noexcept
{
	if (buffer == nullptr || offset == nullptr || text == nullptr)
	{
		return;
	}
	while (*text != '\0' && *offset + 1 < capacity)
	{
		buffer[(*offset)++] = *text++;
	}
}

void AppendHex(char* buffer, size_t capacity, size_t* offset, uint64_t value) noexcept
{
	static constexpr char kHex[] = "0123456789abcdef";
	AppendText(buffer, capacity, offset, "0x");
	for (int shift = 60; shift >= 0 && *offset + 1 < capacity; shift -= 4)
	{
		buffer[(*offset)++] = kHex[(value >> shift) & 0xfu];
	}
}

void AppendHexJson(char* buffer, size_t capacity, size_t* offset, uint64_t value) noexcept
{
	AppendText(buffer, capacity, offset, "\"");
	AppendHex(buffer, capacity, offset, value);
	AppendText(buffer, capacity, offset, "\"");
}

const char* AccessName(ExceptionHandler::AccessViolationType type) noexcept
{
	switch (type)
	{
		case ExceptionHandler::AccessViolationType::Read: return "read";
		case ExceptionHandler::AccessViolationType::Write: return "write";
		case ExceptionHandler::AccessViolationType::Execute: return "execute";
		default: return "unknown";
	}
}

void WriteCrashReport(const ExceptionHandler::ExceptionInfo& info) noexcept
{
	if (g_crash_report_path_size == 0)
	{
		return;
	}
	char   json[4096] = {};
	size_t size       = 0;
	AppendText(json, sizeof(json), &size, "{\"schema\":\"kyty_crash_context_v1\",\"type\":\"");
	AppendText(json, sizeof(json), &size,
	           info.type == ExceptionHandler::ExceptionType::AccessViolation ? "access_violation" : "unknown");
	AppendText(json, sizeof(json), &size, "\",\"access\":\"");
	AppendText(json, sizeof(json), &size, AccessName(info.access_violation_type));
	AppendText(json, sizeof(json), &size, "\",\"exception_code\":");
	AppendHexJson(json, sizeof(json), &size, info.exception_win_code);
	AppendText(json, sizeof(json), &size, ",\"fault_address\":");
	AppendHexJson(json, sizeof(json), &size, info.access_violation_vaddr);
	AppendText(json, sizeof(json), &size, ",\"rip\":");
	AppendHexJson(json, sizeof(json), &size, info.exception_address);
	AppendText(json, sizeof(json), &size, ",\"registers\":{");
	static constexpr const char* names[] = {"rsp", "rbp", "rflags", "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
	                                        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
	const uint64_t values[] = {info.rsp, info.rbp, info.rflags, info.rax, info.rbx, info.rcx, info.rdx, info.rsi, info.rdi,
	                           info.r8, info.r9, info.r10, info.r11, info.r12, info.r13, info.r14, info.r15};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
	{
		if (i != 0)
		{
			AppendText(json, sizeof(json), &size, ",");
		}
		AppendText(json, sizeof(json), &size, "\"");
		AppendText(json, sizeof(json), &size, names[i]);
		AppendText(json, sizeof(json), &size, "\":");
		AppendHexJson(json, sizeof(json), &size, values[i]);
	}
	AppendText(json, sizeof(json), &size, "},\"stack\":[");
	const uint32_t stack_count = info.stack_count > 16u ? 16u : info.stack_count;
	for (uint32_t i = 0; i < stack_count; ++i)
	{
		if (i != 0u)
		{
			AppendText(json, sizeof(json), &size, ",");
		}
		AppendHexJson(json, sizeof(json), &size, info.stack[i]);
	}
	AppendText(json, sizeof(json), &size, "]}\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	HANDLE file = CreateFileA(g_crash_report_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		(void)WriteFile(file, json, static_cast<DWORD>(size), &written, nullptr);
		(void)CloseHandle(file);
	}
#else
	const int fd = ::open(g_crash_report_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0)
	{
		(void)::write(fd, json, size);
		(void)::close(fd);
	}
#endif
}

} // namespace

void ConfigureFatalFaultReport(const char* path) noexcept
{
	g_crash_report_path_size = 0;
	if (path == nullptr || path[0] == '\0')
	{
		return;
	}
	while (path[g_crash_report_path_size] != '\0' && g_crash_report_path_size + 1 < kCrashReportPathMax)
	{
		g_crash_report_path[g_crash_report_path_size] = path[g_crash_report_path_size];
		++g_crash_report_path_size;
	}
	g_crash_report_path[g_crash_report_path_size] = '\0';
}

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

		const PCONTEXT context = dispatcher_context->ContextRecord;
		info.rbp               = context->Rbp;
		info.rsp               = context->Rsp;
		info.rflags            = context->EFlags;
		info.rax               = context->Rax;
		info.rbx               = context->Rbx;
		info.rcx               = context->Rcx;
		info.rdx               = context->Rdx;
		info.rsi               = context->Rsi;
		info.rdi               = context->Rdi;
		info.r8                = context->R8;
		info.r9                = context->R9;
		info.r10               = context->R10;
		info.r11               = context->R11;
		info.r12               = context->R12;
		info.r13               = context->R13;
		info.r14               = context->R14;
		info.r15               = context->R15;
		if (context->Rsp >= 0x1000u)
		{
			const auto* stack = reinterpret_cast<const uint64_t*>(context->Rsp);
			for (uint32_t i = 0; i < 16u; ++i)
			{
				info.stack[i] = stack[i];
			}
			info.stack_count = 16;
		}
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

// Guest ELF images are placed at 0x900000000 + n*0x10000000 (see RuntimeLinker
// CODE_BASE_OFFSET / CODE_BASE_INCR). Adjacent PRX loads routinely land at
// 0x980000000 and beyond; the old 0x900..0x920 window missed those modules, so
// SIGILL/ud2 diagnostics never dumped bytes and KYTY_SKIP_UD2 never applied.
static constexpr uint64_t kGuestCodeAddrBegin = 0x900000000ull;
static constexpr uint64_t kGuestCodeAddrEnd   = 0xB00000000ull;

static inline bool IsGuestCodeAddress(uint64_t rip) noexcept
{
	return rip >= kGuestCodeAddrBegin && rip < kGuestCodeAddrEnd;
}

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
static inline uint64_t uc_get_rflags(ucontext_t* /*uc*/) { return 0; }
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
static inline void     uc_set_rax(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_RAX] = static_cast<greg_t>(v); }
static inline void     uc_set_rbx(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_RBX] = static_cast<greg_t>(v); }
static inline void     uc_set_rcx(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_RCX] = static_cast<greg_t>(v); }
static inline void     uc_set_rdx(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_RDX] = static_cast<greg_t>(v); }
static inline void     uc_set_rsi(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_RSI] = static_cast<greg_t>(v); }
static inline void     uc_set_rdi(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_RDI] = static_cast<greg_t>(v); }
static inline void     uc_set_rbp(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_RBP] = static_cast<greg_t>(v); }
static inline void     uc_set_r8(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_R8] = static_cast<greg_t>(v); }
static inline void     uc_set_r9(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_R9] = static_cast<greg_t>(v); }
static inline void     uc_set_r10(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_R10] = static_cast<greg_t>(v); }
static inline void     uc_set_r11(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_R11] = static_cast<greg_t>(v); }
static inline void     uc_set_r12(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_R12] = static_cast<greg_t>(v); }
static inline void     uc_set_r13(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_R13] = static_cast<greg_t>(v); }
static inline void     uc_set_r14(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_R14] = static_cast<greg_t>(v); }
static inline void     uc_set_r15(ucontext_t* uc, uint64_t v) { uc->uc_mcontext.gregs[REG_R15] = static_cast<greg_t>(v); }
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
		const char* tag = IsGuestCodeAddress(rip)                              ? "PROF-GUEST"
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
	if (IsGuestCodeAddress(rip)) // any guest module
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
// ---------------------------------------------------------------------------
// Minimal x86-64 instruction length decoder (signal-safe, no allocations).
// Returns instruction length in bytes, or 0 if the instruction cannot be
// decoded. Used to skip null-page data faults that cannot be backed by mmap
// when vm.mmap_min_addr prevents mapping pages below 64 KiB.
// ---------------------------------------------------------------------------
#if defined(__x86_64__) || defined(__i386__)

// Opcode table encoding: bit0 = has ModRM, bits 4..7 = immediate size in bytes.
// 0xFF = invalid/unknown opcode (decoder bails out).
static constexpr uint8_t kModRM = 0x01;

// One-byte opcode map (256 entries).
// Encoding: (imm_size << 4) | has_modrm
//   imm_size: 0=none, 1=imm8, 2=imm16, 3=imm16+imm8(ENTER), 4=imm32, 8=imm64/moffs
// clang-format off
static constexpr uint8_t kOpcodeMap1[256] = {
	/* 00-03 ADD r/m,r  */ 0x11,0x11,0x11,0x11,
	/* 04-05 ADD AL,r   */ 0x10,0x40,
	/* 06-07 invalid64  */ 0xFF,0xFF,
	/* 08-0B OR         */ 0x11,0x11,0x11,0x11,
	/* 0C-0D OR AL,r    */ 0x10,0x40,
	/* 0E-0F invalid/2byte */ 0xFF,0xFF,
	/* 10-13 ADC        */ 0x11,0x11,0x11,0x11,
	/* 14-15 ADC AL,r   */ 0x10,0x40,
	/* 16-17 invalid64  */ 0xFF,0xFF,
	/* 18-1B SBB        */ 0x11,0x11,0x11,0x11,
	/* 1C-1D SBB AL,r   */ 0x10,0x40,
	/* 1E-1F invalid64  */ 0xFF,0xFF,
	/* 20-23 AND        */ 0x11,0x11,0x11,0x11,
	/* 24-25 AND AL,r   */ 0x10,0x40,
	/* 26-27 prefix/inv */ 0xFF,0xFF,
	/* 28-2B SUB        */ 0x11,0x11,0x11,0x11,
	/* 2C-2D SUB AL,r   */ 0x10,0x40,
	/* 2E-2F prefix/inv */ 0xFF,0xFF,
	/* 30-33 XOR        */ 0x11,0x11,0x11,0x11,
	/* 34-35 XOR AL,r   */ 0x10,0x40,
	/* 36-37 prefix/inv */ 0xFF,0xFF,
	/* 38-3B CMP        */ 0x11,0x11,0x11,0x11,
	/* 3C-3D CMP AL,r   */ 0x10,0x40,
	/* 3E-3F prefix/inv */ 0xFF,0xFF,
	/* 40-4F REX (64-bit) */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
	                         0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
	/* 50-57 PUSH reg   */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	/* 58-5F POP reg    */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	/* 60-62 invalid64  */ 0xFF,0xFF,0xFF,
	/* 63 MOVSXD        */ 0x01,
	/* 64-67 prefix     */ 0xFF,0xFF,0xFF,0xFF,
	/* 68 PUSH imm32    */ 0x40,
	/* 69 IMUL r,r/m,imm32 */ 0x41,
	/* 6A PUSH imm8     */ 0x10,
	/* 6B IMUL r,r/m,imm8  */ 0x11,
	/* 6C-6F INS/OUTS   */ 0x00,0x00,0x00,0x00,
	/* 70-7F Jcc rel8   */ 0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
	                         0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
	/* 80 group1 r/m,imm8  */ 0x11,
	/* 81 group1 r/m,imm32 */ 0x41,
	/* 82 group1 r/m,imm8 (alias) */ 0x11,
	/* 83 group1 r/m,imm8se */ 0x11,
	/* 84-85 TEST r/m,r */ 0x01,0x01,
	/* 86-87 XCHG r/m,r */ 0x01,0x01,
	/* 88-8B MOV r/m,r  */ 0x01,0x01,0x01,0x01,
	/* 8C MOV r/m,seg   */ 0x01,
	/* 8D LEA           */ 0x01,
	/* 8E MOV seg,r/m   */ 0x01,
	/* 8F POP r/m       */ 0x01,
	/* 90-97 NOP/XCHG   */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	/* 98-9F CBW/CWD/etc */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	/* A0-A3 MOV moffs  */ 0x80,0x80,0x80,0x80,
	/* A4-A7 MOVS/CMPS  */ 0x00,0x00,0x00,0x00,
	/* A8-A9 TEST AL,r  */ 0x10,0x40,
	/* AA-AF STOS/LODS/SCAS */ 0x00,0x00,0x00,0x00,0x00,0x00,
	/* B0-B7 MOV r8,imm8 */ 0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
	/* B8-BF MOV r64,imm64 */ 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	/* C0 group2 r/m,imm8 */ 0x11,
	/* C1 group2 r/m,imm8 */ 0x11,
	/* C2 RET imm16     */ 0x20,
	/* C3 RET           */ 0x00,
	/* C4-C5 VEX inv64  */ 0xFF,0xFF,
	/* C6 MOV r/m8,imm8 */ 0x11,
	/* C7 MOV r/m,imm32 */ 0x41,
	/* C8 ENTER imm16+8 */ 0x30,
	/* C9 LEAVE         */ 0x00,
	/* CA RETF imm16    */ 0x20,
	/* CB RETF          */ 0x00,
	/* CC-CE INT/INTO   */ 0x00,0x00,0x00,
	/* CF IRET          */ 0x00,
	/* D0-D3 group2     */ 0x01,0x01,0x01,0x01,
	/* D4-D7 invalid64  */ 0xFF,0xFF,0xFF,0xFF,
	/* D8-DF x87 ModRM  */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* E0-E3 LOOPx/JCXZ */ 0x10,0x10,0x10,0x10,
	/* E4-E7 IN/OUT imm8 */ 0x10,0x10,0x10,0x10,
	/* E8 CALL rel32    */ 0x40,
	/* E9 JMP rel32     */ 0x40,
	/* EA JMPF inv64    */ 0xFF,
	/* EB JMP rel8      */ 0x10,
	/* EC-EF IN/OUT DX  */ 0x00,0x00,0x00,0x00,
	/* F0-F3 prefixes   */ 0xFF,0xFF,0xFF,0xFF,
	/* F4-F5 HLT/CMC    */ 0x00,0x00,
	/* F6 group3 r/m8   */ 0x01,
	/* F7 group3 r/m    */ 0x01,
	/* F8-FD flags      */ 0x00,0x00,0x00,0x00,0x00,0x00,
	/* FE group4 inc/dec */ 0x01,
	/* FF group5 call/jmp/push */ 0x01,
};

// Two-byte opcode map (0F xx).
static constexpr uint8_t kOpcodeMap2[256] = {
	/* 0F 00-01 group6/7 */ 0x01,0x01,
	/* 0F 02-03 LAR/LSL  */ 0x01,0x01,
	/* 0F 04-05 inv      */ 0xFF,0x00,
	/* 0F 06-07 CLTS/INV */ 0x00,0x00,
	/* 0F 08-09 INVD/etc */ 0x00,0x00,
	/* 0F 0A-0B inv/UD2  */ 0xFF,0x00,
	/* 0F 0C-0F inv      */ 0xFF,0xFF,0xFF,0xFF,
	/* 0F 10-17 SSE MOV  */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F 18-1F prefetch */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F 20-23 MOV CR/DR */ 0x01,0x01,0x01,0x01,
	/* 0F 24-27 inv      */ 0xFF,0xFF,0xFF,0xFF,
	/* 0F 28-2F SSE MOV  */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F 30-37 sys      */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	/* 0F 38-3F 3byte esc */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
	/* 0F 40-4F CMOVcc   */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	                         0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F 50-5F SSE      */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	                         0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F 60-6F SSE/MMX  */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	                         0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F 70-77 SSE shift */ 0x11,0x11,0x11,0x11,0x01,0x01,0x01,0x00,
	                         0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F 80-8F Jcc rel32 */ 0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,
	                          0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,
	/* 0F 90-9F SETcc    */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	                         0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F A0-AF          */ 0x00,0x00,0x00,0x01,0x11,0x01,0xFF,0xFF,
	                         0x00,0x00,0x00,0x01,0x11,0x01,0x01,0x01,
	/* 0F B0-BF          */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	                         0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F C0-CF          */ 0x01,0x01,0x11,0x01,0x11,0x11,0x11,0x11,
	                         0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	/* 0F D0-DF SSE/MMX  */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	                         0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F E0-EF SSE/MMX  */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	                         0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	/* 0F F0-FF SSE/MMX  */ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
	                         0x01,0x01,0x01,0x01,0x01,0x01,0x01,0xFF,
};
// clang-format on

// Decode the length of the x86-64 instruction at |code|. Returns 0 on failure.
static int x64_instruction_length(const uint8_t* code, bool* has_rex_w) noexcept
{
	int  i      = 0;
	bool opsize = false;
	bool rex_w  = false;

	// Legacy prefixes
	for (;; i++)
	{
		const uint8_t b = code[i];
		if (b == 0x66) { opsize = true; continue; }
		if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
		    b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
		    b == 0x64 || b == 0x65) { continue; }
		break;
	}

	// REX prefix (0x40-0x4F)
	if ((code[i] & 0xF0) == 0x40)
	{
		if (code[i] & 0x08) rex_w = true;
		i++;
	}

	if (has_rex_w != nullptr) *has_rex_w = rex_w;

	// Opcode
	uint8_t op = code[i++];
	const uint8_t* table;
	if (op == 0x0F)
	{
		op    = code[i++];
		table = kOpcodeMap2;
	} else
	{
		table = kOpcodeMap1;
	}

	const uint8_t flags = table[op];
	if (flags == 0xFF) return 0;

	const uint8_t imm_size = (flags >> 4) & 0x0F;

	// ModRM + SIB + displacement
	int modrm_pos = -1;
	if (flags & kModRM)
	{
		modrm_pos       = i;
		const uint8_t modrm = code[i++];
		const uint8_t mod   = modrm >> 6;
		const uint8_t rm    = modrm & 0x07;

		if (mod != 3)
		{
			if (rm == 4) // SIB
			{
				const uint8_t sib  = code[i++];
				const uint8_t base = sib & 0x07;
				if (mod == 0 && base == 5)
				{
					i += 4; // disp32
				}
			}
			if (mod == 0 && rm == 5)
			{
				i += 4; // RIP-relative disp32
			} else if (mod == 1)
			{
				i += 1; // disp8
			} else if (mod == 2)
			{
				i += 4; // disp32
			}
		}
	}

	// Immediate
	if (imm_size == 1)
	{
		i += 1;
	} else if (imm_size == 2)
	{
		i += 2;
	} else if (imm_size == 3)
	{
		i += 3; // ENTER: imm16 + imm8
	} else if (imm_size == 4)
	{
		i += (opsize ? 2 : 4);
	} else if (imm_size == 8)
	{
		// MOV reg, imm64 (B8-BF with REX.W) or moffs (A0-A3 in 64-bit)
		if (op >= 0xB8 && op <= 0xBF && rex_w)
		{
			i += 8;
		} else if (op >= 0xA0 && op <= 0xA3)
		{
			i += 8; // moffs64
		} else
		{
			i += (opsize ? 2 : 4);
		}
	}

	// F6/F7 group3: TEST (/0,/1) has an immediate that the table doesn't encode.
	if ((op == 0xF6 || op == 0xF7) && modrm_pos >= 0)
	{
		const uint8_t reg_field = (code[modrm_pos] >> 3) & 0x07;
		if (reg_field <= 1) // TEST
		{
			i += (op == 0xF6) ? 1 : (opsize ? 2 : 4);
		}
	}

	return i;
}

// Set the register identified by ModRM.reg (with REX.R extension) to zero.
// Used to emulate a zero-read from the null page.
static void null_page_zero_dest_reg(ucontext_t* uc, const uint8_t* code) noexcept
{
	int  i     = 0;
	bool rex_r = false;
	// Skip legacy prefixes
	for (;; i++)
	{
		const uint8_t b = code[i];
		if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
		    b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
		    b == 0x64 || b == 0x65) continue;
		break;
	}
	// REX
	if ((code[i] & 0xF0) == 0x40)
	{
		rex_r = (code[i] & 0x04) != 0;
		i++;
	}
	// Skip opcode (1 or 2 bytes)
	if (code[i] == 0x0F) i++;
	i++;
	// ModRM byte
	const uint8_t modrm = code[i];
	const int     reg  = ((modrm >> 3) & 0x07) | (rex_r ? 8 : 0);

	switch (reg)
	{
		case 0: uc_set_rax(uc, 0); break;
		case 1: uc_set_rcx(uc, 0); break;
		case 2: uc_set_rdx(uc, 0); break;
		case 3: uc_set_rbx(uc, 0); break;
		case 4: /* RSP — never zero */ break;
		case 5: uc_set_rbp(uc, 0); break;
		case 6: uc_set_rsi(uc, 0); break;
		case 7: uc_set_rdi(uc, 0); break;
		case 8: uc_set_r8(uc, 0); break;
		case 9: uc_set_r9(uc, 0); break;
		case 10: uc_set_r10(uc, 0); break;
		case 11: uc_set_r11(uc, 0); break;
		case 12: uc_set_r12(uc, 0); break;
		case 13: uc_set_r13(uc, 0); break;
		case 14: uc_set_r14(uc, 0); break;
		case 15: uc_set_r15(uc, 0); break;
		default: break;
	}
}

// Attempt to handle a null-page data fault by skipping the faulting instruction.
// Returns true if the instruction was decoded and skipped.
static bool try_skip_null_page_fault(ucontext_t* uc, uint64_t fault_addr, bool is_write) noexcept
{
	// Only handle faults in the unmappable region (below mmap_min_addr).
	if (fault_addr >= 0x10000ull) return false;

	const uint64_t rip = uc_get_rip(uc);
	if (!IsGuestCodeAddress(rip)) return false;

	const auto* code = reinterpret_cast<const uint8_t*>(rip);
	bool        rex_w = false;
	const int   len   = x64_instruction_length(code, &rex_w);
	if (len <= 0 || len > 15) return false;

	// For read faults, zero the destination register to emulate reading zeros
	// from the PS5's mapped null page.
	if (!is_write)
	{
		null_page_zero_dest_reg(uc, code);
	}

	uc_set_rip(uc, rip + static_cast<uint64_t>(len));
	return true;
}

#endif // __x86_64__ || __i386__


// Guest soft-debug `int $0x41` is NOP'd at load time (LoaderPatchGuestSoftDebugInterrupts);
// do not peek guest RIP here — LOAD code can be X-only and re-fault in the handler.
static void kyty_posix_signal_handler(int sig, siginfo_t* info, void* ucontext)
{
	auto* uc = static_cast<ucontext_t*>(ucontext);

	if (sig == SIGILL)
	{
		uint64_t rip = uc_get_rip(uc);
	#if defined(__x86_64__) || defined(__i386__)
		if (IsGuestCodeAddress(rip))
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
		if (g_signal_skip_ud2 != 0 && IsGuestCodeAddress(rip))
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
	einfo.rsp               = uc_get_rsp(uc);
	einfo.rflags            = uc_get_rflags(uc);
	einfo.rax               = uc_get_rax(uc);
	einfo.rbx               = uc_get_rbx(uc);
	einfo.rcx               = uc_get_rcx(uc);
	einfo.rdx               = uc_get_rdx(uc);
	einfo.rsi               = uc_get_rsi(uc);
	einfo.rdi               = uc_get_rdi(uc);
	einfo.r8                = uc_get_r8(uc);
	einfo.r9                = uc_get_r9(uc);
	einfo.r10               = uc_get_r10(uc);
	einfo.r11               = uc_get_r11(uc);
	einfo.r12               = uc_get_r12(uc);
	einfo.r13               = uc_get_r13(uc);
	einfo.r14               = uc_get_r14(uc);
	einfo.r15               = uc_get_r15(uc);
	if (einfo.rsp >= 0x10000u)
	{
		// The guest stack may point at an unmapped or partially-mapped region
		// (the fault that brought us here can be a bad RSP, a recycled command
		// buffer, or an unmapped page). Dereferencing it directly inside the
		// signal handler would double-fault. Read it fault-safe instead so a
		// capture attempt can never replace the original fault with a crash.
		uint64_t buffer[16] = {};
		struct iovec local  = {buffer, sizeof(buffer)};
		struct iovec remote = {reinterpret_cast<void*>(einfo.rsp), sizeof(buffer)};
		const ssize_t got = syscall(SYS_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL);
		if (got > 0)
		{
			const auto count = static_cast<uint32_t>(static_cast<size_t>(got) / sizeof(uint64_t));
			for (uint32_t i = 0; i < count; ++i)
			{
				einfo.stack[i] = buffer[i];
			}
			einfo.stack_count = count;
		}
	}

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
				// Walk guest frame pointers fault-safely; an unmapped frame
				// must not turn the diagnostic into a second fault.
				uint64_t frame_addr = rbp;
				for (int depth = 0; depth < 4; depth++)
				{
					uint64_t pair[2] = {};
					struct iovec l   = {pair, sizeof(pair)};
					struct iovec r   = {reinterpret_cast<void*>(frame_addr), sizeof(pair)};
					const ssize_t got = syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL);
					if (got != static_cast<ssize_t>(sizeof(pair)))
					{
						break;
					}
					const uint64_t next = pair[0];
					sigsafe_fault("  frame", next, pair[1]);
					if (next <= frame_addr || next - frame_addr > 0x10000ull)
					{
						break;
					}
					frame_addr = next;
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

void FatalFault(const ExceptionHandler::ExceptionInfo* info) noexcept
{
	if (info != nullptr)
	{
		WriteCrashReport(*info);
	}
	std::_Exit(139);
}

void FatalFault(uint64_t vaddr, uint64_t rip)
{
	ExceptionHandler::ExceptionInfo info {};
	info.type                   = ExceptionHandler::ExceptionType::AccessViolation;
	info.access_violation_type = ExceptionHandler::AccessViolationType::Unknown;
	info.access_violation_vaddr = vaddr;
	info.exception_address      = rip;
	FatalFault(&info);
}

#if !defined(KYTY_HAS_SIGNAL_EXCEPTIONS)
void RegisterDemandRange(uint64_t, uint64_t) {}

bool TryDemandMap(uint64_t)
{
	return false;
}

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

	if (exception_record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
	{
		// Host libraries use SEH for recoverable control flow. In particular,
		// Intel's Vulkan compiler raises and handles floating-point exceptions
		// while creating pipelines. Kyty's vectored handler only owns access
		// violations used by guest demand paging and GPU write watches; let every
		// other exception continue to the component that raised it.
		return EXCEPTION_CONTINUE_SEARCH;
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

	info.rbp               = exception->ContextRecord->Rbp;
	info.rsp               = exception->ContextRecord->Rsp;
	info.rflags            = exception->ContextRecord->EFlags;
	info.rax               = exception->ContextRecord->Rax;
	info.rbx               = exception->ContextRecord->Rbx;
	info.rcx               = exception->ContextRecord->Rcx;
	info.rdx               = exception->ContextRecord->Rdx;
	info.rsi               = exception->ContextRecord->Rsi;
	info.rdi               = exception->ContextRecord->Rdi;
	info.r8                = exception->ContextRecord->R8;
	info.r9                = exception->ContextRecord->R9;
	info.r10               = exception->ContextRecord->R10;
	info.r11               = exception->ContextRecord->R11;
	info.r12               = exception->ContextRecord->R12;
	info.r13               = exception->ContextRecord->R13;
	info.r14               = exception->ContextRecord->R14;
	info.r15               = exception->ContextRecord->R15;
	if (exception->ContextRecord->Rsp >= 0x1000u)
	{
		const auto* stack = reinterpret_cast<const uint64_t*>(exception->ContextRecord->Rsp);
		for (uint32_t i = 0; i < 16u; ++i)
		{
			info.stack[i] = stack[i];
		}
		info.stack_count = 16;
	}
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
	const char* crash_report = std::getenv("KYTY_CRASH_REPORT");
	char        default_report[kCrashReportPathMax] = {};
	if (crash_report == nullptr || crash_report[0] == '\0')
	{
		const char* capture_dir = std::getenv("KYTY_CAPTURE_DIR");
		if (capture_dir != nullptr && capture_dir[0] != '\0')
		{
			std::snprintf(default_report, sizeof(default_report), "%s/crash-context.json", capture_dir);
			crash_report = default_report;
		}
	}
	ConfigureFatalFaultReport(crash_report);
#ifdef KYTY_HAS_SIGNAL_EXCEPTIONS
	LoadSignalDiagnosticsConfigFromEnvironment();
#endif
	sys_virtual_init();
#ifdef KYTY_HAS_SIGNAL_EXCEPTIONS
	EnsureDemandPageSize();
#endif
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

bool AllocFixedReplacingOwnedReservation(uint64_t address, uint64_t size, Mode mode)
{
	if (address == 0 || size == 0 || address > UINT64_MAX - size)
	{
		return false;
	}
	return sys_virtual_alloc_fixed_replacing_owned_reservation(address, size, mode);
}

uint64_t Reserve(uint64_t address, uint64_t size)
{
	return sys_virtual_reserve(address, size);
}

uint64_t ReserveAligned(uint64_t address, uint64_t size, uint64_t alignment)
{
	if (alignment == 0)
	{
		return Reserve(address, size);
	}
	return sys_virtual_reserve_aligned(address, size, alignment);
}

bool ReserveFixed(uint64_t address, uint64_t size)
{
	return sys_virtual_reserve_fixed(address, size);
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

bool MapSharedFixedReplacingOwnedReservation(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
                                             Mode mode)
{
	if (address == 0 || !shared_range_is_valid(backing, backing_offset, size))
	{
		return false;
	}
	return sys_virtual_map_shared_fixed_replacing_owned_reservation(backing->handle, address, backing_offset, size, mode);
}

bool SupportsSharedFixedOwnedReservationReplacement()
{
	return sys_virtual_supports_shared_fixed_owned_reservation_replacement();
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

bool IsRangeReadable(uint64_t address, uint64_t size)
{
	return sys_virtual_is_range_readable(address, size);
}

ProtectionChangeResult RemoveWriteAndCapture(uint64_t address, uint64_t size, CapturedProtectionVisitor visitor,
	                                         void* context) noexcept
{
	return sys_virtual_remove_write_and_capture(address, size, visitor, context);
}

bool RemoveWriteFromProtection(uint64_t address, uint64_t size, uint32_t restore_token) noexcept
{
	return sys_virtual_remove_write_from_protection(address, size, restore_token);
}

bool RestoreProtection(uint64_t address, uint64_t size, uint32_t restore_token) noexcept
{
	return sys_virtual_restore_protection(address, size, restore_token);
}

bool RestoreProtectionSignalSafe(uint64_t address, uint64_t size, uint32_t restore_token) noexcept
{
	return sys_virtual_restore_protection_signal_safe(address, size, restore_token);
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
