#include "Kyty/Agent/Runner/CaptureMetrics.h"
#include "Kyty/Agent/Runner/Playable.h"

#include "RunnerSupport.h"

#include <algorithm>
#include <array>

namespace Kyty::Agent::Runner {
namespace {

constexpr const char* kDefaultProfile = R"({
  "schema": "kyty_playable_regression_profile_v1",
  "mode": "strict",
  "deadlines_s": {"agent_ready": 60, "first_present": 120, "present_stability": 25, "input_window": 30, "total": 180},
  "milestones": {"min_present_after_ready": 1, "present_delta_stable": 15, "min_pad_taps": 3, "min_guest_read_state_samples": 1},
  "pad_sequence": [{"tool": "pad_tap", "button": "cross"}, {"tool": "pad_tap", "button": "cross"}, {"tool": "pad_tap", "button": "cross"}],
  "input": {"menu_settle_s": 5, "inter_tap_settle_s": 3},
  "post_input": {"require_loading_transition": true, "min_present_delta": 240, "min_settle_s": 15},
  "visual": {"require_baseline": true, "require_capture": true}
})";

constexpr std::array<const char*, 5> kMaterialVisualChecks = {
    "white_ratio_not_worse", "entropy_not_collapsed", "colors_not_collapsed", "not_stripey", "absolute_world_gate",
};

int64_t IntOr(const Json& object, const char* key, int64_t fallback)
{
	return object.Has(key) ? object[key].AsInt(fallback) : fallback;
}

double TimeoutUntil(RunnerHost* host, double deadline, double cap)
{
	return std::max(0.0, std::min(cap, deadline - host->Now()));
}

// Python float() of a hold value; -1 marks an invalid one.
double HoldSeconds(const Json& step, bool axis_tool)
{
	if (!step.Has("hold_s"))
	{
		return 0.0;
	}
	const Json& value = step["hold_s"];
	if (value.IsNumber())
	{
		return value.AsNumber();
	}
	if (value.IsBool() && !axis_tool)
	{
		return value.AsBool() ? 1.0 : 0.0;
	}
	double parsed = 0.0;
	if (value.IsString() && !axis_tool && ParseDouble(Strip(value.AsString()), &parsed))
	{
		return parsed;
	}
	return -1.0;
}

bool IsAxisName(const Json& axis)
{
	if (!axis.IsString())
	{
		return false;
	}
	const std::string& name = axis.AsString();
	return name == "left_x" || name == "left_y" || name == "right_x" || name == "right_y";
}

Json AxisEvent(const char* event, const Json& axis, const Json& value)
{
	Json out = Json::MakeObject();
	out.Set("event", event).Set("axis", axis).Set("value", value);
	return out;
}

Json ButtonEvent(const std::string& event, const std::string& button)
{
	Json out = Json::MakeObject();
	out.Set("event", event).Set("button", button);
	return out;
}

class PadSequenceRunner
{
public:
	PadSequenceRunner(RunnerHost* host, const std::string& endpoint, std::optional<double> deadline,
	                  const std::function<void(const Json&)>& observer)
	    : m_host(host), m_endpoint(endpoint), m_deadline(deadline), m_observer(observer)
	{
		if (deadline.has_value())
		{
			const double budget  = std::max(0.0, *deadline - host->Now());
			const double reserve = std::min({2.0, std::max(0.1, budget * 0.2), budget});
			m_action_deadline    = *deadline - reserve;
		}
		m_result.events = Json::MakeArray();
	}

	PadSequenceResult Run(const Json& steps, double inter_tap_settle_s)
	{
		const size_t count = steps.Size();
		for (size_t index = 0; index < count; ++index)
		{
			if (ActionExpired() || !RunStep(steps[index], index + 1 < count, inter_tap_settle_s))
			{
				m_result.ok = false;
				break;
			}
		}
		const double clear_timeout = m_deadline.has_value() ? TimeoutUntil(m_host, *m_deadline, 2.0) : 2.0;
		const int    clear_code    = m_host->CallAgent(m_endpoint, "pad_clear", Json::MakeObject(), std::max(0.001, clear_timeout)).code;
		Json         clear         = Json::MakeObject();
		clear.Set("event", "pad_clear").Set("ok", clear_code == 0);
		m_result.events.Push(std::move(clear));
		m_result.ok = m_result.ok && clear_code == 0;
		return std::move(m_result);
	}

private:
	[[nodiscard]] bool ActionExpired() const { return m_action_deadline.has_value() && m_host->Now() >= *m_action_deadline; }

