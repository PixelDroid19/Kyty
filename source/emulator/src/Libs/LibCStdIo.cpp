#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/VideoFrameMemory.h"

#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibC {

KYTY_SYSV_ABI FILE* c_fopen(const char* path, const char* mode)
{
	if (path == nullptr)
	{
		return nullptr;
	}
	// Translate mounted guest paths (e.g. /app0/...) to real host paths.
	String host = LibKernel::FileSystem::GetRealFilename(String::FromUtf8(path));
	String use  = host.IsEmpty() ? String::FromUtf8(path) : host;
	FILE*  f    = ::fopen(use.C_Str(), mode);
	printf("\t fopen('%s' -> '%s', '%s') = %p\n", path, use.C_Str(), mode, static_cast<void*>(f));
	return f;
}

KYTY_SYSV_ABI int c_fclose(FILE* f)
{
	return (f != nullptr) ? ::fclose(f) : 0;
}

KYTY_SYSV_ABI size_t c_fread(void* p, size_t sz, size_t n, FILE* f)
{
	const size_t result = ::fread(p, sz, n, f);
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(p), result * sz);
	return result;
}

// Gen5 libc_v1 fgets — NID KdP-nULpuGw.
KYTY_SYSV_ABI char* c_fgets(char* s, int n, FILE* f)
{
	if (s == nullptr || n <= 0 || f == nullptr)
	{
		return nullptr;
	}
	char* result = ::fgets(s, n, f);
	if (result != nullptr)
	{
		Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(s), std::strlen(s) + 1);
	}
	return result;
}

KYTY_SYSV_ABI size_t c_fwrite(const void* p, size_t sz, size_t n, FILE* f)
{
	return ::fwrite(p, sz, n, f);
}

KYTY_SYSV_ABI int c_setvbuf(FILE* stream, char* buffer, int mode, size_t size)
{
	if (stream == nullptr)
	{
		return -1;
	}

	int host_mode = 0;
	switch (mode)
	{
		case 0: host_mode = _IOFBF; break;
		case 1: host_mode = _IOLBF; break;
		case 2: host_mode = _IONBF; break;
		default: return -1;
	}
	return ::setvbuf(stream, buffer, host_mode, size);
}

KYTY_SYSV_ABI int c_fseek(FILE* f, long off, int w)
{
	return ::fseek(f, off, w);
}

KYTY_SYSV_ABI long c_ftell(FILE* f)
{
	return ::ftell(f);
}

KYTY_SYSV_ABI int c_feof(FILE* f)
{
	return ::feof(f);
}

KYTY_SYSV_ABI int c_ferror(FILE* f)
{
	return ::ferror(f);
}

KYTY_SYSV_ABI int c_fileno(FILE* f)
{
	if (f == nullptr)
	{
		return -1;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return ::_fileno(f);
#else
	return ::fileno(f);
#endif
}

KYTY_SYSV_ABI int c_fputc(int ch, FILE* f)
{
	return ::fputc(ch, f);
}

KYTY_SYSV_ABI int c_remove(const char* p)
{
	String host = LibKernel::FileSystem::GetRealFilename(String::FromUtf8(p));
	return ::remove((host.IsEmpty() ? String::FromUtf8(p) : host).C_Str());
}

} // namespace Kyty::Libs::LibC

#endif // KYTY_EMU_ENABLED
