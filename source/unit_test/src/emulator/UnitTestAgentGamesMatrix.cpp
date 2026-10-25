#include "Kyty/Agent/Runner/GamesMatrix.h"
#include "Kyty/Agent/Runner/JsonValue.h"
#include "Kyty/Agent/Runner/RunnerHost.h"
#include "Kyty/Agent/Runner/Sha256.h"
#include "Kyty/UnitTest.h"

#include "ScopedTestDirectory.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

UT_BEGIN(AgentGamesMatrix);

using namespace Kyty::Agent::Runner; // NOLINT(google-build-using-namespace)
namespace fs = std::filesystem;

namespace {

void Touch(const fs::path& path, const std::string& text = "x")
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out << text;
}

Json Status(const char* phase, int64_t present, bool graphic_ready, const char* frontier = "")
{
	return Json::MakeObject().Set("phase", phase).Set("present", present).Set("graphic_ready", graphic_ready).Set("frontier", frontier);
}

std::pair<std::string, std::string> Map(std::optional<int> exit_code, bool agent_ready, const Json& status, const std::string& error = "",
                                        bool timed_out = false, bool runner_stopped = false)
{
	return MapMatrixOutcome(exit_code, agent_ready, status, error, timed_out, runner_stopped);
}

bool HasNote(const MatrixCase& result, const std::string& note)
{
	return std::find(result.notes.begin(), result.notes.end(), note) != result.notes.end();
}

// One launched case behind a fake agent socket. The agent answers after
// `ready_at`; status follows `status_at`; the child dies only when terminated
// or when `exit_at` passes.
class MatrixGuest: public RunnerHost
{
public:
	double Now() override { return now; }
	void   Sleep(double seconds) override { now += seconds; }

	bool Launch(const LaunchSpec& spec, std::string* /*error*/) override
	{
		launched = spec;
		if (!log_text.empty())
		{
			Touch(spec.log_path, log_text);
		}
		return launch_ok;
	}
	[[nodiscard]] int64_t ChildPid() const override { return 1; }
	std::optional<int>    PollChild() override
	{
		if (!exit_code.has_value() && exit_at.has_value() && now >= *exit_at)
		{
			exit_code = exit_value;
		}
		return exit_code;
	}
	void TerminateChild(std::vector<std::string>* notes) override
	{
		++terminations;
		exit_code = -15;
		notes->emplace_back("killed_sigterm");
	}

	AgentReply CallAgent(const std::string& /*endpoint*/, const std::string& tool, const Json& /*args*/, double /*timeout_s*/) override
	{
		if (now < ready_at || exit_code.has_value())
		{
			return {};
		}
		AgentReply reply;
		reply.code = 0;
		if (tool == "status")
		{
			reply.body = Json::MakeObject().Set("result", status_at(now));
		} else if (tool == "last_error")
		{
			reply.body = Json::MakeObject().Set("result", Json::MakeObject().Set("event", last_error));
		} else
		{
			reply.body = Json::MakeObject();
		}
		return reply;
	}

	double                         now      = 0.0;
	double                         ready_at = 1.0;
	std::function<Json(double)>    status_at = [](double /*t*/) { return Status("loading", 0, false); };
	Json                           last_error;
	std::optional<double>          exit_at;
	int                            exit_value = 0;
	std::optional<int>             exit_code;
	bool                           launch_ok = true;
	std::string                    log_text;
	int                            terminations = 0;
	LaunchSpec                     launched;
};

class MatrixCaseHarness
{
public:
	MatrixCaseHarness(): m_directory("agent-games-matrix")
	{
		options.case_id            = "case_test";
		options.package_root       = (m_directory.Path() / "pkg").string();
		options.repo_root          = m_directory.Path().string();
		options.fc_script          = (m_directory.Path() / "fc_script").string();
		options.case_dir           = (m_directory.Path() / "run" / "case_test").string();
		options.run_id             = "test-run-" + m_directory.Path().filename().string();
		options.startup_deadline_s = 5.0;
		options.present_deadline_s = 10.0;
		options.total_deadline_s   = 30.0;
		options.stability_s        = 3.0;
		options.base_environment   = {{"PATH", "/usr/bin"}, {"KYTY_GFX_PERMISSIVE", "1"}, {"KYTY_PRESENT_FPS_LIMIT", "60"}};
	}

	MatrixCase Run() { return RunMatrixCase(&guest, options); }

	MatrixGuest       guest;
	MatrixCaseOptions options;

private:
	ScopedTestDirectory m_directory;
};

} // namespace

