#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("NpCppWebApi", 1, "NpCppWebApi", 1, 1);

namespace NpCppWebApi {

// NID Y295ygEccqk — first NpCppWebApi import on Astro boot after Http2Init.
// Observed SysV args: rdi = "/app0/param.sfx" (or this-pointer), rsi = 0x24,
// rdx = 0x12. Guest continues without consuming the return value before the
// next subobject constructor, so treat as a no-op setup and return rdi for
// C++ constructor ABI (this-pointer passthrough).
static KYTY_SYSV_ABI void* NpCppWebApiUnknownY295(void* self_or_path, int arg1, int arg2)
{
	PRINT_NAME();
	printf("\t self_or_path = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self_or_path));
	printf("\t arg1         = 0x%x\n", arg1);
	printf("\t arg2         = 0x%x\n", arg2);
	if (self_or_path != nullptr)
	{
		const auto* as_c = static_cast<const char*>(self_or_path);
		// Path form is printable ASCII starting with '/'; do not assume.
		if (as_c[0] == '/')
		{
			printf("\t path         = %s\n", as_c);
		}
	}
	return self_or_path;
}

// NID 8x++mBOUeso — observed after NpWebApi2Initialize while installing user
// context logging: rdi = guest object/buffer, rsi = "Np-%s: Set userCtxId…".
// Treat as constructor/setup that returns this.
static KYTY_SYSV_ABI void* NpCppWebApiUnknown8x(void* self, const char* msg, void* arg2)
{
	PRINT_NAME();
	printf("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
	printf("\t msg  = %s\n", (msg != nullptr ? msg : "(null)"));
	printf("\t arg2 = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(arg2));
	return self;
}

// NID UYPxv8MIzGo — method on the object from 8x++: guest tests eax for <0.
static int KYTY_SYSV_ABI NpCppWebApiUnknownUY(void* self, void* arg)
{
	PRINT_NAME();
	printf("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
	printf("\t arg  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(arg));
	return OK;
}

} // namespace NpCppWebApi

LIB_DEFINE(InitNpCppWebApi_1)
{
	LIB_FUNC("Y295ygEccqk", NpCppWebApi::NpCppWebApiUnknownY295);
	LIB_FUNC("8x++mBOUeso", NpCppWebApi::NpCppWebApiUnknown8x);
	LIB_FUNC("UYPxv8MIzGo", NpCppWebApi::NpCppWebApiUnknownUY);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
