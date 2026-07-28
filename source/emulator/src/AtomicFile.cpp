#include "Emulator/AtomicFile.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace {

std::atomic<uint64_t> g_temporary_sequence {0};

uint64_t ProcessId()
{
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return static_cast<uint64_t>(getpid());
#endif
}

bool ReplaceFile(const std::filesystem::path& source, const std::filesystem::path& destination)
{
#ifdef _WIN32
	return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
	return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

} // namespace

bool AtomicFileWrite(const std::filesystem::path& destination, const void* data, size_t size)
{
	if (destination.empty() || (data == nullptr && size != 0))
	{
		return false;
	}

	std::error_code error;
	const auto      parent = destination.parent_path();
	if (!parent.empty())
	{
		std::filesystem::create_directories(parent, error);
		if (error)
		{
			return false;
		}
	}

	auto temporary = destination;
	temporary += "." + std::to_string(ProcessId()) + "." + std::to_string(g_temporary_sequence.fetch_add(1)) + ".tmp";
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if (!file || (size != 0 && !file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size))) || !file.flush())
		{
			file.close();
			std::filesystem::remove(temporary, error);
			return false;
		}
		file.close();
		if (!file)
		{
			std::filesystem::remove(temporary, error);
			return false;
		}
	}

	if (!ReplaceFile(temporary, destination))
	{
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

} // namespace Kyty::Libs

#endif
