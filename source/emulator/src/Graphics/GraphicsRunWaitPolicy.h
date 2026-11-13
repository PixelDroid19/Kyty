#ifndef EMULATOR_SRC_GRAPHICS_GRAPHICSRUNWAITPOLICY_H_
#define EMULATOR_SRC_GRAPHICS_GRAPHICSRUNWAITPOLICY_H_

#include <cstdint>

namespace Kyty::Libs::Graphics {

// Top-level memory waits of one PM4 buffer. A wait is "own" when an earlier packet of the same buffer writes
// the awaited address, so the buffer does not depend on any other submission to make progress.
struct BatchMemoryWaitScan
{
	uint32_t waits     = 0;
	uint32_t own       = 0;
	uint32_t nested    = 0; // indirect buffers; their content is not scanned
	uint32_t unparsed  = 0; // headers that cannot be sized; the packets after them are not known
};

// Walks the top level of a PM4 buffer (null or empty is a buffer without waits).
BatchMemoryWaitScan ScanBatchMemoryWaits(const uint32_t* words, uint32_t num_dw);

// True when no wait of the batch can depend on another submission: a graphics submit may then return before
// its own decode completes, because the guest cannot be the producer of anything the batch waits on.
// A buffer the scan could not fully walk is never self-contained.
bool BatchProducesItsOwnWaits(const BatchMemoryWaitScan& draw, const BatchMemoryWaitScan& constant);

// An optional host diagnostic deadline, never permission to skip a guest wait.
// Missing or invalid input disables the deadline.
uint64_t ParseSuspendedWaitTimeoutMs(const char* value);

constexpr bool SuspendedWaitDiagnosticDeadlineReached(uint64_t now_ns, uint64_t blocked_since_ns, uint64_t timeout_ns)
{
	return timeout_ns != 0 && blocked_since_ns != 0 && now_ns >= blocked_since_ns && now_ns - blocked_since_ns >= timeout_ns;
}

} // namespace Kyty::Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_GRAPHICSRUNWAITPOLICY_H_
