#ifndef EMULATOR_INCLUDE_EMULATOR_VIDEO_FRAME_MEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_VIDEO_FRAME_MEMORY_H_

#include <cstddef>
#include <cstdint>

namespace Kyty::Core::VirtualMemory {
struct GuestMappingIdentitySnapshot;
}

namespace Kyty::Emulator::VideoFrameMemory {

// This boundary transports frame metadata only. It never grants callers
// access to host memory or graphics-owned handles.
// The mapping identity is valid for the callback duration; retain a value copy if needed later.
using RegisterLinearFrameCallback = bool (*)(uint64_t base, size_t size, uint32_t row_pitch_bytes,
                                             const Core::VirtualMemory::GuestMappingIdentitySnapshot& identity);
using UnregisterFrameCallback     = void (*)(uint64_t base);
using PruneStaleLayoutsCallback   = void (*)();
// Called immediately before a host/HLE operation writes guest memory. The
// graphics implementation may need to restore write permission on pages that
// are temporarily protected for dirty tracking.
using NotifyHostWriteCallback     = void (*)(uint64_t base, uint64_t size);
using BeginHostWriteCallback      = uint64_t (*)(uint64_t base, uint64_t size);
using EndHostWriteCallback        = void (*)(uint64_t token);

struct Callbacks
{
	RegisterLinearFrameCallback register_linear_frame = nullptr;
	UnregisterFrameCallback     unregister_frame      = nullptr;
	NotifyHostWriteCallback     notify_host_write     = nullptr;
	BeginHostWriteCallback      begin_host_write      = nullptr;
	EndHostWriteCallback        end_host_write        = nullptr;
	PruneStaleLayoutsCallback   prune_stale_layouts   = nullptr;
};

// Installs a complete callback bundle, or an empty bundle to restore no-op
// dispatch. Partial bundles are rejected and leave the current bundle intact.
// Dispatch copies the bundle under its internal lock and invokes it after
// releasing the lock, so installed function targets must remain valid for the
// emulator process lifetime.
bool InstallCallbacks(const Callbacks& callbacks);

// Empty callback bundles preserve CPU-only no-consumer behavior and return true.
// With a registry installed, invalid or unmapped ranges and rejected admission return false.
bool RegisterLinearFrame(uint64_t base, size_t size, uint32_t row_pitch_bytes);
// Transaction callers pass the identity captured while holding the VM lock;
// this overload validates only its range fields and never re-enters VM state.
bool RegisterLinearFrame(uint64_t base, size_t size, uint32_t row_pitch_bytes,
                         const Core::VirtualMemory::GuestMappingIdentitySnapshot& identity);
// Dispatches the optional layout-pruning callback after releasing the callback lock.
void PruneStaleLayouts();
void UnregisterFrame(uint64_t base);
// The requested range may exceed the bytes eventually written. Conservatively
// marking that range dirty is preferable to letting host I/O fail on a watched
// read-only destination.
void NotifyHostWrite(uint64_t base, uint64_t size);

// Owns artificial write-watch permission through a blocking host operation.
// Construct before waiting for the file/decoder lock; keep alive through every
// transfer and error exit. Both acquisition and destruction preserve errno.
// The paired callbacks are optional only for clients without a write tracker.
class HostWriteLease final
{
public:
	HostWriteLease(uint64_t base, uint64_t size);
	~HostWriteLease();
	HostWriteLease(const HostWriteLease&) = delete;
	HostWriteLease& operator=(const HostWriteLease&) = delete;

private:
	uint64_t m_token = 0;
	EndHostWriteCallback m_end = nullptr;
};

} // namespace Kyty::Emulator::VideoFrameMemory

#endif /* EMULATOR_INCLUDE_EMULATOR_VIDEO_FRAME_MEMORY_H_ */
