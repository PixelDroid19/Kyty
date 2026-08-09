#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/AudioHost.h"
#include "Emulator/Host/Clock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Kyty::Libs::Audio::HostAudio;
namespace HostClock = Kyty::Emulator::HostClock;

namespace {

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "audio host integration failed: %s\n", message);
	}
	return condition;
}

std::shared_ptr<HostAudio> CreateAudio()
{
	std::string error;
	auto        audio = HostAudio::Create(&error);
	if (audio == nullptr)
	{
		std::fprintf(stderr, "audio host integration failed to initialize SDL: %s\n", error.c_str());
	}
	return audio;
}

bool HostClockContract()
{
	constexpr uint64_t sleep_microseconds       = 2'000;
	constexpr uint64_t max_elapsed_microseconds = 100'000;
	const uint64_t     first                    = HostClock::NowMicroseconds();
	const uint64_t     second                   = HostClock::NowMicroseconds();
	const uint64_t     deadline                 = second + sleep_microseconds;
	HostClock::SleepUntil(deadline);
	const uint64_t after = HostClock::NowMicroseconds();
	return Check(second >= first, "host clock moved backwards") &&
	       Check(after >= deadline, "host clock sleep returned before its deadline") &&
	       Check(after - first < max_elapsed_microseconds, "host clock sleep exceeded its bounded duration");
}

bool PeriodicIntervalContract()
{
	constexpr uint32_t samples           = 256;
	constexpr uint32_t frequency         = 44'100;
	constexpr uint32_t intervals         = 441;
	uint64_t           remainder         = 0;
	uint64_t           invalid_remainder = 17;
	uint64_t           total             = 0;
	for (uint32_t i = 0; i < intervals; i++)
	{
		total += HostClock::NextPeriodicIntervalMicroseconds(samples, frequency, &remainder);
	}
	constexpr uint64_t expected = (1'000'000ull * samples * intervals) / frequency;
	return Check(HostClock::IsPeriodicIntervalValid(1, 1), "maximum periodic interval was rejected") &&
	       Check(HostClock::IsPeriodicIntervalValid(4'800, 48'000), "known 48 kHz interval was rejected") &&
	       Check(HostClock::IsPeriodicIntervalValid(samples, frequency), "known 44.1 kHz interval was rejected") &&
	       Check(!HostClock::IsPeriodicIntervalValid(0, frequency), "zero-sample interval was accepted") &&
	       Check(!HostClock::IsPeriodicIntervalValid(samples, 0), "zero-frequency interval was accepted") &&
	       Check(!HostClock::IsPeriodicIntervalValid(std::numeric_limits<uint32_t>::max(), 1), "unbounded interval was accepted") &&
	       Check(total == expected, "fractional periodic intervals accumulated drift") &&
	       Check(remainder == 0, "fractional periodic interval retained an unexpected remainder") &&
	       Check(HostClock::NextPeriodicIntervalMicroseconds(0, frequency, &invalid_remainder) == 0,
	             "zero-sample periodic interval was accepted") &&
	       Check(HostClock::NextPeriodicIntervalMicroseconds(samples, 0, &invalid_remainder) == 0,
	             "zero-frequency periodic interval was accepted") &&
	       Check(HostClock::NextPeriodicIntervalMicroseconds(samples, frequency, nullptr) == 0,
	             "null periodic interval remainder was accepted") &&
	       Check(invalid_remainder == 17, "invalid periodic interval modified its remainder");
}

