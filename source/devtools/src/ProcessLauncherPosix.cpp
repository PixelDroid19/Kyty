#include "Kyty/DevTools/Supervisor/ProcessLauncher.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace Kyty::DevTools {

struct ProcessHandle::State
{
	pid_t              pid            = -1;
	bool               valid          = false;
	bool               terminal       = false;
	ProcessObservation cached {};
};

ProcessHandle::ProcessHandle() noexcept = default;
ProcessHandle::ProcessHandle(ProcessHandle&&) noexcept = default;
ProcessHandle& ProcessHandle::operator=(ProcessHandle&&) noexcept = default;
ProcessHandle::~ProcessHandle()                                   = default;

bool ProcessHandle::IsValid() const noexcept
{
	return state_ && state_->valid;
}

void ProcessHandle::Reset(std::unique_ptr<State> s) noexcept
{
	state_ = std::move(s);
}

namespace {

void DecodeStatus(int status, ProcessStatus* out) noexcept
{
	*out = {};
	if (WIFEXITED(status))
	{
		out->liveness    = ProcessLiveness::Terminated;
		out->termination = ProcessTermination::ExitCode;
		out->code_valid  = 1;
		out->code        = static_cast<uint32_t>(WEXITSTATUS(status));
		return;
	}
	if (WIFSIGNALED(status))
	{
		out->liveness    = ProcessLiveness::Terminated;
		out->termination = ProcessTermination::Signal;
		out->code_valid  = 1;
		out->code        = static_cast<uint32_t>(WTERMSIG(status));
		return;
	}
	out->liveness = ProcessLiveness::Unknown;
	out->error    = ProcessStatusError::MalformedTerminalStatus;
}

void FillUsage(const struct rusage& ru, ProcessUsage* usage) noexcept
{
	usage->valid     = 1;
	usage->user_ns   = static_cast<uint64_t>(ru.ru_utime.tv_sec) * 1000000000ull +
	                 static_cast<uint64_t>(ru.ru_utime.tv_usec) * 1000ull;
	usage->system_ns = static_cast<uint64_t>(ru.ru_stime.tv_sec) * 1000000000ull +
	                   static_cast<uint64_t>(ru.ru_stime.tv_usec) * 1000ull;
}

ProcessOperationError ReapOnce(ProcessHandle::State* st, bool hang, ProcessObservation* out) noexcept
{
	if (st == nullptr || out == nullptr)
	{
		return ProcessOperationError::InvalidArgument;
	}
	if (st->terminal)
	{
		*out = st->cached;
		return ProcessOperationError::None;
	}
	for (;;)
	{
		struct rusage ru {};
		int           status = 0;
		const int     flags  = hang ? 0 : WNOHANG;
		const pid_t   r      = ::wait4(st->pid, &status, flags, &ru);
		if (r < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			out->status                = {};
			out->status.error          = ProcessStatusError::WaitFailed;
			out->status.platform_error = static_cast<uint32_t>(errno);
			out->status.liveness       = ProcessLiveness::Unknown;
			out->usage                 = {};
			return ProcessOperationError::HandleFailed;
		}
		if (r == 0)
		{
			out->status.liveness = ProcessLiveness::Running;
			out->usage           = {};
			return ProcessOperationError::None;
		}
		DecodeStatus(status, &out->status);
		FillUsage(ru, &out->usage);
		st->cached   = *out;
		st->terminal = true;
		return ProcessOperationError::None;
	}
}

} // namespace

