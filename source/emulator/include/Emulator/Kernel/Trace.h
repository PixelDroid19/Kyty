#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_TRACE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_TRACE_H_

#include "Emulator/Common.h"
#include "Emulator/Loader/Timer.h"
#include "Emulator/Log.h"

#include "Kyty/Core/Threads.h"

#ifdef KYTY_EMU_ENABLED

// Kernel implementations may trace guest calls without importing the HLE
// registration macros. Registration remains in the Libs adapters; this small
// port only preserves the existing opt-out switch and log format.
#define KERNEL_LIB_NAME()                                                                                                                  \
	[[maybe_unused]] static thread_local bool g_kernel_print_name_enabled = true;                                                         \
	static constexpr char                     g_kernel_library[]            = "libkernel";                                                \
	static constexpr char                     g_kernel_module[]             = "libkernel"

#define PRINT_NAME_ENABLED g_kernel_print_name_enabled
#define PRINT_NAME_ENABLE(flag) PRINT_NAME_ENABLED = flag;

#define PRINT_NAME()                                                                                                                       \
	if (PRINT_NAME_ENABLED)                                                                                                                  \
	{                                                                                                                                         \
		if (Kyty::Log::GetDirection() != Kyty::Log::Direction::Silent)                                                                       \
		{                                                                                                                                     \
			Kyty::printf(FG_CYAN "[%d][%s] %s::%s::%s()" DEFAULT "\n", Core::Thread::GetThreadIdUnique(),                                  \
			             Loader::Timer::GetTime().ToString("HH24:MI:SS.FFF").C_Str(), g_kernel_library, g_kernel_module, __func__);       \
		}                                                                                                                                     \
	}

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_TRACE_H_ */
