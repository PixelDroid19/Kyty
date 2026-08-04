#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
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

constexpr int kLaunchFailure = 125;

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

int RunChecker(const std::vector<std::wstring>& wide_arguments)
{
	std::wstring command_line;
	for (const auto& argument: wide_arguments)
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
	if (CreateProcessW(wide_arguments.front().c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
	                   &startup_info, &process_info) == FALSE)
	{
		std::fprintf(stderr, "architecture boundary integration failed to start checker: %lu\n",
		             static_cast<unsigned long>(GetLastError()));
		return kLaunchFailure;
	}

	const DWORD wait_result = WaitForSingleObject(process_info.hProcess, INFINITE);
	if (wait_result != WAIT_OBJECT_0)
	{
		std::fprintf(stderr, "architecture boundary integration failed while waiting for checker: %lu\n",
		             static_cast<unsigned long>(GetLastError()));
		CloseHandle(process_info.hThread);
		CloseHandle(process_info.hProcess);
		return kLaunchFailure;
	}

	DWORD exit_code = kLaunchFailure;
	if (GetExitCodeProcess(process_info.hProcess, &exit_code) == FALSE)
	{
		std::fprintf(stderr, "architecture boundary integration could not read checker exit code: %lu\n",
		             static_cast<unsigned long>(GetLastError()));
		exit_code = kLaunchFailure;
	}
	CloseHandle(process_info.hThread);
	CloseHandle(process_info.hProcess);
	return static_cast<int>(exit_code);
}

#else

int RunChecker(const std::vector<char*>& arguments)
{
	std::vector<char*> child_arguments = arguments;
	child_arguments.push_back(nullptr);
	pid_t process_id = 0;
	const int spawn_result = posix_spawn(&process_id, child_arguments.front(), nullptr, nullptr, child_arguments.data(), environ);
	if (spawn_result != 0)
	{
		std::fprintf(stderr, "architecture boundary integration failed to start checker: %s\n", std::strerror(spawn_result));
		return kLaunchFailure;
	}

	int status = 0;
	while (waitpid(process_id, &status, 0) < 0)
	{
		if (errno == EINTR)
		{
			continue;
		}
		std::fprintf(stderr, "architecture boundary integration failed while waiting for checker: %s\n", std::strerror(errno));
		return kLaunchFailure;
	}
	if (WIFEXITED(status))
	{
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status))
	{
		std::fprintf(stderr, "architecture boundary integration checker terminated by signal %d\n", WTERMSIG(status));
	}
	return kLaunchFailure;
}

#endif

} // namespace

#if defined(_WIN32)

int RunWideArguments(int argc, wchar_t** argv)
{
	if (argc != 4 && argc != 5)
	{
		std::fputs("usage: ArchitectureBoundaryIntegration <python> <checker> <source-root> [--strict]\n", stderr);
		return kLaunchFailure;
	}
	if (argc == 5 && std::wcscmp(argv[4], L"--strict") != 0)
	{
		std::fputs("architecture boundary integration only accepts --strict as an optional argument\n", stderr);
		return kLaunchFailure;
	}

	std::vector<std::wstring> checker_arguments;
	checker_arguments.reserve(static_cast<size_t>(argc - 1));
	checker_arguments.emplace_back(argv[1]);
	checker_arguments.emplace_back(argv[2]);
	if (argc == 5)
	{
		checker_arguments.emplace_back(L"--strict");
	}
	checker_arguments.emplace_back(argv[3]);

	const int result = RunChecker(checker_arguments);
	if (result != 0)
	{
		std::fprintf(stderr, "architecture boundary integration failed: checker exited with %d\n", result);
	}
	return result;
}

int RunWindowsMain()
{
	int       argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv == nullptr)
	{
		std::fprintf(stderr, "architecture boundary integration failed to read Unicode arguments: %lu\n",
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
	if (argc != 4 && argc != 5)
	{
		std::fputs("usage: ArchitectureBoundaryIntegration <python> <checker> <source-root> [--strict]\n", stderr);
		return kLaunchFailure;
	}
	if (argc == 5 && std::strcmp(argv[4], "--strict") != 0)
	{
		std::fputs("architecture boundary integration only accepts --strict as an optional argument\n", stderr);
		return kLaunchFailure;
	}

	char strict_argument[] = "--strict";
	std::vector<char*> checker_arguments;
	checker_arguments.reserve(static_cast<size_t>(argc));
	checker_arguments.push_back(argv[1]);
	checker_arguments.push_back(argv[2]);
	if (argc == 5)
	{
		checker_arguments.push_back(strict_argument);
	}
	checker_arguments.push_back(argv[3]);

	const int result = RunChecker(checker_arguments);
	if (result != 0)
	{
		std::fprintf(stderr, "architecture boundary integration failed: checker exited with %d\n", result);
	}
	return result;
}

#endif

int main(int argc, char** argv)
{
#if defined(_WIN32)
	(void) argc;
	(void) argv;
	return RunWindowsMain();
#else
	return RunNarrowArguments(argc, argv);
#endif
}