	[[nodiscard]] double ActionTimeout(double cap) const
	{
		return m_action_deadline.has_value() ? TimeoutUntil(m_host, *m_action_deadline, cap) : cap;
	}

	// Remaining time until the action deadline, capped; no cap without one.
	[[nodiscard]] double ActionWait(double wait) const
	{
		return m_action_deadline.has_value() ? std::min(wait, std::max(0.0, *m_action_deadline - m_host->Now())) : wait;
	}

	bool Status(double timeout, PadCounters* counters)
	{
		const AgentReply reply = m_host->CallAgent(m_endpoint, "status", Json::MakeObject(), timeout);
		if (reply.code != 0)
		{
			return false;
		}
		const Json& status = ExtractResult(reply.body);
		if (m_observer)
		{
			m_observer(status);
		}
		if (counters != nullptr)
		{
			*counters = PadCountersFromStatus(status);
		}
		return true;
	}

	bool Fail(Json event)
	{
		m_result.events.Push(std::move(event));
		return false;
	}

	bool RunStep(const Json& step, bool has_next, double inter_tap_settle_s)
	{
		const std::string tool   = ValueText(FirstTruthy(step["tool"], "pad_tap"));
		const std::string button = ValueText(FirstTruthy(step["button"], "cross"));
		const Json&       axis   = step["axis"];
		const Json&       value  = step["value"];
		const bool        is_axis = tool == "pad_axis";
		if (tool != "pad_tap" && tool != "pad_down" && tool != "pad_up" && !is_axis)
		{
			return Fail(ButtonEvent(tool, button).Set("ok", false));
		}
		if (is_axis && (!IsAxisName(axis) || !value.IsInteger() || value.AsInt() < 0 || value.AsInt() > 255))
		{
			return Fail(AxisEvent("pad_axis", axis.IsString() ? axis : Json(""), value.IsInteger() ? value : Json())
			                .Set("ok", false)
			                .Set("error", "invalid_axis"));
		}
		const double hold_s = HoldSeconds(step, is_axis);
		if (!(hold_s >= 0.0 && hold_s <= 30.0) || (hold_s != 0.0 && tool != "pad_down" && !is_axis))
		{
			Json event = is_axis ? AxisEvent("pad_axis", axis, value) : ButtonEvent(tool, button);
			return Fail(event.Set("ok", false).Set("error", "invalid_hold"));
		}

		std::optional<PadCounters> hold_before;
		if (hold_s != 0.0)
		{
			PadCounters counters;
			if (!Status(std::max(0.001, ActionTimeout(2.0)), &counters))
			{
				return false;
			}
			hold_before = counters;
		}
		const double budget = ActionTimeout(2.0);
		if (budget <= 0.0)
		{
			return false;
		}
		Json args = Json::MakeObject();
		if (is_axis)
		{
			args.Set("axis", axis).Set("value", value);
		} else
		{
			args.Set("button", button);
		}
		const int code  = m_host->CallAgent(m_endpoint, tool, args, budget).code;
		Json      event = is_axis ? AxisEvent("pad_axis", axis, value) : ButtonEvent(tool, button);
		m_result.events.Push(event.Set("ok", code == 0));
		if (code != 0)
		{
			return false;
		}

		bool success = HoldInput(hold_s, hold_before, tool, button, axis, value);
		if (m_action_deadline.has_value() && m_host->Now() > *m_action_deadline)
		{
			success = false;
		}
		if (!success)
		{
			return false;
		}
		return tool != "pad_tap" || (WaitTapDrained() && (!has_next || SettleBetweenTaps(inter_tap_settle_s)));
	}

