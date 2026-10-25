#include "Kyty/Agent/Runner/CaptureMetrics.h"
#include "Kyty/Agent/Runner/Playable.h"
#include "Kyty/Agent/Runner/SceneCheckpoint.h"
#include "Kyty/Agent/Runner/Sha256.h"

#include "RunnerSupport.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace Kyty::Agent::Runner {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxRetainedCaptureBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxBaselineBytes        = 4u * 1024u * 1024u;

const std::vector<std::string>& PlayableOptionalKeys()
{
	static const std::vector<std::string> keys = {
	    "KYTY_RENDER_RESOLUTION_MODE",
	    "KYTY_RENDER_RESOLUTION_SCOPE",
	    "KYTY_RENDER_RESOLUTION_WIDTH",
	    "KYTY_RENDER_RESOLUTION_HEIGHT",
	    "KYTY_PRESENTATION_FILTER",
	    "KYTY_PRESENT_FPS_LIMIT",
	    // Explicit strict runtime and writable-path choices; never switch optimizer, driver cache or filesystem silently.
	    "KYTY_SHADER_OPTIMIZATION",
	    "KYTY_SHADER_VALIDATION",
	    "KYTY_WAIT_TIMEOUT_MS",
	    "KYTY_SPIRV_CACHE",
	    "KYTY_VULKAN_PIPELINE_CACHE",
	    "KYTY_SAVEDATA_DIR",
	    "KYTY_SANDBOX_DIR",
	    "XDG_CACHE_HOME",
	    "TMPDIR",
	    "MESA_SHADER_CACHE_DIR",
	    "MESA_SHADER_CACHE_MAX_SIZE",
	    "MESA_SHADER_CACHE_DISABLE",
	    "shader_spilling_rate",
	};
	return keys;
}

double NumberOr(const Json& object, const char* key, double fallback)
{
	return object.Has(key) ? object[key].AsNumber(fallback) : fallback;
}

int64_t IntOr(const Json& object, const char* key, int64_t fallback)
{
	return object.Has(key) ? object[key].AsInt(fallback) : fallback;
}

// float(value or fallback)
double TruthyNumber(const Json& value, double fallback)
{
	return value.Truthy() ? value.AsNumber(fallback) : fallback;
}

Json Rounded(double value, int digits)
{
	return RoundDecimal(value, digits);
}

bool IsInside(const fs::path& path, const fs::path& directory)
{
	const auto relative = path.lexically_relative(directory);
	return !relative.empty() && *relative.begin() != "..";
}

// Creates scratch/run_<stamp>_<n> exclusively.
bool CreateRunDirectory(const std::string& scratch, std::string* run_dir, std::string* error)
{
	std::error_code ec;
	fs::create_directories(scratch, ec);
	const std::string stamp = UtcTime("%Y%m%dT%H%M%SZ");
	for (int attempt = 0; attempt < 1000; ++attempt)
	{
		const fs::path candidate = fs::path(scratch) / ("run_" + stamp + "_" + std::to_string(attempt));
		if (fs::create_directory(candidate, ec))
		{
			*run_dir = candidate.string();
			return true;
		}
	}
	*error = "cannot create run directory under scratch";
	return false;
}

std::string AgentSocketPath(const std::string& run_name)
{
	return "/tmp/kyty_pr_" + Sha256Hex(run_name.data(), run_name.size()).substr(0, 12) + ".sock";
}

class PlayableSession
{
public:
	PlayableSession(RunnerHost* host, const PlayableOptions& options): m_host(host), m_options(options) {}

	int Run(PlayableReport* out);

private:
	[[nodiscard]] double Elapsed() const { return m_host->Now() - m_started; }
	[[nodiscard]] double Budget(double cap) const { return std::max(0.0, std::min(cap, m_deadline_total - m_host->Now())); }
	void                 Note(std::string note) { m_notes.Push(std::move(note)); }

	[[nodiscard]] bool VisualRequired() const
	{
		const Json& visual = m_options.profile["visual"];
		return !visual.Has("require_baseline") || visual["require_baseline"].Truthy();
	}

	Json TimelineEvent(double t, const char* event)
	{
		Json entry = Json::MakeObject();
		entry.Set("t", Rounded(t, 3)).Set("event", event);
		return entry;
	}

	AgentReply Call(const char* tool, double timeout, const Json& args = Json::MakeObject())
	{
		return m_host->CallAgent(m_socket, tool, args, timeout);
	}