LaunchResult ProcessLauncher::Launch(const LaunchOptions& options) noexcept
{
	LaunchResult result {};
	if (options.executable == nullptr || options.argv == nullptr || options.argc == 0u)
	{
		result.error = ProcessOperationError::InvalidArgument;
		return result;
	}
	if (options.mapping_fd < 0 || options.liveness_fd < 0)
	{
		result.error = ProcessOperationError::InvalidArgument;
		return result;
	}

	posix_spawn_file_actions_t actions;
	if (posix_spawn_file_actions_init(&actions) != 0)
	{
		result.error          = ProcessOperationError::SpawnFailed;
		result.platform_error = static_cast<uint32_t>(errno);
		return result;
	}

	// Remap mapping/liveness to fixed descriptors 3 and 4.
	if (posix_spawn_file_actions_adddup2(&actions, options.mapping_fd, 3) != 0 ||
	    posix_spawn_file_actions_adddup2(&actions, options.liveness_fd, 4) != 0)
	{
		posix_spawn_file_actions_destroy(&actions);
		result.error = ProcessOperationError::SpawnFailed;
		return result;
	}

#if defined(__linux__) && defined(POSIX_SPAWN_SETSID)
	// Prefer closefrom when available (glibc 2.34+).
#if defined(__GLIBC__)
#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)
	if (posix_spawn_file_actions_addclosefrom_np(&actions, 5) != 0)
	{
		posix_spawn_file_actions_destroy(&actions);
		result.error = ProcessOperationError::Unsupported;
		return result;
	}
#else
	// Without closefrom, fail closed rather than leak descriptors.
	posix_spawn_file_actions_destroy(&actions);
	result.error = ProcessOperationError::Unsupported;
	return result;
#endif
#else
	posix_spawn_file_actions_destroy(&actions);
	result.error = ProcessOperationError::Unsupported;
	return result;
#endif
#else
	// Non-Linux POSIX without proven close-all: fail closed.
	posix_spawn_file_actions_destroy(&actions);
	result.error = ProcessOperationError::Unsupported;
	return result;
#endif

	// Build child env with bootstrap replacement.
	// Count environ.
	int env_count = 0;
	for (char** e = environ; e != nullptr && *e != nullptr; ++e)
	{
		++env_count;
	}
	// Allocate envp: existing + 1 bootstrap + nullptr. Use heap.
	char** envp = static_cast<char**>(std::calloc(static_cast<size_t>(env_count) + 2u, sizeof(char*)));
	if (envp == nullptr)
	{
		posix_spawn_file_actions_destroy(&actions);
		result.error = ProcessOperationError::SpawnFailed;
		return result;
	}
	int o = 0;
	for (char** e = environ; e != nullptr && *e != nullptr; ++e)
	{
		if (std::strncmp(*e, "KYTY_DEVTOOLS_BOOTSTRAP_V1=", 27) == 0)
		{
			continue; // replace
		}
		envp[o++] = *e;
	}
	char bootstrap_kv[160] = {};
	std::snprintf(bootstrap_kv, sizeof(bootstrap_kv), "KYTY_DEVTOOLS_BOOTSTRAP_V1=%.*s",
	              static_cast<int>(options.bootstrap.size), options.bootstrap.bytes);
	envp[o++] = bootstrap_kv;
	envp[o]   = nullptr;

	// argv must be null-terminated copy of pointers.
	char** argv = static_cast<char**>(std::calloc(options.argc + 1u, sizeof(char*)));
	if (argv == nullptr)
	{
		std::free(envp);
		posix_spawn_file_actions_destroy(&actions);
		result.error = ProcessOperationError::SpawnFailed;
		return result;
	}
	for (uint32_t i = 0; i < options.argc; ++i)
	{
		argv[i] = const_cast<char*>(options.argv[i]);
	}

	pid_t pid = -1;
	const int rc = posix_spawn(&pid, options.executable, &actions, nullptr, argv, envp);
	posix_spawn_file_actions_destroy(&actions);
	std::free(argv);
	std::free(envp);

	if (rc != 0)
	{
		result.error          = ProcessOperationError::SpawnFailed;
		result.platform_error = static_cast<uint32_t>(rc);
		return result;
	}

	auto st   = std::make_unique<ProcessHandle::State>();
	st->pid   = pid;
	st->valid = true;
	result.process.Reset(std::move(st));
	return result;
}

ProcessOperationError ProcessLauncher::Poll(ProcessHandle* handle, ProcessObservation* out) noexcept
{
	if (handle == nullptr || out == nullptr || !handle->IsValid())
	{
		return ProcessOperationError::InvalidArgument;
	}
	return ReapOnce(handle->GetState(), false, out);
}