	// Keeps a held input active while guest input counters are observed.
	bool HoldInput(double hold_s, const std::optional<PadCounters>& before, const std::string& tool, const std::string& button,
	               const Json& axis, const Json& value)
	{
		bool              success    = true;
		const double      hold_until = m_host->Now() + hold_s;
		std::optional<PadCounters> after = before;
		while (m_host->Now() < hold_until)
		{
			if (ActionExpired())
			{
				success = false;
				break;
			}
			m_host->Sleep(ActionWait(std::min(0.05, hold_until - m_host->Now())));
			PadCounters counters;
			if (!Status(std::max(0.001, ActionTimeout(2.0)), &counters))
			{
				success = false;
				break;
			}
			after = counters;
		}
		if (!before.has_value() || !after.has_value())
		{
			return success;
		}
		const bool consumed = after->guest_read_state_samples + after->guest_read_samples >
		                      before->guest_read_state_samples + before->guest_read_samples;
		Json event = Json::MakeObject();
		event.Set("event", "hold_observed").Set("hold_s", hold_s);
		if (tool == "pad_axis")
		{
			event.Set("axis", axis).Set("value", value);
		} else
		{
			event.Set("button", button);
		}
		event.Set("input_before", PadCountersJson(*before)).Set("input_after", PadCountersJson(*after)).Set("ok", success && consumed);
		m_result.events.Push(std::move(event));
		return success && consumed;
	}

	bool WaitTapDrained()
	{
		for (int attempt = 0; attempt < 60; ++attempt)
		{
			if (ActionExpired())
			{
				return false;
			}
			PadCounters counters;
			if (!Status(ActionTimeout(2.0), &counters))
			{
				return false;
			}
			if (!counters.tap_pending)
			{
				return true;
			}
			const double wait = ActionWait(0.05);
			if (wait > 0.0)
			{
				m_host->Sleep(wait);
			}
		}
		return false;
	}

	bool SettleBetweenTaps(double settle_s)
	{
		if (settle_s <= 0.0)
		{
			return true;
		}
		double settle_until = m_host->Now() + settle_s;
		if (m_action_deadline.has_value())
		{
			settle_until = std::min(settle_until, *m_action_deadline);
		}
		while (m_host->Now() < settle_until)
		{
			m_host->Sleep(std::min(0.25, settle_until - m_host->Now()));
			if (!Status(ActionTimeout(2.0), nullptr))
			{
				return false;
			}
		}
		return !ActionExpired();
	}

	RunnerHost*                             m_host;
	const std::string&                      m_endpoint;
	std::optional<double>                   m_deadline;
	std::optional<double>                   m_action_deadline;
	const std::function<void(const Json&)>& m_observer;
	PadSequenceResult                       m_result;
};

std::string GateJson(const Json& compare)
{
	return TruncateCodePoints(FirstTruthy(compare["checks"], compare).Dump(), 200);
}

bool AllTrue(const Json& checks, std::initializer_list<const char*> keys)
{
	return std::all_of(keys.begin(), keys.end(), [&](const char* key) { return checks[key].Truthy(); });
}

} // namespace

Json DefaultPlayableProfile()
{
	Json        profile;
	std::string error;
	(void)ParseJson(kDefaultProfile, &profile, &error);
	return profile;
}

Json MergePlayableProfile(const Json& data)
{
	Json out = DefaultPlayableProfile();
	for (const auto& [key, value]: data.Members())
	{
		if (!value.IsObject() || !out[key].IsObject())
		{
			out.Set(key, value);
			continue;
		}
		Json merged = out[key];
		for (const auto& [inner_key, inner_value]: value.Members())
		{
			merged.Set(inner_key, inner_value);
		}
		out.Set(key, std::move(merged));
	}
	return out;
}

PadCounters PadCountersFromStatus(const Json& status)
{
	const Json& pad = status["pad"];
	PadCounters counters;
	counters.delivered_taps           = pad["delivered_taps"].AsInt();
	counters.guest_read_state_samples = pad["guest_read_state_samples"].AsInt();
	counters.guest_read_samples       = pad["guest_read_samples"].AsInt();
	counters.tap_pending              = pad["tap_pending"].Truthy();
	return counters;
}