	bool Prepare(std::string* error);
	bool WriteJson(const char* name, const Json& value);
	int  FinishEarly(const std::string& gate, const std::string& detail);
	bool ObserveErrors(const std::string& stage);
	Json RetainCapture(const Json& result, const std::string& label, int64_t min_present);
	Json TakeCapture(const std::string& label, int64_t min_present);
	Json ExerciseCheckpoint(const Json& before);
	bool Iterate();
	bool HandleStartup();
	void HandleStatus(const Json& status, double now, double elapsed);
	bool HandleInput(double elapsed);
	void HandleCapture();
	void Finish();
	Json Summary() const;

	RunnerHost*            m_host;
	const PlayableOptions& m_options;

	std::string m_run_dir;
	std::string m_capture_dir;
	std::string m_log_path;
	std::string m_socket;
	Json        m_notes    = Json::MakeArray();
	Json        m_timeline = Json::MakeArray();

	std::optional<SceneCheckpoint> m_checkpoint;
	Json                           m_checkpoint_report = Json::MakeObject();

	double m_started        = 0.0;
	double m_deadline_total = 0.0;
	double m_ready_s        = 60.0;
	double m_first_present_s = 120.0;
	double m_stability_s    = 25.0;
	double m_input_s        = 30.0;
	double m_menu_settle_s  = 0.0;
	double m_inter_tap_settle_s = 0.0;
	int64_t m_post_input_min_present_delta = 240;
	double  m_post_input_min_settle_s      = 15.0;

	bool                   m_agent_ready       = false;
	bool                   m_video_initialized = false;
	std::optional<int64_t> m_first_present;
	std::optional<int64_t> m_present_at_stable_start;
	int64_t                m_present_delta = 0;
	Json                   m_input_before  = Json::MakeObject();
	Json                   m_input_after   = Json::MakeObject();
	std::string            m_capture_rel;
	Json                   m_compare = Json::MakeObject();
	bool                   m_baseline_missing     = true;
	bool                   m_timed_out            = false;
	bool                   m_coordinator_stop     = false;
	bool                   m_coordinator_stop_requested = false;
	bool                   m_final_error_observed = false;
	Json                   m_baseline_candidate   = Json::MakeObject();
	Json                   m_status               = Json::MakeObject();
	bool                   m_input_done           = false;
	bool                   m_stable_done          = false;
	bool                   m_capture_done         = false;
	bool                   m_input_sequence_ok    = false;
	PostInputWaitState     m_post_input_wait;
	bool                   m_post_input_ready = false;
	std::optional<double>  m_interactive_since;
	std::string            m_first_error;
	std::string            m_stop_reason;
	std::string            m_shutdown;
	std::optional<int>     m_exit_code;
	std::vector<GateResult> m_gates;
};

bool PlayableSession::WriteJson(const char* name, const Json& value)
{
	std::string error;
	if (!WriteTextFile((fs::path(m_run_dir) / name).string(), value.Dump(2) + "\n", &error))
	{
		std::fprintf(stderr, "kyty_agent: %s\n", error.c_str());
		return false;
	}
	return true;
}

bool PlayableSession::ObserveErrors(const std::string& stage)
{
	bool observed = true;
	for (const char* tool: {"last_error", "events"})
	{
		const double budget = Budget(1.0);
		if (budget <= 0.0)
		{
			observed = false;
			continue;
		}
		Json args = Json::MakeObject();
		if (std::string(tool) == "events")
		{
			args.Set("last", 100);
		}
		const AgentReply reply = Call(tool, budget, args);
		if (reply.code != 0)
		{
			observed = false;
			continue;
		}
		const Json&       result = ExtractResult(reply.body);
		const std::string error  = FirstActionableError(FirstTruthy(result["events"], Json::MakeArray()),
		                                                FirstTruthy(result["event"], Json::MakeObject()));
		if (!error.empty() && m_first_error.empty())
		{
			m_first_error = error;
			m_timeline.Push(TimelineEvent(Elapsed(), "runtime_error").Set("stage", stage).Set("error", error));
		}
	}
	if (!observed)
	{
		Note("error_observation_incomplete:" + stage);
	}
	return observed;
}