ProcessOperationError ProcessLauncher::Wait(ProcessHandle* handle, ProcessObservation* out) noexcept
{
	if (handle == nullptr || out == nullptr || !handle->IsValid())
	{
		return ProcessOperationError::InvalidArgument;
	}
	return ReapOnce(handle->GetState(), true, out);
}

ProcessOperationError ProcessLauncher::ForwardSignal(ProcessHandle* handle, uint32_t signal) noexcept
{
	if (handle == nullptr || !handle->IsValid())
	{
		return ProcessOperationError::InvalidArgument;
	}
	if (::kill(handle->GetState()->pid, static_cast<int>(signal)) != 0)
	{
		return ProcessOperationError::HandleFailed;
	}
	return ProcessOperationError::None;
}

// --- Process identity ---

bool ParseLinuxProcStatStartTicks(const char* stat_line, uint64_t* start_ticks) noexcept
{
	if (stat_line == nullptr || start_ticks == nullptr)
	{
		return false;
	}
	// Format: pid (comm) state ... field 22 is starttime after comm.
	const char* open = std::strchr(stat_line, '(');
	const char* close = std::strrchr(stat_line, ')');
	if (open == nullptr || close == nullptr || close < open)
	{
		return false;
	}
	// After ") " come fields starting at state = field 3.
	const char* p = close + 1;
	while (*p == ' ')
	{
		++p;
	}
	// We need field index 22 overall = 20 fields after state (field 3).
	// After ')': field3=state, field4=ppid, ... field22=starttime is the 20th token after state.
	// Tokens after ") ": index 0 is state (field 3), index 19 is field 22.
	for (int i = 0; i < 19; ++i)
	{
		while (*p != '\0' && *p != ' ')
		{
			++p;
		}
		while (*p == ' ')
		{
			++p;
		}
		if (*p == '\0')
		{
			return false;
		}
	}
	char* end = nullptr;
	errno     = 0;
	const unsigned long long v = std::strtoull(p, &end, 10);
	if (errno != 0 || end == p || v == 0ull)
	{
		return false;
	}
	*start_ticks = static_cast<uint64_t>(v);
	return true;
}

ProcessIdentityError QueryProcessIdentity(const ProcessHandle& handle, ProcessIdentity* out) noexcept
{
	if (out == nullptr || !handle.IsValid())
	{
		return ProcessIdentityError::Unavailable;
	}
	const pid_t pid = handle.GetState()->pid;
	char        path[64] = {};
	std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
	const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return ProcessIdentityError::Unavailable;
	}
	char    buf[1024] = {};
	const ssize_t n = ::read(fd, buf, sizeof(buf) - 1u);
	::close(fd);
	if (n <= 0)
	{
		return ProcessIdentityError::Unavailable;
	}
	buf[n] = '\0';
	uint64_t ticks = 0;
	if (!ParseLinuxProcStatStartTicks(buf, &ticks))
	{
		return ProcessIdentityError::Malformed;
	}
	out->pid         = static_cast<uint64_t>(pid);
	out->start_token = ticks;
	return ProcessIdentityError::None;
}

ProcessIdentityProbe ProbeProcessIdentity(uint64_t pid, uint64_t expected_start_token) noexcept
{
	char path[64] = {};
	std::snprintf(path, sizeof(path), "/proc/%llu/stat", static_cast<unsigned long long>(pid));
	const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return ProcessIdentityProbe::Dead;
	}
	char buf[1024] = {};
	const ssize_t n = ::read(fd, buf, sizeof(buf) - 1u);
	::close(fd);
	if (n <= 0)
	{
		return ProcessIdentityProbe::Unreadable;
	}
	buf[n] = '\0';
	uint64_t ticks = 0;
	if (!ParseLinuxProcStatStartTicks(buf, &ticks))
	{
		return ProcessIdentityProbe::Malformed;
	}
	if (ticks == expected_start_token)
	{
		return ProcessIdentityProbe::AliveMatch;
	}
	return ProcessIdentityProbe::AliveDifferentStart;
}

} // namespace Kyty::DevTools