Json PadCountersJson(const PadCounters& counters)
{
	Json out = Json::MakeObject();
	out.Set("delivered_taps", counters.delivered_taps)
	    .Set("guest_read_state_samples", counters.guest_read_state_samples)
	    .Set("guest_read_samples", counters.guest_read_samples)
	    .Set("tap_pending", counters.tap_pending);
	return out;
}

PadSequenceResult DeliverPadSequence(RunnerHost* host, const std::string& endpoint, const Json& steps, std::optional<double> deadline,
                                     double inter_tap_settle_s, const std::function<void(const Json&)>& status_observer)
{
	PadSequenceRunner runner(host, endpoint, deadline, status_observer);
	return runner.Run(steps, inter_tap_settle_s);
}

bool AdvancePostInputWait(PostInputWaitState* state, int64_t present, double now, int64_t min_present_delta, double min_settle_s)
{
	if (state->interactive_start_present.has_value() && present < *state->interactive_start_present)
	{
		state->interactive_since.reset();
		state->interactive_start_present.reset();
	}
	if (!state->interactive_since.has_value() || !state->interactive_start_present.has_value())
	{
		state->interactive_since         = now;
		state->interactive_start_present = present;
	}
	const int64_t present_delta = std::max<int64_t>(0, present - *state->interactive_start_present);
	const double  elapsed_s     = std::max(0.0, now - *state->interactive_since);
	return present_delta >= min_present_delta && elapsed_s >= min_settle_s;
}

bool RecordInputPhaseObservation(PostInputWaitState* state, const std::string& phase)
{
	if (phase != "loading")
	{
		return false;
	}
	const bool newly_seen = !state->loading_seen;
	state->loading_seen   = true;
	state->interactive_since.reset();
	state->interactive_start_present.reset();
	return newly_seen;
}

std::string ClassifyExit(std::optional<int> exit_code, bool timed_out, bool coordinator_stop)
{
	if (!exit_code.has_value())
	{
		return "unknown";
	}
	const int code = *exit_code;
	if (coordinator_stop && (code == 0 || code == -15 || code == -9 || code == 143 || code == 137))
	{
		return timed_out ? "controlled_timeout" : "coordinator_stop";
	}
	if (code == 0)
	{
		return "guest_exit_ok";
	}
	if (code < 0 || code == 139 || code == 132 || code == 134 || code == 136 || code == 65 || code == 321)
	{
		return "host_crash";
	}
	return "exit_" + std::to_string(code);
}

Json InputDeliveryChecks(const Json& before, const Json& after, const Json& milestones, bool sequence_ok)
{
	const int64_t taps   = IntOr(after, "delivered_taps", 0) - IntOr(before, "delivered_taps", 0);
	const int64_t states = IntOr(after, "guest_read_state_samples", 0) - IntOr(before, "guest_read_state_samples", 0);
	const int64_t reads  = IntOr(after, "guest_read_samples", 0) - IntOr(before, "guest_read_samples", 0);
	Json          checks = Json::MakeObject();
	checks.Set("sequence_and_clear", sequence_ok)
	    .Set("tap_count", taps == IntOr(milestones, "min_pad_taps", 1))
	    .Set("read_state_samples", states >= IntOr(milestones, "min_guest_read_state_samples", 1))
	    .Set("read_samples", reads >= IntOr(milestones, "min_guest_read_samples", 0))
	    .Set("guest_consumed_input", states >= 0 && reads >= 0 && states + reads > 0);
	return checks;
}

