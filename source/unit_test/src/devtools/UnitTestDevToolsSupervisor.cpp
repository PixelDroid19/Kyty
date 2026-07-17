#include "Kyty/DevTools/Protocol/Protocol.h"
#include "Kyty/DevTools/Supervisor/ProcessLauncher.h"
#include "Kyty/DevTools/Supervisor/SharedMapping.h"
#include "Kyty/DevTools/Telemetry/WorkerTelemetry.h"
#include "Kyty/DevTools/Transport/Bootstrap.h"
#include "Kyty/DevTools/Transport/WorkerSession.h"
#include "Kyty/UnitTest.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

UT_BEGIN(DevToolsSupervisor);

using namespace Kyty::DevTools;

TEST(DevToolsSupervisor, CreatesOwnerOnlyMapping)
{
	SharedMapping map;
	ASSERT_EQ(SharedMapping::CreateOwnerOnly(kProtocolMappingSize, &map), ProcessOperationError::None);
	ASSERT_TRUE(map.IsValid());
	auto view = map.MutableView();
	ASSERT_NE(view.data, nullptr);
	EXPECT_EQ(view.size, kProtocolMappingSize);
	EXPECT_NE(map.InheritableHandle(), 0u);
	// Writable
	view.data[0] = 0xab;
	EXPECT_EQ(map.View().data[0], 0xab);
	map.Close();
	EXPECT_FALSE(map.IsValid());
}

TEST(DevToolsSupervisor, BootstrapMetadataRoundTripsExactly)
{
	BootstrapMetadata meta {};
	meta.platform        = BootstrapPlatform::Posix;
	meta.mapping_handle  = 3;
	meta.liveness_handle = 4;
	for (int i = 0; i < 16; ++i)
	{
		meta.nonce.bytes[i] = static_cast<uint8_t>(0xa0 + i);
	}
	BootstrapText text {};
	ASSERT_TRUE(EncodeBootstrapMetadata(meta, &text));
	EXPECT_EQ(std::strncmp(text.bytes, "p:3:4:", 6), 0);
	EXPECT_EQ(text.size, 6u + 32u);

	BootstrapMetadata out {};
	EXPECT_EQ(ParseBootstrapMetadata(text.bytes, &out), BootstrapParseResult::Valid);
	EXPECT_EQ(out.platform, BootstrapPlatform::Posix);
	EXPECT_EQ(out.mapping_handle, 3u);
	EXPECT_EQ(out.liveness_handle, 4u);
	EXPECT_TRUE(NonceEqual(meta.nonce, out.nonce));
}

TEST(DevToolsSupervisor, MissingBootstrapSelectsStandalone)
{
	BootstrapMetadata out {};
	EXPECT_EQ(ParseBootstrapMetadata(nullptr, &out), BootstrapParseResult::Missing);
	EXPECT_EQ(ParseBootstrapMetadata("", &out), BootstrapParseResult::Missing);
}

