#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_THREADINSTANCEALLOCATOR_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_THREADINSTANCEALLOCATOR_H_

#include <atomic>
#include <cstdint>

namespace Kyty::DevTools {

// Allocates process-lifetime diagnostic thread identities without wrapping the
// 56-bit instance field used by ProgressRef.
class ThreadInstanceAllocator
{
public:
	static constexpr uint64_t kMaxInstance = (uint64_t{1} << 56u) - 1u;

	explicit ThreadInstanceAllocator(uint64_t next_instance = 1u) noexcept;

	ThreadInstanceAllocator(const ThreadInstanceAllocator&)            = delete;
	ThreadInstanceAllocator& operator=(const ThreadInstanceAllocator&) = delete;

	[[nodiscard]] bool Allocate(uint64_t* out) noexcept;

private:
	std::atomic<uint64_t> next_instance_;
};

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_THREADINSTANCEALLOCATOR_H_ */
