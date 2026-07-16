#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Acm", 1, "Acm", 1, 1);

namespace Acm {

// sceAcmContextCreate — NID ZIXln2K3XMk.
// Observed Astro SysV: rdi=preallocated guest buffer, rsi=0x10b (byte size),
// rdx=0xe (type/flags). Zero the buffer when size is a plausible context size
// and return success so boot can proceed; expand when more ACM surface is hit.
static int KYTY_SYSV_ABI AcmContextCreate(void* ctx, uint64_t size, uint64_t type_or_flags)
{
	PRINT_NAME();
	printf("\t ctx           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(ctx));
	printf("\t size          = 0x%016" PRIx64 "\n", size);
	printf("\t type_or_flags = 0x%016" PRIx64 "\n", type_or_flags);

	if (ctx == nullptr)
	{
		return -1;
	}
	if (size > 0 && size <= 0x10000)
	{
		std::memset(ctx, 0, static_cast<size_t>(size));
	}
	return OK;
}

} // namespace Acm

LIB_DEFINE(InitAcm_1)
{
	LIB_FUNC("ZIXln2K3XMk", Acm::AcmContextCreate);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
