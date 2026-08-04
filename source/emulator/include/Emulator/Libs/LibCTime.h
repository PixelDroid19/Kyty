#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBC_TIME_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBC_TIME_H_

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace Kyty::Libs::LibC::Time {

// Guest libc uses the 32-bit tm layout even when the host time_t is wider.
// Keep this ABI-shaped value in the time HLE instead of the general libc
// implementation so registration and host conversion can evolve separately.
struct GuestTm
{
	int32_t sec;
	int32_t min;
	int32_t hour;
	int32_t mday;
	int32_t mon;
	int32_t year;
	int32_t wday;
	int32_t yday;
	int32_t isdst;
};

static_assert(sizeof(GuestTm) == 36);

void HostToGuestTm(const std::tm& host, GuestTm* guest);
std::tm GuestToHostTm(const GuestTm& guest);

KYTY_SYSV_ABI int64_t c_time(int64_t* t);
KYTY_SYSV_ABI int64_t c_mktime(GuestTm* tmv);
KYTY_SYSV_ABI GuestTm* c_gmtime(const int64_t* t);
KYTY_SYSV_ABI GuestTm* c_gmtime_s(const int64_t* t, GuestTm* result);
KYTY_SYSV_ABI GuestTm* c_localtime(const int64_t* t);
KYTY_SYSV_ABI GuestTm* c_localtime_s(const int64_t* t, GuestTm* result);
KYTY_SYSV_ABI size_t c_strftime(char* s, size_t n, const char* f, const GuestTm* tmv);
KYTY_SYSV_ABI char* c_asctime(const GuestTm* tmv);

} // namespace Kyty::Libs::LibC::Time

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBC_TIME_H_ */
