#include "Emulator/Libs/Np.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::NpTrophy2 {

LIB_VERSION("NpTrophy2", 1, "NpTrophy2", 1, 1);

static constexpr int error_invalid_argument = static_cast<int32_t>(0x80551604u);
static std::atomic<int32_t> g_next_context {0};
static std::atomic<int32_t> g_next_handle {0};

// sceNpTrophy2CreateContext — NID Fbshr7OQ6Q
int KYTY_SYSV_ABI CreateContext(int32_t* context, int32_t /*user_id*/, uint32_t /*service_label*/, uint64_t options)
{
	if (context == nullptr || options != 0)
	{
		return error_invalid_argument;
	}

	*context = g_next_context.fetch_add(1, std::memory_order_relaxed) + 1;
	return OK;
}

// sceNpTrophy2CreateHandle — NID Gz1rmUZpROM
int KYTY_SYSV_ABI CreateHandle(int32_t* handle)
{
	if (handle == nullptr)
	{
		return error_invalid_argument;
	}

	*handle = g_next_handle.fetch_add(1, std::memory_order_relaxed) + 1;
	return OK;
}

// sceNpTrophy2RegisterContext — NID bIDov3wBu5Q
int KYTY_SYSV_ABI RegisterContext(int32_t context, int32_t handle, uint64_t options)
{
	return (context > 0 && handle > 0 && options == 0 ? OK : error_invalid_argument);
}

// sceNpTrophy2RegisterUnlockCallback — NID sUXGfNMalIo
// Gen5 capture: rdi is a guest code pointer (callback), not a context out-param.
// Name/ABI from public Gen5 export tables. Host has no trophy UI; accept registration without invoking the callback.
int KYTY_SYSV_ABI RegisterUnlockCallback(void* /*callback*/, void* /*userdata*/)
{
	return OK;
}

LIB_DEFINE(InitNpTrophy2_1)
{
	LIB_FUNC("Fbshr7OQ6Q", CreateContext);
	LIB_FUNC("Gz1rmUZpROM", CreateHandle);
	LIB_FUNC("bIDov3wBu5Q", RegisterContext);
	LIB_FUNC("sUXGfNMalIo", RegisterUnlockCallback);
}

} // namespace Kyty::Libs::NpTrophy2

#endif // KYTY_EMU_ENABLED
