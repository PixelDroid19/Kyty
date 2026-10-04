#include "Kyty/Core/Common.h"
#include "Kyty/Core/MSpace.h"

#include "Emulator/Libs/Libs.h"

#include "LibCInternal.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

// C standard library entry points whose guest semantics are fully defined by
// ISO C (and Annex K for the *_s forms). The guest wchar_t is 16 bits wide.
namespace Kyty::Libs::LibC {

namespace {

constexpr int kErrnoInval = 22; // EINVAL as the guest libc reports it
constexpr int kErrnoRange = 34; // ERANGE

} // namespace

KYTY_SYSV_ABI size_t c_strspn(const char* s, const char* accept)
{
	return ::strspn(s, accept);
}

KYTY_SYSV_ABI size_t c_strcspn(const char* s, const char* reject)
{
	return ::strcspn(s, reject);
}

// The guest runs in the "C" locale, where collation is byte order.
KYTY_SYSV_ABI int c_strcoll(const char* s1, const char* s2)
{
	return ::strcmp(s1, s2);
}

KYTY_SYSV_ABI size_t c_strnlen_s(const char* s, size_t max_size)
{
	return s == nullptr ? 0 : ::strnlen(s, max_size);
}

KYTY_SYSV_ABI uint16_t* c_wcschr(const uint16_t* s, uint16_t c)
{
	for (;; s++)
	{
		if (*s == c)
		{
			return const_cast<uint16_t*>(s);
		}
		if (*s == 0)
		{
			return nullptr;
		}
	}
}

KYTY_SYSV_ABI uint16_t* c_wcsrchr(const uint16_t* s, uint16_t c)
{
	const uint16_t* found = nullptr;
	for (;; s++)
	{
		if (*s == c)
		{
			found = s;
		}
		if (*s == 0)
		{
			return const_cast<uint16_t*>(found);
		}
	}
}

KYTY_SYSV_ABI uint16_t* c_wcscat(uint16_t* dst, const uint16_t* src)
{
	uint16_t* end = dst + c_wcslen(dst);
	while ((*end++ = *src++) != 0)
	{
	}
	return dst;
}

// Annex K: on a constraint violation the destination becomes an empty string.
KYTY_SYSV_ABI int c_wcscpy_s(uint16_t* dst, size_t dst_size, const uint16_t* src)
{
	if (dst == nullptr || dst_size == 0)
	{
		return kErrnoInval;
	}
	if (src == nullptr)
	{
		dst[0] = 0;
		return kErrnoInval;
	}
	const size_t length = c_wcslen(src);
	if (length >= dst_size)
	{
		dst[0] = 0;
		return kErrnoRange;
	}
	std::memcpy(dst, src, (length + 1) * sizeof(uint16_t));
	return 0;
}

KYTY_SYSV_ABI int c_abs(int x)
{
	return std::abs(x);
}

// div_t is two ints returned together in RAX, the same as the host ABI.
KYTY_SYSV_ABI div_t c_div(int numerator, int denominator)
{
	return std::div(numerator, denominator);
}

KYTY_SYSV_ABI double c_exp2(double x)
{
	return std::exp2(x);
}

KYTY_SYSV_ABI float c_sinhf(float x)
{
	return std::sinh(x);
}

KYTY_SYSV_ABI float c_coshf(float x)
{
	return std::cosh(x);
}

KYTY_SYSV_ABI float c_tanhf(float x)
{
	return std::tanh(x);
}

KYTY_SYSV_ABI float c_asinhf(float x)
{
	return std::asinh(x);
}

KYTY_SYSV_ABI float c_acoshf(float x)
{
	return std::acosh(x);
}

KYTY_SYSV_ABI float c_atanhf(float x)
{
	return std::atanh(x);
}

KYTY_SYSV_ABI float c_cbrtf(float x)
{
	return std::cbrt(x);
}

KYTY_SYSV_ABI float c_fmaf(float x, float y, float z)
{
	return std::fma(x, y, z);
}

KYTY_SYSV_ABI long c_lrint(double x)
{
	return std::lrint(x);
}

KYTY_SYSV_ABI long c_lrintf(float x)
{
	return std::lrint(x);
}

KYTY_SYSV_ABI double c_nan(const char* tag)
{
	return std::nan(tag);
}

KYTY_SYSV_ABI float c_nanf(const char* tag)
{
	return std::nanf(tag);
}

KYTY_SYSV_ABI double c_scalbln(double x, long exponent)
{
	return std::scalbln(x, exponent);
}

KYTY_SYSV_ABI float c_scalbnf(float x, int exponent)
{
	return std::scalbn(x, exponent);
}

// Dinkumware helpers behind sinh/cosh: y * sinh(x) and y * cosh(x).
KYTY_SYSV_ABI double c_Sinh(double x, double y)
{
	return y * std::sinh(x);
}

KYTY_SYSV_ABI double c_Cosh(double x, double y)
{
	return y * std::cosh(x);
}

KYTY_SYSV_ABI float c_FSinh(float x, float y)
{
	return y * std::sinh(x);
}

KYTY_SYSV_ABI float c_FCosh(float x, float y)
{
	return y * std::cosh(x);
}

} // namespace Kyty::Libs::LibC

namespace Kyty::Libs::LibcInternal {

KYTY_SYSV_ABI int LibcMspaceDestroy(void* msp)
{
	return msp != nullptr && Core::MSpaceDestroy(msp) ? 0 : -1;
}

KYTY_SYSV_ABI void* LibcMspaceRealloc(void* msp, void* ptr, size_t size)
{
	return msp == nullptr ? nullptr : Core::MSpaceRealloc(msp, ptr, size);
}

KYTY_SYSV_ABI void* LibcMspaceReallocalign(void* msp, void* ptr, size_t boundary, size_t size)
{
	return msp == nullptr ? nullptr : Core::MSpaceReallocalign(msp, ptr, boundary, size);
}

KYTY_SYSV_ABI void* LibcMspaceAlignedAlloc(void* msp, size_t alignment, size_t size)
{
	return msp == nullptr ? nullptr : Core::MSpaceAlignedAlloc(msp, alignment, size);
}

// posix_memalign contract: 0, or the error number with *ptr untouched.
KYTY_SYSV_ABI int LibcMspacePosixMemalign(void* msp, void** ptr, size_t boundary, size_t size)
{
	constexpr int kErrnoNoMem = 12;
	if (msp == nullptr || ptr == nullptr)
	{
		return 22;
	}
	return Core::MSpacePosixMemalign(msp, ptr, boundary, size) ? 0 : kErrnoNoMem;
}

} // namespace Kyty::Libs::LibcInternal

#endif // KYTY_EMU_ENABLED
