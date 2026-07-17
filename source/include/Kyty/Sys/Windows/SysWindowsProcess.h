#ifndef INCLUDE_KYTY_SYS_WINDOWS_SYSPROCESS_H_
#define INCLUDE_KYTY_SYS_WINDOWS_SYSPROCESS_H_

// IWYU pragma: private

#include "Kyty/Core/Common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
//#error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#include <cstdint>

namespace Kyty {

struct SysProcessSample
{
	double   cpu_percent = 0.0;
	uint64_t heap_bytes  = 0;
	bool     valid       = false;
};

void SysProcessSampleReset();
SysProcessSample SysProcessSampleNow();

} // namespace Kyty

#endif

#endif /* INCLUDE_KYTY_SYS_WINDOWS_SYSPROCESS_H_ */