std::vector<GateResult> EvaluateGates(const GateInputs& in)
{
	std::vector<GateResult> gates;
	gates.push_back({"agent_ready", in.agent_ready, in.agent_ready ? "agent_ready" : "agent_not_ready"});
	gates.push_back({"video_initialized", in.video_initialized, in.video_initialized ? "graphic_ready" : "graphic_not_ready"});
	const int64_t min_present = IntOr(in.milestones, "min_present_after_ready", 1);
	gates.push_back({"first_present", in.first_present.has_value() && *in.first_present >= min_present,
	                 "first_present=" + (in.first_present.has_value() ? std::to_string(*in.first_present) : std::string("None"))});
	const int64_t need_delta = IntOr(in.milestones, "present_delta_stable", 15);
	gates.push_back({"present_advancing", in.present_delta >= need_delta,
	                 "present_delta=" + std::to_string(in.present_delta) + " need>=" + std::to_string(need_delta)});
	const Json delivery = InputDeliveryChecks(in.input_before, in.input_after, in.milestones, in.input_sequence_ok);
	gates.push_back({"input_sequence_complete", in.input_sequence_ok, in.input_sequence_ok ? "sequence_and_clear_ok" : "tap_or_clear_failed"});
	const bool delivered = std::all_of(delivery.Members().begin(), delivery.Members().end(),
	                                   [](const auto& entry) { return entry.second.Truthy(); });
	gates.push_back({"input_delivered", delivered, delivery.Dump()});
	gates.push_back({"native_capture", !in.capture_path.empty(), in.capture_path.empty() ? "no_capture" : in.capture_path});
	const bool compare_pass = in.compare["pass"].Truthy();
	if (in.baseline_missing && in.create_baseline)
	{
		gates.push_back({"visual_compare", compare_pass, compare_pass ? "material_baseline_candidate" : GateJson(in.compare)});
	} else if (in.baseline_missing && in.visual_require_baseline)
	{
		gates.push_back({"visual_compare", false, "baseline_missing"});
	} else
	{
		gates.push_back({"visual_compare", compare_pass, GateJson(in.compare)});
	}
	gates.push_back({"no_host_crash", in.shutdown != "host_crash", in.shutdown});
	gates.push_back({"expected_exit", in.shutdown == "guest_exit_ok" || in.shutdown == "coordinator_stop", in.shutdown});
	gates.push_back({"within_deadline", !in.timed_out && in.shutdown != "controlled_timeout", in.shutdown});
	gates.push_back({"no_runtime_failure", in.first_error.empty(), in.first_error.empty() ? "none" : in.first_error});
	gates.push_back({"final_error_observed", in.final_error_observed, in.final_error_observed ? "observed" : "unknown"});

	Json missing = Json::MakeObject();
	missing.Set("error", "checkpoint_missing");
	const Json& checkpoint = in.checkpoint.Truthy() ? in.checkpoint : missing;
	const Json& checks     = in.checkpoint["checks"];
	gates.push_back({"scene_checkpoint", AllTrue(checks, {"gameplay_checkpoint", "before_scene", "after_scene"}),
	                 TruncateCodePoints(checkpoint.Dump(), 300)});
	gates.push_back({"action_response", AllTrue(checks, {"capture_input_order", "action_delivered", "action_settled", "expected_response"}),
	                 checks.IsObject() ? checks.Dump() : "{}"});
	return gates;
}

Json VisualFloorFromMetrics(const Json& metrics)
{
	const Json&   world         = metrics["world"];
	const double  white_ratio   = world["white_ratio"].AsNumber();
	const double  entropy       = world["entropy"].AsNumber();
	const int64_t unique_colors = world["unique_quantized_colors"].AsInt();
	Json          checks        = Json::MakeObject();
	checks.Set("absolute_world_gate", white_ratio < 0.35 && entropy >= 2.5 && unique_colors >= 80).Set("not_stripey", !world["stripey"].Truthy());
	Json floor_world = Json::MakeObject();
	floor_world.Set("white_ratio", white_ratio).Set("entropy", entropy).Set("unique_quantized_colors", unique_colors);
	Json out = Json::MakeObject();
	out.Set("pass", checks["absolute_world_gate"].AsBool() && checks["not_stripey"].AsBool())
	    .Set("assessment", "material_health_only")
	    .Set("checks", std::move(checks))
	    .Set("world", std::move(floor_world));
	return out;
}

Json ComparePlayableVisualMetrics(const Json& metrics, const Json& baseline)
{
	const Json raw      = CompareMetrics(metrics, baseline);
	Json       material = Json::MakeObject();
	bool       pass     = true;
	for (const char* key: kMaterialVisualChecks)
	{
		const bool ok = raw["checks"][key].Truthy();
		material.Set(key, ok);
		pass = pass && ok;
	}
	Json out = Json::MakeObject();
	out.Set("pass", pass)
	    .Set("assessment", "material_health_only")
	    .Set("checks", std::move(material))
	    .Set("delta", raw["delta"].Truthy() ? raw["delta"] : Json::MakeObject())
	    .Set("raw_checks", raw["checks"].Truthy() ? raw["checks"] : Json::MakeObject());
	return out;
}