bool GrainPacing()
{
	auto audio = CreateAudio();
	if (audio == nullptr)
	{
		return false;
	}
	constexpr uint32_t samples = 480;
	auto               port    = audio->AudioOutOpen(0, samples, 48000, HostAudio::Format::Signed16bitStereo);
	if (!Check(port.IsValid(), "could not open the pacing port"))
	{
		return false;
	}
	std::vector<int16_t>   pcm(samples * 2, 0);
	HostAudio::OutputParam param {port, pcm.data()};
	const auto             start = std::chrono::steady_clock::now();
	for (int i = 0; i < 12; i++)
	{
		uint32_t written = 0;
		if (!Check(audio->AudioOutOutputs(&param, 1, &written), "grain output failed") ||
		    !Check(written == samples, "grain returned the wrong sample count"))
		{
			return false;
		}
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;
	return Check(elapsed >= std::chrono::milliseconds(100), "grains were delivered in a burst") &&
	       Check(elapsed < std::chrono::milliseconds(300), "grain pacing accumulated excessive delay") &&
	       Check(audio->AudioOutClose(port), "could not close the pacing port");
}

bool FractionalOutputPacing()
{
	auto audio = CreateAudio();
	if (audio == nullptr)
	{
		return false;
	}
	constexpr uint32_t samples = 256;
	auto               port    = audio->AudioOutOpen(0, samples, 44'100, HostAudio::Format::Signed16bitStereo);
	if (!Check(port.IsValid(), "could not open the fractional pacing port"))
	{
		return false;
	}
	std::vector<int16_t>   pcm(samples * 2, 0);
	HostAudio::OutputParam param {port, pcm.data()};
	const auto             start = std::chrono::steady_clock::now();
	// Multiple fractional AudioOut calls consume the per-port carry in production.
	for (int i = 0; i < 3; i++)
	{
		uint32_t written = 0;
		if (!Check(audio->AudioOutOutputs(&param, 1, &written), "fractional grain output failed") ||
		    !Check(written == samples, "fractional grain returned the wrong sample count"))
		{
			return false;
		}
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;
	return Check(elapsed >= std::chrono::milliseconds(12), "fractional output grains were delivered in a burst") &&
	       Check(elapsed < std::chrono::milliseconds(100), "fractional output pacing accumulated excessive delay") &&
	       Check(audio->AudioOutClose(port), "could not close the fractional pacing port");
}

bool FractionalInputPacing()
{
	auto audio = CreateAudio();
	if (audio == nullptr)
	{
		return false;
	}
	constexpr uint32_t samples = 256;
	auto               port    = audio->AudioInOpen(0, samples, 44'100, HostAudio::Format::Signed16bitStereo);
	if (!Check(port.IsValid(), "could not open the input pacing port") || !Check(audio->AudioInValid(port), "input pacing port is invalid"))
	{
		return false;
	}
	std::vector<int16_t> pcm(samples * 2, 0);
	const auto           start = std::chrono::steady_clock::now();
	// Keep two production AudioIn calls on the fractional host pacing path.
	const uint32_t first   = audio->AudioInInput(port, pcm.data());
	const uint32_t second  = audio->AudioInInput(port, pcm.data());
	const auto     elapsed = std::chrono::steady_clock::now() - start;
	return Check(first == samples, "first input grain returned the wrong sample count") &&
	       Check(second == samples, "second input grain returned the wrong sample count") &&
	       Check(elapsed >= std::chrono::milliseconds(3), "input grains were delivered in a burst") &&
	       Check(elapsed < std::chrono::milliseconds(100), "input grain pacing accumulated excessive delay");
}

bool InputPortLifecycle()
{
	auto audio = CreateAudio();
	if (audio == nullptr)
	{
		return false;
	}
	for (int iteration = 0; iteration < HostAudio::IN_PORTS_MAX * 2; ++iteration)
	{
		auto port = audio->AudioInOpen(0, 256, 48'000, HostAudio::Format::Signed16bitStereo);
		if (!Check(port.IsValid(), "input port slots were exhausted after close") ||
		    !Check(audio->AudioInClose(port), "could not close the input port") ||
		    !Check(!audio->AudioInValid(port), "closed input port remained valid"))
		{
			return false;
		}
	}
	return true;
}

bool RejectsInvalidPacing()
{
	auto audio = CreateAudio();
	if (audio == nullptr)
	{
		return false;
	}
	constexpr uint32_t extreme     = std::numeric_limits<uint32_t>::max();
	const auto         start       = std::chrono::steady_clock::now();
	const auto         zero_output = audio->AudioOutOpen(0, 0, 48'000, HostAudio::Format::Signed16bitStereo);
	const auto         zero_input  = audio->AudioInOpen(0, 48'000, 0, HostAudio::Format::Signed16bitStereo);
	const auto         output      = audio->AudioOutOpen(0, extreme, 1, HostAudio::Format::Signed16bitStereo);
	const auto         input       = audio->AudioInOpen(0, extreme, 1, HostAudio::Format::Signed16bitStereo);
	const auto         elapsed     = std::chrono::steady_clock::now() - start;
	return Check(!zero_output.IsValid(), "zero-sample output pacing port was accepted") &&
	       Check(!zero_input.IsValid(), "zero-frequency input pacing port was accepted") &&
	       Check(!output.IsValid(), "unbounded output pacing port was accepted") &&
	       Check(!input.IsValid(), "unbounded input pacing port was accepted") &&
	       Check(elapsed < std::chrono::milliseconds(100), "unbounded pacing ports were not rejected quickly");
}

bool CloseWhileProducerSleeps()
{
	auto audio = CreateAudio();
	if (audio == nullptr)
	{
		return false;
	}
	constexpr uint32_t samples = 4800;
	auto               port    = audio->AudioOutOpen(0, samples, 48000, HostAudio::Format::Signed16bitStereo);
	if (!Check(port.IsValid(), "could not open the lifecycle port"))
	{
		return false;
	}
	std::vector<int16_t>   pcm(samples * 2, 0);
	HostAudio::OutputParam param {port, pcm.data()};
	std::atomic<bool>      entered {false};
	std::atomic<bool>      output_ok {false};
	std::thread            producer(
	    [&]
	    {
		    entered.store(true, std::memory_order_release);
		    uint32_t written = 0;
		    output_ok.store(audio->AudioOutOutputs(&param, 1, &written) && written == samples, std::memory_order_release);
	    });
	while (!entered.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(15));
	const bool closed = audio->AudioOutClose(port);
	producer.join();
	uint32_t written = 0;
	return Check(closed, "concurrent close failed") && Check(output_ok.load(std::memory_order_acquire), "in-flight grain failed") &&
	       Check(!audio->AudioOutOutputs(&param, 1, &written), "closed port accepted another grain");
}

} // namespace

int main(int argc, char** argv)
{
	constexpr const char* output_path = "/tmp/kyty-audio-host-integration.raw";
	std::remove(output_path);
	auto* subsystems = Kyty::Core::SubsystemsListSingleton::Instance();
	subsystems->SetArgs(argc, argv);
	auto* core    = Kyty::Core::CoreSubsystem::Instance();
	auto* threads = Kyty::Core::ThreadsSubsystem::Instance();
	subsystems->Add(core, {});
	subsystems->Add(threads, {core});
	if (!subsystems->InitAll(false))
	{
		std::fprintf(stderr, "audio host integration failed to initialize host subsystems\n");
		return 125;
	}
	if (!HostClockContract() || !PeriodicIntervalContract() || !GrainPacing() || !FractionalOutputPacing() || !FractionalInputPacing() ||
	    !InputPortLifecycle() || !RejectsInvalidPacing() || !CloseWhileProducerSleeps())
	{
		std::remove(output_path);
		return 1;
	}
	std::remove(output_path);
	std::puts("audio host integration passed");
	return 0;
}
