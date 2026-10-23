// Offline replay of one resource-fold capture document.
//
// The tool consumes only the document. It never reads guest memory: every
// descriptor-table word comes from the recorded transcript. A refused replay
// (missing, out-of-order or unreadable read) halts through the production EXIT
// path, which is a nonzero process exit with the production diagnostic.
//
// The tool replays PS5 documents only: it sets the guest platform explicitly,
// because the sampled-image path of the evaluator reads it.
//
// Stdout may begin with startup diagnostics from runtime initialization. With
// --print-output, the canonical output follows as one frame:
//   resource-fold-canonical-begin bytes=<N>\n<exactly N payload bytes>resource-fold-canonical-end\n
// then the summary line "fingerprint=<hex> output_matches_document=<0|1>".
//
// Exit codes:
//   0  replay consumed every recorded read and reproduced the recorded output
//      byte for byte; with --validate-only, the document passed schema and
//      safe-input validation (the evaluator may still refuse it during replay)
//   1  usage error
//   2  the document is malformed, outside the schema bounds, or not a PS5 document
//   3  the transcript holds reads that production never requested
//   4  replay consumed the transcript but the output differs from the document
//   5  the runtime configuration could not be initialized

#include "../../emulator/src/Graphics/ShaderResourceFoldCapture.h"

#include "Emulator/Config.h"
#include "Emulator/Log.h"
#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Math/MathAll.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using Kyty::Libs::Graphics::ShaderResourceFoldReplayReport;
using Kyty::Libs::Graphics::ShaderResourceFoldToolResult;

constexpr int k_exit_environment = 5;

// --print-output frame: a begin line carrying the exact byte count, the payload
// bytes, then the end line. Startup diagnostics printed by runtime initialization
// may precede the frame; readers take exactly `bytes` after the begin line.
constexpr const char* k_canonical_begin = "resource-fold-canonical-begin";
constexpr const char* k_canonical_end   = "resource-fold-canonical-end";

void PrintUsage()
{
	std::fprintf(stderr, "usage: kyty_resource_fold_replay <document.json> [--print-output | --validate-only]\n");
}

// Same subsystem set as the graphics integration binaries; nothing else is started.
bool InitializeRuntime(char* program)
{
	char*                       argv[]     = {program, nullptr};
	Kyty::Core::SubsystemsList* subsystems = Kyty::Core::SubsystemsListSingleton::Instance();
	subsystems->SetArgs(1, argv);
	using Kyty::Config::ConfigSubsystem;
	using Kyty::Core::CoreSubsystem;
	using Kyty::Core::ThreadsSubsystem;
	using Kyty::Log::LogSubsystem;
	using Kyty::Math::MathSubsystem;
	subsystems->Add(CoreSubsystem::Instance(), {});
	subsystems->Add(ConfigSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(MathSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(ThreadsSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(LogSubsystem::Instance(), {CoreSubsystem::Instance(), ConfigSubsystem::Instance(), ThreadsSubsystem::Instance()});
	return subsystems->InitAll(false) && Kyty::Config::SetGuestPlatform(Kyty::GuestPlatform::Ps5);
}

} // namespace

int main(int argc, char** argv)
{
	const bool option        = argc == 3;
	const bool print_output  = option && std::strcmp(argv[2], "--print-output") == 0;
	const bool validate_only = option && std::strcmp(argv[2], "--validate-only") == 0;
	if (argc < 2 || argc > 3 || (option && !print_output && !validate_only))
	{
		PrintUsage();
		return static_cast<int>(ShaderResourceFoldToolResult::Usage);
	}
	if (!InitializeRuntime(argv[0]))
	{
		std::fprintf(stderr, "resource-fold-replay: runtime configuration could not be initialized\n");
		return k_exit_environment;
	}

	ShaderResourceFoldReplayReport report;
	std::string                    error;
	const auto result = Kyty::Libs::Graphics::ShaderResourceFoldRunFile(argv[1], validate_only, &report, &error);
	switch (result)
	{
		case ShaderResourceFoldToolResult::Malformed: std::fprintf(stderr, "resource-fold-replay malformed: %s\n", error.c_str()); break;
		case ShaderResourceFoldToolResult::Unused: std::fprintf(stderr, "resource-fold-replay status=unused\n"); break;
		case ShaderResourceFoldToolResult::Ok:
		case ShaderResourceFoldToolResult::OutputMismatch:
			if (validate_only)
			{
				std::printf("valid\n");
				break;
			}
			if (print_output)
			{
				// Length-framed: runtime startup diagnostics may precede it on stdout.
				std::printf("%s bytes=%zu\n", k_canonical_begin, report.canonical_output.size());
				std::fwrite(report.canonical_output.data(), 1, report.canonical_output.size(), stdout);
				std::printf("%s\n", k_canonical_end);
			}
			std::printf("fingerprint=%016" PRIx64 " output_matches_document=%d\n", report.fingerprint,
			            report.output_matches_document ? 1 : 0);
			break;
		case ShaderResourceFoldToolResult::Usage: PrintUsage(); break;
	}
	return static_cast<int>(result);
}