// Binds evidence to the fresh native response and this child's output tree.
Json PlayableSession::RetainCapture(const Json& result, const std::string& label, int64_t min_present)
{
	std::error_code ec;
	const fs::path  path    = fs::weakly_canonical(fs::path(result["path"].AsString()), ec);
	const fs::path  root    = fs::weakly_canonical(fs::path(m_capture_dir), ec);
	const int64_t   present = result["present"].AsInt();
	std::string     extension = Lower(path.extension().string());
	std::vector<uint8_t> data;
	std::string          error;
	if (result["image_format"].AsString() != "PNG" || extension != ".png" || !IsInside(path, root) || !fs::is_regular_file(path, ec) ||
	    present <= min_present || !ReadFileBytes(path.string(), kMaxRetainedCaptureBytes, &data, &error))
	{
		Note("capture_invalid:invalid_or_stale_native_capture");
		return {};
	}
	static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	if (data.size() < 8 || !std::equal(kSignature, kSignature + 8, data.begin()))
	{
		Note("capture_invalid:native_capture_not_png");
		return {};
	}
	const fs::path dest = fs::path(m_capture_dir) / (label + "_present_" + std::to_string(present) + ".png");
	std::FILE*     file = std::fopen(dest.string().c_str(), "wb");
	const bool     written = file != nullptr && std::fwrite(data.data(), 1, data.size(), file) == data.size();
	if (file != nullptr && std::fclose(file) != 0)
	{
		Note("capture_invalid:write_failed");
		return {};
	}
	if (!written)
	{
		Note("capture_invalid:write_failed");
		return {};
	}
	Json record = Json::MakeObject();
	record.Set("path", dest.string())
	    .Set("file", dest.filename().string())
	    .Set("sha256", Sha256Hex(data.data(), data.size()))
	    .Set("present", present)
	    .Set("frame", result["frame"])
	    .Set("source", "kyty_agent.capture");
	return record;
}

Json PlayableSession::TakeCapture(const std::string& label, int64_t min_present)
{
	const double budget = Budget(20.0);
	if (budget < 2.0)
	{
		m_timed_out = true;
		Note("capture_budget_exhausted");
		return {};
	}
	Json args = Json::MakeObject();
	args.Set("timeout_ms", std::min<int64_t>(15000, static_cast<int64_t>((budget - 0.25) * 1000.0))).Set("score", true);
	const AgentReply reply = Call("capture", budget, args);
	ObserveErrors("post_capture");
	if (reply.code != 0)
	{
		Note("capture_failed");
		return {};
	}
	Json record = RetainCapture(ExtractResult(reply.body), label, min_present);
	if (record.IsNull())
	{
		return {};
	}
	Json entry = TimelineEvent(Elapsed(), "capture");
	for (const auto& [key, value]: record.Members())
	{
		if (key != "path")
		{
			entry.Set(key, value);
		}
	}
	m_timeline.Push(std::move(entry));
	return record;
}

Json PlayableSession::ExerciseCheckpoint(const Json& before)
{
	Json action = Json::MakeObject();
	Json after;
	if (before.Truthy() && m_checkpoint->Match(before["path"].AsString(), "before")["matches"].AsBool())
	{
		const Json& config = m_checkpoint->Contract()["action"];
		double      budget = Budget(2.0);
		AgentReply  reply  = Call("status", std::max(0.001, budget));
		if (reply.code == 0 && budget > 0.0)
		{
			const Json& pre = ExtractResult(reply.body);
			action.Set("start_present", pre["present"].AsInt()).Set("input_before", PadCountersJson(PadCountersFromStatus(pre)));
			const PadSequenceResult delivered =
			    DeliverPadSequence(m_host, m_socket, config["pad_sequence"], std::min(m_deadline_total, m_host->Now() + m_input_s),
			                       m_inter_tap_settle_s, [this](const Json& /*status*/) { (void)ObserveErrors("checkpoint_input"); });
			action.Set("events", delivered.events);
			ObserveErrors("post_checkpoint_input");
			budget = Budget(2.0);
			reply  = Call("status", std::max(0.001, budget));
			if (reply.code == 0 && budget > 0.0)
			{
				const Json&   post        = ExtractResult(reply.body);
				const int64_t end_present = post["present"].AsInt();
				action.Set("end_present", end_present).Set("input_after", PadCountersJson(PadCountersFromStatus(post)));
				action.Set("delivery_checks",
				           InputDeliveryChecks(action["input_before"], action["input_after"], config["milestones"], delivered.ok));
				PostInputWaitState wait;
				wait.interactive_since         = m_host->Now();
				wait.interactive_start_present = end_present;
				while (m_host->Now() < m_deadline_total && !m_host->PollChild().has_value())
				{
					reply = Call("status", std::max(0.001, Budget(2.0)));
					ObserveErrors("checkpoint_settle");
					if (reply.code != 0)
					{
						break;
					}
					const int64_t present = ExtractResult(reply.body)["present"].AsInt();
					if (AdvancePostInputWait(&wait, present, m_host->Now(), config["min_present_delta"].AsInt(),
					                         config["min_settle_s"].AsNumber()))
					{
						action.Set("settled", true);
						after = TakeCapture("checkpoint_after", present);
						break;
					}
					m_host->Sleep(std::min(0.1, std::max(0.0, m_deadline_total - m_host->Now())));
				}
			}
		}
	}
	m_checkpoint_report = m_checkpoint->Evaluate(before.IsNull() ? Json::MakeObject() : before,
	                                             after.IsNull() ? Json::MakeObject() : after, action);
	return after;
}

