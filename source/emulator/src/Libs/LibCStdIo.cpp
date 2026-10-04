#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/VideoFrameMemory.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibC {

KYTY_SYSV_ABI FILE* c_fopen(const char* path, const char* mode)
{
	return Kernel::FileSystem::OpenStream(path, mode);
}

KYTY_SYSV_ABI FILE* c_fdopen(int descriptor, const char* mode)
{
	return Kernel::FileSystem::OpenDescriptorStream(descriptor, mode);
}

KYTY_SYSV_ABI FILE* c_freopen(const char* path, const char* mode, FILE* stream)
{
	return Kernel::FileSystem::ReopenStream(path, mode, stream);
}

KYTY_SYSV_ABI int c_fclose(FILE* f)
{
	return (f != nullptr) ? Kernel::FileSystem::CloseStream(f) : 0;
}

KYTY_SYSV_ABI size_t c_fread(void* p, size_t sz, size_t n, FILE* f)
{
	if (f == nullptr || (p == nullptr && sz != 0 && n != 0) || (sz != 0 && n > std::numeric_limits<size_t>::max() / sz))
	{
		errno = EINVAL;
		return 0;
	}
	const size_t requested = sz * n;
	const Kyty::Emulator::VideoFrameMemory::HostWriteLease write_lease(reinterpret_cast<uint64_t>(p), requested);
	const Kernel::FileSystem::StreamOperation operation(f);
	if (!operation.IsValid()) { return 0; }
	return ::fread(p, sz, n, f);
}

// Gen5 libc_v1 fgets — NID KdP-nULpuGw.
KYTY_SYSV_ABI char* c_fgets(char* s, int n, FILE* f)
{
	if (s == nullptr || n <= 0 || f == nullptr)
	{
		return nullptr;
	}
	const Kyty::Emulator::VideoFrameMemory::HostWriteLease write_lease(reinterpret_cast<uint64_t>(s), static_cast<size_t>(n));
	const Kernel::FileSystem::StreamOperation operation(f);
	if (!operation.IsValid()) { return nullptr; }
	return ::fgets(s, n, f);
}

KYTY_SYSV_ABI size_t c_fwrite(const void* p, size_t sz, size_t n, FILE* f)
{
	if (f == nullptr || (p == nullptr && sz != 0 && n != 0) || (sz != 0 && n > std::numeric_limits<size_t>::max() / sz))
	{
		errno = EINVAL;
		return 0;
	}
	const Kernel::FileSystem::StreamOperation operation(f);
	return operation.IsValid() ? ::fwrite(p, sz, n, f) : 0;
}

KYTY_SYSV_ABI int c_setvbuf(FILE* stream, char* buffer, int mode, size_t size)
{
	if (stream == nullptr)
	{
		errno = EINVAL;
		return -1;
	}

	int host_mode = 0;
	switch (mode)
	{
		case 0: host_mode = _IOFBF; break;
		case 1: host_mode = _IOLBF; break;
		case 2: host_mode = _IONBF; break;
		default: errno = EINVAL; return -1;
	}
	return Kernel::FileSystem::SetStreamBuffer(stream, buffer, host_mode, size);
}

KYTY_SYSV_ABI int c_fseek(FILE* f, long off, int w)
{
	const Kernel::FileSystem::StreamOperation operation(f);
	return operation.IsValid() ? ::fseek(f, off, w) : -1;
}

KYTY_SYSV_ABI long c_ftell(FILE* f)
{
	const Kernel::FileSystem::StreamOperation operation(f);
	return operation.IsValid() ? ::ftell(f) : -1;
}

KYTY_SYSV_ABI int c_feof(FILE* f)
{
	const Kernel::FileSystem::StreamOperation operation(f);
	return operation.IsValid() ? ::feof(f) : 0;
}

KYTY_SYSV_ABI int c_ferror(FILE* f)
{
	const Kernel::FileSystem::StreamOperation operation(f);
	return operation.IsValid() ? ::ferror(f) : 0;
}

KYTY_SYSV_ABI int c_fileno(FILE* f)
{
	if (f == nullptr)
	{
		return -1;
	}
	return Kernel::FileSystem::StreamDescriptor(f);
}

KYTY_SYSV_ABI int c_fputc(int ch, FILE* f)
{
	const Kernel::FileSystem::StreamOperation operation(f);
	return operation.IsValid() ? ::fputc(ch, f) : EOF;
}

KYTY_SYSV_ABI int c_fputs(const char* s, FILE* f)
{
	if (s == nullptr) { errno = EINVAL; return EOF; }
	const Kernel::FileSystem::StreamOperation operation(f);
	return operation.IsValid() ? ::fputs(s, f) : EOF;
}

KYTY_SYSV_ABI int c_fgetc(FILE* f)
{
	const Kernel::FileSystem::StreamOperation operation(f);
	return operation.IsValid() ? ::fgetc(f) : EOF;
}

KYTY_SYSV_ABI void c_rewind(FILE* f)
{
	const Kernel::FileSystem::StreamOperation operation(f);
	if (operation.IsValid()) { ::rewind(f); }
}

KYTY_SYSV_ABI int c_remove(const char* p)
{
	if (p == nullptr) { return -1; }
	String host = Kernel::FileSystem::GetRealFilename(String::FromUtf8(p));
	return host.IsEmpty() ? -1 : ::remove(host.C_Str());
}

} // namespace Kyty::Libs::LibC

#endif // KYTY_EMU_ENABLED
