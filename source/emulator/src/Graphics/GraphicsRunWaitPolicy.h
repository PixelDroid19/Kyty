#ifndef EMULATOR_SRC_GRAPHICS_GRAPHICSRUNWAITPOLICY_H_
#define EMULATOR_SRC_GRAPHICS_GRAPHICSRUNWAITPOLICY_H_

#include <cstdint>

namespace Kyty::Libs::Graphics {

// An optional host diagnostic deadline, never permission to skip a guest wait.
// Missing or invalid input disables the deadline.
uint64_t ParseSuspendedWaitTimeoutMs(const char* value);

constexpr bool SuspendedWaitDiagnosticDeadlineReached(uint64_t now_ns, uint64_t blocked_since_ns, uint64_t timeout_ns)
{
	return timeout_ns != 0 && blocked_since_ns != 0 && now_ns >= blocked_since_ns && now_ns - blocked_since_ns >= timeout_ns;
}

} // namespace Kyty::Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_GRAPHICSRUNWAITPOLICY_H_