bool PlayableSession::HandleStartup()
{
	const double elapsed = Elapsed();
	if (elapsed > m_ready_s)
	{
		Note("agent_ready_deadline");
		m_timed_out = true;
		return false;
	}
	const double timeout = Budget(1.0);
	if (timeout <= 0.0)
	{
		m_timed_out = true;
		Note("total_deadline");
		return false;
	}
	if (Call("ping", timeout).code == 0)
	{
		m_agent_ready = true;
		Note("agent_ready");
		m_timeline.Push(TimelineEvent(elapsed, "agent_ready"));
	}
	m_host->Sleep(std::min(0.25, std::max(0.0, m_deadline_total - m_host->Now())));
	return true;
}

void PlayableSession::HandleStatus(const Json& status, double now, double elapsed)
{
	const int64_t     present = status["present"].AsInt();
	const int64_t     frame   = status["frame"].AsInt();
	const bool        graphic = status["graphic_ready"].Truthy();
	const std::string phase   = ValueText(FirstTruthy(status["phase"], ""));
	if (!m_input_done)
	{
		if (graphic && present > 0)
		{
			m_interactive_since = m_interactive_since.value_or(elapsed);
		} else
		{
			m_interactive_since.reset();
		}
	} else
	{
		m_input_after = PadCountersJson(PadCountersFromStatus(status));
	}
	m_timeline.Push(TimelineEvent(elapsed, "status")
	                    .Set("present", present)
	                    .Set("frame", frame)
	                    .Set("phase", phase)
	                    .Set("graphic_ready", graphic)
	                    .Set("fps", status["fps"]));
	if (graphic && !m_video_initialized)
	{
		m_video_initialized = true;
		m_timeline.Push(TimelineEvent(elapsed, "video_initialized"));
	}
	if (present >= 1 && !m_first_present.has_value())
	{
		m_first_present = present;
		m_timeline.Push(TimelineEvent(elapsed, "first_present").Set("present", present));
		m_present_at_stable_start = present;
	}
	if (!m_input_done || m_post_input_ready)
	{
		return;
	}
	m_post_input_ready =
	    AdvancePostInputWait(&m_post_input_wait, present, now, m_post_input_min_present_delta, m_post_input_min_settle_s);
	if (!m_post_input_ready)
	{
		return;
	}
	const int64_t stable_delta = std::max<int64_t>(0, present - m_post_input_wait.interactive_start_present.value_or(present));
	const double  settle_s     = std::max(0.0, now - m_post_input_wait.interactive_since.value_or(now));
	Note("post_input_settled");
	m_timeline.Push(TimelineEvent(elapsed, "post_input_settled").Set("present_delta", stable_delta).Set("settle_s", Rounded(settle_s, 3)));
}