TEST(DevToolsSupervisor, MalformedBootstrapOpensNoHandle)
{
	BootstrapMetadata out {};
	EXPECT_EQ(ParseBootstrapMetadata("p:3:4:ZZ", &out), BootstrapParseResult::Malformed);
	EXPECT_EQ(ParseBootstrapMetadata("p:3:5:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &out), BootstrapParseResult::Malformed);
	EXPECT_EQ(ParseBootstrapMetadata("w:0:1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &out), BootstrapParseResult::Malformed);
	EXPECT_EQ(ParseBootstrapMetadata("nope", &out), BootstrapParseResult::Malformed);
}

TEST(DevToolsSupervisor, CurrentExecutablePathIsResolvable)
{
	char path[512] = {};
	ASSERT_TRUE(QueryCurrentExecutablePath(path, sizeof(path)));
	EXPECT_NE(path[0], '\0');
}

TEST(DevToolsSupervisor, LinuxProcessStatParsesParenthesizedCommand)
{
	// Field 22 = starttime. Tokens after ") ": state is index 0; field 22 is index 19.
	// Crafted line includes spaces inside the parenthesized command name.
	// Tokens after ") ": S,1..18,424242  => index 19 is 424242.
	const char* crafted =
	    "42 (cmd with spaces) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 424242 20 21";
	uint64_t ticks = 0;
	ASSERT_TRUE(ParseLinuxProcStatStartTicks(crafted, &ticks));
	EXPECT_EQ(ticks, 424242u);
	EXPECT_FALSE(ParseLinuxProcStatStartTicks("broken", &ticks));
	EXPECT_FALSE(ParseLinuxProcStatStartTicks(nullptr, &ticks));
}

TEST(DevToolsSupervisor, SecureRandomFillsNonZero)
{
	uint8_t a[16] = {};
	uint8_t b[16] = {};
	ASSERT_EQ(SecureRandomFill(a, 16), ProcessOperationError::None);
	ASSERT_EQ(SecureRandomFill(b, 16), ProcessOperationError::None);
	// Extremely unlikely both all-zero or equal.
	bool a_zero = true;
	for (int i = 0; i < 16; ++i)
	{
		if (a[i] != 0)
		{
			a_zero = false;
		}
	}
	EXPECT_FALSE(a_zero);
}

TEST(DevToolsSupervisor, PosixExitDecodesExitCode)
{
	// Launch /bin/true with dummy mapping/liveness FDs remapped to 3/4.
	SharedMapping map;
	ASSERT_EQ(SharedMapping::CreateOwnerOnly(4096, &map), ProcessOperationError::None);
	int pipes[2] = {-1, -1};
	ASSERT_EQ(::pipe(pipes), 0);

	BootstrapMetadata meta {};
	meta.platform        = BootstrapPlatform::Posix;
	meta.mapping_handle  = 3;
	meta.liveness_handle = 4;
	ASSERT_EQ(SecureRandomFill(meta.nonce.bytes, 16), ProcessOperationError::None);
	BootstrapText boot {};
	ASSERT_TRUE(EncodeBootstrapMetadata(meta, &boot));

	#if defined(__APPLE__)
	const char* true_executable = "/usr/bin/true";
	#else
	const char* true_executable = "/bin/true";
	#endif
	const char* argv[] = {true_executable, nullptr};
	LaunchOptions opt {};
	opt.executable  = true_executable;
	opt.argv        = argv;
	opt.argc        = 1;
	opt.bootstrap   = boot;
	opt.mapping_fd  = static_cast<int>(map.InheritableHandle());
	opt.liveness_fd = pipes[0];

	LaunchResult lr = ProcessLauncher::Launch(opt);
	::close(pipes[1]);
	if (lr.error == ProcessOperationError::Unsupported)
	{
		::close(pipes[0]);
		GTEST_SKIP() << "posix closefrom unavailable on this host";
	}
	ASSERT_EQ(lr.error, ProcessOperationError::None);
	ASSERT_TRUE(lr.process.IsValid());

	ProcessObservation obs {};
	ASSERT_EQ(ProcessLauncher::Wait(&lr.process, &obs), ProcessOperationError::None);
	EXPECT_EQ(obs.status.liveness, ProcessLiveness::Terminated);
	EXPECT_EQ(obs.status.termination, ProcessTermination::ExitCode);
	EXPECT_EQ(obs.status.code_valid, 1);
	EXPECT_EQ(obs.status.code, 0u);
	EXPECT_EQ(obs.status.error, ProcessStatusError::None);

	// Cached terminal observation.
	ProcessObservation again {};
	ASSERT_EQ(ProcessLauncher::Poll(&lr.process, &again), ProcessOperationError::None);
	EXPECT_EQ(again.status.code, 0u);

	::close(pipes[0]);
}

TEST(DevToolsSupervisor, WorkerSessionClassifiesBootstrapWithoutActivation)
{
	WorkerTelemetryOptions options {};
	options.diagnostic_thread_instance = 1;
	WorkerSession session;

	EXPECT_EQ(session.StartFromBootstrap(nullptr, options), WorkerSessionResult::MissingBootstrap);
	EXPECT_EQ(session.StartFromBootstrap("p:3:4:invalid", options), WorkerSessionResult::MalformedBootstrap);
	EXPECT_FALSE(session.Active());
}

TEST(DevToolsSupervisor, WorkerSessionScrubsBootstrapEnvironment)
{
	const char* previous_value = std::getenv(kBootstrapEnvName);
	const std::string previous = previous_value == nullptr ? std::string() : std::string(previous_value);
	ASSERT_EQ(::setenv(kBootstrapEnvName, "malformed", 1), 0);

	WorkerTelemetryOptions options {};
	WorkerSession session;
	EXPECT_EQ(session.StartFromBootstrap(std::getenv(kBootstrapEnvName), options), WorkerSessionResult::MalformedBootstrap);
	EXPECT_EQ(std::getenv(kBootstrapEnvName), nullptr);

	::unsetenv(kBootstrapEnvName);
	if (previous_value != nullptr)
	{
		ASSERT_EQ(::setenv(kBootstrapEnvName, previous.c_str(), 1), 0);
	}
}

UT_END();
