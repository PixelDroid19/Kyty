#ifndef EMULATOR_INCLUDE_EMULATOR_PORTS_AUDIOPAUSEPORT_H_
#define EMULATOR_INCLUDE_EMULATOR_PORTS_AUDIOPAUSEPORT_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

// Neutral contract between the host window (pause on focus loss) and the
// guest audio HLE. The composition root installs the HLE implementation;
// callers before install are safe no-ops.
namespace Kyty::Emulator::Ports {

class AudioPausePort final
{
public:
	static void Install(void (*set_host_paused)(bool));
	static void SetHostPaused(bool paused);
};

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_PORTS_AUDIOPAUSEPORT_H_ */
