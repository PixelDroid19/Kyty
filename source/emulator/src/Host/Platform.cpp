#include "Emulator/Host/Platform.h"

#include "SDL_filesystem.h"
#include "SDL_stdinc.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace Kyty::Emulator::Host {

std::string UtcTimestamp()
{
	const auto now = std::chrono::system_clock::now();
	const auto tt  = std::chrono::system_clock::to_time_t(now);

	std::tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &tt);
#else
	gmtime_r(&tt, &utc);
#endif

	char stamp[32] {};
	std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc);
	return stamp;
}

std::string ApplicationBasePath()
{
	char* base_path = SDL_GetBasePath();
	if (base_path == nullptr || base_path[0] == '\0')
	{
		SDL_free(base_path);
		return {};
	}

	std::string result(base_path);
	SDL_free(base_path);
	return result;
}

std::filesystem::path DefaultCacheDirectory()
{
#if defined(_WIN32)
	if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data != nullptr && local_app_data[0] != '\0')
	{
		return std::filesystem::path(local_app_data) / "Kyty" / "Cache";
	}
#elif defined(__APPLE__)
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
	{
		return std::filesystem::path(home) / "Library" / "Caches" / "Kyty";
	}
#else
	if (const char* xdg_cache = std::getenv("XDG_CACHE_HOME"); xdg_cache != nullptr && xdg_cache[0] != '\0')
	{
		return std::filesystem::path(xdg_cache) / "kyty";
	}
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
	{
		return std::filesystem::path(home) / ".cache" / "kyty";
	}
#endif

	std::error_code error;
	const auto      temp = std::filesystem::temp_directory_path(error);
	return error ? std::filesystem::path() : temp / "kyty";
}

uint64_t PeakRssBytes()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS counters {};
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
	{
		return 0;
	}
	return static_cast<uint64_t>(counters.PeakWorkingSetSize);
#else
	struct rusage usage {};
	if (getrusage(RUSAGE_SELF, &usage) != 0)
	{
		return 0;
	}
#if defined(__APPLE__)
	return static_cast<uint64_t>(usage.ru_maxrss);
#else
	return static_cast<uint64_t>(usage.ru_maxrss) * 1024u;
#endif
#endif
}

} // namespace Kyty::Emulator::Host
