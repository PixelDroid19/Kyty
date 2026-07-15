#include "Kyty/DevTools/Supervisor/DurableFile.h"
#include "Kyty/DevTools/Supervisor/ProcessLauncher.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace Kyty::DevTools {
namespace {

// Matches BundleWriter::kBundleTempMaxAgeNs (24h). Kept local to avoid header cycles.
inline constexpr uint64_t kTempMaxAgeNs = 24ull * 60ull * 60ull * 1000000000ull;

[[nodiscard]] uint64_t RealtimeNs() noexcept
{
	struct timespec ts {};
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
	{
		return 0;
	}
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

[[nodiscard]] bool WriteAll(int fd, const uint8_t* data, uint64_t size) noexcept
{
	uint64_t off = 0;
	while (off < size)
	{
		const size_t chunk = static_cast<size_t>(size - off > 1u << 20 ? 1u << 20 : size - off);
		const ssize_t n    = ::write(fd, data + off, chunk);
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return false;
		}
		if (n == 0)
		{
			return false;
		}
		off += static_cast<uint64_t>(n);
	}
	return true;
}

// Parse ".kyty-bundle-tmp.<pid>.<start>.<gen>" → true if all three nonzero.
[[nodiscard]] bool ParseTempName(const char* name, uint64_t* pid, uint64_t* start, uint64_t* gen) noexcept
{
	static constexpr char kPrefix[] = ".kyty-bundle-tmp.";
	if (name == nullptr || std::strncmp(name, kPrefix, sizeof(kPrefix) - 1u) != 0)
	{
		return false;
	}
	const char* p = name + (sizeof(kPrefix) - 1u);
	char*       end = nullptr;
	errno           = 0;
	const unsigned long long p_v = std::strtoull(p, &end, 10);
	if (errno != 0 || end == p || *end != '.')
	{
		return false;
	}
	p = end + 1;
	errno = 0;
	const unsigned long long s_v = std::strtoull(p, &end, 10);
	if (errno != 0 || end == p || *end != '.')
	{
		return false;
	}
	p = end + 1;
	errno = 0;
	const unsigned long long g_v = std::strtoull(p, &end, 10);
	if (errno != 0 || end == p || *end != '\0')
	{
		return false;
	}
	if (p_v == 0ull || s_v == 0ull || g_v == 0ull)
	{
		return false;
	}
	*pid   = static_cast<uint64_t>(p_v);
	*start = static_cast<uint64_t>(s_v);
	*gen   = static_cast<uint64_t>(g_v);
	return true;
}

[[nodiscard]] DurableIoResult RemoveTreeFilesOnly(const char* dir_path) noexcept
{
	// Only unlink regular files one level deep, then rmdir. Never recursive descent.
	DIR* d = ::opendir(dir_path);
	if (d == nullptr)
	{
		return DurableIoResult::IoError;
	}
	for (;;)
	{
		errno                 = 0;
		struct dirent* entry = ::readdir(d);
		if (entry == nullptr)
		{
			if (errno != 0)
			{
				::closedir(d);
				return DurableIoResult::IoError;
			}
			break;
		}
		if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
		{
			continue;
		}
		char child[1400] = {};
		const int n = std::snprintf(child, sizeof(child), "%s/%s", dir_path, entry->d_name);
		if (n <= 0 || static_cast<size_t>(n) >= sizeof(child))
		{
			::closedir(d);
			return DurableIoResult::IoError;
		}
		struct stat st {};
		if (::lstat(child, &st) != 0)
		{
			::closedir(d);
			return DurableIoResult::IoError;
		}
		if (!S_ISREG(st.st_mode))
		{
			// Refuse to touch unexpected node types.
			::closedir(d);
			return DurableIoResult::Conflict;
		}
		if (::unlink(child) != 0)
		{
			::closedir(d);
			return DurableIoResult::IoError;
		}
	}
	::closedir(d);
	if (::rmdir(dir_path) != 0)
	{
		return DurableIoResult::IoError;
	}
	return DurableIoResult::Ok;
}

} // namespace

DurableClockNs DefaultDurableClock() noexcept
{
	return &RealtimeNs;
}

