#include "KeyNames.h"

#include <QKeyEvent>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <iterator>

// The tables SDL itself reads keys with, so a key bound here is the key the emulator sees.
#if defined(Q_OS_WIN)
#include "scancodes_windows.h"
#elif defined(Q_OS_MACOS)
#include "scancodes_darwin.h"
#else
#include "scancodes_linux.h"
#endif

namespace {

template <size_t N>
SDL_Scancode FromTable(const SDL_Scancode (&table)[N], quint32 index)
{
	return index < N ? table[index] : SDL_SCANCODE_UNKNOWN;
}

SDL_Scancode ScancodeOf(const QKeyEvent* event)
{
#if defined(Q_OS_WIN)
	// Qt keeps the scan code's low byte and the extended-key flag as bit 8; SDL packs that flag into bit 7.
	const quint32 code  = event->nativeScanCode();
	const quint32 index = (code & 0x7f) | ((code & 0x100) != 0 ? 0x80 : 0);
	// Pause sends the scan code of Num Lock without the extended flag.
	return code == 0x45 ? SDL_SCANCODE_PAUSE : FromTable(windows_scancode_table, index);
#elif defined(Q_OS_MACOS)
	return event->nativeScanCode() == 0 && event->nativeVirtualKey() == 0 ? SDL_SCANCODE_UNKNOWN
	                                                                        : FromTable(darwin_scancode_table, event->nativeVirtualKey());
#else
	// X11 and Wayland key codes are the kernel's plus 8.
	const quint32 code = event->nativeScanCode();
	return code < 8 ? SDL_SCANCODE_UNKNOWN : FromTable(linux_scancode_table, code - 8);
#endif
}

} // namespace

QString KeyNameFromEvent(const QKeyEvent* event)
{
	const auto code = ScancodeOf(event);
	if (code == SDL_SCANCODE_UNKNOWN)
	{
		return {};
	}
	return QString::fromUtf8(SDL_GetScancodeName(code));
}
