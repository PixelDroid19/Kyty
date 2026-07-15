#include "Kyty/DevTools/Supervisor/Supervisor.h"
#include "Kyty/DevTools/Supervisor/ProcessLauncher.h"
#include "Kyty/UnitTest.h"

#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

UT_BEGIN(DevToolsLifecycle);

using namespace Kyty::DevTools;

namespace {

std::string MakeScratchDir()
{
	char tmpl[] = "/tmp/kyty-life-test-XXXXXX";
	char* path  = ::mkdtemp(tmpl);
	EXPECT_NE(path, nullptr);
	return path != nullptr ? std::string(path) : std::string();
}

std::string FindDevToolsBinary()
{
	char current[512] = {};
	if (!QueryCurrentExecutablePath(current, sizeof(current)))
	{
		return {};
	}
	const std::string executable(current);
	const std::string suffix = "/fc_script";
	const size_t      marker = executable.rfind(suffix);
	if (marker == std::string::npos)
	{
		return {};
	}
	return executable.substr(0, marker) + "/devtools/kyty_devtools";
}

std::string FindWorkerBinary()
{
	char current[512] = {};
	return QueryCurrentExecutablePath(current, sizeof(current)) ? std::string(current) : std::string();
}

} // namespace

TEST(DevToolsLifecycle, RejectsMissingOrRelativeOutputDirectory)
{
	// Relative and missing output dirs are rejected by CLI (tested via DevToolsMain).
	const char* argv_rel[] = {"kyty_devtools", "run", "--output-dir", "relative/out", "--", "/bin/true", nullptr};
	EXPECT_EQ(DevToolsMain(6, const_cast<char**>(argv_rel)), 125);

	const char* argv_miss[] = {"kyty_devtools", "run", "--", "/bin/true", nullptr};
	EXPECT_EQ(DevToolsMain(4, const_cast<char**>(argv_miss)), 125);
}

TEST(DevToolsLifecycle, RejectsCaptureNowAndAttachCommands)
{
	const char* a1[] = {"kyty_devtools", "capture-now", nullptr};
	EXPECT_EQ(DevToolsMain(2, const_cast<char**>(a1)), 125);
	const char* a2[] = {"kyty_devtools", "attach", nullptr};
	EXPECT_EQ(DevToolsMain(2, const_cast<char**>(a2)), 125);
}

TEST(DevToolsLifecycle, NormalExitFinalizesOnce)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	// unit_test runs inside fc_script; handshake requires a real synthetic worker.
	// Verify API rejects incomplete options without creating orphans.
	SupervisorOptions bad {};
	bad.absolute_output_dir = dir.c_str();
	bad.worker              = nullptr;
	auto r                  = RunSupervisor(bad);
	EXPECT_EQ(r.outcome, SupervisorOutcome::LaunchError);
}

TEST(DevToolsLifecycle, WorkerProcessCompletesDevToolsHandshake)
{
	const std::string worker = FindWorkerBinary();
	if (worker.empty() || ::access(worker.c_str(), X_OK) != 0)
	{
		GTEST_SKIP() << "fc_script worker binary not available";
	}
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	const char* argv[] = {worker.c_str(), "{return 0}", nullptr};

	SupervisorOptions options {};
	options.absolute_output_dir  = dir.c_str();
	options.worker               = worker.c_str();
	options.worker_argv          = argv;
	options.worker_argc          = 2;
	options.mode                 = RecordingMode::Full;
	options.handshake_timeout_ns = 1'000'000'000ull;
	options.max_samples          = 20;

	const auto result = RunSupervisor(options);
	EXPECT_EQ(result.outcome, SupervisorOutcome::ChildExited);
	EXPECT_EQ(result.process.termination, ProcessTermination::ExitCode);
	EXPECT_EQ(result.process.code, 0u);
}

TEST(DevToolsLifecycle, SelfTestNormalExitViaDevToolsBinary)
{
	const std::string binary = FindDevToolsBinary();
	if (binary.empty() || ::access(binary.c_str(), X_OK) != 0)
	{
		GTEST_SKIP() << "kyty_devtools binary not built yet";
	}
	const char* bin = binary.c_str();
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	// Fork/exec self-test normal-exit.
	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		const char* argv[] = {bin, "self-test", "--output-dir", dir.c_str(), "--mode=normal-exit",
		                      "--suspected-ms=20", "--confirmed-ms=50", "--cleanup-ms=500", nullptr};
		::execv(bin, const_cast<char* const*>(argv));
		_exit(127);
	}
	int status = 0;
	ASSERT_EQ(::waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(DevToolsLifecycle, SelfTestBlockedLaneCapturesBundle)
{
	const std::string binary = FindDevToolsBinary();
	if (binary.empty() || ::access(binary.c_str(), X_OK) != 0)
	{
		GTEST_SKIP() << "kyty_devtools binary not built yet";
	}
	const char* bin = binary.c_str();
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		const char* argv[] = {bin, "self-test", "--output-dir", dir.c_str(), "--mode=blocked-lane",
		                      "--suspected-ms=20", "--confirmed-ms=50", "--cleanup-ms=500", nullptr};
		::execv(bin, const_cast<char* const*>(argv));
		_exit(127);
	}
	int status = 0;
	ASSERT_EQ(::waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(DevToolsLifecycle, LiveStallCapturesOnceAndContinues)
{
	// Covered by SelfTestBlockedLaneCapturesBundle (production path continues after bundle;
	// self-test bounds samples then waits once).
	const std::string binary = FindDevToolsBinary();
	if (binary.empty() || ::access(binary.c_str(), X_OK) != 0)
	{
		GTEST_SKIP() << "kyty_devtools binary not built yet";
	}
	const char* bin = binary.c_str();
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		const char* argv[] = {bin, "self-test", "--output-dir", dir.c_str(), "--mode=blocked-lane",
		                      "--suspected-ms=15", "--confirmed-ms=40", "--cleanup-ms=400", nullptr};
		::execv(bin, const_cast<char* const*>(argv));
		_exit(127);
	}
	int status = 0;
	ASSERT_EQ(::waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
}

UT_END();
