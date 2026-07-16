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

LIB_VERSION("Keyboard", 1, "Keyboard", 1, 1);

namespace Keyboard {

constexpr int KEYBOARD_ERROR_INVALID_ARG    = -2133196799; /* 0x80DA0001 */
constexpr int KEYBOARD_ERROR_INVALID_HANDLE = -2133196797; /* 0x80DA0003 */
constexpr int KEYBOARD_MAX_KEYCODES         = 16;
constexpr int KEYBOARD_MAX_DATA_NUM         = 16;

struct KeyboardData
{
	uint64_t timestamp;
	bool     intercepted;
	uint8_t  reserve1[7];
	bool     connected;
	int32_t  length;
	uint32_t led;
	uint32_t modifier_key;
	uint16_t key_code[KEYBOARD_MAX_KEYCODES];
	uint8_t  reserve2[32];
};

// sceKeyboardInit — NID wadT3QBCGY0
static int KYTY_SYSV_ABI KeyboardInit()
{
	PRINT_NAME();
	return OK;
}

// sceKeyboardOpen — NID HJ+KnEHcaxI. Returns positive handle.
static int KYTY_SYSV_ABI KeyboardOpen(int user_id, int32_t type, int32_t index, const void* param)
{
	PRINT_NAME();
	printf("\t user_id = %d\n", user_id);
	printf("\t type    = %" PRId32 "\n", type);
	printf("\t index   = %" PRId32 "\n", index);
	printf("\t param   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	if (type != 0 || index < 0 || index >= 2)
	{
		return KEYBOARD_ERROR_INVALID_ARG;
	}
	return 1;
}

// sceKeyboardClose — NID 0LWei+c7RNc
static int KYTY_SYSV_ABI KeyboardClose(int32_t handle)
{
	PRINT_NAME();
	printf("\t handle = %" PRId32 "\n", handle);
	if (handle <= 0)
	{
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}
	return OK;
}

// sceKeyboardRead — NID xybbGMCr738
static int KYTY_SYSV_ABI KeyboardRead(int32_t handle, KeyboardData* data, int32_t num)
{
	PRINT_NAME();
	printf("\t handle = %" PRId32 "\n", handle);
	printf("\t data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	printf("\t num    = %" PRId32 "\n", num);
	if (handle <= 0)
	{
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr || num <= 0 || num > KEYBOARD_MAX_DATA_NUM)
	{
		return KEYBOARD_ERROR_INVALID_ARG;
	}
	std::memset(data, 0, sizeof(KeyboardData) * static_cast<size_t>(num));
	return 0;
}

// sceKeyboardReadState — NID 6HpE68bzX6M
static int KYTY_SYSV_ABI KeyboardReadState(int32_t handle, KeyboardData* data)
{
	PRINT_NAME();
	printf("\t handle = %" PRId32 "\n", handle);
	printf("\t data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	if (handle <= 0)
	{
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr)
	{
		return KEYBOARD_ERROR_INVALID_ARG;
	}
	std::memset(data, 0, sizeof(KeyboardData));
	return 0;
}

} // namespace Keyboard

LIB_DEFINE(InitKeyboard_1)
{
	LIB_FUNC("wadT3QBCGY0", Keyboard::KeyboardInit);
	LIB_FUNC("HJ+KnEHcaxI", Keyboard::KeyboardOpen);
	LIB_FUNC("0LWei+c7RNc", Keyboard::KeyboardClose);
	LIB_FUNC("xybbGMCr738", Keyboard::KeyboardRead);
	LIB_FUNC("6HpE68bzX6M", Keyboard::KeyboardReadState);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
