#ifndef LAUNCHER_INCLUDE_KEY_NAMES_H_
#define LAUNCHER_INCLUDE_KEY_NAMES_H_

#include "Common.h"

#include <QString>

class QKeyEvent;

// The input profile name of the physical key behind event (SDL's scancode name, "Left Shift", "A", ...), so the
// emulator finds the same key whatever the keyboard layout. Empty when the event has no physical key, such as one
// a controller sent.
[[nodiscard]] QString KeyNameFromEvent(const QKeyEvent* event);

#endif /* LAUNCHER_INCLUDE_KEY_NAMES_H_ */
