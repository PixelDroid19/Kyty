#include "Kyty/Agent/Runner/RunnerHost.h"

#if !defined(_WIN32)

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Kyty::Agent::Runner {
namespace {

constexpr size_t kMaxResponseBytes = 262144;

AgentReply TransportFailure(const char* message)
{
	Json error = Json::MakeObject();
	error.Set("code", "transport").Set("message", message);
	Json body = Json::MakeObject();
	body.Set("ok", false).Set("error", std::move(error));
	return {125, std::move(body)};
}

bool FillUnixAddress(const std::string& path, sockaddr_un* address)
{
	std::memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;
	if (path.empty() || path.size() >= sizeof(address->sun_path))
	{
		return false;
	}
	std::memcpy(address->sun_path, path.data(), path.size());
	return true;
}

double MonotonicSeconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Waits until fd is ready for `events` or the deadline passes.
bool WaitFd(int fd, short events, double deadline)
{
	for (;;)
	{
		const double remaining = deadline - MonotonicSeconds();
		if (remaining <= 0.0)
		{
			return false;
		}
		pollfd entry {fd, events, 0};
		const int rc = ::poll(&entry, 1, static_cast<int>(remaining * 1000.0) + 1);
		if (rc > 0)
		{
			return true;
		}
		if (rc < 0 && errno != EINTR)
		{
			return false;
		}
	}
}

bool ConnectWithDeadline(int fd, const sockaddr_un& address, double deadline)
{
	if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
	{
		return true;
	}
	// On a unix socket EAGAIN means the listener backlog is full and nothing
	// is in progress; the caller retries.
	if (errno != EINPROGRESS)
	{
		return false;
	}
	if (!WaitFd(fd, POLLOUT, deadline))
	{
		errno = ETIMEDOUT;
		return false;
	}
	int       socket_error = 0;
	socklen_t length       = sizeof(socket_error);
	if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 || socket_error != 0)
	{
		errno = socket_error != 0 ? socket_error : errno;
		return false;
	}
	return true;
}

bool SendAll(int fd, const std::string& payload, double deadline)
{
	size_t sent = 0;
	while (sent < payload.size())
	{
		if (!WaitFd(fd, POLLOUT, deadline))
		{
			return false;
		}
		const ssize_t written = ::send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
		if (written < 0 && errno != EINTR && errno != EAGAIN)
		{
			return false;
		}
		sent += written > 0 ? static_cast<size_t>(written) : 0u;
	}
	return true;
}

// Reads up to the first newline. Returns false with `message` on failure.
bool ReadLine(int fd, double deadline, std::string* line, const char** message)
{
	std::string response;
	char        chunk[8192];
	while (response.find('\n') == std::string::npos)
	{
		if (!WaitFd(fd, POLLIN, deadline))
		{
			*message = "TimeoutError";
			return false;
		}
		const ssize_t got = ::recv(fd, chunk, std::min(sizeof(chunk), kMaxResponseBytes - response.size()), 0);
		if (got < 0 && (errno == EINTR || errno == EAGAIN))
		{
			continue;
		}
		if (got < 0)
		{
			*message = "OSError";
			return false;
		}
		if (got == 0)
		{
			break;
		}
		response.append(chunk, static_cast<size_t>(got));
		if (response.size() >= kMaxResponseBytes && response.find('\n') == std::string::npos)
		{
			*message = "response_too_large";
			return false;
		}
	}
	*line = response.substr(0, response.find('\n'));
	return true;
}

class PosixRunnerHost final: public RunnerHost
{
public:
	~PosixRunnerHost() override
	{
		if (m_pid > 0 && !m_exit.has_value())
		{
			std::vector<std::string> notes;
			TerminateChild(&notes);
		}
	}

	double Now() override { return MonotonicSeconds(); }

	void Sleep(double seconds) override
	{
		if (seconds > 0.0)
		{
			std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
		}
	}

	bool Launch(const LaunchSpec& spec, std::string* error) override
	{
		if (spec.argv.empty())
		{
			*error = "empty command";
			return false;
		}
		const int log_fd = ::open(spec.log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (log_fd < 0)
		{
			*error = "cannot open child log: " + std::string(std::strerror(errno));
			return false;
		}
		// Everything the child needs is built before fork; the child only calls async-signal-safe functions.
		std::vector<std::string> env_lines;
		env_lines.reserve(spec.env.size());
		for (const auto& [key, value]: spec.env)
		{
			env_lines.push_back(key + "=" + value);
		}
		std::vector<char*> argv;
		for (const auto& arg: spec.argv)
		{
			argv.push_back(const_cast<char*>(arg.c_str()));
		}
		argv.push_back(nullptr);
		std::vector<char*> envp;
		for (auto& line: env_lines)
		{
			envp.push_back(line.data());
		}
		envp.push_back(nullptr);

		const pid_t pid = ::fork();
		if (pid < 0)
		{
			*error = "fork failed: " + std::string(std::strerror(errno));
			::close(log_fd);
			return false;
		}
		if (pid == 0)
		{
			::setsid();
			if (::chdir(spec.cwd.c_str()) != 0 || ::dup2(log_fd, STDOUT_FILENO) < 0 || ::dup2(log_fd, STDERR_FILENO) < 0)
			{
				::_exit(127);
			}
			const int null_fd = ::open("/dev/null", O_RDONLY);
			if (null_fd >= 0)
			{
				::dup2(null_fd, STDIN_FILENO);
			}
			::execve(argv[0], argv.data(), envp.data());
			::_exit(127);
		}
		::close(log_fd);
		m_pid = pid;
		m_exit.reset();
		return true;
	}

	[[nodiscard]] int64_t ChildPid() const override { return m_pid; }

	std::optional<int> PollChild() override
	{
		if (m_pid <= 0 || m_exit.has_value())
		{
			return m_exit;
		}
		siginfo_t info {};
		if (::waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0 || info.si_pid != m_pid)
		{
			return m_exit;
		}
		// The leader is not reaped yet, so its group id cannot be reused: the
		// kill reaches only processes the run left behind in its session.
		(void)::killpg(static_cast<pid_t>(m_pid), SIGKILL);
		int status = 0;
		if (::waitpid(static_cast<pid_t>(m_pid), &status, 0) == m_pid)
		{
			m_exit = WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
		}
		return m_exit;
	}

	void TerminateChild(std::vector<std::string>* notes) override
	{
		if (PollChild().has_value() || m_pid <= 0)
		{
			return;
		}
		if (!SignalGroup(SIGTERM, notes))
		{
			return;
		}
		if (WaitExit(5.0))
		{
			notes->emplace_back("killed_sigterm");
			return;
		}
		if (!SignalGroup(SIGKILL, notes))
		{
			return;
		}
		notes->emplace_back(WaitExit(5.0) ? "killed_sigkill" : "kill_timeout_after_sigkill");
	}

	AgentReply CallAgent(const std::string& endpoint, const std::string& tool, const Json& args, double timeout_s) override
	{
		if (timeout_s <= 0.0)
		{
			return TransportFailure("TimeoutError");
		}
		sockaddr_un address {};
		if (!FillUnixAddress(endpoint, &address))
		{
			return TransportFailure("OSError");
		}
		const double deadline = MonotonicSeconds() + timeout_s;
		const int    fd       = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (fd < 0)
		{
			return TransportFailure("OSError");
		}
		Json request = Json::MakeObject();
		request.Set("id", 1).Set("tool", tool).Set("args", args.IsObject() ? args : Json::MakeObject());
		std::string line;
		const char* message = "OSError";
		bool        ok      = false;
		if (!ConnectWithDeadline(fd, address, deadline))
		{
			message = errno == ENOENT ? "socket_missing" : errno == ECONNREFUSED ? "ConnectionRefusedError" : "OSError";
		} else if (!SendAll(fd, request.Dump() + "\n", deadline))
		{
			message = "OSError";
		} else
		{
			ok = ReadLine(fd, deadline, &line, &message);
		}
		::close(fd);
		if (!ok)
		{
			return TransportFailure(message);
		}
		if (line.empty())
		{
			return TransportFailure("empty_response");
		}
		Json        body;
		std::string parse_error;
		if (!ParseJson(line, &body, &parse_error))
		{
			return TransportFailure("JSONDecodeError");
		}
		if (!body.IsObject())
		{
			return TransportFailure("invalid_response");
		}
		const bool tool_ok = body["ok"].Truthy();
		return {tool_ok ? 0 : 1, std::move(body)};
	}

private:
	bool SignalGroup(int signal_number, std::vector<std::string>* notes)
	{
		if (::killpg(static_cast<pid_t>(m_pid), signal_number) == 0)
		{
			return true;
		}
		if (errno == ESRCH)
		{
			notes->emplace_back("process_already_gone");
		} else
		{
			notes->emplace_back("kill_failed:" + std::string(std::strerror(errno)));
		}
		return false;
	}

	bool WaitExit(double seconds)
	{
		const double deadline = MonotonicSeconds() + seconds;
		while (MonotonicSeconds() < deadline)
		{
			if (PollChild().has_value())
			{
				return true;
			}
			Sleep(0.05);
		}
		return PollChild().has_value();
	}

	int64_t            m_pid = 0;
	std::optional<int> m_exit;
};

bool IsStaleUnixSocket(const std::string& path)
{
	struct stat info {};
	if (::lstat(path.c_str(), &info) != 0)
	{
		return false;
	}
	if (!S_ISSOCK(info.st_mode))
	{
		return true;
	}
	sockaddr_un address {};
	if (!FillUnixAddress(path, &address))
	{
		return true;
	}
	const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
	{
		return true;
	}
	const bool live = ConnectWithDeadline(fd, address, MonotonicSeconds() + 0.2);
	::close(fd);
	return !live;
}

} // namespace

std::unique_ptr<RunnerHost> CreateSystemRunnerHost()
{
	return std::make_unique<PosixRunnerHost>();
}

void RemoveStaleUnixSocket(const std::string& path, std::vector<std::string>* notes)
{
	if (!IsStaleUnixSocket(path))
	{
		return;
	}
	if (::unlink(path.c_str()) == 0)
	{
		notes->emplace_back("removed_stale_socket");
	} else
	{
		notes->emplace_back("stale_socket_unlink_failed:" + std::string(std::strerror(errno)));
	}
}

} // namespace Kyty::Agent::Runner

#else

namespace Kyty::Agent::Runner {

std::unique_ptr<RunnerHost> CreateSystemRunnerHost()
{
	return nullptr;
}

void RemoveStaleUnixSocket(const std::string& /*path*/, std::vector<std::string>* /*notes*/) {}

} // namespace Kyty::Agent::Runner

#endif