bool PlayableSession::HandleInput(double elapsed)
{
	const double menu_ready_at      = m_interactive_since.value_or(elapsed) + m_menu_settle_s;
	const double stability_fallback = m_first_present_s + m_stability_s * 0.5;
	const bool   can_start          = m_agent_ready && m_first_present.has_value() && elapsed >= menu_ready_at &&
	                       (m_stable_done || elapsed >= stability_fallback);
	if (m_input_done || !can_start)
	{
		return true;
	}
	const double timeout = Budget(2.0);
	if (timeout <= 0.0)
	{
		m_timed_out = true;
		Note("total_deadline");
		return false;
	}
	const AgentReply pre_reply = Call("status", timeout);
	const Json       pre       = pre_reply.code == 0 ? ExtractResult(pre_reply.body) : Json::MakeObject();
	if (pre_reply.code != 0 || !pre["graphic_ready"].Truthy())
	{
		m_interactive_since.reset();
		return true;
	}
	m_input_before = PadCountersJson(PadCountersFromStatus(pre));
	const auto observe = [this](const Json& observed) {
		(void)ObserveErrors("startup_input");
		if (RecordInputPhaseObservation(&m_post_input_wait, ValueText(FirstTruthy(observed["phase"], ""))))
		{
			Note("post_input_loading_seen");
			m_timeline.Push(TimelineEvent(Elapsed(), "post_input_loading"));
		}
	};
	const PadSequenceResult delivered = DeliverPadSequence(m_host, m_socket, FirstTruthy(m_options.profile["pad_sequence"], Json::MakeArray()),
	                                                       std::min(m_deadline_total, m_host->Now() + m_input_s), m_inter_tap_settle_s, observe);
	m_input_sequence_ok = delivered.ok;
	for (const Json& event: delivered.events.Items())
	{
		Json entry = Json::MakeObject();
		entry.Set("t", Rounded(Elapsed(), 3));
		for (const auto& [key, value]: event.Members())
		{
			entry.Set(key, value);
		}
		m_timeline.Push(std::move(entry));
	}
	if (!m_input_sequence_ok)
	{
		Note("input_sequence_failed");
	}
	ObserveErrors("post_input");
	const AgentReply fresh = Call("status", std::max(0.001, Budget(2.0)));
	if (fresh.code == 0)
	{
		m_status      = ExtractResult(fresh.body);
		m_input_after = PadCountersJson(PadCountersFromStatus(m_status));
		const bool was_loading_seen = m_post_input_wait.loading_seen;
		m_post_input_ready = AdvancePostInputWait(&m_post_input_wait, m_status["present"].AsInt(), m_host->Now(),
		                                          m_post_input_min_present_delta, m_post_input_min_settle_s);
		if (m_post_input_wait.loading_seen && !was_loading_seen)
		{
			Note("post_input_loading_seen");
			m_timeline.Push(TimelineEvent(Elapsed(), "post_input_loading"));
		}
	} else
	{
		m_input_sequence_ok = false;
		Note("post_input_status_failed");
	}
	m_input_done = true;
	Note("input_sequence_done");
	return true;
}

// The first capture is the fixture's pre-action checkpoint; material health
// alone cannot promote it to a playable baseline.
void PlayableSession::HandleCapture()
{
	if (!m_agent_ready || !m_first_present.has_value() || m_capture_done || !m_input_done || !m_post_input_ready)
	{
		return;
	}
	m_capture_done        = true;
	const Json before     = TakeCapture("checkpoint_before", m_status["present"].AsInt());
	Json       selected   = before;
	if (m_checkpoint.has_value())
	{
		const Json after = ExerciseCheckpoint(before);
		selected         = after.Truthy() ? after : before;
	}
	if (!selected.Truthy())
	{
		return;
	}
	m_capture_rel = selected["file"].AsString();
	Json        metrics;
	std::string error;
	const auto  fail = [this](const char* what) {
        Note(std::string("checkpoint_or_metrics_error:") + what);
        m_checkpoint_report = Json::MakeObject().Set("error", what);
        m_compare           = Json::MakeObject().Set("pass", false).Set("error", what);
	};
	if (!ScoreImage(selected["path"].AsString(), &metrics, &error))
	{
		fail("metrics");
		return;
	}
	(void)WriteJson("capture_metrics.json", SanitizeJson(metrics, m_options.guest_root));
	std::error_code ec;
	if (m_options.create_baseline)
	{
		m_compare = VisualFloorFromMetrics(metrics);
		m_baseline_candidate = Json::MakeObject();
		m_baseline_candidate.Set("schema", "kyty_playable_visual_baseline_v1")
		    .Set("world", metrics["world"])
		    .Set("checkpoint_contract_sha256", m_checkpoint.has_value() ? Json(m_checkpoint->Sha256()) : Json())
		    .Set("capture_sha256", selected["sha256"])
		    .Set("created_utc", UtcTime("%Y-%m-%dT%H:%M:%SZ"));
		return;
	}
	if (!fs::is_regular_file(m_options.baseline_path, ec))
	{
		return;
	}
	std::string text;
	Json        baseline;
	if (!ReadTextFile(m_options.baseline_path, kMaxBaselineBytes, &text, &error) || !ParseJson(text, &baseline, &error) ||
	    !baseline.IsObject())
	{
		fail("baseline");
		return;
	}
	const Json baseline_metrics = baseline.Has("world") ? baseline : Json::MakeObject().Set("world", baseline);
	m_compare                   = ComparePlayableVisualMetrics(metrics, baseline_metrics);
	Note("visual_compare_done");
}