DurableIoResult DurableWriteFile(const char* absolute_path, const void* data, uint64_t size) noexcept
{
	if (absolute_path == nullptr || absolute_path[0] != '/' || (data == nullptr && size != 0u))
	{
		return DurableIoResult::InvalidArgument;
	}
	const int fd = ::open(absolute_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
	{
		return DurableIoResult::IoError;
	}
	if (size > 0u && !WriteAll(fd, static_cast<const uint8_t*>(data), size))
	{
		::close(fd);
		return DurableIoResult::IoError;
	}
	if (::fsync(fd) != 0)
	{
		::close(fd);
		return DurableIoResult::DurabilityError;
	}
	if (::close(fd) != 0)
	{
		return DurableIoResult::IoError;
	}
	return DurableIoResult::Ok;
}

DurableIoResult DurableFsyncPath(const char* absolute_path) noexcept
{
	if (absolute_path == nullptr || absolute_path[0] != '/')
	{
		return DurableIoResult::InvalidArgument;
	}
	const int fd = ::open(absolute_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return DurableIoResult::IoError;
	}
	if (::fsync(fd) != 0)
	{
		::close(fd);
		return DurableIoResult::DurabilityError;
	}
	::close(fd);
	return DurableIoResult::Ok;
}

DurableIoResult DurableFsyncDirectory(const char* absolute_dir) noexcept
{
	if (absolute_dir == nullptr || absolute_dir[0] != '/')
	{
		return DurableIoResult::InvalidArgument;
	}
	const int fd = ::open(absolute_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
	{
		return DurableIoResult::IoError;
	}
	if (::fsync(fd) != 0)
	{
		::close(fd);
		return DurableIoResult::DurabilityError;
	}
	::close(fd);
	return DurableIoResult::Ok;
}

DurableIoResult DurableCreateDirectory(const char* absolute_dir) noexcept
{
	if (absolute_dir == nullptr || absolute_dir[0] != '/')
	{
		return DurableIoResult::InvalidArgument;
	}
	if (::mkdir(absolute_dir, 0700) != 0)
	{
		if (errno == EEXIST)
		{
			return DurableIoResult::Conflict;
		}
		return DurableIoResult::IoError;
	}
	return DurableIoResult::Ok;
}

DurableIoResult DurableRename(const char* from_absolute, const char* to_absolute) noexcept
{
	if (from_absolute == nullptr || to_absolute == nullptr || from_absolute[0] != '/' || to_absolute[0] != '/')
	{
		return DurableIoResult::InvalidArgument;
	}
	if (::rename(from_absolute, to_absolute) != 0)
	{
		if (errno == EEXIST || errno == ENOTEMPTY)
		{
			return DurableIoResult::Conflict;
		}
		return DurableIoResult::IoError;
	}
	return DurableIoResult::Ok;
}

DurableIoResult DurableRemoveEmptyDirectory(const char* absolute_dir) noexcept
{
	if (absolute_dir == nullptr || absolute_dir[0] != '/')
	{
		return DurableIoResult::InvalidArgument;
	}
	if (::rmdir(absolute_dir) != 0)
	{
		return DurableIoResult::IoError;
	}
	return DurableIoResult::Ok;
}

DurableIoResult DurableUnlinkFile(const char* absolute_path) noexcept
{
	if (absolute_path == nullptr || absolute_path[0] != '/')
	{
		return DurableIoResult::InvalidArgument;
	}
	if (::unlink(absolute_path) != 0)
	{
		return DurableIoResult::IoError;
	}
	return DurableIoResult::Ok;
}

bool DurablePathExists(const char* absolute_path) noexcept
{
	if (absolute_path == nullptr)
	{
		return false;
	}
	struct stat st {};
	return ::stat(absolute_path, &st) == 0;
}

bool QuerySelfProcessIdentity(uint64_t* pid, uint64_t* start_token) noexcept
{
	if (pid == nullptr || start_token == nullptr)
	{
		return false;
	}
	const int fd = ::open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return false;
	}
	char buf[1024] = {};
	const ssize_t n = ::read(fd, buf, sizeof(buf) - 1u);
	::close(fd);
	if (n <= 0)
	{
		return false;
	}
	buf[n] = '\0';
	// Leading pid.
	char* end = nullptr;
	errno     = 0;
	const unsigned long long p = std::strtoull(buf, &end, 10);
	if (errno != 0 || end == buf || p == 0ull)
	{
		return false;
	}
	uint64_t ticks = 0;
	if (!ParseLinuxProcStatStartTicks(buf, &ticks))
	{
		return false;
	}
	*pid         = static_cast<uint64_t>(p);
	*start_token = ticks;
	return true;
}

DurableIoResult DurableCleanupOwnTemps(const char* parent_dir, DurableClockNs clock, TempCleanupResult* out) noexcept
{
	if (parent_dir == nullptr || parent_dir[0] != '/' || clock == nullptr)
	{
		return DurableIoResult::InvalidArgument;
	}
	TempCleanupResult local {};
	DIR*              d = ::opendir(parent_dir);
	if (d == nullptr)
	{
		return DurableIoResult::IoError;
	}
	const uint64_t now = clock();
	for (;;)
	{
		errno                 = 0;
		struct dirent* entry = ::readdir(d);
		if (entry == nullptr)
		{
			if (errno != 0)
			{
				::closedir(d);
				return DurableIoResult::IoError;
			}
			break;
		}
		uint64_t pid = 0;
		uint64_t start = 0;
		uint64_t gen = 0;
		if (!ParseTempName(entry->d_name, &pid, &start, &gen))
		{
			continue;
		}
		++local.scanned;
		char path[1400] = {};
		const int n = std::snprintf(path, sizeof(path), "%s/%s", parent_dir, entry->d_name);
		if (n <= 0 || static_cast<size_t>(n) >= sizeof(path))
		{
			++local.retained;
			continue;
		}
		struct stat st {};
		if (::lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		{
			++local.retained;
			continue;
		}
		// Age from mtime (wall clock seconds via injected clock for tests).
		const uint64_t mtime_ns =
		    static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ull + static_cast<uint64_t>(st.st_mtim.tv_nsec);
		if (now == 0u || now < mtime_ns || (now - mtime_ns) < kTempMaxAgeNs)
		{
			++local.retained;
			continue;
		}
		const ProcessIdentityProbe probe = ProbeProcessIdentity(pid, start);
		if (probe != ProcessIdentityProbe::Dead)
		{
			// Live match, different start (reuse), unreadable, or malformed → retain.
			++local.retained;
			continue;
		}
		if (RemoveTreeFilesOnly(path) != DurableIoResult::Ok)
		{
			++local.retained;
			continue;
		}
		++local.removed;
	}
	::closedir(d);
	if (out != nullptr)
	{
		*out = local;
	}
	return DurableIoResult::Ok;
}

} // namespace Kyty::DevTools
