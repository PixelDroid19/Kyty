#include "Kyty/Agent/LocalTransport.h"
#include "Kyty/Agent/Runner/RunnerHost.h"

#if defined(_WIN32)

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

double MonotonicSeconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One argument quoted so CommandLineToArgvW and the CRT parse it back unchanged.
std::string QuoteArgument(const std::string& arg)
{
	if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
	{
		return arg;
	}
	std::string quoted = "\"";
	size_t      slashes = 0;
	for (const char ch: arg)
	{
		if (ch == '\\')
		{
			++slashes;
			continue;
		}
		// Backslashes double only before a quote, which is then escaped.
		quoted.append(ch == '"' ? slashes * 2 + 1 : slashes, '\\');
		quoted += ch;
		slashes = 0;
	}
	quoted.append(slashes * 2, '\\');
	return quoted + "\"";
}

// Sorted "KEY=VALUE\0...\0\0" block; Environment is ordered by key already.
std::string EnvironmentBlock(const Environment& env)
{
	std::string block;
	for (const auto& [key, value]: env)
	{
		block += key + "=" + value;
		block += '\0';
	}
	block += '\0';
	return block;
}

// Interrupts a pipe connection when its deadline passes, so a silent agent cannot block a call.
class ConnectionWatchdog
{
public:
	ConnectionWatchdog(LocalTransport::Connection* connection, double deadline)
	    : m_thread([this, connection, deadline] {
		      std::unique_lock lock(m_mutex);
		      const auto       timeout = std::chrono::duration<double>(deadline - MonotonicSeconds());
		      if (!m_done.wait_for(lock, timeout, [this] { return m_finished; }))
		      {
			      m_fired = true;
			      LocalTransport::Interrupt(connection);
		      }
	      })
	{
	}

	~ConnectionWatchdog() { Stop(); }

	// True when the deadline interrupted the connection.
	bool Stop()
	{
		{
			std::scoped_lock lock(m_mutex);
			m_finished = true;
		}
		m_done.notify_all();
		if (m_thread.joinable())
		{
			m_thread.join();
		}
		return m_fired;
	}

private:
	std::mutex              m_mutex;
	std::condition_variable m_done;
	bool                    m_finished = false;
	bool                    m_fired    = false;
	std::thread             m_thread;
};

