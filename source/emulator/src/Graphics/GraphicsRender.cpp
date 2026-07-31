// GraphicsRender was modularized. See GraphicsRenderInternal.h for the module map.
// This translation unit remains as the stable landing point for the subsystem;
// implementations live in the domain-specific GraphicsRender*.cpp modules.

#include "Emulator/Graphics/GraphicsRender.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Intentionally empty — implementations are in GraphicsRender{Core,HwCheck,Context,
// Framebuffer,Pipeline,Descriptor,Attachments,Bind,Draw,Eop,CommandBuffer}.cpp

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
