#include "Emulator/AtomicFile.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
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

bool ProcessAlive(uint64_t pid)
{
#ifdef _WIN32
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	if (process == nullptr)
	{
		return GetLastError() == ERROR_ACCESS_DENIED;
	}
	const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
	CloseHandle(process);
	return alive;
#else
	return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

// The writer pid of a "<destination>.<pid>.<sequence>.tmp" name, 0 for any other name.
uint64_t TemporaryWriterPid(const std::string& name)
{
	const std::string suffix = ".tmp";
	if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
	{
		return 0;
	}
	const auto sequence_dot = name.rfind('.', name.size() - suffix.size() - 1u);
	if (sequence_dot == std::string::npos || sequence_dot == 0)
	{
		return 0;
	}
	const auto pid_dot = name.rfind('.', sequence_dot - 1u);
	if (pid_dot == std::string::npos)
	{
		return 0;
	}
	const auto all_digits = [&](size_t begin, size_t end)
	{ return begin < end && std::all_of(name.begin() + begin, name.begin() + end, [](char c) { return c >= '0' && c <= '9'; }); };
	if (!all_digits(pid_dot + 1u, sequence_dot) || !all_digits(sequence_dot + 1u, name.size() - suffix.size()) ||
	    sequence_dot - pid_dot - 1u > 10u)
	{
		return 0;
	}
	return std::stoull(name.substr(pid_dot + 1u, sequence_dot - pid_dot - 1u));
}

} // namespace

void AtomicFileRemoveOrphans(const std::filesystem::path& directory)
{
	std::error_code error;
	for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
	{
		const uint64_t pid = TemporaryWriterPid(it->path().filename().string());
		if (pid != 0 && pid != ProcessId() && !ProcessAlive(pid) && it->is_regular_file(error))
		{
			std::filesystem::remove(it->path(), error);
		}
		error.clear();
	}
}

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
