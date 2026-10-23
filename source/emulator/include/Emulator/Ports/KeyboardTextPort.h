#ifndef EMULATOR_INCLUDE_EMULATOR_PORTS_KEYBOARDTEXTPORT_H_
#define EMULATOR_INCLUDE_EMULATOR_PORTS_KEYBOARDTEXTPORT_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef KYTY_EMU_ENABLED

// Neutral contract between the host keyboard (SDL events) and the guest IME
// HLE. The host pushes key edges while text entry is on; the HLE drains them on
// the guest thread: the IME dialog while one runs, otherwise the IME keyboard
// at sceImeUpdate. Keys pushed while neither is open are dropped.
namespace Kyty::Emulator::Ports {

struct KeyboardTextKey
{
	uint16_t usage     = 0; // USB HID keyboard usage
	uint16_t character = 0; // UTF-16 code unit the key types, 0 when it types none
	bool     down      = false;
	bool     repeat    = false;
};

class KeyboardTextPort final
{
public:
	// Bounded: a full queue drops the newest key edges.
	static constexpr size_t QUEUE_MAX = 128;

	static void SetGuestOpen(bool open);
	[[nodiscard]] static bool GuestOpen();

	// A running IME dialog takes every key; the host turns text entry on meanwhile.
	static void SetDialogOpen(bool open);
	[[nodiscard]] static bool DialogOpen();

	// Text of the running dialog, for the host to show the user.
	static void SetDialogText(const std::u16string& text);
	[[nodiscard]] static std::u16string DialogText();

	// False when the key was dropped.
	static bool Push(const KeyboardTextKey& key);

	// Moves up to max queued keys into out, oldest first, and returns how many.
	[[nodiscard]] static size_t Drain(KeyboardTextKey* out, size_t max);
};

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_PORTS_KEYBOARDTEXTPORT_H_ */
