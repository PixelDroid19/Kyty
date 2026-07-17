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

static KYTY_SYSV_ABI void* NpCppWebApiUnknownY295(void* self_or_path, int arg1, int arg2)
{
	PRINT_NAME();
	printf("\t self_or_path = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self_or_path));
	printf("\t arg1         = 0x%x\n", arg1);
	printf("\t arg2         = 0x%x\n", arg2);
	if (self_or_path != nullptr)
	{
		const auto* as_c = static_cast<const char*>(self_or_path);
		if (as_c[0] == '/')
		{
			printf("\t path         = %s\n", as_c);
		}
	}
	return self_or_path;
}

static KYTY_SYSV_ABI void* NpCppWebApiUnknown8x(void* self, const char* msg, void* arg2)
{
	PRINT_NAME();
	printf("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
	printf("\t msg  = %s\n", (msg != nullptr ? msg : "(null)"));
	printf("\t arg2 = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(arg2));
	return self;
}

static int KYTY_SYSV_ABI NpCppWebApiUnknownUY(void* self, void* arg)
{
	PRINT_NAME();
	printf("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
	printf("\t arg  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(arg));
	return OK;
}

static int KYTY_SYSV_ABI NpCppWebApiUnknown52Al(void* self, int user_or_flag)
{
	PRINT_NAME();
	printf("\t self         = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
	printf("\t user_or_flag = %d\n", user_or_flag);
	static int next = 1;
	return next++;
}

} // namespace NpCppWebApi

LIB_DEFINE(InitNpCppWebApi_1)
{
	LIB_FUNC("Y295ygEccqk", NpCppWebApi::NpCppWebApiUnknownY295);
	LIB_FUNC("8x++mBOUeso", NpCppWebApi::NpCppWebApiUnknown8x);
	LIB_FUNC("UYPxv8MIzGo", NpCppWebApi::NpCppWebApiUnknownUY);
	LIB_FUNC("52AlYvq+dmk", NpCppWebApi::NpCppWebApiUnknown52Al);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
