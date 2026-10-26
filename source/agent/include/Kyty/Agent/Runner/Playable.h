#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_PLAYABLE_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_PLAYABLE_H_

#include "Kyty/Agent/Runner/JsonValue.h"
#include "Kyty/Agent/Runner/RunnerHost.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Kyty::Agent::Runner {

// Strict playable regression (orchestration only): launches the standard guest
// path, observes it through kyty_agent tools, delivers pad edges, captures a
// native frame and compares material metrics against an untracked baseline.
//
// Exit codes: 0 all gates passed, 1 playable gate failed, 2 usage or missing
// tooling, 3 baseline missing without --create-baseline.

[[nodiscard]] Json DefaultPlayableProfile();
// Shallow merge over the defaults; object values merge one level deep.
[[nodiscard]] Json MergePlayableProfile(const Json& data);

struct PadCounters
{
	int64_t delivered_taps           = 0;
	int64_t guest_read_state_samples = 0;
	int64_t guest_read_samples       = 0;
	bool    tap_pending              = false;
};

[[nodiscard]] PadCounters PadCountersFromStatus(const Json& status);
[[nodiscard]] Json        PadCountersJson(const PadCounters& counters);

struct PadSequenceResult
{
	bool ok = true;
	Json events;
};

// Delivers pad steps, keeps holds active while guest reads are observed, waits
// for each tap to drain, and always sends pad_clear. A deadline reserves time
// for the clear.
[[nodiscard]] PadSequenceResult DeliverPadSequence(RunnerHost* host, const std::string& endpoint, const Json& steps,
                                                   std::optional<double> deadline, double inter_tap_settle_s,
                                                   const std::function<void(const Json&)>& status_observer);

struct PostInputWaitState
{
	bool                   loading_seen = false;
	std::optional<double>  interactive_since;
	std::optional<int64_t> interactive_start_present;
};

// True once presents advanced by min_present_delta and min_settle_s elapsed
// since the window started. The agent phase is diagnostic only.
[[nodiscard]] bool AdvancePostInputWait(PostInputWaitState* state, int64_t present, double now, int64_t min_present_delta,
                                        double min_settle_s);
// Records a phase seen during input; loading resets the settle window.
[[nodiscard]] bool RecordInputPhaseObservation(PostInputWaitState* state, const std::string& phase);

[[nodiscard]] std::string ClassifyExit(std::optional<int> exit_code, bool timed_out, bool coordinator_stop);

[[nodiscard]] Json InputDeliveryChecks(const Json& before, const Json& after, const Json& milestones, bool sequence_ok);

struct GateResult
{
	std::string name;
	bool        passed = false;
	std::string detail;
};

struct GateInputs
{
	bool                   agent_ready       = false;
	bool                   video_initialized = false;
	std::optional<int64_t> first_present;
	int64_t                present_delta = 0;
	Json                   input_before  = Json::MakeObject();
	Json                   input_after   = Json::MakeObject();
	std::string            capture_path;
	Json                   compare           = Json::MakeObject();
	bool                   baseline_missing  = false;
	bool                   create_baseline   = false;
	std::string            shutdown;
	std::string            first_error;
	Json                   milestones = Json::MakeObject();
	bool                   visual_require_baseline = true;
	bool                   input_sequence_ok       = true;
	Json                   checkpoint;
	bool                   timed_out            = false;
	bool                   final_error_observed = true;
};

[[nodiscard]] std::vector<GateResult> EvaluateGates(const GateInputs& inputs);

[[nodiscard]] Json VisualFloorFromMetrics(const Json& metrics);
[[nodiscard]] Json ComparePlayableVisualMetrics(const Json& metrics, const Json& baseline);

[[nodiscard]] std::string FirstActionableError(const Json& events, const Json& last_error);
[[nodiscard]] std::string FirstActionableFromLog(const std::string& log_path);

// Replaces the guest root and home/title-like markers in every string value.
[[nodiscard]] std::string SanitizeText(const std::string& text, const std::string& guest_root);
[[nodiscard]] Json        SanitizeJson(const Json& value, const std::string& guest_root);

struct PlayableOptions
{
	std::string repo_root;
	std::string guest_root;
	std::string fc_script;
	std::string runtime_cwd;
	std::string guest_script;
	std::string scratch;
	std::string baseline_path;
	std::string scene_checkpoint_path; // empty: no checkpoint (gate fails)
	bool        create_baseline = false;
	Json        profile;
	Environment base_env;
};

struct PlayableReport
{
	std::vector<GateResult> gates;
	Json                    summary; // sanitized
	std::string             run_dir;
};

// Runs one bounded strict session. Returns the exit code.
[[nodiscard]] int RunPlayableSession(RunnerHost* host, const PlayableOptions& options, PlayableReport* report);

// `kyty_agent playable-regression ...`
[[nodiscard]] int PlayableRegressionMain(const std::vector<std::string>& args);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_PLAYABLE_H_ */
