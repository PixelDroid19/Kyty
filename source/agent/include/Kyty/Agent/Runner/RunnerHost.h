#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_RUNNERHOST_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_RUNNERHOST_H_

#include "Kyty/Agent/Runner/JsonValue.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Kyty::Agent::Runner {

using Environment = std::map<std::string, std::string>;

// Every permissive or automation-only switch is forbidden in strict children.
[[nodiscard]] const std::vector<std::string>& StrictForbiddenEnvironmentKeys();

struct ChildEnvironmentOptions
{
	std::string              guest_root;
	std::string              capture_directory;
	std::string              agent_endpoint;   // empty: no agent endpoint
	std::vector<std::string> optional_values;  // copied only when set and non-empty
	Environment              default_values;   // parent value when set, otherwise this default
};

// Canonical minimal environment for a strict Kyty child. No unrelated parent
// state crosses the boundary.
[[nodiscard]] Environment BuildChildEnvironment(const Environment& base, const ChildEnvironmentOptions& options);

// Active (non-empty) keys of `keys` in `env`, in declaration order, excluding `allowed`.
[[nodiscard]] std::vector<std::string> FindActiveEnvironmentKeys(const Environment& env, const std::vector<std::string>& keys,
                                                                 const std::vector<std::string>& allowed = {});

[[nodiscard]] Environment CurrentEnvironment();

struct LaunchSpec
{
	std::vector<std::string> argv;
	std::string              cwd;
	Environment              env;
	std::string              log_path; // child stdout and stderr
};

struct AgentReply
{
	int  code = 125; // 0 ok, 1 tool error, 125 transport
	Json body;
};

// Host boundary of a runner session: clock, child process and agent socket.
// Sessions use only this interface, so tests can drive them deterministically.
class RunnerHost
{
public:
	virtual ~RunnerHost() = default;

	[[nodiscard]] virtual double Now() = 0; // monotonic seconds
	virtual void                 Sleep(double seconds) = 0;

	[[nodiscard]] virtual bool Launch(const LaunchSpec& spec, std::string* error) = 0;
	[[nodiscard]] virtual int64_t ChildPid() const = 0;
	// Exit code once the child is gone; a signal death is the negative signal number.
	[[nodiscard]] virtual std::optional<int> PollChild() = 0;
	// SIGTERM to the child's process group, then SIGKILL after 5 s; records the outcome in notes.
	virtual void TerminateChild(std::vector<std::string>* notes) = 0;

	// One line-delimited agent request. A non-positive timeout is a transport failure.
	[[nodiscard]] virtual AgentReply CallAgent(const std::string& endpoint, const std::string& tool, const Json& args,
	                                           double timeout_s) = 0;
};

// Real host for POSIX systems; nullptr where runner sessions are unsupported.
[[nodiscard]] std::unique_ptr<RunnerHost> CreateSystemRunnerHost();

// Removes a socket path that no live listener backs; records the outcome.
void RemoveStaleUnixSocket(const std::string& path, std::vector<std::string>* notes);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_RUNNERHOST_H_ */
