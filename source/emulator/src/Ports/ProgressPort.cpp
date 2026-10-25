#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Ports/ProgressPort.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Ports {

namespace {

// All entry points live in one installed table so a lane never sees a begin
// without the matching end implementation.
std::atomic<const ProgressPort::Sink*> g_sink {nullptr};
ProgressPort::Sink                     g_installed {};

} // namespace

void ProgressPort::Install(Sink sink)
{
	if (sink.begin == nullptr || sink.end == nullptr || sink.begin_phase == nullptr || sink.end_phase == nullptr ||
	    sink.begin_wait == nullptr || sink.end_wait == nullptr)
	{
		g_sink.store(nullptr);
		return;
	}
	g_installed = sink;
	g_sink.store(&g_installed);
}

void ProgressPort::Begin(ProgressLane lane)
{
	const auto* sink = g_sink.load(std::memory_order_acquire);
	if (sink != nullptr)
	{
		sink->begin(lane);
	}
}

void ProgressPort::End(ProgressLane lane)
{
	const auto* sink = g_sink.load(std::memory_order_acquire);
	if (sink != nullptr)
	{
		sink->end(lane);
	}
}

void ProgressPort::BeginCommandPhase(CommandPhase phase)
{
	const auto* sink = g_sink.load(std::memory_order_acquire);
	if (sink != nullptr)
	{
		sink->begin_phase(phase);
	}
}

void ProgressPort::EndCommandPhase()
{
	const auto* sink = g_sink.load(std::memory_order_acquire);
	if (sink != nullptr)
	{
		sink->end_phase();
	}
}

void ProgressPort::BeginGuestWait(GuestWait wait)
{
	const auto* sink = g_sink.load(std::memory_order_acquire);
	if (sink != nullptr)
	{
		sink->begin_wait(wait);
	}
}

void ProgressPort::EndGuestWait()
{
	const auto* sink = g_sink.load(std::memory_order_acquire);
	if (sink != nullptr)
	{
		sink->end_wait();
	}
}

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED
