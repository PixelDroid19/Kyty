#include "Kyty/DevTools/Transport/ProcessIdentity.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Kyty::DevTools {
namespace {

#if defined(__linux__)
[[nodiscard]] bool ParseLinuxStartTicks(const char* stat_line, uint64_t* out) noexcept
{
	if (stat_line == nullptr || out == nullptr)
	{
		return false;
	}
	const char* close = std::strrchr(stat_line, ')');
	if (close == nullptr || close[1] != ' ')
	{
		return false;
	}
	const char* field = close + 2;
	unsigned    number = 3;
	while (number <= 22)
	{
		while (*field == ' ')
		{
			++field;
		}
		if (*field == '\0')
		{
			return false;
		}
		const char* end = field;
		while (*end != '\0' && *end != ' ')
		{
			++end;
		}
		if (number == 22)
		{
			char* parse_end = nullptr;
			errno = 0;
			const unsigned long long ticks = std::strtoull(field, &parse_end, 10);
			if (errno != 0 || parse_end != end || ticks == 0ull)
			{
				return false;
			}
			*out = static_cast<uint64_t>(ticks);
			return true;
		}
		field = end;
		++number;
	}
	return false;
}
#endif

} // namespace

bool QueryCurrentProcessIdentity(uint64_t* pid, uint64_t* start_token) noexcept
{
	if (pid == nullptr || start_token == nullptr)
	{
		return false;
	}
	*pid = 0;
	*start_token = 0;

#if defined(_WIN32)
	FILETIME creation {}, exit {}, kernel {}, user {};
	if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE)
	{
		return false;
	}
	const uint64_t token = (static_cast<uint64_t>(creation.dwHighDateTime) << 32u) | creation.dwLowDateTime;
	const uint64_t current_pid = static_cast<uint64_t>(::GetCurrentProcessId());
	if (current_pid == 0u || token == 0u)
	{
		return false;
	}
	*pid = current_pid;
	*start_token = token;
	return true;
#elif defined(__APPLE__)
	const pid_t current_pid = ::getpid();
	if (current_pid <= 0)
	{
		return false;
	}
	struct proc_bsdinfo info {};
	const int size = ::proc_pidinfo(current_pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info));
	if (size != static_cast<int>(sizeof(info)) || (info.pbi_start_tvsec == 0u && info.pbi_start_tvusec == 0u))
	{
		return false;
	}
	const uint64_t seconds = static_cast<uint64_t>(info.pbi_start_tvsec);
	const uint64_t micros = static_cast<uint64_t>(info.pbi_start_tvusec);
	if (seconds > (std::numeric_limits<uint64_t>::max() - micros) / 1000000ull)
	{
		return false;
	}
	*pid = static_cast<uint64_t>(current_pid);
	*start_token = seconds * 1000000ull + micros;
	return *start_token != 0u;
#elif defined(__linux__)
	const pid_t current_pid = ::getpid();
	if (current_pid <= 0)
	{
		return false;
	}
	char path[64] = {};
	const int path_size = std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(current_pid));
	if (path_size <= 0 || static_cast<size_t>(path_size) >= sizeof(path))
	{
		return false;
	}
	const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return false;
	}
	char buffer[1024] = {};
	const ssize_t length = ::read(fd, buffer, sizeof(buffer) - 1u);
	::close(fd);
	if (length <= 0)
	{
		return false;
	}
	buffer[length] = '\0';
	uint64_t ticks = 0;
	if (!ParseLinuxStartTicks(buffer, &ticks))
	{
		return false;
	}
	*pid = static_cast<uint64_t>(current_pid);
	*start_token = ticks;
	return true;
#else
	return false;
#endif
}

} // namespace Kyty::DevTools