TEST(AgentGamesMatrix, DiscoversOnlyDirectEbootRootsAndSkipsPackageBulk)
{
	const ScopedTestDirectory directory("agent-games-matrix");
	const fs::path            games = directory.Path() / "games";
	Touch(games / "pkg_a" / "eboot.bin");
	Touch(games / "pkg_a" / "sce_module" / "inner" / "eboot.bin");
	Touch(games / "nested" / "pkg_b" / "eboot.bin");
	Touch(games / "nested" / "pkg_c" / "EBOOT.ELF");
	Touch(games / ".hidden" / "pkg_d" / "eboot.bin");
	Touch(games / "a" / "b" / "c" / "eboot.bin");

	const auto roots = DiscoverGameRoots(games.string(), 6);
	ASSERT_EQ(roots.size(), 3u);
	EXPECT_EQ(fs::path(roots[0]).filename(), "c");
	EXPECT_EQ(fs::path(roots[1]).filename(), "pkg_b");
	EXPECT_EQ(fs::path(roots[2]).filename(), "pkg_a");
	EXPECT_EQ(DiscoverGameRoots(games.string(), 2).size(), 2u);
	EXPECT_TRUE(DiscoverGameRoots((games / "missing").string(), 6).empty());

	EXPECT_TRUE(InvalidPackageReason((games / "pkg_a").string()).empty());
	EXPECT_EQ(InvalidPackageReason((games / "nested").string()), "missing_primary_executable");
	EXPECT_EQ(InvalidPackageReason((games / "pkg_a" / "eboot.bin").string()), "not_a_directory");
}

TEST(AgentGamesMatrix, CaseIdsAreStableHashesOfTheRelativePath)
{
	const ScopedTestDirectory directory("agent-games-matrix");
	const fs::path            games = directory.Path() / "games";
	Touch(games / "deep" / "pkg" / "eboot.bin");
	const std::string id = AnonymousCaseId(games.string(), (games / "deep" / "pkg").string());
	const std::string key = "deep/pkg";
	EXPECT_EQ(id, "case_" + Sha256Hex(key.data(), key.size()).substr(0, 12));
	EXPECT_EQ(id, AnonymousCaseId(games.string(), (games / "deep" / "pkg").string()));
	EXPECT_EQ(id.find("pkg"), std::string::npos);
}

TEST(AgentGamesMatrix, OutcomeFollowsExitAndAgentEvidence)
{
	const Json loading = Status("loading", 0, false);
	EXPECT_EQ(Map(139, true, Status("interactive", 30, true)).first, "host_crash");
	EXPECT_EQ(Map(65, true, loading, "bringup_halt"), (std::pair<std::string, std::string> {"unsupported", "bringup_halt"}));
	EXPECT_EQ(Map(65, true, loading).first, "loader_failed");
	// A strict abort after the agent was ready is never a video milestone.
	EXPECT_EQ(Map(1, true, Status("loading", 0, true)).first, "loader_failed");
	EXPECT_EQ(Map(1, false, loading), (std::pair<std::string, std::string> {"launch_failed", "launch"}));
	EXPECT_EQ(Map(std::nullopt, false, loading, "", true).second, "agent_ready_timeout");
	EXPECT_EQ(Map(0, false, loading).first, "guest_exit");
	EXPECT_EQ(Map(std::nullopt, true, loading).first, "runtime_started");
	EXPECT_EQ(Map(0, true, loading, "", false).first, "guest_exit");
	EXPECT_EQ(Map(std::nullopt, true, Status("loading", 0, false, "unsupported")).first, "unsupported");
	EXPECT_EQ(Map(std::nullopt, true, loading, "host_fault").first, "host_crash");

	// The runner's own SIGTERM or SIGKILL after a deadline keeps the observed frontier.
	EXPECT_EQ(Map(-15, true, Status("loading", 12, true), "", true), (std::pair<std::string, std::string> {"controlled_timeout", "presenting"}));
	EXPECT_EQ(Map(-9, true, Status("loading", 0, true), "", true).first, "controlled_timeout");
	// Stopping a stable interactive case is not a load failure.
	EXPECT_EQ(Map(-15, true, Status("interactive", 300, true), "", false, true),
	          (std::pair<std::string, std::string> {"interactive", "interactive"}));
	EXPECT_EQ(Map(-15, false, loading, "", false, true).first, "launch_failed");
}

TEST(AgentGamesMatrix, SummariesCarryNoPathsOrTitleIdentifiers)
{
	const Json value = Json::MakeObject()
	                       .Set("/keys/stay", Json::MakeArray().Push("crash in /home/user/Games/x.bin now").Push("C:\\Games\\pkg\\eboot.bin"))
	                       .Set("id", "ppsa01234 and CUSA0001,")
	                       .Set("kept", "xPPSA0123 PPSA12ab ratio 1/2")
	                       .Set("number", 3);
	const Json clean = SanitizeMatrixJson(value);
	EXPECT_TRUE(clean.Has("/keys/stay"));
	EXPECT_EQ(clean["/keys/stay"][0].AsString(), "crash in [redacted-path] now");
	EXPECT_EQ(clean["/keys/stay"][1].AsString(), "[redacted-path]");
	EXPECT_EQ(clean["id"].AsString(), "[redacted-title-id] and [redacted-title-id],");
	EXPECT_EQ(clean["kept"].AsString(), "xPPSA0123 PPSA12ab ratio 1/2");
	EXPECT_EQ(clean["number"].AsInt(), 3);
}

