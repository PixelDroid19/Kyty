#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <atomic>
#include <cinttypes>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Acm", 1, "Acm", 1, 1);

namespace Acm {

// sceAcmContextCreate — NID ZIXln2K3XMk. Its only argument is the context it
// creates, a 32-bit handle: a title tests it with a 32-bit compare and creates
// it while it is zero, tail-calling this function with the other argument
// registers holding whatever the caller left there. Reading one of them as a
// buffer size cleared that many bytes past the handle and destroyed the audio
// engine globals that follow it in the title's data.
static int KYTY_SYSV_ABI AcmContextCreate(uint32_t* ctx)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t ctx = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(ctx));

	if (ctx == nullptr)
	{
		return -1;
	}
	static std::atomic<uint32_t> next_context {1};
	*ctx = next_context.fetch_add(1);
	return OK;
}

} // namespace Acm

LIB_DEFINE(InitAcm_1)
{
	LIB_FUNC("ZIXln2K3XMk", Acm::AcmContextCreate);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