// One coordinator iteration; false ends the session loop.
bool PlayableSession::Iterate()
{
	if (m_host->Now() >= m_deadline_total)
	{
		m_timed_out = true;
		Note("total_deadline");
		return false;
	}
	if (m_host->PollChild().has_value())
	{
		Note("child_exited_early");
		return false;
	}
	if (!m_agent_ready)
	{
		return HandleStartup();
	}
	const double now     = m_host->Now();
	const double elapsed = now - m_started;
	const double timeout = Budget(2.0);
	if (timeout <= 0.0)
	{
		m_timed_out = true;
		Note("total_deadline");
		return false;
	}
	const AgentReply reply = Call("status", timeout);
	if (reply.code == 0)
	{
		m_status = ExtractResult(reply.body);
		HandleStatus(m_status, now, elapsed);
	} else if (!m_input_done)
	{
		m_interactive_since.reset();
	}
	ObserveErrors("poll");

	if (!m_first_present.has_value() && elapsed > m_first_present_s)
	{
		Note("first_present_deadline");
		m_timed_out = true;
		return false;
	}
	if (m_first_present.has_value() && !m_stable_done)
	{
		const int64_t present_now = m_status["present"].AsInt();
		m_present_at_stable_start = m_present_at_stable_start.value_or(present_now);
		m_present_delta           = std::max<int64_t>(0, present_now - *m_present_at_stable_start);
		if (m_present_delta >= IntOr(m_options.profile["milestones"], "present_delta_stable", 15))
		{
			m_stable_done = true;
			Note("present_stability_met");
			m_timeline.Push(TimelineEvent(elapsed, "present_stable").Set("delta", m_present_delta));
		} else if (elapsed > m_first_present_s + m_stability_s)
		{
			// Input and capture still run with whatever stability was reached.
			Note("present_stability_deadline");
		}
	}
	if (!HandleInput(elapsed))
	{
		return false;
	}
	HandleCapture();
	if (m_stable_done && m_input_done && m_capture_done)
	{
		Note("session_milestones_complete");
		return false;
	}
	m_host->Sleep(std::min(0.35, std::max(0.0, m_deadline_total - m_host->Now())));
	return true;
}

// Observes failures after input, capture and scoring before stopping the child.
// A lost final observation is unknown, never an invented clean state.
void PlayableSession::Finish()
{
	if (m_agent_ready)
	{
		m_final_error_observed = ObserveErrors("final");
	}
	if (m_host->Now() >= m_deadline_total)
	{
		m_timed_out = true;
	}
	const bool complete = m_stable_done && m_input_done && m_capture_done;
	m_stop_reason       = m_timed_out ? "deadline" : (complete ? "milestones_complete" : "incomplete");
	if (!m_host->PollChild().has_value())
	{
		m_coordinator_stop_requested = true;
		std::vector<std::string> stop_notes;
		m_host->TerminateChild(&stop_notes);
		for (const auto& note: stop_notes)
		{
			m_coordinator_stop = m_coordinator_stop || note == "killed_sigterm" || note == "killed_sigkill" || note == "kill_timeout_after_sigkill";
			Note(note);
		}
	}
	m_exit_code = m_host->PollChild();
	if (!m_coordinator_stop)
	{
		m_stop_reason = !m_exit_code.has_value() ? "unknown" : (*m_exit_code == 0 ? "natural_exit" : "abnormal_exit");
	}
	std::error_code ec;
	fs::remove(m_socket, ec);

	m_shutdown = ClassifyExit(m_exit_code, m_timed_out, m_coordinator_stop);
	if (m_first_error.empty())
	{
		m_first_error = FirstActionableFromLog(m_log_path);
	}
	GateInputs inputs;
	inputs.agent_ready             = m_agent_ready;
	inputs.video_initialized       = m_video_initialized;
	inputs.first_present           = m_first_present;
	inputs.present_delta           = m_present_delta;
	inputs.input_before            = m_input_before;
	inputs.input_after             = m_input_after;
	inputs.capture_path            = m_capture_rel;
	inputs.compare                 = m_compare;
	inputs.baseline_missing        = m_baseline_missing;
	inputs.create_baseline         = m_options.create_baseline && !m_capture_rel.empty();
	inputs.shutdown                = m_shutdown;
	inputs.first_error             = m_first_error;
	inputs.milestones              = FirstTruthy(m_options.profile["milestones"], Json::MakeObject());
	inputs.visual_require_baseline = VisualRequired();
	inputs.input_sequence_ok       = m_input_sequence_ok;
	inputs.checkpoint              = m_checkpoint_report;
	inputs.timed_out               = m_timed_out;
	inputs.final_error_observed    = m_final_error_observed;
	m_gates                        = EvaluateGates(inputs);
}