TEST(AgentGamesMatrix, LogLabelIsTheFirstActionableLine)
{
	const ScopedTestDirectory directory("agent-games-matrix");
	const fs::path            log = directory.Path() / "child.log";
	Touch(log, "[info] boot\n--- Fatal Error ---\n=== Unexpected non-Func after import ===\n");
	EXPECT_EQ(MatrixActionableFromLog(log.string()), "Unexpected non-Func after import");
	Touch(log, "loading\nEXIT: decoder failed for PPSA01234 in /home/user/src/Decoder.cpp:42\n");
	EXPECT_EQ(MatrixActionableFromLog(log.string()), "EXIT: decoder failed for [redacted-title-id]");
	Touch(log, "Fatal: not implemented opcode at /opt/k/x\n");
	EXPECT_EQ(MatrixActionableFromLog(log.string()), "Fatal: not implemented opcode at [redacted-path]");
	Touch(log, "all good\n");
	EXPECT_EQ(MatrixActionableFromLog(log.string()), "");
	EXPECT_EQ(MatrixActionableFromLog((directory.Path() / "missing.log").string()), "");
}

TEST(AgentGamesMatrix, StableInteractiveCaseIsStoppedAndPasses)
{
	MatrixCaseHarness harness;
	harness.guest.status_at = [](double t) { return t < 4.0 ? Status("loading", 0, true) : Status("interactive", 100, true); };
	const MatrixCase result = harness.Run();
	EXPECT_EQ(result.outcome, "interactive");
	EXPECT_EQ(result.first_frontier, "interactive");
	EXPECT_TRUE(result.progressed);
	EXPECT_TRUE(HasNote(result, "stability_reached"));
	EXPECT_EQ(harness.guest.terminations, 1);
	EXPECT_EQ(result.child_exit, -15);
	EXPECT_EQ(MatrixExitCode({result}), 0);

	// The strict child gets the canonical environment without permissive switches.
	const Environment& env = harness.guest.launched.env;
	EXPECT_EQ(env.count("KYTY_GFX_PERMISSIVE"), 0u);
	EXPECT_EQ(env.at("KYTY_PRESENT_FPS_LIMIT"), "60");
	EXPECT_EQ(env.at("KYTY_GUEST_ROOT"), harness.options.package_root);
	EXPECT_EQ(harness.guest.launched.argv[1], "scripts/run_guest.lua");
}

TEST(AgentGamesMatrix, CaseStopsAtTheFirstReachedDeadline)
{
	MatrixCaseHarness never_ready;
	never_ready.guest.ready_at = 1000.0;
	const MatrixCase launch    = never_ready.Run();
	EXPECT_EQ(launch.outcome, "launch_failed");
	EXPECT_EQ(launch.first_frontier, "launch");
	EXPECT_TRUE(HasNote(launch, "startup_deadline"));
	EXPECT_EQ(MatrixExitCode({launch}), 1);

	MatrixCaseHarness runtime_only;
	const MatrixCase  runtime = runtime_only.Run();
	EXPECT_EQ(runtime.outcome, "controlled_timeout");
	EXPECT_EQ(runtime.first_frontier, "runtime_started");
	EXPECT_TRUE(HasNote(runtime, "present_deadline"));
	EXPECT_LT(runtime_only.guest.now, 12.0);

	MatrixCaseHarness presenting;
	presenting.guest.status_at = [](double /*t*/) { return Status("loading", 5, true); };
	const MatrixCase present   = presenting.Run();
	EXPECT_EQ(present.outcome, "controlled_timeout");
	EXPECT_EQ(present.first_frontier, "presenting");
	EXPECT_TRUE(HasNote(present, "total_deadline"));
	EXPECT_EQ(MatrixExitCode({present}), 0);
}

TEST(AgentGamesMatrix, GuestFailuresKeepTheirEvidence)
{
	MatrixCaseHarness halted;
	halted.options.mode   = "bringup";
	halted.guest.last_error = Json::MakeObject().Set("code", "bringup_halt");
	const MatrixCase unsupported = halted.Run();
	EXPECT_EQ(unsupported.outcome, "unsupported");
	EXPECT_TRUE(HasNote(unsupported, "unsupported_from_last_error"));
	EXPECT_EQ(halted.guest.launched.env.at("KYTY_BRINGUP_MODE"), "unsafe");
	EXPECT_EQ(MatrixExitCode({unsupported}), 1);

	MatrixCaseHarness crashed;
	crashed.guest.exit_at    = 3.0;
	crashed.guest.exit_value = 139;
	crashed.guest.log_text   = "boot\n=== Unexpected import state ===\n";
	const MatrixCase crash   = crashed.Run();
	EXPECT_EQ(crash.outcome, "host_crash");
	EXPECT_EQ(crash.first_frontier, "Unexpected import state");
	EXPECT_EQ(crashed.guest.terminations, 0);

	MatrixCaseHarness refused;
	refused.guest.launch_ok = false;
	const MatrixCase launch = refused.Run();
	EXPECT_EQ(launch.outcome, "launch_failed");
	EXPECT_EQ(launch.first_frontier, "parent_exception");
	EXPECT_TRUE(launch.attempted);
}

UT_END();
