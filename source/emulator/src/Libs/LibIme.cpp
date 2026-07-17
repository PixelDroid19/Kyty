#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Ime", 1, "Ime", 1, 1);

namespace Ime {

// Soft keyboard / IME bootstrap used by Gen5 titles during early UI setup.
// No host OSK yet: accept open/close/query and return success so boot continues.

// sceImeKeyboardOpen — NID eaFXjfJv3xs (user_id, param)
static int KYTY_SYSV_ABI ImeKeyboardOpen(int user_id, const void* param)
{
	PRINT_NAME();
	printf("\t user_id = %d\n", user_id);
	printf("\t param   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	return OK;
}

// sceImeKeyboardClose — NID PMVehSlfZ94
static int KYTY_SYSV_ABI ImeKeyboardClose(int user_id)
{
	PRINT_NAME();
	printf("\t user_id = %d\n", user_id);
	return OK;
}

// sceImeKeyboardGetResourceId — NID dKadqZFgKKQ
static int KYTY_SYSV_ABI ImeKeyboardGetResourceId(int user_id, void* resource_id_array)
{
	PRINT_NAME();
	printf("\t user_id           = %d\n", user_id);
	printf("\t resource_id_array = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(resource_id_array));
	return OK;
}

// sceImeKeyboardGetInfo — NID VkqLPArfFdc
static int KYTY_SYSV_ABI ImeKeyboardGetInfo(uint32_t resource_id, void* info)
{
	PRINT_NAME();
	printf("\t resource_id = 0x%08" PRIx32 "\n", resource_id);
	printf("\t info        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(info));
	return OK;
}

// sceImeKeyboardSetMode — NID ua+13Hk9kKs
static int KYTY_SYSV_ABI ImeKeyboardSetMode(int user_id, uint32_t mode)
{
	PRINT_NAME();
	printf("\t user_id = %d\n", user_id);
	printf("\t mode    = 0x%08" PRIx32 "\n", mode);
	return OK;
}

// sceImeUpdate — NID -4GCfYdNF1s
static int KYTY_SYSV_ABI ImeUpdate(void* handler)
{
	PRINT_NAME();
	printf("\t handler = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handler));
	return OK;
}

} // namespace Ime

LIB_DEFINE(InitIme_1)
{
	LIB_FUNC("eaFXjfJv3xs", Ime::ImeKeyboardOpen);
	LIB_FUNC("PMVehSlfZ94", Ime::ImeKeyboardClose);
	LIB_FUNC("dKadqZFgKKQ", Ime::ImeKeyboardGetResourceId);
	LIB_FUNC("VkqLPArfFdc", Ime::ImeKeyboardGetInfo);
	LIB_FUNC("ua+13Hk9kKs", Ime::ImeKeyboardSetMode);
	LIB_FUNC("-4GCfYdNF1s", Ime::ImeUpdate);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
