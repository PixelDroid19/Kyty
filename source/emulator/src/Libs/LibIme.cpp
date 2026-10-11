#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Ports/KeyboardTextPort.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Ime", 1, "Ime", 1, 1);

namespace Ime {

// Soft keyboard / IME bootstrap used by Gen5 titles during early UI setup.
// The keyboard API delivers host key edges, while the user has text entry on,
// to the handler the title registered at open.

// sceImeKeyboardOpen param, as titles fill it: the handler gets (arg, event).
struct ImeKeyboardParam
{
	uint32_t option;
	uint32_t reserved1;
	uint64_t arg;
	uint64_t handler;
	uint64_t reserved2;
};
static_assert(sizeof(ImeKeyboardParam) == 0x20);

// Key event as the handler reads it: the HID usage indexes its keycode table and a
// nonzero character is appended to the typed text.
struct ImeKeyboardEvent
{
	uint32_t id;
	uint32_t reserved0;
	uint16_t keycode;
	uint16_t character;
	uint32_t status;
	uint8_t  reserved1[0x70];
};

constexpr uint32_t IME_EVENT_KEYCODE_DOWN   = 0x101;
constexpr uint32_t IME_EVENT_KEYCODE_UP     = 0x102;
constexpr uint32_t IME_EVENT_KEYCODE_REPEAT = 0x103;

// Opaque handle of the one keyboard; titles pass it back to sceImeKeyboardGetInfo.
constexpr uint32_t IME_KEYBOARD_RESOURCE_ID = 1;

struct ImeKeyboardState
{
	bool     open    = false;
	int      user_id = 0;
	uint64_t arg     = 0;
	uint64_t handler = 0;
};

static std::mutex       g_mutex;
static ImeKeyboardState g_keyboard;

// sceImeKeyboardOpen — NID eaFXjfJv3xs (user_id, param)
static int KYTY_SYSV_ABI ImeKeyboardOpen(int user_id, const ImeKeyboardParam* param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t user_id = %d\n", user_id);
	KYTY_LOG_DEBUG("\t param   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	if (param != nullptr && param->handler != 0)
	{
		std::lock_guard lock(g_mutex);
		g_keyboard.open    = true;
		g_keyboard.user_id = user_id;
		g_keyboard.arg     = param->arg;
		g_keyboard.handler = param->handler;
		::Kyty::Emulator::Ports::KeyboardTextPort::SetGuestOpen(true);
	}
	return OK;
}

// sceImeKeyboardClose — NID PMVehSlfZ94
static int KYTY_SYSV_ABI ImeKeyboardClose(int user_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t user_id = %d\n", user_id);
	std::lock_guard lock(g_mutex);
	if (g_keyboard.open && g_keyboard.user_id == user_id)
	{
		g_keyboard = {};
		::Kyty::Emulator::Ports::KeyboardTextPort::SetGuestOpen(false);
	}
	return OK;
}

// sceImeKeyboardGetResourceId — NID dKadqZFgKKQ
// Fills {user_id, resource_id[5]}; titles read resource_id[0] at +4.
static int KYTY_SYSV_ABI ImeKeyboardGetResourceId(int user_id, void* resource_id_array)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t user_id           = %d\n", user_id);
	KYTY_LOG_DEBUG("\t resource_id_array = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(resource_id_array));
	std::lock_guard lock(g_mutex);
	if (resource_id_array != nullptr && g_keyboard.open && g_keyboard.user_id == user_id)
	{
		auto* out = static_cast<uint8_t*>(resource_id_array);
		std::memcpy(out, &user_id, sizeof(user_id));
		std::memcpy(out + 4, &IME_KEYBOARD_RESOURCE_ID, sizeof(IME_KEYBOARD_RESOURCE_ID));
	}
	return OK;
}

// sceImeKeyboardGetInfo — NID VkqLPArfFdc
static int KYTY_SYSV_ABI ImeKeyboardGetInfo(uint32_t resource_id, void* info)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t resource_id = 0x%08" PRIx32 "\n", resource_id);
	KYTY_LOG_DEBUG("\t info        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(info));
	// The info is left as the title passed it: no field of it is known to the titles' input paths.
	return OK;
}

// sceImeKeyboardSetMode — NID ua+13Hk9kKs
static int KYTY_SYSV_ABI ImeKeyboardSetMode(int user_id, uint32_t mode)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t user_id = %d\n", user_id);
	KYTY_LOG_DEBUG("\t mode    = 0x%08" PRIx32 "\n", mode);
	return OK;
}

// sceImeUpdate — NID -4GCfYdNF1s
// Runs on the title's thread; delivers the queued keyboard events to the keyboard handler.
static int KYTY_SYSV_ABI ImeUpdate(void* handler)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handler = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handler));

	uint64_t keyboard_handler = 0;
	uint64_t keyboard_arg     = 0;
	{
		std::lock_guard lock(g_mutex);
		// A running IME dialog takes the keys instead.
		if (!g_keyboard.open || ::Kyty::Emulator::Ports::KeyboardTextPort::DialogOpen())
		{
			return OK;
		}
		keyboard_handler = g_keyboard.handler;
		keyboard_arg     = g_keyboard.arg;
	}

	::Kyty::Emulator::Ports::KeyboardTextKey keys[::Kyty::Emulator::Ports::KeyboardTextPort::QUEUE_MAX];
	const size_t num = ::Kyty::Emulator::Ports::KeyboardTextPort::Drain(keys, ::Kyty::Emulator::Ports::KeyboardTextPort::QUEUE_MAX);
	for (size_t i = 0; i < num; i++)
	{
		ImeKeyboardEvent event {};
		event.id        = !keys[i].down ? IME_EVENT_KEYCODE_UP : (keys[i].repeat ? IME_EVENT_KEYCODE_REPEAT : IME_EVENT_KEYCODE_DOWN);
		event.keycode   = keys[i].usage;
		event.character = keys[i].character;
		(void)::Kyty::Emulator::GuestRuntimePort::Invoke(keyboard_handler, keyboard_arg, reinterpret_cast<uint64_t>(&event), 0);
	}
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
