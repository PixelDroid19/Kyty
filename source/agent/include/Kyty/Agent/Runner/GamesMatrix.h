#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_GAMESMATRIX_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_GAMESMATRIX_H_

#include "Kyty/Agent/Runner/JsonValue.h"
#include "Kyty/Agent/Runner/RunnerHost.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Kyty::Agent::Runner {

// Multi-game integration matrix: discovers package roots (directories holding
// eboot.bin) under a games root, launches each once in strict or explicitly
// diagnostic bring-up mode, observes it through the agent socket, and writes a
// summary with anonymous case IDs and no host paths or title identifiers.

struct MatrixCase
{
	std::string              case_id;
	std::string              outcome;
	std::string              first_frontier;
	bool                     valid_package = false;
	bool                     attempted     = false;
	bool                     progressed    = false; // reached runtime_started or later
	std::optional<int>       child_exit;
	std::string              agent_phase;
	std::string              agent_frontier;
	int64_t                  present = 0;
	std::vector<std::string> notes;

	[[nodiscard]] Json ToJson() const;
};

// Directories that directly contain eboot.bin, sorted case-insensitively.
[[nodiscard]] std::vector<std::string> DiscoverGameRoots(const std::string& games_root, int max_depth);
// Empty when `root` is a launchable package, otherwise the invalid_fixture reason.
[[nodiscard]] std::string InvalidPackageReason(const std::string& root);
// "case_" + 12 hex digits of SHA-256 over the root's path relative to the games root.
[[nodiscard]] std::string AnonymousCaseId(const std::string& games_root, const std::string& package_root);

// (outcome, first_frontier) from the process exit and the agent observation.
// `runner_stopped`: the runner terminated a still-running child itself.
[[nodiscard]] std::pair<std::string, std::string> MapMatrixOutcome(std::optional<int> process_exit, bool agent_ready, const Json& status,
                                                                   const std::string& last_error_code, bool timed_out,
                                                                   bool runner_stopped);

// Replaces Windows and POSIX paths and PPSA/CUSA identifiers in every string value.
[[nodiscard]] std::string SanitizeMatrixText(const std::string& text);
[[nodiscard]] Json        SanitizeMatrixJson(const Json& value);
// One short host or guest error label from a child log, or empty.
[[nodiscard]] std::string MatrixActionableFromLog(const std::string& log_path);

struct MatrixCaseOptions
{
	std::string case_id;
	std::string package_root;
	std::string repo_root;
	std::string fc_script;
	std::string case_dir;
	std::string run_id;
	std::string mode = "strict"; // or "bringup"
	double      startup_deadline_s = 45.0;
	double      present_deadline_s = 90.0;
	double      total_deadline_s   = 120.0;
	double      stability_s        = 10.0;
	Environment base_environment;
};

[[nodiscard]] MatrixCase RunMatrixCase(RunnerHost* host, const MatrixCaseOptions& options);

// 1 when an attempted case failed to launch or load, was unsupported or crashed.
[[nodiscard]] int MatrixExitCode(const std::vector<MatrixCase>& results);

// `kyty_agent games-matrix [--games-root DIR] [--scratch DIR] [--fc-script PATH] ...`
[[nodiscard]] int GamesMatrixMain(const std::vector<std::string>& args);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_GAMESMATRIX_H_ */
