#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Remoteplay", 1, "Remoteplay", 1, 1);

// Remote Play is a host service the emulator does not offer: the library starts,
// and every user reports the disconnected status.
namespace Remoteplay {

constexpr int32_t kConnectionStatusDisconnected = 0;

static int KYTY_SYSV_ABI RemoteplayInitialize(void* /*heap*/, size_t /*heap_size*/)
{
	PRINT_NAME();
	return OK;
}

static int KYTY_SYSV_ABI RemoteplayTerminate()
{
	PRINT_NAME();
	return OK;
}

static int KYTY_SYSV_ABI RemoteplayGetConnectionStatus(int32_t /*user_id*/, int32_t* status)
{
	PRINT_NAME();
	if (status == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	*status = kConnectionStatusDisconnected;
	return OK;
}

} // namespace Remoteplay

LIB_DEFINE(InitRemoteplay_1)
{
	LIB_FUNC("k1SwgkMSOM8", Remoteplay::RemoteplayInitialize);
	LIB_FUNC("BOwybKVa3Do", Remoteplay::RemoteplayTerminate);
	LIB_FUNC("g3PNjYKWqnQ", Remoteplay::RemoteplayGetConnectionStatus);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
