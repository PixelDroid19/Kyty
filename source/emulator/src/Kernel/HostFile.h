#ifndef EMULATOR_SRC_KERNEL_HOSTFILE_H_
#define EMULATOR_SRC_KERNEL_HOSTFILE_H_

#include "Kyty/Core/Common.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace Kyty::Kernel::FileSystem::Host {

// Native integers never leave this boundary. Duplicated streams share the
// native open-file description (offset and status flags), not a reopened path.
class File
{
public:
	File() = default;
	~File()
	{
		const int saved = errno;
		if (m_descriptor >= 0) { Close(m_descriptor); }
		errno = saved;
	}
	KYTY_CLASS_NO_COPY(File);

	// Host resource budget, not an asserted console ABI limit. POSIX bounds
	// descriptor numbers by RLIMIT_NOFILE. On the CRT use its reported stdio
	// capacity, since every admitted guest descriptor can also back a FILE.
	static int DescriptorCapacity()
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return ::_getmaxstdio();
#else
		struct rlimit limit {};
		if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) { return -1; }
		if (limit.rlim_cur != RLIM_INFINITY)
		{
			return limit.rlim_cur > INT_MAX ? INT_MAX : static_cast<int>(limit.rlim_cur);
		}
		const long count = ::sysconf(_SC_OPEN_MAX);
		if (count < 0) { errno = ENOSYS; return -1; }
		return count > INT_MAX ? INT_MAX : static_cast<int>(count);
#endif
	}

	bool Open(const char* path, uint32_t guest_flags, uint16_t mode)
	{
		if ((guest_flags & 3u) == 3u) { errno = EINVAL; return false; }
		const int access[] = {O_RDONLY, O_WRONLY, O_RDWR};
		int flags = access[guest_flags & 3u];
		if ((guest_flags & 0x0008u) != 0) { flags |= O_APPEND; }
		if ((guest_flags & 0x0200u) != 0) { flags |= O_CREAT; }
		if ((guest_flags & 0x0400u) != 0) { flags |= O_TRUNC; }
		if ((guest_flags & 0x0800u) != 0) { flags |= O_EXCL; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_descriptor = ::_open(path, flags | _O_BINARY | _O_NOINHERIT, mode);
#else
		m_descriptor = ::open(path, flags | O_CLOEXEC, mode);
#endif
		m_append = (guest_flags & 8u) != 0;
		return !IsInvalid();
	}

	bool IsInvalid() const { return m_descriptor < 0; }

	int64_t Read(void* buffer, uint32_t size) const
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return ::_read(m_descriptor, buffer, size);
#else
		return ::read(m_descriptor, buffer, size);
#endif
	}

	int64_t Write(const void* buffer, uint32_t size) const
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (m_append)
		{
			// Win32's EOF offset performs placement as part of the write; a CRT
			// seek-to-end followed by _write would race independent append opens.
			OVERLAPPED at {};
			at.Offset = at.OffsetHigh = MAXDWORD;
			DWORD written = 0;
			if (!::WriteFile(reinterpret_cast<HANDLE>(::_get_osfhandle(m_descriptor)), buffer, size, &written, &at))
			{
				errno = EIO;
				return -1;
			}
			return written;
		}
		return ::_write(m_descriptor, buffer, size);
#else
		return ::write(m_descriptor, buffer, size);
#endif
	}

	int64_t Seek(int64_t offset, int whence) const
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return ::_lseeki64(m_descriptor, offset, whence);
#else
		return ::lseek(m_descriptor, static_cast<off_t>(offset), whence);
#endif
	}
	int64_t Tell() const { return Seek(0, SEEK_CUR); }

	int64_t ReadAt(void* buffer, uint32_t size, int64_t offset) const
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const int64_t previous = Tell();
		if (previous < 0 || Seek(offset, SEEK_SET) < 0) { return -1; }
		const int64_t result = Read(buffer, size);
		const int saved = errno;
		if (Seek(previous, SEEK_SET) < 0 && result == 0) { return -1; }
		errno = saved;
		return result;
#else
		return ::pread(m_descriptor, buffer, size, static_cast<off_t>(offset));
