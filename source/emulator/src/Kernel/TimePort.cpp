#include "Emulator/Kernel/TimePort.h"

#include "Kyty/Core/Timer.h"

#include <atomic>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::TimePort {
namespace {

Core::Timer g_fallback_timer;
std::once_flag g_fallback_timer_once;

void EnsureFallbackTimerStarted()
{
	std::call_once(g_fallback_timer_once, []() { g_fallback_timer.Start(); });
}

double FallbackGetTimeMs()
{
	EnsureFallbackTimerStarted();
	return g_fallback_timer.GetTimeMs();
}

uint64_t FallbackGetCounter()
{
	EnsureFallbackTimerStarted();
	return g_fallback_timer.GetTicks();
}

uint64_t FallbackGetFrequency()
{
	EnsureFallbackTimerStarted();
	return g_fallback_timer.GetFrequency();
}

std::atomic<GetTimeMsFunction>    g_get_time_ms {FallbackGetTimeMs};
std::atomic<GetCounterFunction>   g_get_counter {FallbackGetCounter};
std::atomic<GetFrequencyFunction> g_get_frequency {FallbackGetFrequency};

} // namespace

void Install(const Provider& provider) noexcept
{
	g_get_time_ms.store(provider.get_time_ms != nullptr ? provider.get_time_ms : FallbackGetTimeMs, std::memory_order_release);
	g_get_counter.store(provider.get_counter != nullptr ? provider.get_counter : FallbackGetCounter, std::memory_order_release);
	g_get_frequency.store(provider.get_frequency != nullptr ? provider.get_frequency : FallbackGetFrequency,
	                      std::memory_order_release);
}

double GetTimeMs() noexcept
{
	return g_get_time_ms.load(std::memory_order_acquire)();
}

uint64_t GetProcessTimeUs() noexcept
{
	return static_cast<uint64_t>(GetTimeMs() * 1000.0);
}

Core::Time GetTime() noexcept
{
	return Core::Time(static_cast<int>(GetTimeMs()));
}

uint64_t GetCounter() noexcept
{
	return g_get_counter.load(std::memory_order_acquire)();
}

uint64_t GetFrequency() noexcept
{
	return g_get_frequency.load(std::memory_order_acquire)();
}

} // namespace Kyty::Kernel::TimePort

#endif // KYTY_EMU_ENABLED
