#ifndef EMULATOR_INCLUDE_EMULATOR_VIDEO_FRAME_MEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_VIDEO_FRAME_MEMORY_H_

#include <cstddef>
#include <cstdint>

namespace Kyty::Emulator::VideoFrameMemory {

// This boundary transports frame metadata only. It never grants callers
// access to host memory or graphics-owned handles.
using RegisterLinearFrameCallback = void (*)(uint64_t base, size_t size, uint32_t row_pitch_bytes);
using UnregisterFrameCallback     = void (*)(uint64_t base);
using NotifyHostWriteCallback     = void (*)(uint64_t base, uint64_t size);

struct Callbacks
{
	RegisterLinearFrameCallback register_linear_frame = nullptr;
	UnregisterFrameCallback     unregister_frame      = nullptr;
	NotifyHostWriteCallback     notify_host_write     = nullptr;
};

// Installs a complete callback bundle, or an empty bundle to restore no-op
// dispatch. Partial bundles are rejected and leave the current bundle intact.
// Dispatch copies the bundle under its internal lock and invokes it after
// releasing the lock, so installed function targets must remain valid for the
// emulator process lifetime.
bool InstallCallbacks(const Callbacks& callbacks);

void RegisterLinearFrame(uint64_t base, size_t size, uint32_t row_pitch_bytes);
void UnregisterFrame(uint64_t base);
void NotifyHostWrite(uint64_t base, uint64_t size);

} // namespace Kyty::Emulator::VideoFrameMemory

#endif /* EMULATOR_INCLUDE_EMULATOR_VIDEO_FRAME_MEMORY_H_ */
