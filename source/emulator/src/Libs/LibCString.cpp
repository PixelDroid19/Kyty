#include "LibCInternal.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Libs/ProcessEnvironment.h"
#include "Emulator/VideoFrameMemory.h"

#include <clocale>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cwchar>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibC {
KYTY_SYSV_ABI void* c_memcpy(void* d, const void* s, size_t n)
{
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(d), n);
	return ::memcpy(d, s, n);
}
KYTY_SYSV_ABI int c_memcpy_s(void* d, size_t dn, const void* s, size_t n)
{
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(d), n < dn ? n : dn);
	return (::memcpy(d, s, n < dn ? n : dn), 0);
}
// Gen5 libc_v1 memmove_s — NID B59+zQQCcbU (Astro after strtoull).
KYTY_SYSV_ABI int c_memmove_s(void* d, size_t dn, const void* s, size_t n)
{
	if (d == nullptr || s == nullptr)
	{
		return -1;
	}
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(d), n < dn ? n : dn);
	::memmove(d, s, n < dn ? n : dn);
	return 0;
}
KYTY_SYSV_ABI void* c_memmove(void* d, const void* s, size_t n)
{
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(d), n);
	return ::memmove(d, s, n);
}
KYTY_SYSV_ABI void* c_memset(void* d, int c, size_t n)
{
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(d), n);
	return ::memset(d, c, n);
}
// Gen5 libc_v1 memset_s — NID h8GwqPFbu6I (Astro after DrawIndexIndirect).
// SysV: rdi=s, rsi=smax, rdx=c, rcx=n. Returns 0 on success.
KYTY_SYSV_ABI int c_memset_s(void* s, size_t smax, int c, size_t n)
{
	if (s == nullptr)
	{
		return -1;
	}
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(s), n < smax ? n : smax);
	::memset(s, c, n < smax ? n : smax);
	return 0;
}
KYTY_SYSV_ABI int c_memcmp(const void* a, const void* b, size_t n)
{
	return ::memcmp(a, b, n);
}
KYTY_SYSV_ABI void* c_memchr(const void* s, int c, size_t n)
{
	return const_cast<void*>(::memchr(s, c, n));
}
KYTY_SYSV_ABI size_t c_strlen(const char* s)
{
	return ::strlen(s);
}
KYTY_SYSV_ABI size_t c_wcslen(const uint16_t* s)
{
	const uint16_t* end = s;
	while (*end != 0)
	{
		end++;
	}
	return static_cast<size_t>(end - s);
}
KYTY_SYSV_ABI uint16_t* c_wcsncpy(uint16_t* destination, const uint16_t* source, size_t count)
{
	size_t index = 0;
	while (index < count && source[index] != 0)
	{
		destination[index] = source[index];
		index++;
	}
	while (index < count)
	{
		destination[index] = 0;
		index++;
	}
	return destination;
}
KYTY_SYSV_ABI int c_Iswctype(uint32_t character, int character_class)
{
	if (character > 0x7f)
	{
		EXIT_NOT_IMPLEMENTED(character > 0x7f);
		return 0;
	}

	// The verified descriptor scans decimal width characters. Do not delegate to
	// the host locale or accept unrelated descriptor values: either behavior can
	// change guest control flow without an established ABI contract.
	if (character_class != 2)
	{
		EXIT_NOT_IMPLEMENTED(character_class != 2);
		return 0;
	}

	return character >= '0' && character <= '9' ? 1 : 0;
}
KYTY_SYSV_ABI int c_Wctombx(char* dst, uint32_t character, std::mbstate_t* /*state*/, const void* /*cvtvec*/)
{
	if (dst == nullptr)
	{
		return 0;
	}
	if (character > 0x7f) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	dst[0] = static_cast<char>(character);
	return 1;
}
KYTY_SYSV_ABI int c_Mbtowcx(uint16_t* dst, const char* src, size_t count, std::mbstate_t* /*state*/, const void* /*cvtvec*/)
{
	if (src == nullptr)
	{
		return 0;
	}
	if (count == 0)
	{
		return -2;
	}
	const auto ch = static_cast<uint8_t>(src[0]);
	if (ch > 0x7f) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (dst != nullptr)
	{
		*dst = ch;
	}
	return ch == 0 ? 0 : 1;
}
KYTY_SYSV_ABI char* c_strcpy(char* d, const char* s)
{
	return ::strcpy(d, s);
}
// Gen5 libc_v1 wide mem* — NIDs from the public Gen5 export hash (SHA1+suffix, byte-reversed).
KYTY_SYSV_ABI wchar_t* c_wmemchr(const wchar_t* s, wchar_t c, size_t n)
{
	return const_cast<wchar_t*>(std::wmemchr(s, c, n));
}
KYTY_SYSV_ABI int      c_wmemcmp(const wchar_t* a, const wchar_t* b, size_t n)
{
	return std::wmemcmp(a, b, n);
}
KYTY_SYSV_ABI int c_wmemcmp16(const char16_t* a, const char16_t* b, size_t n)
{
	if (n == 0)
	{
		return 0;
	}
	EXIT_IF(a == nullptr || b == nullptr);
	for (size_t i = 0; i < n; i++)
	{
		if (a[i] != b[i])
		{
			return (a[i] < b[i] ? -1 : 1);
		}
	}
	return 0;
}
KYTY_SYSV_ABI char16_t* c_wmemcpy16(char16_t* d, const char16_t* s, size_t n)
{
	if (n == 0)
	{
		return d;
	}
	EXIT_IF(d == nullptr || s == nullptr);
	for (size_t i = 0; i < n; i++)
	{
		d[i] = s[i];
	}
	return d;
}
KYTY_SYSV_ABI wchar_t* c_wmemcpy(wchar_t* d, const wchar_t* s, size_t n)
{
	return std::wmemcpy(d, s, n);
}
KYTY_SYSV_ABI wchar_t* c_wmemmove(wchar_t* d, const wchar_t* s, size_t n)
{
	return std::wmemmove(d, s, n);
}
KYTY_SYSV_ABI wchar_t* c_wmemset(wchar_t* s, wchar_t c, size_t n)
{
	return std::wmemset(s, c, n);
}
// Gen5 strcpy_s — NID 5Xa2ACNECdo: dest, destsz, src. Returns 0 on success.
KYTY_SYSV_ABI int c_strcpy_s(char* d, size_t destsz, const char* s)
{
	if (d == nullptr || s == nullptr || destsz == 0)
	{
		return -1;
	}
	const size_t n = ::strlen(s);
	if (n + 1 > destsz)
	{
		d[0] = '\0';
		return -1;
	}
	::memcpy(d, s, n + 1);
	return 0;
}
KYTY_SYSV_ABI char* c_strncpy(char* d, const char* s, size_t n)
{
	return ::strncpy(d, s, n);
}
KYTY_SYSV_ABI int c_strcmp(const char* a, const char* b)
{
	return ::strcmp(a, b);
}
KYTY_SYSV_ABI int c_strncmp(const char* a, const char* b, size_t n)
{
	return ::strncmp(a, b, n);
}

