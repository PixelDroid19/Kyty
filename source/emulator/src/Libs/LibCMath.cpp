#include "Kyty/Core/Common.h"

#include "Emulator/Libs/Libs.h"

#include <cmath>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace LibC {

KYTY_SYSV_ABI double c_sin(double x)
{
	return ::sin(x);
}
KYTY_SYSV_ABI double c_cos(double x)
{
	return ::cos(x);
}
KYTY_SYSV_ABI double c_tan(double x)
{
	return ::tan(x);
}
KYTY_SYSV_ABI double c_tanh(double x)
{
	return std::tanh(x);
}
KYTY_SYSV_ABI double c_asin(double x)
{
	return ::asin(x);
}
KYTY_SYSV_ABI double c_acos(double x)
{
	return ::acos(x);
}
KYTY_SYSV_ABI double c_atan(double x)
{
	return ::atan(x);
}
KYTY_SYSV_ABI double c_atan2(double y, double x)
{
	return ::atan2(y, x);
}
KYTY_SYSV_ABI double c_exp(double x)
{
	return ::exp(x);
}
KYTY_SYSV_ABI double c_log(double x)
{
	return ::log(x);
}
KYTY_SYSV_ABI double c_pow(double x, double y)
{
	return ::pow(x, y);
}
KYTY_SYSV_ABI double c_powidf2(double x, int y)
{
	return ::pow(x, y);
}
KYTY_SYSV_ABI double c_fmod(double x, double y)
{
	return ::fmod(x, y);
}
KYTY_SYSV_ABI double c_ceil(double x)
{
	return ::ceil(x);
}
KYTY_SYSV_ABI double c_floor(double x)
{
	return ::floor(x);
}
KYTY_SYSV_ABI double c_round(double x)
{
	return ::round(x);
}
KYTY_SYSV_ABI double c_sqrt(double x)
{
	return ::sqrt(x);
}
KYTY_SYSV_ABI double c_fabs(double x)
{
	return ::fabs(x);
}
KYTY_SYSV_ABI double c_modf(double x, double* ip)
{
	return ::modf(x, ip);
}
KYTY_SYSV_ABI double c_ldexp(double x, int e)
{
	return ::ldexp(x, e);
}
KYTY_SYSV_ABI double c_frexp(double x, int* e)
{
	return ::frexp(x, e);
}
KYTY_SYSV_ABI void c_sincos(double x, double* s, double* c)
{
	*s = ::sin(x);
	*c = ::cos(x);
}
// --- math (float) ------------------------------------------------------------
KYTY_SYSV_ABI float c_powf(float x, float y)
{
	return ::powf(x, y);
}
// Gen5 libc_v1 __isnanf — NID lA94ZgT+vMM. Float in xmm0; non-zero if NaN.
// Observed Astro after pthread_self: call site loads float via vmovss then tests eax.
KYTY_SYSV_ABI int c_isnanf(float x)
{
	return std::isnan(x) ? 1 : 0;
}
// Gen5 libc_v1 __isfinitef — float in xmm0, integer predicate in eax.
KYTY_SYSV_ABI int c_isfinitef(float x)
{
	return std::isfinite(x) ? 1 : 0;
}
// Gen5 libc_v1 isfinite(double) — NID dhK16CKwhQg. Dreaming Sarah Construct
// number parser after strtod: store double, call, test %eax; non-zero keeps value.
// xmm0 = value; return non-zero when finite.
KYTY_SYSV_ABI int c_isfinite(double x)
{
	return std::isfinite(x) ? 1 : 0;
}
KYTY_SYSV_ABI int c_isnan(double x)
{
	return std::isnan(x) ? 1 : 0;
}
KYTY_SYSV_ABI int c_isinf(double x)
{
	return std::isinf(x) ? 1 : 0;
}
// Gen5 libc_v1 sinf — NID Q4rRL34CEeE (Astro after usleep).
KYTY_SYSV_ABI float c_sinf(float x)
{
	return ::sinf(x);
}
KYTY_SYSV_ABI float c_cosf(float x)
{
	return ::cosf(x);
}
// Gen5 libc_v1 tanf — NID ZE6RNL+eLbk (Astro after Posix pthread_detach; float in xmm0).
KYTY_SYSV_ABI float c_tanf(float x)
{
	return ::tanf(x);
}
// Gen5 libc_v1 inverse/extra float math (name→NID; import tables use '-' for '/').
KYTY_SYSV_ABI float c_atanf(float x)
{
	return ::atanf(x);
}
KYTY_SYSV_ABI float c_asinf(float x)
{
	return ::asinf(x);
}
KYTY_SYSV_ABI float c_acosf(float x)
{
	return ::acosf(x);
}
KYTY_SYSV_ABI float c_atan2f(float y, float x)
{
	return ::atan2f(y, x);
}
KYTY_SYSV_ABI float c_fmodf(float x, float y)
{
	return ::fmodf(x, y);
}
KYTY_SYSV_ABI float c_hypotf(float x, float y)
{
	return ::hypotf(x, y);
}
KYTY_SYSV_ABI float c_truncf(float x)
{
	return ::truncf(x);
}
KYTY_SYSV_ABI float c_roundf(float x)
{
	return ::roundf(x);
}
KYTY_SYSV_ABI float c_log10f(float x)
{
	return ::log10f(x);
}
KYTY_SYSV_ABI float c_logf(float x)
{
	return ::logf(x);
}
KYTY_SYSV_ABI float c_sqrtf(float x)
{
	return ::sqrtf(x);
}
KYTY_SYSV_ABI float c_fabsf(float x)
{
	return ::fabsf(x);
}
KYTY_SYSV_ABI float c_floorf(float x)
{
	return ::floorf(x);
}
KYTY_SYSV_ABI float c_ceilf(float x)
{
	return ::ceilf(x);
}
KYTY_SYSV_ABI float c_log2f(float x)
{
	return ::log2f(x);
}
KYTY_SYSV_ABI float c_exp2f(float x)
{
	return ::exp2f(x);
}
KYTY_SYSV_ABI float c_expf(float x)
{
	return ::expf(x);
}
KYTY_SYSV_ABI float c_ldexpf(float x, int e)
{
	return ::ldexpf(x, e);
}
KYTY_SYSV_ABI void c_sincosf(float x, float* s, float* c)
{
	*s = ::sinf(x);
	*c = ::cosf(x);
}

} // namespace LibC

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
