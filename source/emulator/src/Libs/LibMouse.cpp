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

LIB_VERSION("Mouse", 1, "Mouse", 1, 1);

namespace Mouse {

constexpr int MOUSE_ERROR_INVALID_ARG    = -2132869119; /* 0x80DF0001 */
constexpr int MOUSE_ERROR_INVALID_HANDLE = -2132869117; /* 0x80DF0003 */

struct MouseData
{
	uint64_t timestamp;
	bool     connected;
	uint8_t  padding[3];
	uint32_t buttons;
	int32_t  x_axis;
	int32_t  y_axis;
	int32_t  wheel;
	int32_t  tilt;
	uint8_t  reserved[8];
};

// sceMouseInit — NID Qs0wWulgl7U
static int KYTY_SYSV_ABI MouseInit()
{
	PRINT_NAME();
	return OK;
}

// sceMouseOpen — NID RaqxZIf6DvE. Returns positive handle.
static int KYTY_SYSV_ABI MouseOpen(int user_id, int32_t type, int32_t index, const void* param)
{
	PRINT_NAME();
	printf("\t user_id = %d\n", user_id);
	printf("\t type    = %" PRId32 "\n", type);
	printf("\t index   = %" PRId32 "\n", index);
	printf("\t param   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	if (type != 0 || index < 0 || index >= 2)
	{
		return MOUSE_ERROR_INVALID_ARG;
	}
	return 1;
}

// sceMouseClose — NID cAnT0Rw-IwU
static int KYTY_SYSV_ABI MouseClose(int32_t handle)
{
	PRINT_NAME();
	printf("\t handle = %" PRId32 "\n", handle);
	if (handle <= 0)
	{
		return MOUSE_ERROR_INVALID_HANDLE;
	}
	return OK;
}

// sceMouseRead — NID x8qnXqh-tiM. Returns count of reports filled (0 = none).
static int KYTY_SYSV_ABI MouseRead(int32_t handle, MouseData* data, int32_t num)
{
	PRINT_NAME();
	printf("\t handle = %" PRId32 "\n", handle);
	printf("\t data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	printf("\t num    = %" PRId32 "\n", num);
	if (handle <= 0)
	{
		return MOUSE_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr || num <= 0)
	{
		return MOUSE_ERROR_INVALID_ARG;
	}
	std::memset(data, 0, sizeof(MouseData) * static_cast<size_t>(num));
	return 0;
}

} // namespace Mouse

LIB_DEFINE(InitMouse_1)
{
	LIB_FUNC("Qs0wWulgl7U", Mouse::MouseInit);
	LIB_FUNC("RaqxZIf6DvE", Mouse::MouseOpen);
	LIB_FUNC("cAnT0Rw-IwU", Mouse::MouseClose);
	LIB_FUNC("x8qnXqh-tiM", Mouse::MouseRead);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