class WindowsRunnerHost final: public RunnerHost
{
public:
	~WindowsRunnerHost() override
	{
		if (m_process != nullptr && !m_exit.has_value())
		{
			std::vector<std::string> notes;
			TerminateChild(&notes);
		}
		CloseChild();
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
		CloseChild();
		SECURITY_ATTRIBUTES inherit {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
		HANDLE log = ::CreateFileA(spec.log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
		                           FILE_ATTRIBUTE_NORMAL, nullptr);
		if (log == INVALID_HANDLE_VALUE)
		{
			*error = "cannot open child log: " + std::to_string(::GetLastError());
			return false;
		}
		HANDLE null_input = ::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
		// The job ends every process the child starts once it is closed or terminated.
		HANDLE job = ::CreateJobObjectA(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		const bool job_ready = job != nullptr && ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0;

		std::string command_line;
		for (const auto& arg: spec.argv)
		{
			command_line += (command_line.empty() ? "" : " ") + QuoteArgument(arg);
		}
		std::string         env_block = EnvironmentBlock(spec.env);
		STARTUPINFOA        startup {};
		PROCESS_INFORMATION process {};
		startup.cb         = sizeof(startup);
		startup.dwFlags    = STARTF_USESTDHANDLES;
		startup.hStdInput  = null_input != INVALID_HANDLE_VALUE ? null_input : nullptr;
		startup.hStdOutput = log;
		startup.hStdError  = log;
		const bool created = job_ready && ::CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
		                                                   env_block.data(), spec.cwd.c_str(), &startup, &process) != 0;
		const DWORD create_error = ::GetLastError();
		::CloseHandle(log);
		if (null_input != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(null_input);
		}
		if (!created)
		{
			*error = std::string(job_ready ? "CreateProcess" : "job object") + " failed: " + std::to_string(create_error);
			if (job != nullptr)
			{
				::CloseHandle(job);
			}
			return false;
		}
		if (::AssignProcessToJobObject(job, process.hProcess) == 0)
		{
			*error = "AssignProcessToJobObject failed: " + std::to_string(::GetLastError());
			::TerminateProcess(process.hProcess, 1);
			::CloseHandle(process.hThread);
			::CloseHandle(process.hProcess);
			::CloseHandle(job);
			return false;
		}
		::ResumeThread(process.hThread);
		::CloseHandle(process.hThread);
		m_process = process.hProcess;
		m_job     = job;
		m_pid     = process.dwProcessId;
		m_exit.reset();
		return true;
	}

	[[nodiscard]] int64_t ChildPid() const override { return m_pid; }

	std::optional<int> PollChild() override
	{
		if (m_process == nullptr || m_exit.has_value() || ::WaitForSingleObject(m_process, 0) != WAIT_OBJECT_0)
		{
			return m_exit;
		}
		DWORD code = 0;
		if (::GetExitCodeProcess(m_process, &code) == 0)
		{
			return m_exit;
		}
		// The leader is gone: end whatever it left in its job, as the POSIX host does with its group.
		(void)::TerminateJobObject(m_job, code);
		m_exit = static_cast<int>(code);
		return m_exit;
	}

	void TerminateChild(std::vector<std::string>* notes) override
	{
		if (m_process == nullptr || PollChild().has_value())
		{
			return;
		}
		if (::TerminateJobObject(m_job, 1) == 0)
		{
			notes->emplace_back("kill_failed:" + std::to_string(::GetLastError()));
			return;
		}
		// Job termination is a hard kill, the counterpart of SIGKILL in the shared note vocabulary.
		const bool exited = ::WaitForSingleObject(m_process, 5000) == WAIT_OBJECT_0;
		(void)PollChild();
		notes->emplace_back(exited ? "killed_sigkill" : "kill_timeout_after_sigkill");
	}

	AgentReply CallAgent(const std::string& endpoint, const std::string& tool, const Json& args, double timeout_s) override
	{
		if (timeout_s <= 0.0)
		{
			return TransportFailure("TimeoutError");
		}
		LocalTransport::Connection connection;
		const LocalTransport::Result connected = LocalTransport::Connect(endpoint.c_str(), &connection);
		if (connected != LocalTransport::Result::Ok)
		{
			return TransportFailure(connected == LocalTransport::Result::NotFound ? "socket_missing"
			                        : connected == LocalTransport::Result::Busy   ? "ConnectionRefusedError"
			                                                                      : "OSError");
		}
		Json request = Json::MakeObject();
		request.Set("id", 1).Set("tool", tool).Set("args", args.IsObject() ? args : Json::MakeObject());
		const std::string payload = request.Dump() + "\n";
		std::string       line;
		LocalTransport::Result io = LocalTransport::Result::Ok;
		bool                   timed_out = false;
		{
			ConnectionWatchdog watchdog(&connection, MonotonicSeconds() + timeout_s);
			io = LocalTransport::WriteAll(&connection, payload.data(), payload.size());
			if (io == LocalTransport::Result::Ok)
			{
				io = LocalTransport::ReadLine(&connection, &line, kMaxResponseBytes);
			}
			timed_out = watchdog.Stop();
		}
		LocalTransport::Close(&connection);
		if (timed_out)
		{
			return TransportFailure("TimeoutError");
		}
		if (io == LocalTransport::Result::MessageTooLarge)
		{
			return TransportFailure("response_too_large");
		}
		if (io != LocalTransport::Result::Ok && io != LocalTransport::Result::NotFound)
		{
			return TransportFailure("OSError");
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
	void CloseChild()
	{
		if (m_process != nullptr)
		{
			::CloseHandle(m_process);
			m_process = nullptr;
		}
		if (m_job != nullptr)
		{
			// KILL_ON_JOB_CLOSE ends anything still running in the job.
			::CloseHandle(m_job);
			m_job = nullptr;
		}
	}

	HANDLE             m_process = nullptr;
	HANDLE             m_job     = nullptr;
	int64_t            m_pid     = 0;
	std::optional<int> m_exit;
};

} // namespace

std::unique_ptr<RunnerHost> CreateSystemRunnerHost()
{
	return std::make_unique<WindowsRunnerHost>();
}

std::string LocalAgentEndpoint(const std::string& name)
{
	return R"(\\.\pipe\)" + name;
}

void RemoveStaleUnixSocket(const std::string& /*path*/, std::vector<std::string>* /*notes*/) {}

} // namespace Kyty::Agent::Runner

#endif
