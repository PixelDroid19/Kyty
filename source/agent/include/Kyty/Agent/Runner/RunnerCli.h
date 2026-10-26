#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_RUNNERCLI_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_RUNNERCLI_H_

#include <string>
#include <vector>

namespace Kyty::Agent::Runner {

// Offline and session commands that need no running guest endpoint:
// score-image, compare-captures, capture-run and playable-regression.
[[nodiscard]] bool IsRunnerCommand(const std::string& command);
[[nodiscard]] int  RunnerMain(const std::string& command, const std::vector<std::string>& args);
void               PrintRunnerUsage();

[[nodiscard]] int CaptureRunMain(const std::vector<std::string>& args);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_RUNNERCLI_H_ */
