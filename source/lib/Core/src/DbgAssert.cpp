#include "Kyty/Core/DbgAssert.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Debug.h"
#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Subsystems.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
// IWYU pragma: no_include <debugapi.h>
#endif

namespace Kyty::Core {
namespace {

std::atomic<HostFaultHook> g_host_fault_hook {nullptr};

void NotifyHostFault(const char* code, const char* message) noexcept
{
	const HostFaultHook hook = g_host_fault_hook.load(std::memory_order_acquire);
	if (hook != nullptr)
	{
		hook(code != nullptr ? code : "host_fault", message != nullptr ? message : "");
	}
}

} // namespace

void SetHostFaultHook(HostFaultHook hook) noexcept
{
	g_host_fault_hook.store(hook, std::memory_order_release);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && KYTY_BUILD == KYTY_BUILD_DEBUG && KYTY_COMPILER == KYTY_COMPILER_CLANG
constexpr int PRINT_STACK_FROM = 4;
#else
constexpr int PRINT_STACK_FROM = 2;
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
int IsDebuggerPresent()
{
	bool  dbg = false;
	FILE* f   = fopen("/proc/self/status", "r");

	if (f != nullptr)
	{
		char str[1024];

		while (feof(f) == 0)
		{
			str[1023] = '\0';
			int pid   = 0;

			[[maybe_unused]] auto* result = fgets(str, 1023, f);
			if (sscanf(str, "TracerPid: %d", &pid) == 1) // NOLINT
			{
				dbg = (pid != 0);
				break;
			}
		}

		[[maybe_unused]] auto result = fclose(f);
	}

	return (dbg ? 1 : 0);
}
#endif

void dbg_print_stack()
{
	DebugStack s;
	DebugStack::Trace(&s);

	printf("--- Stack Trace ---\n");
	s.Print(PRINT_STACK_FROM);
}

int dbg_assert_handler(char const* expr, char const* file, int line)
{
	dbg_print_stack();
	KYTY_LOGE("--- Fatal Error ---\n");
	KYTY_LOGE("Assertion (%s) failed in %s:%d\n", expr, file, line);
	char msg[192];
	std::snprintf(msg, sizeof(msg), "assert %s", expr != nullptr ? expr : "");
	NotifyHostFault("assert", msg);
	SubsystemsListSingleton::Instance()->ShutdownAll();
	return 1;
}

int dbg_exit_if_handler(char const* expr, char const* file, int line)
{
	dbg_print_stack();
	KYTY_LOGE("--- Fatal Error ---\n");
	KYTY_LOGE("Error: condition (%s) is true in %s:%d\n", expr, file, line);
	char msg[192];
	std::snprintf(msg, sizeof(msg), "exit_if %s", expr != nullptr ? expr : "");
	NotifyHostFault("exit_if", msg);
	SubsystemsListSingleton::Instance()->ShutdownAll();
	return 1;
}

int dbg_not_implemented_handler(char const* expr, char const* file, int line)
{
	const auto decision = Core::BringUp::ReportNotImplemented(expr, file, line);
	if (decision == Core::BringUp::Decision::Continue)
	{
		// Unsafe bring-up: first hit is logged by BringUp::Report; no stack spam.
		return 0;
	}

	// Strict Halt (or circuit-break): historical stack + ShutdownAll + abort.
	dbg_print_stack();
	KYTY_LOGE("--- Fatal Error ---\n");
	KYTY_LOGE("Not implemented (%s) in %s:%d\n", expr, file, line);
	char msg[192];
	std::snprintf(msg, sizeof(msg), "not_implemented %s", expr != nullptr ? expr : "");
	NotifyHostFault("not_implemented_halt", msg);
	SubsystemsListSingleton::Instance()->ShutdownAll();
	return 1;
}

int dbg_exit_handler(char const* file, int line, const char* f, ...)
{
	va_list args {};
	va_start(args, f);

	dbg_print_stack();

	KYTY_LOGE("--- Error ---\n");
	vprintf(f, args);
	KYTY_LOGE(" in %s:%d\n", file, line);

	char msg[192];
	std::snprintf(msg, sizeof(msg), "exit %s:%d", file != nullptr ? file : "?", line);
	NotifyHostFault("exit", msg);

	SubsystemsListSingleton::Instance()->ShutdownAll();

	va_end(args);

	return 1;
}

void dbg_exit(int status)
{
	char msg[64];
	std::snprintf(msg, sizeof(msg), "dbg_exit status=%d", status);
	NotifyHostFault("dbg_exit", msg);
	::fflush(nullptr);
	std::_Exit(status);
}

bool dbg_is_debugger_present()
{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	return !(IsDebuggerPresent() == 0);
#endif
	return false;
}

} // namespace Kyty::Core
