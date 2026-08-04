#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

constexpr int         kLaunchFailure = 125;
constexpr const char* kStderrPrefix  = "kyty architecture boundary test launcher: prefix\n";

void WriteStderrPrefix()
{
	std::fputs(kStderrPrefix, stderr);
	std::fflush(stderr);
}

#if defined(_WIN32)

std::wstring QuoteWindowsArgument(const std::wstring& argument)
{
	std::wstring quoted = L"\"";
	size_t       slash_count = 0;
	for (const wchar_t character: argument)
	{
		if (character == L'\\')
		{
			slash_count += 1;
			continue;
		}
		if (character == L'\"')
		{
			quoted.append(slash_count * 2 + 1, L'\\');
			quoted += character;
			slash_count = 0;
			continue;
		}
		quoted.append(slash_count, L'\\');
		quoted += character;
		slash_count = 0;
	}
	quoted.append(slash_count * 2, L'\\');
	quoted += L'\"';
	return quoted;
}

int RunTarget(const std::vector<std::wstring>& target_arguments)
{
	std::wstring command_line;
	for (const auto& argument: target_arguments)
	{
		if (!command_line.empty())
		{
			command_line += L' ';
		}
		command_line += QuoteWindowsArgument(argument);
	}
	std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
	mutable_command_line.push_back(L'\0');

	STARTUPINFOW        startup_info {};
	PROCESS_INFORMATION process_info {};
	startup_info.cb = sizeof(startup_info);
	if (CreateProcessW(target_arguments.front().c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
	                   &startup_info, &process_info) == FALSE)
	{
		std::fprintf(stderr, "architecture boundary test launcher failed to start target: %lu\n",
		             static_cast<unsigned long>(GetLastError()));
		return kLaunchFailure;
	}

	const DWORD wait_result = WaitForSingleObject(process_info.hProcess, INFINITE);
	if (wait_result != WAIT_OBJECT_0)
	{
		std::fprintf(stderr, "architecture boundary test launcher failed while waiting for target: %lu\n",
		             static_cast<unsigned long>(GetLastError()));
		CloseHandle(process_info.hThread);
		CloseHandle(process_info.hProcess);
		return kLaunchFailure;
	}

	DWORD exit_code = kLaunchFailure;
	if (GetExitCodeProcess(process_info.hProcess, &exit_code) == FALSE)
	{
		std::fprintf(stderr, "architecture boundary test launcher could not read target exit code: %lu\n",
		             static_cast<unsigned long>(GetLastError()));
		exit_code = kLaunchFailure;
	}
	CloseHandle(process_info.hThread);
	CloseHandle(process_info.hProcess);
	return static_cast<int>(exit_code);
}

int RunWideArguments(int argc, wchar_t** argv)
{
	if (argc < 2 || argv[1] == nullptr || argv[1][0] == L'\0')
	{
		std::fputs("architecture boundary test launcher requires a target executable\n", stderr);
		return kLaunchFailure;
	}

	std::vector<std::wstring> target_arguments;
	target_arguments.reserve(static_cast<size_t>(argc - 1));
	for (int index = 1; index < argc; index += 1)
	{
		target_arguments.emplace_back(argv[index]);
	}
	return RunTarget(target_arguments);
}

int RunWindowsMain()
{
	WriteStderrPrefix();

	int       argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv == nullptr)
	{
		std::fprintf(stderr, "architecture boundary test launcher failed to read Unicode arguments: %lu\n",
		             static_cast<unsigned long>(GetLastError()));
		return kLaunchFailure;
	}
	const int result = RunWideArguments(argc, argv);
	LocalFree(argv);
	return result;
}

#else

int RunNarrowArguments(int argc, char** argv)
{
	if (argc < 2 || argv[1] == nullptr || argv[1][0] == '\0')
	{
		std::fputs("architecture boundary test launcher requires a target executable\n", stderr);
		return kLaunchFailure;
	}

	std::vector<char*> target_arguments;
	target_arguments.reserve(static_cast<size_t>(argc));
	for (int index = 1; index < argc; index += 1)
	{
		target_arguments.push_back(argv[index]);
	}
	target_arguments.push_back(nullptr);

	pid_t process_id = 0;
	const int spawn_result = posix_spawn(&process_id, target_arguments.front(), nullptr, nullptr, target_arguments.data(), environ);
	if (spawn_result != 0)
	{
		std::fprintf(stderr, "architecture boundary test launcher failed to start target: %s\n", std::strerror(spawn_result));
		return kLaunchFailure;
	}

	int status = 0;
	while (waitpid(process_id, &status, 0) < 0)
	{
		if (errno == EINTR)
		{
			continue;
		}
		std::fprintf(stderr, "architecture boundary test launcher failed while waiting for target: %s\n", std::strerror(errno));
		return kLaunchFailure;
	}
	if (WIFEXITED(status))
	{
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status))
	{
		std::fprintf(stderr, "architecture boundary test launcher target terminated by signal %d\n", WTERMSIG(status));
	}
	return kLaunchFailure;
}

#endif

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
	(void) argc;
	(void) argv;
	return RunWindowsMain();
#else
	WriteStderrPrefix();
	return RunNarrowArguments(argc, argv);
#endif
}
