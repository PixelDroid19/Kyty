#pragma once

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class ShaderCode;
struct GraphicContext;

namespace FragmentTransport {

// True when the program moves data between lanes beyond a four-lane quad
// (DPP row controls or PERMLANE). The native fragment emitter cannot express
// those on a host subgroup narrower than the guest wave; quad-local DPP and
// ordinary wave-uniform code stay on the native path.
[[nodiscard]] bool ProgramRequiresWaveTransport(const ShaderCode& code);

// The first host capability the capture, shade and resolve phases need but the
// context lacks or never enabled, or nullptr when all are available. The text
// names the capability for a strict-mode diagnostic.
[[nodiscard]] const char* MissingHostCapability(const GraphicContext& context);

} // namespace FragmentTransport
} // namespace Kyty::Libs::Graphics

#endif