std::string FirstActionableError(const Json& events, const Json& last_error)
{
	if (last_error.Truthy())
	{
		const std::string code = Strip(ValueText(FirstTruthy(FirstTruthy(last_error["code"], last_error["message"]), "")));
		const std::string lowered = Lower(code);
		if (!code.empty() && lowered != "none" && lowered != "null")
		{
			return TruncateCodePoints(code, 160);
		}
		const std::string kind = Lower(ValueText(FirstTruthy(last_error["kind"], "")));
		if (kind == "error" || kind == "fatal")
		{
			return TruncateCodePoints(ValueText(FirstTruthy(last_error["message"], last_error["kind"])), 160);
		}
	}
	for (const Json& event: events.Items())
	{
		if (!event.IsObject())
		{
			continue;
		}
		const std::string kind = Lower(ValueText(FirstTruthy(event["kind"], "")));
		const std::string code = Strip(ValueText(FirstTruthy(event["code"], "")));
		if (kind == "error" || kind == "fatal")
		{
			return TruncateCodePoints(code.empty() ? ValueText(FirstTruthy(event["message"], Json(kind))) : code, 160);
		}
	}
	return {};
}

std::string FirstActionableFromLog(const std::string& log_path)
{
	std::string text;
	std::string error;
	if (!ReadTextFile(log_path, 256u * 1024u * 1024u, &text, &error))
	{
		return {};
	}
	const std::vector<std::string> lines = SplitLines(text);
	for (size_t index = 0; index < lines.size(); ++index)
	{
		const std::string line = Strip(lines[index]);
		if (line == "--- Error ---" || line == "--- Fatal Error ---")
		{
			for (size_t next = index + 1; next < lines.size(); ++next)
			{
				const std::string diagnostic = Strip(lines[next]);
				if (!diagnostic.empty() && diagnostic.rfind("---", 0) != 0)
				{
					return TruncateCodePoints(diagnostic, 160);
				}
			}
		}
		if (line.empty() || line.rfind("---", 0) == 0 || line[0] == '[' || line.find("/home/") != std::string::npos ||
		    line.find("Documents/") != std::string::npos)
		{
			continue;
		}
		if (line.size() >= 3 && line.rfind("===", 0) == 0 && line.compare(line.size() - 3, 3, "===") == 0)
		{
			return TruncateCodePoints(Strip(Strip(line, "= ")), 160);
		}
		const std::string lowered = Lower(line);
		if (lowered.find("assert") != std::string::npos || lowered.find("fatal") != std::string::npos ||
		    lowered.find("unpatched") != std::string::npos)
		{
			return TruncateCodePoints(line, 160);
		}
	}
	return {};
}

std::string SanitizeText(const std::string& text, const std::string& guest_root)
{
	std::string out = text;
	const auto  replace_all = [&out](const std::string& needle, const std::string& replacement) {
        if (needle.empty())
        {
            return;
        }
        for (size_t at = out.find(needle); at != std::string::npos; at = out.find(needle, at + replacement.size()))
        {
            out.replace(at, needle.size(), replacement);
        }
	};
	replace_all(guest_root, "$KYTY_GUEST_ROOT");
	for (const char* marker: {"/home/", "Documents/PS5", "PPSA", "CUSA"})
	{
		replace_all(marker, "<redacted>");
	}
	return out;
}

Json SanitizeJson(const Json& value, const std::string& guest_root)
{
	if (value.IsString())
	{
		return SanitizeText(value.AsString(), guest_root);
	}
	if (value.IsArray())
	{
		Json out = Json::MakeArray();
		for (const Json& item: value.Items())
		{
			out.Push(SanitizeJson(item, guest_root));
		}
		return out;
	}
	if (value.IsObject())
	{
		Json out = Json::MakeObject();
		for (const auto& [key, item]: value.Members())
		{
			out.Set(key, SanitizeJson(item, guest_root));
		}
		return out;
	}
	return value;
}

} // namespace Kyty::Agent::Runner
