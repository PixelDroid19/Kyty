#ifndef INCLUDE_KYTY_SYS_LINUX_SYSPROCESS_H_
#define INCLUDE_KYTY_SYS_LINUX_SYSPROCESS_H_

// IWYU pragma: private

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
//#error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include <cstdint>

namespace Kyty {

// Host-only process sample for the debug HUD. Never used for guest semantics.
struct SysProcessSample
{
	double   cpu_percent  = 0.0; // one-core equivalent; may exceed 100 on multi-threaded hosts
	uint64_t heap_bytes   = 0;   // resident set size (RSS / working set)
	bool     valid        = false;
};

// Sample process CPU time and RSS. Call periodically from the window thread.
// First call after Reset seeds baselines and returns valid=false.
void SysProcessSampleReset();
SysProcessSample SysProcessSampleNow();

} // namespace Kyty

#endif

#endif /* INCLUDE_KYTY_SYS_LINUX_SYSPROCESS_H_ */