#endif
	}

	// Caller holds the description mutex and every attached native FILE lock.
	// Linux pwrite otherwise honors O_APPEND, unlike the positioned guest API.
	int64_t WriteAt(const void* buffer, uint32_t size, int64_t offset)
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const int64_t previous = Tell();
		if (previous < 0) { return -1; }
		OVERLAPPED at {};
		at.Offset = static_cast<DWORD>(offset);
		at.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32u);
		DWORD written = 0;
		const bool ok = ::WriteFile(reinterpret_cast<HANDLE>(::_get_osfhandle(m_descriptor)), buffer, size, &written, &at) != 0;
		if (Seek(previous, SEEK_SET) < 0 && written == 0) { return -1; }
		if (!ok) { errno = EIO; return -1; }
		return written;
#else
		const int flags = ::fcntl(m_descriptor, F_GETFL);
		if (flags < 0) { return -1; }
		const bool append = (flags & O_APPEND) != 0;
		if (append && ::fcntl(m_descriptor, F_SETFL, flags & ~O_APPEND) < 0) { return -1; }
		const int64_t result = ::pwrite(m_descriptor, buffer, size, static_cast<off_t>(offset));
		const int saved = errno;
		if (append && ::fcntl(m_descriptor, F_SETFL, flags) < 0 && result == 0) { return -1; }
		errno = saved;
		return result;
#endif
	}

	bool Truncate(int64_t size) const
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const int error = ::_chsize_s(m_descriptor, size);
		if (error != 0) { errno = error; }
		return error == 0;
#else
		return ::ftruncate(m_descriptor, static_cast<off_t>(size)) == 0;
#endif
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	using Stat = struct _stat64;
#else
	using Stat = struct stat;
#endif
	bool GetStat(Stat* result) const
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return ::_fstat64(m_descriptor, result) == 0;
#else
		return ::fstat(m_descriptor, result) == 0;
#endif
	}
	int64_t Size() const
	{
		Stat result {};
		return GetStat(&result) ? static_cast<int64_t>(result.st_size) : -1;
	}

	bool SetAppend(bool append)
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// The CRT has no F_SETFL equivalent. Do not claim a status change that
		// existing fdopen streams would fail to observe.
		if (append != m_append) { errno = ENOSYS; return false; }
#else
		const int flags = ::fcntl(m_descriptor, F_GETFL);
		if (flags < 0 || ::fcntl(m_descriptor, F_SETFL, append ? flags | O_APPEND : flags & ~O_APPEND) != 0)
		{
			return false;
		}
#endif
		m_append = append;
		return true;
	}

	FILE* DuplicateStream(const char* mode) const
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const int descriptor = ::_dup(m_descriptor);
		FILE* stream = descriptor < 0 ? nullptr : ::_fdopen(descriptor, mode);
#else
		const int descriptor = ::fcntl(m_descriptor, F_DUPFD_CLOEXEC, 0);
		FILE* stream = descriptor < 0 ? nullptr : ::fdopen(descriptor, mode);
#endif
		if (stream == nullptr && descriptor >= 0)
		{
			const int saved = errno;
			Close(descriptor);
			errno = saved;
		}
		return stream;
	}

	bool ReplaceStreamDescriptor(FILE* stream) const
	{
		// Guest dup2 explicitly replaces this stream's descriptor. Flush and
		// discard any read-ahead before replacing the private native handle.
		if (std::fflush(stream) != 0) { return false; }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (::_dup2(m_descriptor, ::_fileno(stream)) != 0) { return false; }
#else
		if (::dup2(m_descriptor, ::fileno(stream)) < 0) { return false; }
#endif
		return true;
	}

	bool DuplicateFromStream(FILE* stream, bool append)
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_descriptor = ::_dup(::_fileno(stream));
#else
		m_descriptor = ::fcntl(::fileno(stream), F_DUPFD_CLOEXEC, 0);
#endif
		m_append = append;
		return !IsInvalid();
	}

	static void LockStream(FILE* stream)
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		::_lock_file(stream);
#else
		::flockfile(stream);
#endif
	}
	static void UnlockStream(FILE* stream)
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		::_unlock_file(stream);
#else
		::funlockfile(stream);
#endif
	}

private:
	static void Close(int descriptor)
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		::_close(descriptor);
#else
		::close(descriptor);
#endif
	}
	int m_descriptor = -1;
	bool m_append = false;
};

} // namespace Kyty::Kernel::FileSystem::Host

#endif
