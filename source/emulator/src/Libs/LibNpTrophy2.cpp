#include "Emulator/Libs/Np.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include "Kyty/Core/VirtualMemory.h"

#include <atomic>
#include <cstring>

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
int KYTY_SYSV_ABI RegisterUnlockCallback(void* /*callback*/, void* /*userdata*/)
{
	return OK;
}

int KYTY_SYSV_ABI DestroyContext(int32_t context)
{
	return (context > 0 ? OK : error_invalid_argument);
}

int KYTY_SYSV_ABI DestroyHandle(int32_t handle)
{
	return (handle > 0 ? OK : error_invalid_argument);
}

int KYTY_SYSV_ABI AbortHandle(int32_t handle)
{
	return (handle > 0 ? OK : error_invalid_argument);
}

int KYTY_SYSV_ABI UnregisterUnlockCallback(void* /*callback*/)
{
	return OK;
}

namespace {

constexpr uint64_t kGameDetailsSize   = 152;
constexpr uint64_t kGameDataSize      = 24;
constexpr uint64_t kTrophyDetailsSize = 1312;
constexpr uint64_t kTrophyDataSize    = 32;

bool IsMappedOutput(void* output, uint64_t size)
{
	return output != nullptr && Core::VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(output), size);
}

} // namespace

int KYTY_SYSV_ABI GetGameInfo(int32_t context, int32_t handle, void* details, void* data)
{
	if (context <= 0 || handle <= 0 || !IsMappedOutput(details, kGameDetailsSize) || !IsMappedOutput(data, kGameDataSize))
	{
		return error_invalid_argument;
	}
	std::memset(details, 0, kGameDetailsSize);
	std::memset(data, 0, kGameDataSize);
	return OK;
}

int KYTY_SYSV_ABI GetTrophyInfo(int32_t context, int32_t handle, int32_t trophy_id, void* details, void* data)
{
	if (context <= 0 || handle <= 0 || trophy_id < 0 || !IsMappedOutput(details, kTrophyDetailsSize) ||
	    !IsMappedOutput(data, kTrophyDataSize))
	{
		return error_invalid_argument;
	}
	std::memset(details, 0, kTrophyDetailsSize);
	std::memset(data, 0, kTrophyDataSize);
	std::memcpy(details, &trophy_id, sizeof(trophy_id));
	std::memcpy(data, &trophy_id, sizeof(trophy_id));
	return OK;
}

int KYTY_SYSV_ABI ShowTrophyList(int32_t context, int32_t handle, uint32_t /*flags*/)
{
	return (context > 0 && handle > 0 ? OK : error_invalid_argument);
}

LIB_DEFINE(InitNpTrophy2_1)
{
	LIB_FUNC("Bagshr7OQ6Q", CreateContext);
	LIB_FUNC("Fbshr7OQ6Q", CreateContext);
	LIB_FUNC("Gz1rmUZpROM", CreateHandle);
	LIB_FUNC("bIDov3wBu5Q", RegisterContext);
	LIB_FUNC("sUXGfNMalIo", RegisterUnlockCallback);
	LIB_FUNC("sysY2FHYff4", DestroyContext);
	LIB_FUNC("d8P11CI40KE", DestroyHandle);
	LIB_FUNC("fYapWA9xVmA", AbortHandle);
	LIB_FUNC("wVqxM58sIKs", UnregisterUnlockCallback);
	LIB_FUNC("4IzqhhUQ3nk", GetGameInfo);
	LIB_FUNC("EwNylPdWUTM", GetTrophyInfo);
	LIB_FUNC("EHQEDVXZ0TI", ShowTrophyList);
}

} // namespace Kyty::Libs::NpTrophy2

#endif // KYTY_EMU_ENABLED
