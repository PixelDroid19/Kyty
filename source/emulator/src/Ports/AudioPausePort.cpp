#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Ports/AudioPausePort.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Ports {

namespace {

std::atomic<void (*)(bool)> g_set_host_paused {nullptr};

} // namespace

void AudioPausePort::Install(void (*set_host_paused)(bool))
{
	g_set_host_paused.store(set_host_paused);
}

void AudioPausePort::SetHostPaused(bool paused)
{
	auto set_host_paused = g_set_host_paused.load();
	if (set_host_paused != nullptr)
	{
		set_host_paused(paused);
	}
}

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED
