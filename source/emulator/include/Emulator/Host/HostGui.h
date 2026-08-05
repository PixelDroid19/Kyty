#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_HOSTGUI_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_HOSTGUI_H_

#include "Kyty/Core/Common.h"

namespace Kyty::Emulator::Host {

class HostWindow;

// The host GUI adapter owns the window-system side of ImGui. Graphics only
// asks it to initialize from the host window, consume an opaque native event,
// and start a frame. SDL/window handles remain inside the adapter.
[[nodiscard]] bool HostGuiInit(const HostWindow* window);
void               HostGuiShutdown();
void               HostGuiProcessEvent(const void* native_event);
void               HostGuiNewFrame();

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_HOSTGUI_H_ */
