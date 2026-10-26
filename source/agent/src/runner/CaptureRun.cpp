#include "Kyty/Agent/Runner/CaptureMetrics.h"
#include "Kyty/Agent/Runner/RunnerCli.h"
#include "Kyty/Agent/Runner/RunnerHost.h"
#include "Kyty/Agent/Runner/Sha256.h"

#include "RunnerSupport.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

#if !defined(_WIN32)
#include <sys/utsname.h>
#endif

namespace Kyty::Agent::Runner {
namespace {

namespace fs = std::filesystem;

constexpr int    kSchemaVersion    = 1;
constexpr size_t kMaxManifestBytes = 64u * 1024u * 1024u;

const std::vector<std::string>& CaptureDiagnosticKeys()
{
	static const std::vector<std::string> keys = {
	    "KYTY_LIGHTBUF_PROBE",
	    "KYTY_SKIP_UD2",
	    "KYTY_BLEND_CONSTANT_EVIDENCE",
	    "KYTY_VERTEX_EVIDENCE",
	    "KYTY_SHADER_PROBE_CRC",
	    "KYTY_TEX_PROBE",
	    "KYTY_RT_EVIDENCE",
	    "KYTY_RT_EVIDENCE_PS",
	    "KYTY_FRAMEBUFFER_EVIDENCE",
	    "KYTY_VIDEOOUT_EVIDENCE",
	    "KYTY_FPS_LOG",
	    "KYTY_NATIVE_CAPTURE_DIR",
	    "KYTY_NATIVE_CAPTURE_FIRST_PRESENT",
	    "KYTY_NATIVE_CAPTURE_NOW",
	    "KYTY_NATIVE_CAPTURE_EVERY",
	    "KYTY_NATIVE_CAPTURE_TRIGGER",
	    "KYTY_NATIVE_TELEMETRY",
	    "KYTY_INPUT_LOG",
	};
	return keys;
}

// Boolean diagnostic flags and the variable each one arms.
struct DiagnosticFlag
{
	const char* flag;
	const char* key;
};

constexpr DiagnosticFlag kDiagnosticFlags[] = {
    {"--rt-evidence", "KYTY_RT_EVIDENCE"},
    {"--framebuffer-evidence", "KYTY_FRAMEBUFFER_EVIDENCE"},
    {"--videoout-evidence", "KYTY_VIDEOOUT_EVIDENCE"},
    {"--tex-probe", "KYTY_TEX_PROBE"},
    {"--vulkan-validation", "KYTY_VULKAN_VALIDATION"},
    {"--shader-validation", "KYTY_SHADER_VALIDATION"},
    {"--command-buffer-dump", "KYTY_COMMAND_BUFFER_DUMP"},
    {"--pipeline-dump", "KYTY_PIPELINE_DUMP"},
};

struct ScheduledTap
{
	int64_t     frame = 0;
	std::string button;
};

struct CaptureConfig
{
	std::string               repo_root;
	std::string               build;
	std::string               output;
	std::string               guest_root;
	std::string               baseline;
	int64_t                   min_frame = 1250;
	int64_t                   max_wait  = 240;
	int64_t                   samples   = 3;
	double                    interval  = 2.0;
	int64_t                   width     = 1280;
	int64_t                   height    = 720;
	bool                      auto_cross        = false;
	bool                      allow_diagnostics = false;
	std::string               rt_evidence_ps;
	std::string               lightbuf_probe;
	std::string               shader_probe_crc;
	std::vector<ScheduledTap> pad_at;
	ParsedOptions             options;
};

std::string RunText(const std::string& command)
{
#if defined(_WIN32)
	(void)command;
	return {};
#else
	std::FILE* pipe = ::popen((command + " 2>/dev/null").c_str(), "r");
	if (pipe == nullptr)
	{
		return {};
	}
	std::string output;
	char        chunk[4096];
	size_t      got = 0;
	while ((got = std::fread(chunk, 1, sizeof(chunk), pipe)) > 0 && output.size() < 1024u * 1024u)
	{
		output.append(chunk, got);
	}
	return ::pclose(pipe) == 0 ? Strip(output) : std::string();
#endif
}

Json GitMetadata(const std::string& root)
{
	const std::string git = "git -C '" + root + "' ";
	Json              out = Json::MakeObject();
	out.Set("commit", RunText("timeout 5 " + git + "rev-parse HEAD"))
	    .Set("branch", RunText("timeout 5 " + git + "branch --show-current"))
	    .Set("dirty", !RunText("timeout 5 " + git + "status --porcelain").empty())
	    .Set("diff_stat", RunText("timeout 5 " + git + "diff --stat"));
	return out;
}

Json HostMetadata()
{
	std::string device;
	for (const auto& line: SplitLines(RunText("timeout 15 vulkaninfo --summary")))
	{
		if (line.find("deviceName") != std::string::npos || line.find("GPU id") != std::string::npos)
		{
			device = Strip(line);
			break;
		}
	}
	std::string platform;
#if !defined(_WIN32)
	utsname name {};
	if (::uname(&name) == 0)
	{
		platform = std::string(name.sysname) + "-" + name.release + "-" + name.machine;
	}
#endif
	Json out = Json::MakeObject();
	out.Set("platform", platform).Set("runner", "kyty_agent").Set("gpu_summary_line", device);
	return out;
}

bool ParsePositive(const ParsedOptions& options, const char* name, int64_t* value, std::string* error)
{
	if (!HasOption(options, name))
	{
		return true;
	}
	if (!ParseInt64(OptionValue(options, name), value) || *value <= 0)
	{
		*error = std::string(name) + " must be greater than zero";
		return false;
	}
	return true;
}

bool ParseConfig(const std::vector<std::string>& args, CaptureConfig* config, std::string* error)
{
	std::set<std::string> flags = {"--auto-cross", "--no-auto-cross", "--allow-diagnostics"};
	for (const auto& flag: kDiagnosticFlags)
	{
		flags.insert(flag.flag);
	}
	const std::set<std::string> values = {"--guest-root", "--build",    "--out",           "--min-frame",      "--max-wait",
	                                      "--samples",    "--interval", "--baseline",      "--width",          "--height",
	                                      "--pad-at",     "--repo-root", "--rt-evidence-ps", "--lightbuf-probe", "--shader-probe-crc"};
	ParsedOptions& options = config->options;
	if (!ParseOptions(args, flags, values, &options, error))
	{
		return false;
	}
	if (!options.positionals.empty())
	{
		*error = "unexpected argument " + options.positionals[0];
		return false;
	}
	std::error_code ec;
	const char*     guest_env = std::getenv("KYTY_GUEST_ROOT");
	const char*     build_env = std::getenv("KYTY_BUILD");
	config->repo_root  = ResolvePath(OptionValue(options, "--repo-root", fs::current_path(ec).string()));
	config->guest_root = OptionValue(options, "--guest-root", guest_env != nullptr ? guest_env : "");
	config->build      = OptionValue(options, "--build", build_env != nullptr && *build_env != '\0' ? build_env : "_build_linux");
	config->output     = ResolvePath(OptionValue(options, "--out", "_scratch_playable/captures"));
	config->baseline   = OptionValue(options, "--baseline");
	config->auto_cross = HasOption(options, "--auto-cross") && !HasOption(options, "--no-auto-cross");
	config->allow_diagnostics = HasOption(options, "--allow-diagnostics");
	config->rt_evidence_ps    = OptionValue(options, "--rt-evidence-ps");
	config->lightbuf_probe    = OptionValue(options, "--lightbuf-probe");
	config->shader_probe_crc  = OptionValue(options, "--shader-probe-crc");
	if (config->guest_root.empty())
	{
		*error = "--guest-root or KYTY_GUEST_ROOT is required";
		return false;
	}
	if (!config->lightbuf_probe.empty() && config->lightbuf_probe != "1" && config->lightbuf_probe != "compositor")
	{
		*error = "--lightbuf-probe must be 1 or compositor";
		return false;
	}
	if (!ParsePositive(options, "--min-frame", &config->min_frame, error) || !ParsePositive(options, "--max-wait", &config->max_wait, error) ||
	    !ParsePositive(options, "--samples", &config->samples, error) || !ParsePositive(options, "--width", &config->width, error) ||
	    !ParsePositive(options, "--height", &config->height, error))
	{
		return false;
	}
	if (HasOption(options, "--interval") && (!ParseDouble(OptionValue(options, "--interval"), &config->interval) || config->interval < 0.0))
	{
		*error = "--interval must be non-negative";
		return false;
	}
	const auto range = options.values.equal_range("--pad-at");
	for (auto it = range.first; it != range.second; ++it)
	{
		const size_t colon = it->second.find(':');
		ScheduledTap tap;
		if (colon == std::string::npos || !ParseInt64(it->second.substr(0, colon), &tap.frame) || tap.frame <= 0 ||
		    (tap.button = Strip(it->second.substr(colon + 1))).empty())
		{
			*error = "--pad-at must use FRAME:BUTTON, for example 700:cross";
			return false;
		}
		config->pad_at.push_back(tap);
	}
	std::stable_sort(config->pad_at.begin(), config->pad_at.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
	return true;
}

std::vector<std::string> AllowedDiagnosticKeys(const CaptureConfig& config)
{
	std::vector<std::string> allowed;
	for (const auto& flag: kDiagnosticFlags)
	{
		if (HasOption(config.options, flag.flag))
		{
			allowed.emplace_back(flag.key);
		}
	}
	if (!config.rt_evidence_ps.empty())
	{
		allowed.insert(allowed.end(), {"KYTY_RT_EVIDENCE", "KYTY_RT_EVIDENCE_PS"});
	}
	if (!config.lightbuf_probe.empty())
	{
		allowed.emplace_back("KYTY_LIGHTBUF_PROBE");
	}
	if (!config.shader_probe_crc.empty())
	{
		allowed.emplace_back("KYTY_SHADER_PROBE_CRC");
	}
	return allowed;
}

Environment CaptureEnvironment(const CaptureConfig& config, const std::string& guest_root, const std::string& socket)
{
	ChildEnvironmentOptions options;
	options.guest_root        = guest_root;
	options.capture_directory = (fs::path(config.output) / "native_frames").string();
	options.agent_endpoint    = socket;
	options.optional_values   = {"KYTY_PRESENT_FPS_LIMIT"};
	Environment env           = BuildChildEnvironment(CurrentEnvironment(), options);
	if (config.auto_cross)
	{
		env["KYTY_AUTO_CROSS"] = "1";
	}
	for (const auto& flag: kDiagnosticFlags)
	{
		if (HasOption(config.options, flag.flag))
		{
			env[flag.key] = "1";
		}
	}
	if (!config.rt_evidence_ps.empty())
	{
		env["KYTY_RT_EVIDENCE"]    = "1";
		env["KYTY_RT_EVIDENCE_PS"] = config.rt_evidence_ps;
	}
	if (HasOption(config.options, "--videoout-evidence"))
	{
		env["KYTY_VIDEOOUT_EVIDENCE_MIN_FRAME"] = std::to_string(config.min_frame);
	}
	if (!config.lightbuf_probe.empty())
	{
		env["KYTY_LIGHTBUF_PROBE"] = config.lightbuf_probe;
	}
	if (!config.shader_probe_crc.empty())
	{
		env["KYTY_SHADER_PROBE_CRC"] = config.shader_probe_crc;
	}
	if (HasOption(config.options, "--command-buffer-dump"))
	{
		env["KYTY_COMMAND_BUFFER_DUMP_FOLDER"] = (fs::path(config.output) / "buffers").string();
	}
	if (HasOption(config.options, "--pipeline-dump"))
	{
		env["KYTY_PIPELINE_DUMP_FOLDER"] = (fs::path(config.output) / "pipelines").string();
	}
	env["KYTY_SCREEN_WIDTH"]  = std::to_string(config.width);
	env["KYTY_SCREEN_HEIGHT"] = std::to_string(config.height);
	return env;
}

class CaptureSession
{
public:
	CaptureSession(RunnerHost* host, const CaptureConfig& config): m_host(host), m_config(config) {}

	// Polls the agent until enough samples exist; returns the error, empty on success.
	std::string Collect(const std::string& log_path)
	{
		const double deadline = m_host->Now() + static_cast<double>(m_config.max_wait);
		double       next_capture = 0.0;
		bool         agent_ready  = false;
		size_t       next_tap     = 0;
		while (m_host->Now() < deadline && static_cast<int64_t>(m_captures.Size()) < m_config.samples)
		{
			if (const auto code = m_host->PollChild(); code.has_value())
			{
				return "guest exited early with code " + std::to_string(*code) + "; see " + log_path;
			}
			if (!agent_ready)
			{
				agent_ready = m_host->CallAgent(m_socket, "ping", Json::MakeObject(), 1.0).code == 0;
				m_host->Sleep(0.25);
				continue;
			}
			const AgentReply status = m_host->CallAgent(m_socket, "status", Json::MakeObject(), 2.0);
			if (status.code == 0)
			{
				m_frame = std::max(m_frame, ExtractResult(status.body)["frame"].AsInt());
			}
			for (; next_tap < m_config.pad_at.size() && m_frame >= m_config.pad_at[next_tap].frame; ++next_tap)
			{
				const ScheduledTap& tap = m_config.pad_at[next_tap];
				const bool ok = m_host->CallAgent(m_socket, "pad_tap", Json::MakeObject().Set("button", tap.button), 2.0).code == 0;
				m_input.Push(Json::MakeObject().Set("frame", m_frame).Set("target_frame", tap.frame).Set("button", tap.button).Set("ok", ok));
			}
			if (m_frame >= m_config.min_frame && m_host->Now() >= next_capture)
			{
				Capture();
				next_capture = m_host->Now() + std::max(m_config.interval, 0.25);
			}
			m_host->Sleep(0.25);
		}
		if (static_cast<int64_t>(m_captures.Size()) < m_config.samples)
		{
			return "timed out at frame " + std::to_string(m_frame) + " with " + std::to_string(m_captures.Size()) + "/" +
			       std::to_string(m_config.samples) + " native captures; see " + log_path;
		}
		return {};
	}

	void SetSocket(std::string socket) { m_socket = std::move(socket); }

	[[nodiscard]] const Json& Captures() const { return m_captures; }
	[[nodiscard]] const Json& Input() const { return m_input; }
	[[nodiscard]] const Json& CaptureErrors() const { return m_capture_errors; }

private:
	void Capture()
	{
		const AgentReply reply =
		    m_host->CallAgent(m_socket, "capture", Json::MakeObject().Set("timeout_ms", 15000).Set("score", false), 20.0);
		if (reply.code != 0)
		{
			m_capture_errors.Push(ValueText(FirstTruthy(reply.body["error"]["code"], "capture_failed")));
			return;
		}
		const Json&       result = ExtractResult(reply.body);
		std::error_code   ec;
		const fs::path    path   = fs::weakly_canonical(result["path"].AsString(), ec);
		const fs::path    output = fs::weakly_canonical(m_config.output, ec);
		const fs::path    relative = path.lexically_relative(output);
		Json              metrics;
		std::string       error;
		if (relative.empty() || *relative.begin() == ".." || !ScoreImage(path.string(), &metrics, &error))
		{
			m_capture_errors.Push("capture_invalid");
			return;
		}
		metrics.Set("path", relative.generic_string())
		    .Set("frame", result["frame"])
		    .Set("present", result["present"])
		    .Set("milestone", result["milestone"]);
		m_captures.Push(std::move(metrics));
	}

	RunnerHost*          m_host;
	const CaptureConfig& m_config;
	std::string          m_socket;
	int64_t              m_frame = 0;
	Json                 m_captures       = Json::MakeArray();
	Json                 m_input          = Json::MakeArray();
	Json                 m_capture_errors = Json::MakeArray();
};

bool BaselineMetrics(const std::string& path, Json* metrics, std::string* error)
{
	std::string text;
	Json        baseline;
	if (!ReadTextFile(path, kMaxManifestBytes, &text, error) || !ParseJson(text, &baseline, error))
	{
		return false;
	}
	if (!baseline.IsObject() || baseline["schema_version"] != Json(kSchemaVersion))
	{
		*error = "unsupported capture schema: " + ValueText(baseline["schema_version"]);
		return false;
	}
	if (!baseline["aggregate"].IsNull())
	{
		*metrics = baseline["aggregate"];
		return true;
	}
	if (baseline["captures"].Truthy())
	{
		return AggregateCaptures(baseline["captures"], metrics, error);
	}
	*error = "baseline manifest contains no captures";
	return false;
}

Json ManifestConfig(const CaptureConfig& config, const fs::path& build)
{
	const fs::path relative_build = build.lexically_relative(config.repo_root);
	const bool     inside         = !relative_build.empty() && *relative_build.begin() != "..";
	Json           pad_at         = Json::MakeArray();
	for (const auto& tap: config.pad_at)
	{
		pad_at.Push(Json::MakeObject().Set("frame", tap.frame).Set("button", tap.button));
	}
	const auto flag = [&](const char* name) { return HasOption(config.options, name); };
	const auto text_or_null = [](const std::string& value) { return value.empty() ? Json() : Json(value); };
	Json out = Json::MakeObject();
	out.Set("build", inside ? relative_build.generic_string() : build.filename().string())
	    .Set("screen", Json::MakeArray().Push(config.width).Push(config.height))
	    .Set("min_frame", config.min_frame)
	    .Set("samples", config.samples)
	    .Set("interval_seconds", config.interval)
	    .Set("auto_cross", config.auto_cross)
	    .Set("diagnostic_input", config.auto_cross)
	    .Set("rt_evidence", flag("--rt-evidence"))
	    .Set("rt_evidence_ps", text_or_null(config.rt_evidence_ps))
	    .Set("framebuffer_evidence", flag("--framebuffer-evidence"))
	    .Set("videoout_evidence", flag("--videoout-evidence"))
	    .Set("videoout_evidence_min_frame", flag("--videoout-evidence") ? Json(config.min_frame) : Json())
	    .Set("tex_probe", flag("--tex-probe"))
	    .Set("lightbuf_probe", text_or_null(config.lightbuf_probe))
	    .Set("shader_probe_crc", text_or_null(config.shader_probe_crc))
	    .Set("pad_at", std::move(pad_at))
	    .Set("vulkan_validation", flag("--vulkan-validation"))
	    .Set("shader_validation", flag("--shader-validation"))
	    .Set("command_buffer_dump", flag("--command-buffer-dump"))
	    .Set("pipeline_dump", flag("--pipeline-dump"))
	    .Set("native_capture", true)
	    .Set("native_capture_format", "PNG")
	    .Set("capture_source", "kyty_agent.capture")
	    .Set("strict_environment", !config.allow_diagnostics)
	    .Set("strict_compatibility_candidate", false);
	return out;
}

} // namespace

int CaptureRunMain(const std::vector<std::string>& args)
{
	CaptureConfig config;
	std::string   error;
	if (!ParseConfig(args, &config, &error))
	{
		std::fprintf(stderr, "kyty_agent: capture-run: %s\n", error.c_str());
		return 2;
	}
	std::unique_ptr<RunnerHost> host = CreateSystemRunnerHost();
	if (host == nullptr)
	{
		std::fprintf(stderr, "kyty_agent: capture-run is not supported on this host\n");
		return 2;
	}
	if (!config.allow_diagnostics)
	{
		std::vector<std::string> keys = StrictForbiddenEnvironmentKeys();
		keys.insert(keys.end(), CaptureDiagnosticKeys().begin(), CaptureDiagnosticKeys().end());
		const auto active = FindActiveEnvironmentKeys(CurrentEnvironment(), keys, AllowedDiagnosticKeys(config));
		if (!active.empty())
		{
			std::string joined;
			for (const auto& key: active)
			{
				joined += (joined.empty() ? "" : ", ") + key;
			}
			std::fprintf(stderr, "kyty_agent: error: strict capture refuses diagnostic/permissive environment: %s\n", joined.c_str());
			return 2;
		}
	}
	std::error_code ec;
	fs::create_directories(config.output, ec);
	const std::string stamp         = UtcTime("%Y%m%d-%H%M%S");
	const std::string log_path      = (fs::path(config.output) / ("guest-" + stamp + ".log")).string();
	const std::string manifest_path = (fs::path(config.output) / ("capture-" + stamp + ".json")).string();
	const fs::path    build         = ResolvePath(config.build, config.repo_root);
	const std::string executable    = (build / "fc_script").string();
	const std::string guest_root    = ResolvePath(config.guest_root);
	if (!fs::exists(executable, ec))
	{
		std::fprintf(stderr, "kyty_agent: error: missing build executable: %s\n", executable.c_str());
		return 2;
	}
	if (!fs::exists(guest_root, ec))
	{
		std::fprintf(stderr, "kyty_agent: error: guest root does not exist\n");
		return 2;
	}
	const std::string seed   = manifest_path;
	const std::string socket = "/tmp/kyty_cap_" + Sha256Hex(seed.data(), seed.size()).substr(0, 12) + ".sock";
	std::vector<std::string> termination_notes;
	RemoveStaleUnixSocket(socket, &termination_notes);

	LaunchSpec spec;
	spec.argv     = {executable, "scripts/run_guest.lua", guest_root};
	spec.cwd      = config.repo_root;
	spec.env      = CaptureEnvironment(config, guest_root, socket);
	spec.log_path = log_path;
	const std::string started_utc = UtcTime("%Y-%m-%dT%H:%M:%S+00:00");
	const double      started     = host->Now();
	if (!host->Launch(spec, &error))
	{
		std::fprintf(stderr, "kyty_agent: error: %s\n", error.c_str());
		return 2;
	}
	CaptureSession session(host.get(), config);
	session.SetSocket(socket);
	const std::string capture_error = session.Collect(log_path);
	host->TerminateChild(&termination_notes);
	fs::remove(socket, ec);

	Json aggregate;
	if (session.Captures().Size() > 0 && !AggregateCaptures(session.Captures(), &aggregate, &error))
	{
		aggregate = Json();
	}
	Json screenshots = Json::MakeArray();
	for (const Json& capture: session.Captures().Items())
	{
		screenshots.Push(capture["path"]);
	}
	Json notes = Json::MakeArray();
	for (const auto& note: termination_notes)
	{
		notes.Push(note);
	}
	Json manifest = Json::MakeObject();
	manifest.Set("schema_version", kSchemaVersion)
	    .Set("kind", "kyty.graphics.capture")
	    .Set("started_utc", started_utc)
	    .Set("duration_seconds", RoundDecimal(host->Now() - started, 3))
	    .Set("git", GitMetadata(config.repo_root))
	    .Set("host", HostMetadata())
	    .Set("config", ManifestConfig(config, build))
	    .Set("artifacts", Json::MakeObject()
	                          .Set("log", fs::path(log_path).filename().string())
	                          .Set("screenshots", std::move(screenshots))
	                          .Set("native_capture_directory", "native_frames"))
	    .Set("input", session.Input())
	    .Set("capture_errors", session.CaptureErrors())
	    .Set("process_termination", std::move(notes))
	    .Set("startup_captures", Json::MakeArray())
	    .Set("captures", session.Captures())
	    .Set("aggregate", aggregate)
	    .Set("status", capture_error.empty() ? "ok" : "incomplete");
	if (!capture_error.empty())
	{
		manifest.Set("error", capture_error);
	}
	if (!config.baseline.empty())
	{
		Json baseline_metrics;
		if (!BaselineMetrics(ResolvePath(config.baseline), &baseline_metrics, &error))
		{
			std::fprintf(stderr, "kyty_agent: error: %s\n", error.c_str());
			return 2;
		}
		manifest.Set("comparison", aggregate.IsNull() ? Json::MakeObject()
		                                                   .Set("pass", false)
		                                                   .Set("checks", Json::MakeObject().Set("samples_available", false))
		                                                   .Set("delta", Json::MakeObject())
		                                             : CompareMetrics(aggregate, baseline_metrics));
	}
	if (!WriteTextFile(manifest_path, manifest.Dump(2) + "\n", &error))
	{
		std::fprintf(stderr, "kyty_agent: error: %s\n", error.c_str());
		return 2;
	}
	if (!capture_error.empty())
	{
		std::fprintf(stderr, "kyty_agent: error: %s; partial manifest: %s\n", capture_error.c_str(), manifest_path.c_str());
		return 2;
	}
	Json frames = Json::MakeArray();
	for (const Json& capture: session.Captures().Items())
	{
		frames.Push(capture["frame"]);
	}
	Json result = Json::MakeObject();
	result.Set("manifest", manifest_path).Set("captures", static_cast<int64_t>(session.Captures().Size())).Set("frames", std::move(frames));
	if (manifest.Has("comparison"))
	{
		result.Set("comparison", manifest["comparison"]);
	}
	std::printf("%s\n", result.Dump().c_str());
	return !manifest.Has("comparison") || manifest["comparison"]["pass"].AsBool() ? 0 : 1;
}

} // namespace Kyty::Agent::Runner