static unsigned char c_ascii_fold(unsigned char value)
{
	return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

static int c_ascii_case_compare(const char* a, const char* b, size_t count)
{
	EXIT_IF(a == nullptr || b == nullptr);
	for (size_t i = 0; i < count; ++i)
	{
		const auto left  = c_ascii_fold(static_cast<unsigned char>(a[i]));
		const auto right = c_ascii_fold(static_cast<unsigned char>(b[i]));
		if (left != right)
		{
			return static_cast<int>(left) - static_cast<int>(right);
		}
		if (left == 0)
		{
			return 0;
		}
	}
	return 0;
}

KYTY_SYSV_ABI int c_strcasecmp(const char* a, const char* b)
{
	return c_ascii_case_compare(a, b, static_cast<size_t>(-1));
}

KYTY_SYSV_ABI int c_strncasecmp(const char* a, const char* b, size_t count)
{
	return c_ascii_case_compare(a, b, count);
}

KYTY_SYSV_ABI char* c_strcat(char* d, const char* s)
{
	return ::strcat(d, s);
}
KYTY_SYSV_ABI char* c_strncat(char* d, const char* s, size_t n)
{
	return ::strncat(d, s, n);
}
KYTY_SYSV_ABI char* c_strpbrk(const char* string, const char* accept)
{
	return const_cast<char*>(::strpbrk(string, accept));
}
KYTY_SYSV_ABI char* c_strchr(const char* s, int c)
{
	return const_cast<char*>(::strchr(s, c));
}
KYTY_SYSV_ABI char* c_strstr(const char* haystack, const char* needle)
{
	return const_cast<char*>(::strstr(haystack, needle));
}
KYTY_SYSV_ABI char* c_getenv(const char* name)
{
	const char* value = ProcessEnvironment::GetEnvironmentVariable(name);
	return const_cast<char*>(value != nullptr ? value : (name != nullptr ? ::getenv(name) : nullptr));
}
KYTY_SYSV_ABI char* c_setlocale(int category, const char* locale)
{
	return ::setlocale(category, locale);
}
KYTY_SYSV_ABI unsigned __int128 c_udivti3(unsigned __int128 numerator, unsigned __int128 denominator)
{
	return numerator / denominator;
}
KYTY_SYSV_ABI uint16_t* c_wcsstr(const uint16_t* haystack, const uint16_t* needle)
{
	if (haystack == nullptr || needle == nullptr)
	{
		return nullptr;
	}
	if (*needle == 0)
	{
		return const_cast<uint16_t*>(haystack);
	}

	for (auto* candidate = haystack; *candidate != 0; ++candidate)
	{
		const uint16_t* left  = candidate;
		const uint16_t* right = needle;
		while (*left != 0 && *right != 0 && *left == *right)
		{
			++left;
			++right;
		}
		if (*right == 0)
		{
			return const_cast<uint16_t*>(candidate);
		}
	}
	return nullptr;
}
KYTY_SYSV_ABI int c_wcsncmp(const uint16_t* left, const uint16_t* right, size_t count)
{
	if (count == 0 || left == right)
	{
		return 0;
	}
	if (left == nullptr || right == nullptr)
	{
		return left == nullptr ? -1 : 1;
	}

	for (size_t index = 0; index < count; ++index)
	{
		if (left[index] != right[index])
		{
			return left[index] < right[index] ? -1 : 1;
		}
		if (left[index] == 0)
		{
			return 0;
		}
	}
	return 0;
}
// Helpers kept for pending NID registration; not yet bound via LIB_FUNC.
[[maybe_unused]] KYTY_SYSV_ABI char* c_strrchr(const char* s, int c)
{
	return const_cast<char*>(::strrchr(s, c));
}
KYTY_SYSV_ABI size_t c_strnlen(const char* s, size_t n)
{
	return ::strnlen(s, n);
}
} // namespace Kyty::Libs::LibC

#endif // KYTY_EMU_ENABLED
