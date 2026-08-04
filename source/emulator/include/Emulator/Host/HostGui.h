#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_HOSTGUI_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_HOSTGUI_H_

#include "Kyty/Core/Common.h"

namespace Kyty::Emulator::Host {

// The host GUI adapter owns the window-system side of ImGui. Graphics only
// asks it to initialize, consume an opaque native event, and start a frame.
[[nodiscard]] bool HostGuiInit(void* native_window);
void               HostGuiShutdown();
void               HostGuiProcessEvent(const void* native_event);
void               HostGuiNewFrame();

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_HOSTGUI_H_ */
