#include "Kyty/Agent/Runner/RunnerCli.h"

#include "Kyty/Agent/Runner/CaptureMetrics.h"
#include "Kyty/Agent/Runner/GamesMatrix.h"
#include "Kyty/Agent/Runner/HardwareOracle.h"
#include "Kyty/Agent/Runner/OperationGraph.h"
#include "Kyty/Agent/Runner/Playable.h"

#include "RunnerSupport.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

namespace Kyty::Agent::Runner {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxManifestBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxProfileBytes  = 1024u * 1024u;

std::string EnvOr(const char* key, const std::string& fallback)
{
	const char* value = std::getenv(key);
	return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

void PrintJson(const Json& value, int indent)
{
	std::printf("%s\n", value.Dump(indent).c_str());
}

bool LoadJsonFile(const std::string& path, size_t max_bytes, Json* out, std::string* error)
{
	std::string text;
	return ReadTextFile(path, max_bytes, &text, error) && ParseJson(text, out, error);
}

int ScoreImageMain(const std::vector<std::string>& args)
{
	ParsedOptions options;
	std::string   error;
	if (!ParseOptions(args, {"--gate"}, {}, &options, &error) || options.positionals.size() != 1)
	{
		std::fprintf(stderr, "usage: kyty_agent score-image IMAGE.png [--gate]\n");
		return 2;
	}
	Json metrics;
	if (!ScoreImage(options.positionals[0], &metrics, &error))
	{
		std::fprintf(stderr, "kyty_agent: error: %s\n", error.c_str());
		return 2;
	}
	PrintJson(metrics, 2);
	return !HasOption(options, "--gate") || metrics["world"]["material_healthy"].AsBool() ? 0 : 1;
}

bool LoadManifest(const std::string& path, Json* manifest, std::string* error)
{
	if (!LoadJsonFile(path, kMaxManifestBytes, manifest, error))
	{
		return false;
	}
	if (!manifest->IsObject() || (*manifest)["schema_version"] != Json(1))
	{
		*error = "unsupported capture schema: " + ValueText((*manifest)["schema_version"]);
		return false;
	}
	return true;
}

// The stored aggregate, or one rebuilt from the captures; null when there are none.
bool ManifestMetrics(const Json& manifest, Json* metrics, std::string* error)
{
	if (manifest["aggregate"].Truthy())
	{
		*metrics = manifest["aggregate"];
		return true;
	}
	if (!manifest["captures"].Truthy())
	{
		*metrics = Json();
		return true;
	}
	return AggregateCaptures(manifest["captures"], metrics, error);
}

int CompareCapturesMain(const std::vector<std::string>& args)
{
	ParsedOptions options;
	std::string   error;
	if (!ParseOptions(args, {}, {"--current", "--baseline"}, &options, &error) || !options.positionals.empty() ||
	    !HasOption(options, "--current") || !HasOption(options, "--baseline"))
	{
		std::fprintf(stderr, "usage: kyty_agent compare-captures --current MANIFEST --baseline MANIFEST\n");
		return 2;
	}
	Json current;
	Json baseline;
	Json current_metrics;
	Json baseline_metrics;
	if (!LoadManifest(OptionValue(options, "--current"), &current, &error) ||
	    !LoadManifest(OptionValue(options, "--baseline"), &baseline, &error) || !ManifestMetrics(current, &current_metrics, &error) ||
	    !ManifestMetrics(baseline, &baseline_metrics, &error))
	{
		std::fprintf(stderr, "kyty_agent: error: %s\n", error.c_str());
		return 2;
	}
	if (current_metrics.IsNull() || baseline_metrics.IsNull())
	{
		Json result = Json::MakeObject();
		result.Set("pass", false).Set("checks", Json::MakeObject().Set("samples_available", false)).Set("delta", Json::MakeObject());
		PrintJson(result, 2);
		return 1;
	}
	const Json result = CompareMetrics(current_metrics, baseline_metrics);
	PrintJson(result, 2);
	return result["pass"].AsBool() ? 0 : 1;
}

void WriteSkipSummary(const std::string& scratch)
{
	Json skip = Json::MakeObject();
	skip.Set("schema", "kyty_playable_regression_summary_v1")
	    .Set("mode", "strict")
	    .Set("passed", false)
	    .Set("skipped", true)
	    .Set("reason", "KYTY_GUEST_ROOT unset")
	    .Set("gates", Json::MakeArray())
	    .Set("notes", Json::MakeArray().Push("honest_skip_no_guest_root"));
	std::string error;
	(void)WriteTextFile((fs::path(scratch) / "summary_skip.json").string(), skip.Dump(2) + "\n", &error);
	PrintJson(skip, 2);
}

} // namespace

int PlayableRegressionMain(const std::vector<std::string>& args)
{
	ParsedOptions options;
	std::string   error;
	if (!ParseOptions(args, {"--create-baseline", "--allow-missing-guest"},
	                  {"--guest-root", "--scratch", "--baseline", "--profile", "--scene-checkpoint", "--fc-script", "--runtime-cwd",
	                   "--guest-script", "--repo-root"},
	                  &options, &error) ||
	    !options.positionals.empty())
	{
		std::fprintf(stderr, "kyty_agent: %s\n", error.empty() ? "unexpected argument" : error.c_str());
		return 2;
	}
	std::unique_ptr<RunnerHost> host = CreateSystemRunnerHost();
	if (host == nullptr)
	{
		std::fprintf(stderr, "kyty_agent: playable-regression is not supported on this host\n");
		return 2;
	}
	std::error_code   ec;
	const std::string repo_root = ResolvePath(OptionValue(options, "--repo-root", fs::current_path(ec).string()));
	const std::string profile_arg = OptionValue(options, "--profile");
	const std::string profile_path =
	    profile_arg.empty() ? (fs::path(repo_root) / "scripts" / "profiles" / "strict_playable.json").string() : ResolvePath(profile_arg);
	PlayableOptions run;
	run.profile = DefaultPlayableProfile();
	if (fs::is_regular_file(profile_path, ec))
	{
		Json data;
		if (!LoadJsonFile(profile_path, kMaxProfileBytes, &data, &error) || !data.IsObject())
		{
			std::fprintf(stderr, "kyty_agent: error: profile must be a JSON object: %s\n", error.c_str());
			return 2;
		}
		run.profile = MergePlayableProfile(data);
	} else if (!profile_arg.empty())
	{
		std::fprintf(stderr, "kyty_agent: error: profile not found\n");
		return 2;
	}

	std::string scratch = OptionValue(options, "--scratch");
	if (scratch.empty())
	{
		scratch = EnvOr("KYTY_REGRESSION_SCRATCH", (fs::path(repo_root) / "_scratch_playable" / "playable_regression").string());
	}
	run.scratch = ResolvePath(scratch);
	fs::create_directories(run.scratch, ec);
	std::string baseline = OptionValue(options, "--baseline");
	if (baseline.empty())
	{
		baseline = EnvOr("KYTY_REGRESSION_BASELINE", (fs::path(run.scratch) / "visual_baseline.json").string());
	}
	run.baseline_path = ResolvePath(baseline);

	const std::string guest = OptionValue(options, "--guest-root", EnvOr("KYTY_GUEST_ROOT", ""));
	if (guest.empty())
	{
		WriteSkipSummary(run.scratch);
		return HasOption(options, "--allow-missing-guest") ? 0 : 2;
	}
	run.guest_root = ResolvePath(guest);
	if (!fs::is_directory(run.guest_root, ec))
	{
		std::fprintf(stderr, "error: guest root is not a directory\n");
		return 2;
	}
	run.fc_script = ResolvePath(OptionValue(options, "--fc-script", "_build_linux/fc_script"), repo_root);
	if (!fs::is_regular_file(run.fc_script, ec))
	{
		std::fprintf(stderr, "error: fc_script missing\n");
		return 2;
	}
	// The Lua input stays independent of the writable runtime directory.
	run.runtime_cwd  = ResolvePath(OptionValue(options, "--runtime-cwd", repo_root), repo_root);
	run.guest_script = ResolvePath(OptionValue(options, "--guest-script", "scripts/run_guest.lua"), repo_root);
	if (!fs::is_directory(run.runtime_cwd, ec))
	{
		std::fprintf(stderr, "error: runtime cwd is not a directory\n");
		return 2;
	}
	if (!fs::is_regular_file(run.guest_script, ec))
	{
		std::fprintf(stderr, "error: guest script is not a file\n");
		return 2;
	}
	if (HasOption(options, "--scene-checkpoint"))
	{
		run.scene_checkpoint_path = ResolvePath(OptionValue(options, "--scene-checkpoint"));
	}
	run.create_baseline = HasOption(options, "--create-baseline");
	run.repo_root       = repo_root;
	run.base_env        = CurrentEnvironment();

	PlayableReport report;
	const int      rc = RunPlayableSession(host.get(), run, &report);
	if (report.summary.IsNull())
	{
		return rc;
	}
	PrintJson(report.summary, -1);
	std::fprintf(stderr, "scratch=%s\n", run.scratch.c_str());
	std::fprintf(stderr, "exit=%d passed=%s\n", rc, report.summary["passed"].AsBool() ? "True" : "False");
	return rc;
}

bool IsRunnerCommand(const std::string& command)
{
	return command == "score-image" || command == "compare-captures" || command == "capture-run" || command == "playable-regression" ||
	       command == "operation-graph" || command == "games-matrix" || command == "hardware-oracle";
}

int RunnerMain(const std::string& command, const std::vector<std::string>& args)
{
	if (command == "score-image")
	{
		return ScoreImageMain(args);
	}
	if (command == "compare-captures")
	{
		return CompareCapturesMain(args);
	}
	if (command == "capture-run")
	{
		return CaptureRunMain(args);
	}
	if (command == "operation-graph")
	{
		return OperationGraphMain(args);
	}
	if (command == "games-matrix")
	{
		return GamesMatrixMain(args);
	}
	if (command == "hardware-oracle")
	{
		return HardwareOracleMain(args);
	}
	return PlayableRegressionMain(args);
}

void PrintRunnerUsage()
{
	std::fprintf(stderr, "\n"
	                     "Runner commands (no --endpoint; they launch or score on their own):\n"
	                     "  kyty_agent score-image IMAGE.png [--gate]\n"
	                     "  kyty_agent compare-captures --current MANIFEST --baseline MANIFEST\n"
	                     "  kyty_agent capture-run --guest-root DIR [--build DIR] [--out DIR] [--min-frame N] [--max-wait S]\n"
	                     "      [--samples N] [--interval S] [--baseline MANIFEST] [--pad-at FRAME:BUTTON]... [diagnostic flags]\n"
	                     "  kyty_agent playable-regression [--guest-root DIR] [--scratch DIR] [--baseline JSON] [--create-baseline]\n"
	                     "      [--profile JSON] [--scene-checkpoint JSON] [--fc-script PATH] [--runtime-cwd DIR]\n"
	                     "      [--guest-script LUA] [--repo-root DIR] [--allow-missing-guest]\n"
	                     "  kyty_agent operation-graph INPUT [--json PATH] [--dot PATH] [--meta PATH]\n"
	                     "  kyty_agent games-matrix [--games-root DIR] [--scratch DIR] [--fc-script PATH] [--repo-root DIR]\n"
	                     "      [--discover-only] [--mode strict|bringup] [--max-depth N] [--max-cases N]\n"
	                     "      [--startup-deadline-s S] [--present-deadline-s S] [--total-deadline-s S] [--stability-s S]\n"
	                     "  kyty_agent hardware-oracle probe | compare --hip DIR --vulkan DIR\n"
	                     "  kyty_agent hardware-oracle run|vulkan-run --op OP (--mode seeded|edges --count N | --input-file ABS)\n"
	                     "      --wave-width 32|64 --out-dir ABS [--seed N] [--arch gfx1030] [--endpoint ABS]\n"
	                     "Runner exit codes: 0 pass, 1 gate failed, 2 usage/tooling, 3 playable baseline missing.\n"
	                     "hardware-oracle exit codes: 0 ok or match, 1 mismatch, 2 usage, 3 unavailable, 4 output error.\n");
}

} // namespace Kyty::Agent::Runner
