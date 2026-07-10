#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_PRINTF_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_PRINTF_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

struct VaContext;
struct VaList;

using libc_printf_std_func_t   = KYTY_FORMAT_PRINTF(1, 2) KYTY_SYSV_ABI int (*)(const char* str, ...);
using libc_printf_ctx_func_t   = int (*)(VaContext* c);
using libc_snprintf_ctx_func_t = int (*)(VaContext* c);
using libc_vprintf_func_t      = int (*)(const char* str, VaList* c);

libc_printf_std_func_t   GetPrintfStdFunc();
libc_printf_ctx_func_t   GetPrintfCtxFunc();
libc_snprintf_ctx_func_t GetSnrintfCtxFunc();
libc_vprintf_func_t      GetVprintfFunc();

// Formats into out (bounded by out_size) using Kyty's own conversion of the guest
// VaList, returning the length that would be written. Every guest printf-family
// export shares this path so formatting behaves identically and never hands the
// guest's register-save area to the host libc formatter.
int Format(char* out, size_t out_size, const char* format, VaList* va_list);

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_PRINTF_H_ */