Json PlayableSession::Summary() const
{
	Json gates = Json::MakeArray();
	bool passed = !m_gates.empty();
	for (const auto& gate: m_gates)
	{
		gates.Push(Json::MakeObject().Set("name", gate.name).Set("passed", gate.passed).Set("detail", gate.detail));
		passed = passed && gate.passed;
	}
	const Json& profile = m_options.profile;
	Json        summary = Json::MakeObject();
	summary.Set("schema", "kyty_playable_regression_summary_v1")
	    .Set("mode", "strict")
	    .Set("protocol", "kyty_agent")
	    .Set("passed", passed)
	    .Set("gates", std::move(gates))
	    .Set("first_error", m_first_error)
	    .Set("first_present", m_first_present.has_value() ? Json(*m_first_present) : Json())
	    .Set("present_delta", m_present_delta)
	    .Set("input_before", m_input_before)
	    .Set("input_after", m_input_after)
	    .Set("capture", m_capture_rel)
	    .Set("compare", m_compare)
	    .Set("child_exit", m_exit_code.has_value() ? Json(*m_exit_code) : Json())
	    .Set("shutdown", m_shutdown)
	    .Set("timed_out", m_timed_out)
	    .Set("stop_reason", m_stop_reason)
	    .Set("coordinator_stop_requested", m_coordinator_stop_requested)
	    .Set("checkpoint", m_checkpoint_report)
	    .Set("timeline", m_timeline)
	    .Set("notes", m_notes)
	    .Set("profile_deadlines_s", FirstTruthy(profile["deadlines_s"], Json::MakeObject()))
	    .Set("profile_milestones", FirstTruthy(profile["milestones"], Json::MakeObject()))
	    .Set("profile_input", FirstTruthy(profile["input"], Json::MakeObject()))
	    .Set("profile_post_input", FirstTruthy(profile["post_input"], Json::MakeObject()));
	return SanitizeJson(summary, m_options.guest_root);
}

int PlayableSession::FinishEarly(const std::string& gate, const std::string& detail)
{
	m_gates = {{gate, false, detail}};
	(void)WriteJson("summary.json", Summary());
	return 1;
}

bool PlayableSession::Prepare(std::string* error)
{
	if (!CreateRunDirectory(m_options.scratch, &m_run_dir, error))
	{
		return false;
	}
	m_capture_dir = (fs::path(m_run_dir) / "captures").string();
	m_log_path    = (fs::path(m_run_dir) / "child.log").string();
	std::error_code ec;
	fs::create_directory(m_capture_dir, ec);
	m_socket = AgentSocketPath(fs::path(m_run_dir).filename().string());
	std::vector<std::string> notes;
	RemoveStaleUnixSocket(m_socket, &notes);
	for (auto& note: notes)
	{
		Note(std::move(note));
	}
	m_baseline_missing = !fs::is_regular_file(m_options.baseline_path, ec);

	const Json& profile    = m_options.profile;
	const Json& deadlines  = profile["deadlines_s"];
	m_ready_s              = TruthyNumber(deadlines["agent_ready"], 60);
	m_first_present_s      = TruthyNumber(deadlines["first_present"], 120);
	m_stability_s          = TruthyNumber(deadlines["present_stability"], 25);
	m_input_s              = TruthyNumber(deadlines["input_window"], 30);
	const Json& input      = profile["input"];
	m_menu_settle_s        = TruthyNumber(input["menu_settle_s"], 0.0);
	m_inter_tap_settle_s   = TruthyNumber(input["inter_tap_settle_s"], 0.0);
	const Json& post_input = profile["post_input"];
	m_post_input_min_present_delta = IntOr(post_input, "min_present_delta", 240);
	m_post_input_min_settle_s      = NumberOr(post_input, "min_settle_s", 15);

	if (m_options.scene_checkpoint_path.empty())
	{
		m_checkpoint_report.Set("error", "checkpoint_missing");
	} else
	{
		SceneCheckpoint checkpoint;
		std::string     checkpoint_error;
		if (checkpoint.Load(m_options.scene_checkpoint_path, &checkpoint_error))
		{
			m_checkpoint = std::move(checkpoint);
		} else
		{
			std::fprintf(stderr, "kyty_agent: scene checkpoint invalid: %s\n", checkpoint_error.c_str());
			m_checkpoint_report.Set("error", "checkpoint_invalid");
			Note("checkpoint_invalid");
		}
	}
	return true;
}

