#include "Emulator/Libs/Np.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::NpUniversalDataSystem {

LIB_VERSION("NpUniversalDataSystem", 1, "NpUniversalDataSystem", 1, 1);

static constexpr int error_invalid_argument = static_cast<int32_t>(0x80553102u);
static std::atomic<int32_t> g_next_handle {1};

int KYTY_SYSV_ABI Initialize(const void* parameters)
{
	return (parameters != nullptr ? OK : error_invalid_argument);
}

int KYTY_SYSV_ABI CreateContext(int32_t* context)
{
	if (context != nullptr)
	{
		*context = 1;
	}
	return OK;
}

int KYTY_SYSV_ABI CreateHandle(int32_t* handle, int32_t* alternate_handle)
{
	auto* output = (handle != nullptr ? handle : alternate_handle);
	if (output == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}

	*output = g_next_handle.fetch_add(1, std::memory_order_relaxed) + 1;
	return OK;
}

int KYTY_SYSV_ABI RegisterContext()
{
	return OK;
}

LIB_DEFINE(InitNpUniversalDataSystem_1)
{
	LIB_FUNC("sjaobBgqeB4", Initialize);
	LIB_FUNC("5zBnau1uIEo", CreateContext);
	LIB_FUNC("hT0IAEvN+M0", CreateHandle);
	LIB_FUNC("tpFJ8LIKvPw", RegisterContext);
}

} // namespace Kyty::Libs::NpUniversalDataSystem

#endif // KYTY_EMU_ENABLED
