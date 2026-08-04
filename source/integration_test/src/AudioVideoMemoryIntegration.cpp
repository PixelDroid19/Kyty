#include "Emulator/VideoFrameMemory.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace VideoFrameMemory = Kyty::Emulator::VideoFrameMemory;

namespace {

struct CallbackLog
{
	uint32_t register_count   = 0;
	uint32_t unregister_count = 0;
	uint32_t write_count      = 0;
	uint64_t register_base    = 0;
	size_t   register_size    = 0;
	uint32_t register_pitch   = 0;
	uint64_t unregister_base  = 0;
	uint64_t write_base       = 0;
	uint64_t write_size       = 0;
};

CallbackLog g_log {};

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "audio video memory integration failed: %s\n", message);
	}
	return condition;
}

void RecordLinearFrame(uint64_t base, size_t size, uint32_t pitch)
{
	g_log.register_count++;
	g_log.register_base  = base;
	g_log.register_size  = size;
	g_log.register_pitch = pitch;
}

void RecordUnregisterFrame(uint64_t base)
{
	g_log.unregister_count++;
	g_log.unregister_base = base;
}

void RecordHostWrite(uint64_t base, uint64_t size)
{
	g_log.write_count++;
	g_log.write_base = base;
	g_log.write_size = size;
}

bool CallbackContract()
{
	constexpr uint64_t frame_base = 0x0000'0000'0010'0000ull;
	constexpr size_t   frame_size = 0x3000u;
	constexpr uint32_t frame_pitch = 1920u;
	constexpr uint64_t write_base = frame_base + 0x80u;
	constexpr uint64_t write_size = 512u;

	if (!Check(VideoFrameMemory::InstallCallbacks({}), "could not establish no-op callbacks"))
	{
		return false;
	}

	VideoFrameMemory::RegisterLinearFrame(frame_base, frame_size, frame_pitch);
	VideoFrameMemory::UnregisterFrame(frame_base);
	VideoFrameMemory::NotifyHostWrite(write_base, write_size);
	if (!Check(g_log.register_count == 0 && g_log.unregister_count == 0 && g_log.write_count == 0,
	           "callbacks ran before a complete installation"))
	{
		return false;
	}

	const VideoFrameMemory::Callbacks partial {&RecordLinearFrame, nullptr, nullptr};
	if (!Check(!VideoFrameMemory::InstallCallbacks(partial), "partial callback installation was accepted"))
	{
		return false;
	}
	VideoFrameMemory::RegisterLinearFrame(frame_base, frame_size, frame_pitch);
	if (!Check(g_log.register_count == 0, "rejected partial callback installation changed dispatch"))
	{
		return false;
	}

	const VideoFrameMemory::Callbacks callbacks {&RecordLinearFrame, &RecordUnregisterFrame, &RecordHostWrite};
	if (!Check(VideoFrameMemory::InstallCallbacks(callbacks), "complete callback installation was rejected"))
	{
		return false;
	}

	VideoFrameMemory::RegisterLinearFrame(frame_base, frame_size, frame_pitch);
	VideoFrameMemory::UnregisterFrame(frame_base);
	VideoFrameMemory::NotifyHostWrite(write_base, write_size);
	if (!Check(g_log.register_count == 1 && g_log.register_base == frame_base && g_log.register_size == frame_size &&
	               g_log.register_pitch == frame_pitch,
	           "linear-frame callback did not receive its exact range"))
	{
		return false;
	}
	if (!Check(g_log.unregister_count == 1 && g_log.unregister_base == frame_base,
	           "unregister callback did not receive its exact base"))
	{
		return false;
	}
	if (!Check(g_log.write_count == 1 && g_log.write_base == write_base && g_log.write_size == write_size,
	           "host-write callback did not receive its exact range"))
	{
		return false;
	}

	if (!Check(VideoFrameMemory::InstallCallbacks({}), "could not restore no-op callbacks"))
	{
		return false;
	}
	VideoFrameMemory::RegisterLinearFrame(frame_base, frame_size, frame_pitch);
	VideoFrameMemory::UnregisterFrame(frame_base);
	VideoFrameMemory::NotifyHostWrite(write_base, write_size);
	return Check(g_log.register_count == 1 && g_log.unregister_count == 1 && g_log.write_count == 1,
	             "callbacks ran after restoring no-op dispatch");
}

} // namespace

int main()
{
	if (!CallbackContract())
	{
		return 1;
	}
	std::puts("audio video memory integration passed");
	return 0;
}