int PlayableSession::Run(PlayableReport* out)
{
	std::string error;
	if (!Prepare(&error))
	{
		std::fprintf(stderr, "kyty_agent: %s\n", error.c_str());
		return 2;
	}
	out->run_dir = m_run_dir;

	ChildEnvironmentOptions env_options;
	env_options.guest_root        = m_options.guest_root;
	env_options.capture_directory = m_capture_dir;
	env_options.agent_endpoint    = m_socket;
	env_options.optional_values   = PlayableOptionalKeys();
	env_options.default_values    = {{"KYTY_NATIVE_CAPTURE_MAX_EDGE", "1280"}};
	Environment env               = BuildChildEnvironment(m_options.base_env, env_options);
	env["KYTY_CRASH_REPORT"]      = (fs::path(m_run_dir) / "crash-context.json").string();
	const auto bad                = FindActiveEnvironmentKeys(env, StrictForbiddenEnvironmentKeys());
	if (!bad.empty())
	{
		std::string joined;
		for (const auto& key: bad)
		{
			joined += (joined.empty() ? "" : ",") + key;
		}
		Note("strict_env_violation:" + joined);
		const int code = FinishEarly("strict_env", joined);
		out->gates     = m_gates;
		out->summary   = Summary();
		return code;
	}
	std::string keys;
	for (const auto& [key, value]: env)
	{
		keys += key + "\n";
	}
	(void)WriteTextFile((fs::path(m_run_dir) / "child_env_keys.txt").string(), keys, &error);

	std::string binary_sha256;
	std::string script_sha256;
	if (!Sha256File(m_options.fc_script, &binary_sha256, &error) || !Sha256File(m_options.guest_script, &script_sha256, &error))
	{
		std::fprintf(stderr, "kyty_agent: %s\n", error.c_str());
		return 2;
	}
	LaunchSpec spec;
	spec.argv     = {m_options.fc_script, m_options.guest_script, m_options.guest_root};
	spec.cwd      = m_options.runtime_cwd;
	spec.env      = env;
	spec.log_path = m_log_path;
	Json command  = Json::MakeArray();
	for (const auto& arg: spec.argv)
	{
		command.Push(arg);
	}
	(void)WriteJson("launch-config.json", Json::MakeObject()
	                                          .Set("command", std::move(command))
	                                          .Set("cwd", spec.cwd)
	                                          .Set("binary_sha256", binary_sha256)
	                                          .Set("guest_script_sha256", script_sha256));

	m_started        = m_host->Now();
	m_deadline_total = m_started + TruthyNumber(m_options.profile["deadlines_s"]["total"], 180);
	if (!m_host->Launch(spec, &error))
	{
		std::fprintf(stderr, "kyty_agent: %s\n", error.c_str());
		return 2;
	}
	(void)WriteTextFile((fs::path(m_run_dir) / "child.pid").string(), std::to_string(m_host->ChildPid()), &error);

	while (Iterate())
	{
	}
	Finish();

	bool passed = !m_gates.empty();
	for (const auto& gate: m_gates)
	{
		passed = passed && gate.passed;
	}
	// Never overwrite a retained baseline with a menu, failed action, late runtime error or crash.
	if (m_options.create_baseline && m_baseline_candidate.Size() > 0 && passed)
	{
		std::error_code ec;
		fs::create_directories(fs::path(m_options.baseline_path).parent_path(), ec);
		if (WriteTextFile(m_options.baseline_path, m_baseline_candidate.Dump(2) + "\n", &error))
		{
			Note("baseline_created");
			m_baseline_missing = false;
		}
	} else if (m_options.create_baseline)
	{
		Note("baseline_rejected_gates");
	}
	out->gates   = m_gates;
	out->summary = Summary();
	(void)WriteJson("summary.json", out->summary);
	(void)WriteJson("timeline.json", SanitizeJson(m_timeline, m_options.guest_root));

	if (m_baseline_missing && VisualRequired() && !m_options.create_baseline)
	{
		return 3;
	}
	return passed ? 0 : 1;
}

} // namespace

int RunPlayableSession(RunnerHost* host, const PlayableOptions& options, PlayableReport* report)
{
	PlayableSession session(host, options);
	return session.Run(report);
}

} // namespace Kyty::Agent::Runner
