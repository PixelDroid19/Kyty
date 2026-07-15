#include "Kyty/DevTools/Supervisor/Supervisor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Kyty::DevTools {
namespace {

void PrintUsage() noexcept
{
	std::fprintf(stderr,
	             "kyty_devtools — passive Kyty supervisor (v1)\n"
	             "\n"
	             "Usage:\n"
	             "  kyty_devtools run --output-dir ABS_DIR [--recording=full|metrics-only] -- WORKER [ARG...]\n"
	             "  kyty_devtools self-test --output-dir ABS_DIR --mode=MODE [--suspected-ms=N] [--confirmed-ms=N]\n"
	             "                         [--cleanup-ms=N]\n"
	             "  kyty_devtools synthetic MODE   (internal synthetic worker; not a user command)\n"
	             "\n"
	             "Synthetic modes: progress, blocked-lane, publication-stop, parent-disconnect,\n"
	             "                 privacy-canary, normal-exit, crash\n"
	             "\n"
	             "v1 does not support capture-now, attach, or control queues.\n");
}

[[nodiscard]] bool IsAbsolute(const char* p) noexcept
{
	return p != nullptr && p[0] == '/';
}

[[nodiscard]] int FailReject(const char* msg) noexcept
{
	std::fprintf(stderr, "kyty_devtools: %s\n", msg);
	return 125;
}

} // namespace

int DevToolsMain(int argc, char** argv) noexcept
{
	if (argc < 2)
	{
		PrintUsage();
		return 125;
	}

	// Internal synthetic worker path.
	if (std::strcmp(argv[1], "synthetic") == 0)
	{
		const char* mode = (argc >= 3) ? argv[2] : "";
		return RunSyntheticWorker(mode, argc, argv);
	}

	// Reject non-v1 commands early.
	if (std::strcmp(argv[1], "capture-now") == 0 || std::strcmp(argv[1], "attach") == 0)
	{
		return FailReject("capture-now and attach are not supported in v1");
	}

	if (std::strcmp(argv[1], "run") == 0)
	{
		const char* output_dir = nullptr;
		RecordingMode mode     = RecordingMode::Full;
		int           i        = 2;
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--") == 0)
			{
				++i;
				break;
			}
			if (std::strncmp(argv[i], "--output-dir=", 13) == 0)
			{
				output_dir = argv[i] + 13;
				continue;
			}
			if (std::strcmp(argv[i], "--output-dir") == 0)
			{
				if (i + 1 >= argc)
				{
					return FailReject("missing --output-dir value");
				}
				output_dir = argv[++i];
				continue;
			}
			if (std::strncmp(argv[i], "--recording=", 12) == 0)
			{
				const char* m = argv[i] + 12;
				if (std::strcmp(m, "full") == 0)
				{
					mode = RecordingMode::Full;
				} else if (std::strcmp(m, "metrics-only") == 0)
				{
					mode = RecordingMode::MetricsOnly;
				} else
				{
					return FailReject("invalid --recording value");
				}
				continue;
			}
			return FailReject("unknown run option");
		}
		if (!IsAbsolute(output_dir))
		{
			return FailReject("run requires absolute --output-dir");
		}
		if (i >= argc)
		{
			return FailReject("run requires worker after --");
		}
		SupervisorOptions opt {};
		opt.absolute_output_dir = output_dir;
		opt.worker              = argv[i];
		opt.worker_argv         = const_cast<const char* const*>(&argv[i]);
		opt.worker_argc         = static_cast<uint32_t>(argc - i);
		opt.mode                = mode;
		const SupervisorResult r = RunSupervisor(opt);
		if (r.outcome == SupervisorOutcome::LaunchError || r.outcome == SupervisorOutcome::ProtocolError ||
		    r.outcome == SupervisorOutcome::BundleError || r.outcome == SupervisorOutcome::WorkerHandshakeFailed)
		{
			return 125;
		}
		if (r.outcome == SupervisorOutcome::StatusDecodeError)
		{
			return 125;
		}
		if (r.outcome == SupervisorOutcome::ChildCrashed)
		{
			return 128 + static_cast<int>(r.process.code);
		}
		if (r.outcome == SupervisorOutcome::ChildTerminated)
		{
			return 125;
		}
		// ChildExited
		return static_cast<int>(r.process.code);
	}

	if (std::strcmp(argv[1], "self-test") == 0)
	{
		const char* output_dir   = nullptr;
		const char* mode         = "blocked-lane";
		uint64_t    suspected_ms = 20;
		uint64_t    confirmed_ms = 50;
		uint64_t    cleanup_ms   = 2000;
		for (int i = 2; i < argc; ++i)
		{
			if (std::strncmp(argv[i], "--output-dir=", 13) == 0)
			{
				output_dir = argv[i] + 13;
			} else if (std::strcmp(argv[i], "--output-dir") == 0)
			{
				if (i + 1 >= argc)
				{
					return FailReject("missing --output-dir");
				}
				output_dir = argv[++i];
			} else if (std::strncmp(argv[i], "--mode=", 7) == 0)
			{
				mode = argv[i] + 7;
			} else if (std::strncmp(argv[i], "--suspected-ms=", 15) == 0)
			{
				suspected_ms = static_cast<uint64_t>(std::strtoull(argv[i] + 15, nullptr, 10));
			} else if (std::strncmp(argv[i], "--confirmed-ms=", 15) == 0)
			{
				confirmed_ms = static_cast<uint64_t>(std::strtoull(argv[i] + 15, nullptr, 10));
			} else if (std::strncmp(argv[i], "--cleanup-ms=", 13) == 0)
			{
				cleanup_ms = static_cast<uint64_t>(std::strtoull(argv[i] + 13, nullptr, 10));
			} else
			{
				return FailReject("unknown self-test option");
			}
		}
		if (!IsAbsolute(output_dir))
		{
			return FailReject("self-test requires absolute --output-dir");
		}
		return RunSelfTest(mode, output_dir, suspected_ms, confirmed_ms, cleanup_ms);
	}

	PrintUsage();
	return 125;
}

} // namespace Kyty::DevTools
