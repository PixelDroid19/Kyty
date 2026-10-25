#include "Kyty/Agent/Runner/CaptureMetrics.h"
#include "Kyty/Agent/Runner/JsonValue.h"
#include "Kyty/Agent/Runner/Playable.h"
#include "Kyty/Agent/Runner/RunnerHost.h"
#include "Kyty/Agent/Runner/SceneCheckpoint.h"
#include "Kyty/Agent/Runner/Sha256.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Host/Png.h"

#include "ScopedTestDirectory.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif

UT_BEGIN(AgentRunner);

using namespace Kyty::Agent::Runner; // NOLINT(google-build-using-namespace)
namespace fs = std::filesystem;

namespace {

std::string ReadFile(const fs::path& path)
{
	std::ifstream      in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

void WriteFile(const fs::path& path, const std::string& text)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}

Json ParseOrNull(const std::string& text)
{
	Json        value;
	std::string error;
	return ParseJson(text, &value, &error) ? value : Json();
}

Json Object(const std::string& text)
{
	return ParseOrNull(text);
}

bool AllTrue(const Json& checks)
{
	return std::all_of(checks.Members().begin(), checks.Members().end(), [](const auto& entry) { return entry.second.AsBool(); });
}

AgentReply Ok(Json result = Json::MakeObject())
{
	AgentReply reply;
	reply.code = 0;
	reply.body = Json::MakeObject().Set("ok", true).Set("result", std::move(result));
	return reply;
}

AgentReply Failed(int code)
{
	AgentReply reply;
	reply.code = code;
	reply.body = Json::MakeObject().Set("ok", false);
	return reply;
}

// Synthetic native PNGs, never gameplay evidence for an actual workload: the
// before, after and wrong frames differ only by where a marker sits in the
// response band; the menu frame is the inverted gradient.
class CheckpointFixture
{
public:
	explicit CheckpointFixture(const fs::path& root): m_root(root)
	{
		const std::pair<const char*, int> roles[] = {{"before", 8}, {"after", 32}, {"wrong", 48}, {"menu", -1}};
		for (const auto& [role, marker_x]: roles)
		{
			std::vector<uint8_t> pixels(64u * 48u * 4u);
			for (int y = 0; y < 48; ++y)
			{
				for (int x = 0; x < 64; ++x)
				{
					int rgb[3] = {(x * 7 + y * 3) % 220, (x * 3 + y * 11) % 220, (x * 11 + y * 7) % 220};
					if (marker_x < 0)
					{
						for (int& channel: rgb)
						{
							channel = 220 - channel;
						}
					} else if (marker_x <= x && x < marker_x + 8 && 20 <= y && y < 28)
					{
						rgb[0] = 240;
						rgb[1] = 180;
						rgb[2] = 10;
					}
					uint8_t* pixel = &pixels[(static_cast<size_t>(y) * 64u + static_cast<size_t>(x)) * 4u];
					pixel[0]       = static_cast<uint8_t>(rgb[0]);
					pixel[1]       = static_cast<uint8_t>(rgb[1]);
					pixel[2]       = static_cast<uint8_t>(rgb[2]);
					pixel[3]       = 255;
				}
			}
			const fs::path path = root / (std::string(role) + ".png");
			EXPECT_TRUE(Kyty::Emulator::Host::WriteRgba8Png(path.string().c_str(), pixels.data(), 64, 48, 64));
		}
		contract = Object(R"({
		  "schema": "kyty_scene_checkpoint_v1", "fixture": "synthetic-room", "kind": "gameplay",
		  "scene_regions": [{"box": [0, 0, 1, 0.25], "max_mean_error": 0}],
		  "response": {"box": [0, 0.375, 1, 0.625], "max_mean_error": 0, "pixel_delta": 20, "min_changed_ratio": 0.05},
		  "action": {"pad_sequence": [{"tool": "pad_down", "button": "right", "hold_s": 0.2}, {"tool": "pad_up", "button": "right"}],
		             "milestones": {"min_pad_taps": 0, "min_guest_read_state_samples": 0, "min_guest_read_samples": 1},
		             "min_present_delta": 10, "min_settle_s": 0}
		})");
		contract.Set("before", Reference("before")).Set("after", Reference("after"));
		Write();
	}

	[[nodiscard]] fs::path Image(const std::string& role) const { return m_root / (role + ".png"); }
	[[nodiscard]] fs::path Path() const { return m_root / "checkpoint.json"; }

	[[nodiscard]] std::string Sha(const std::string& role) const
	{
		const std::string data = ReadFile(Image(role));
		return Sha256Hex(data.data(), data.size());
	}

	[[nodiscard]] Json Reference(const std::string& role) const
	{
		return Json::MakeObject().Set("image", role + ".png").Set("sha256", Sha(role));
	}

	void Write() const { WriteFile(Path(), contract.Dump()); }

	[[nodiscard]] Json Record(const std::string& role, int64_t present) const
	{
		return Json::MakeObject()
		    .Set("path", Image(role).string())
		    .Set("file", role + ".png")
		    .Set("sha256", Sha(role))
		    .Set("present", present)
		    .Set("source", "kyty_agent.capture");
	}

	[[nodiscard]] bool Load(SceneCheckpoint* checkpoint) const
	{
		std::string error;
		return checkpoint->Load(Path().string(), &error);
	}

	[[nodiscard]] Json Action(bool consumed) const
	{
		const Json before = Object(R"({"delivered_taps": 0, "guest_read_state_samples": 0, "guest_read_samples": 0})");
		Json       after  = before;
		after.Set("guest_read_samples", consumed ? 1 : 0);
		return Json::MakeObject()
		    .Set("start_present", 10)
		    .Set("end_present", 20)
		    .Set("settled", true)
		    .Set("input_before", before)
		    .Set("input_after", after)
		    .Set("delivery_checks", InputDeliveryChecks(before, after, contract["action"]["milestones"], true));
	}

	[[nodiscard]] Json Evaluate(const std::string& after = "after", bool consumed = true) const
	{
		SceneCheckpoint checkpoint;
		EXPECT_TRUE(Load(&checkpoint));
		return checkpoint.Evaluate(Record("before", 10), Record(after, 30), Action(consumed));
	}

	Json contract;

private:
	fs::path m_root;
};

bool LoadsWithAction(CheckpointFixture* fixture, const std::string& pad_sequence, int64_t min_pad_taps)
{
	Json action = fixture->contract["action"];
	Json milestones = action["milestones"];
	milestones.Set("min_pad_taps", min_pad_taps);
	action.Set("pad_sequence", Object(pad_sequence)).Set("milestones", milestones);
	fixture->contract.Set("action", action);
	fixture->Write();
	SceneCheckpoint checkpoint;
	return fixture->Load(&checkpoint);
}

// Deterministic host whose agent replies come from a test script.
class ScriptedHost: public RunnerHost
{
public:
	using Agent = std::function<AgentReply(const std::string& tool, const Json& args, double timeout_s)>;

	explicit ScriptedHost(Agent agent): m_agent(std::move(agent)) {}

	double Now() override { return now; }
	void   Sleep(double seconds) override { now += seconds; }
	bool   Launch(const LaunchSpec& /*spec*/, std::string* /*error*/) override { return false; }
	[[nodiscard]] int64_t ChildPid() const override { return 0; }
	std::optional<int>    PollChild() override { return std::nullopt; }
	void                  TerminateChild(std::vector<std::string>* /*notes*/) override {}

	AgentReply CallAgent(const std::string& /*endpoint*/, const std::string& tool, const Json& args, double timeout_s) override
	{
		calls.push_back({tool, args, timeout_s});
		return m_agent(tool, args, timeout_s);
	}

	[[nodiscard]] std::vector<std::string> Tools() const
	{
		std::vector<std::string> tools;
		for (const auto& call: calls)
		{
			tools.push_back(call.tool);
		}
		return tools;
	}

	struct Call
	{
		std::string tool;
		Json        args;
		double      timeout_s = 0.0;
	};

	double            now = 0.0;
	std::vector<Call> calls;

private:
	Agent m_agent;
};

PadSequenceResult Deliver(ScriptedHost* host, const std::string& steps, std::optional<double> deadline = std::nullopt,
                          double inter_tap_settle_s = 0.0, const std::function<void(const Json&)>& observer = {})
{
	return DeliverPadSequence(host, "test.sock", Object(steps), deadline, inter_tap_settle_s, observer ? observer : [](const Json&) {});
}

std::vector<std::string> EventNames(const Json& events)
{
	std::vector<std::string> names;
	for (const Json& event: events.Items())
	{
		names.push_back(event["event"].AsString());
	}
	return names;
}

// A fake guest behind the agent socket for one whole strict session. Status
// keeps reporting a low-FPS loading phase throughout: it is not a scene oracle.
class FakeGuest: public RunnerHost
{
public:
	explicit FakeGuest(const CheckpointFixture& fixture): m_fixture(fixture) {}

	double Now() override { return now; }
	void   Sleep(double seconds) override { now += seconds; }

	bool Launch(const LaunchSpec& spec, std::string* /*error*/) override
	{
		launched = spec;
		++launches;
		return true;
	}
	[[nodiscard]] int64_t ChildPid() const override { return 12345; }
	std::optional<int>    PollChild() override { return exit_code; }
	void                  TerminateChild(std::vector<std::string>* notes) override
	{
		exit_code = stop_return;
		notes->push_back("killed_sigterm");
	}

	AgentReply CallAgent(const std::string& /*endpoint*/, const std::string& tool, const Json& /*args*/, double timeout_s) override
	{
		EXPECT_GT(timeout_s, 0.0);
		if (exit_code.has_value())
		{
			return Failed(125);
		}
		if (tool == "status")
		{
			present += 10;
			if (held && consume_hold)
			{
				++reads;
			}
			return Ok(Json::MakeObject()
			              .Set("present", present)
			              .Set("frame", present)
			              .Set("graphic_ready", true)
			              .Set("phase", "loading")
			              .Set("fps", 1)
			              .Set("pad", Json::MakeObject()
			                              .Set("delivered_taps", taps)
			                              .Set("guest_read_state_samples", states)
			                              .Set("guest_read_samples", reads)
			                              .Set("tap_pending", false)));
		}
		if (tool == "pad_tap")
		{
			++taps;
			states += 2;
		} else if (tool == "pad_down")
		{
			held = true;
		} else if (tool == "pad_up" || tool == "pad_clear")
		{
			held = false;
			if (tool == "pad_clear")
			{
				++clears;
				if (error_stage == "post_input")
				{
					error = Object(R"({"kind": "fatal", "code": "late_input_failure"})");
				}
				if (fail_action_clear && clears == 2)
				{
					return Failed(1);
				}
			}
		} else if (tool == "capture")
		{
			return Capture();
		} else if (tool == "last_error" || tool == "events")
		{
			return Errors(tool);
		}
		return Ok();
	}

	double             now     = 0.0;
	int64_t            present = 0;
	int64_t            taps = 0, states = 0, reads = 0, captures = 0, clears = 0;
	bool               held               = false;
	bool               consume_hold       = true;
	int                stop_return        = 0;
	std::optional<int> crash_on_capture;
	bool               timeout_on_capture = false;
	std::string        error_stage;
	bool               error_on_final_observation = false;
	bool               log_error_on_capture       = false;
	Json               error;
	bool               events_only               = false;
	bool               unavailable_after_capture = false;
	bool               fail_action_clear         = false;
	bool               natural_exit              = false;
	std::string        before_role               = "before";
	std::string        after_role                = "after";
	std::optional<int> exit_code;
	LaunchSpec         launched;
	int                launches = 0;

private:
	AgentReply Capture()
	{
		++captures;
		++present;
		const fs::path    path = fs::path(launched.env.at("KYTY_NATIVE_CAPTURE_DIR")) / ("native-" + std::to_string(captures) + ".png");
		const std::string role = captures == 1 ? before_role : after_role;
		WriteFile(path, ReadFile(m_fixture.Image(role)));
		if (captures == 2 && error_stage == "post_capture")
		{
			error = Object(R"({"kind": "fatal", "code": "late_capture_failure"})");
		}
		if (captures == 2 && log_error_on_capture)
		{
			WriteFile(launched.log_path, "--- Fatal Error ---\nlate log failure\n");
		}
		if (crash_on_capture.has_value())
		{
			exit_code = crash_on_capture;
		}
		if (timeout_on_capture)
		{
			now = 31;
		}
		return Ok(Json::MakeObject().Set("path", path.string()).Set("present", present).Set("frame", present).Set("image_format", "PNG"));
	}

	// After the response capture the second observation is the final one.
	AgentReply Errors(const std::string& tool)
	{
		if (captures == 2 && unavailable_after_capture)
		{
			return Failed(125);
		}
		if (tool == "last_error")
		{
			if (captures == 2 && ++m_final_observations == 2 && error_on_final_observation)
			{
				error = Object(R"({"kind": "fatal", "code": "after_score_failure"})");
			}
			return Ok(Json::MakeObject().Set("event", events_only ? Json() : error));
		}
		if (captures == 2 && natural_exit && m_final_observations == 2)
		{
			exit_code = 0;
		}
		Json events = Json::MakeArray().Push(error.Truthy() ? error : Object(R"({"kind": "info", "code": "first_present"})"));
		return Ok(Json::MakeObject().Set("events", events));
	}

	const CheckpointFixture& m_fixture;
	int                      m_final_observations = 0;
};

class SessionHarness
{
public:
	SessionHarness(): m_directory("agent-runner-session"), fixture(m_directory.Path()), guest(fixture)
	{
		const fs::path& root = m_directory.Path();
		WriteFile(root / "fake-fc", "not executable: the host is fake");
		WriteFile(root / "run_guest.lua", "-- fake guest script\n");
		baseline = root / "baseline.json";
		profile  = MergePlayableProfile(Object(R"({
		  "deadlines_s": {"total": 30, "input_window": 5},
		  "milestones": {"present_delta_stable": 10},
		  "input": {"menu_settle_s": 0, "inter_tap_settle_s": 0},
		  "post_input": {"require_loading_transition": true, "min_present_delta": 0, "min_settle_s": 0}
		})"));
	}

	int Run(bool checkpoint = true, bool create_baseline = true)
	{
		const fs::path& root = m_directory.Path();
		PlayableOptions options;
		options.repo_root             = root.string();
		options.guest_root            = (root / "guest").string();
		options.fc_script             = (root / "fake-fc").string();
		options.runtime_cwd           = root.string();
		options.guest_script          = (root / "run_guest.lua").string();
		options.scratch               = (root / "scratch").string();
		options.baseline_path         = baseline.string();
		options.scene_checkpoint_path = checkpoint ? fixture.Path().string() : std::string();
		options.create_baseline       = create_baseline;
		options.profile               = profile;
		options.base_env              = {{"PATH", "/usr/bin"}};
		const int rc                  = RunPlayableSession(&guest, options, &report);
		EXPECT_EQ(guest.launches, 1);
		const Json written = ParseOrNull(ReadFile(fs::path(report.run_dir) / "summary.json"));
		EXPECT_EQ(written["child_exit"], guest.exit_code.has_value() ? Json(*guest.exit_code) : Json());
		return rc;
	}

	[[nodiscard]] const Json& Summary() const { return report.summary; }
	[[nodiscard]] const Json& Checks() const { return report.summary["checkpoint"]["checks"]; }

	[[nodiscard]] bool GatePassed(const std::string& name) const
	{
		for (const auto& gate: report.gates)
		{
			if (gate.name == name)
			{
				return gate.passed;
			}
		}
		ADD_FAILURE() << "missing gate " << name;
		return false;
	}

	[[nodiscard]] bool HasNote(const std::string& note) const
	{
		const auto& notes = report.summary["notes"].Items();
		return std::any_of(notes.begin(), notes.end(), [&](const Json& value) { return value.AsString() == note; });
	}

	[[nodiscard]] std::string BaselineText() const { return fs::exists(baseline) ? ReadFile(baseline) : std::string(); }

	void RetainBaselineOf(const std::string& role)
	{
		Json        metrics;
		std::string error;
		ASSERT_TRUE(ScoreImage(fixture.Image(role).string(), &metrics, &error)) << error;
		WriteFile(baseline, Json::MakeObject().Set("world", metrics["world"]).Dump());
	}

private:
	ScopedTestDirectory m_directory;

public:
	CheckpointFixture fixture;
	FakeGuest         guest;
	fs::path          baseline;
	Json              profile;
	PlayableReport    report;
};

class AcceptanceGates
{
public:
	AcceptanceGates(): m_directory("agent-runner-gates"), fixture(m_directory.Path())
	{
		base.agent_ready             = true;
		base.video_initialized       = true;
		base.first_present           = 1;
		base.present_delta           = 300;
		base.input_before            = Object(R"({"delivered_taps": 0, "guest_read_state_samples": 0})");
		base.input_after             = Object(R"({"delivered_taps": 3, "guest_read_state_samples": 6})");
		base.capture_path            = "synthetic.png";
		base.compare                 = Object(R"({"pass": true})");
		base.milestones              = DefaultPlayableProfile()["milestones"];
		base.visual_require_baseline = true;
		base.checkpoint              = fixture.Evaluate();
		base.shutdown                = "guest_exit_ok";
	}

	[[nodiscard]] static bool Passed(const std::vector<GateResult>& gates, const std::string& name)
	{
		const auto it = std::find_if(gates.begin(), gates.end(), [&](const GateResult& gate) { return gate.name == name; });
		EXPECT_NE(it, gates.end()) << name;
		return it != gates.end() && it->passed;
	}

	[[nodiscard]] bool Passed(const GateInputs& inputs, const std::string& name) const { return Passed(EvaluateGates(inputs), name); }

private:
	ScopedTestDirectory m_directory;

public:
	CheckpointFixture fixture;
	GateInputs        base;
};

} // namespace

TEST(AgentRunnerSha256, MatchesPublishedVectors)
{
	EXPECT_EQ(Sha256Hex("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	EXPECT_EQ(Sha256Hex("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	EXPECT_EQ(Sha256Hex(two_blocks.data(), two_blocks.size()), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	Sha256 split;
	split.Update(two_blocks.data(), 7);
	split.Update(two_blocks.data() + 7, two_blocks.size() - 7);
	EXPECT_EQ(split.FinalHex(), Sha256Hex(two_blocks.data(), two_blocks.size()));
}

TEST(AgentRunnerJson, ParsesAndDumpsTheSameDocument)
{
	const Json parsed = ParseOrNull(R"({"b": [1, 2.5, "x\"y", true, null], "a": {"n": -3}})");
	ASSERT_TRUE(parsed.IsObject());
	EXPECT_TRUE(parsed["b"][0].IsInteger());
	EXPECT_EQ(parsed["b"][1].AsNumber(), 2.5);
	EXPECT_EQ(parsed["b"][2].AsString(), "x\"y");
	EXPECT_EQ(parsed["a"]["n"].AsInt(), -3);
	EXPECT_EQ(ParseOrNull(parsed.Dump()), parsed);
	EXPECT_EQ(ParseOrNull(parsed.Dump(2)), parsed);

	Json        out;
	std::string error;
	for (const char* invalid: {"", "{", "[1,]", "{\"a\":1} trailing", "nul", "\"unterminated"})
	{
		EXPECT_FALSE(ParseJson(invalid, &out, &error)) << invalid;
	}
}

TEST(AgentRunnerEnvironment, BuildsAMinimalStrictEnvironment)
{
	const Environment base = {{"PATH", "/usr/bin"},
	                          {"DISPLAY", ":0"},
	                          {"KYTY_STUB_MISSING", "1"},
	                          {"KYTY_AUTO_CROSS", "1"},
	                          {"UNRELATED_PARENT_VALUE", "private"}};
	ChildEnvironmentOptions options;
	options.guest_root        = "/guest";
	options.capture_directory = "/captures";
	options.agent_endpoint    = "/agent.sock";
	const Environment env     = BuildChildEnvironment(base, options);

	EXPECT_EQ(env.at("PATH"), "/usr/bin");
	EXPECT_EQ(env.at("DISPLAY"), ":0");
	EXPECT_EQ(env.at("KYTY_PRINTF_DIRECTION"), "Silent");
	EXPECT_EQ(env.count("UNRELATED_PARENT_VALUE"), 0u);
	EXPECT_TRUE(FindActiveEnvironmentKeys(env, StrictForbiddenEnvironmentKeys()).empty());
}

TEST(AgentRunnerEnvironment, PreservesOnlyExplicitOptionalValues)
{
	const Environment base = {{"KYTY_RENDER_RESOLUTION_MODE", "Native"},
	                          {"KYTY_RENDER_RESOLUTION_WIDTH", "1920"},
	                          {"KYTY_PRESENTATION_FILTER", ""},
	                          {"KYTY_NATIVE_CAPTURE_MAX_EDGE", "2048"},
	                          {"KYTY_SHADER_PROBE", "1"}};
	ChildEnvironmentOptions options;
	options.guest_root        = "/guest";
	options.capture_directory = "/captures";
	options.optional_values   = {"KYTY_RENDER_RESOLUTION_MODE", "KYTY_RENDER_RESOLUTION_WIDTH", "KYTY_RENDER_RESOLUTION_HEIGHT",
	                             "KYTY_PRESENTATION_FILTER"};
	options.default_values    = {{"KYTY_NATIVE_CAPTURE_MAX_EDGE", "1280"}, {"KYTY_EXTRA_DEFAULT", "7"}};
	const Environment env     = BuildChildEnvironment(base, options);

	EXPECT_EQ(env.at("KYTY_RENDER_RESOLUTION_MODE"), "Native");
	EXPECT_EQ(env.at("KYTY_RENDER_RESOLUTION_WIDTH"), "1920");
	EXPECT_EQ(env.count("KYTY_RENDER_RESOLUTION_HEIGHT"), 0u);
	EXPECT_EQ(env.count("KYTY_PRESENTATION_FILTER"), 0u);
	EXPECT_EQ(env.at("KYTY_NATIVE_CAPTURE_MAX_EDGE"), "2048");
	EXPECT_EQ(env.at("KYTY_EXTRA_DEFAULT"), "7");
	EXPECT_EQ(env.count("KYTY_SHADER_PROBE"), 0u);
}

TEST(AgentRunnerCheckpoint, AcceptsAxisStepsAndCountsOnlyTapsTowardTheMilestone)
{
	ScopedTestDirectory directory("agent-runner-checkpoint");
	CheckpointFixture   fixture(directory.Path());
	EXPECT_TRUE(LoadsWithAction(&fixture,
	                            R"([{"tool": "pad_axis", "axis": "left_x", "value": 0, "hold_s": 0},
	                                {"tool": "pad_axis", "axis": "left_y", "value": 128},
	                                {"tool": "pad_axis", "axis": "right_x", "value": 200},
	                                {"tool": "pad_axis", "axis": "right_y", "value": 255, "hold_s": 30},
	                                {"tool": "pad_tap", "button": "cross"}])",
	                            1));
	EXPECT_FALSE(LoadsWithAction(&fixture, R"([{"tool": "pad_axis", "axis": "left_y", "value": 200}])", 1));
}

TEST(AgentRunnerCheckpoint, RejectsInvalidAxisValueAndHoldFields)
{
	ScopedTestDirectory directory("agent-runner-checkpoint");
	CheckpointFixture   fixture(directory.Path());
	for (const char* step: {R"({"tool": "pad_axis", "axis": "trigger_left", "value": 128})",
	                        R"({"tool": "pad_axis", "axis": "left_x", "value": -1})",
	                        R"({"tool": "pad_axis", "axis": "left_y", "value": 256})",
	                        R"({"tool": "pad_axis", "axis": "right_x", "value": true})",
	                        R"({"tool": "pad_axis", "axis": "right_x", "value": 128.0})",
	                        R"({"tool": "pad_axis", "axis": "right_y", "value": 128, "hold_s": -0.1})",
	                        R"({"tool": "pad_axis", "axis": "left_x", "value": 128, "hold_s": 30.1})",
	                        R"({"tool": "pad_axis", "axis": "left_x", "value": 128, "hold_s": "0.5"})",
	                        R"({"tool": "pad_shell", "button": "cross"})"})
	{
		EXPECT_FALSE(LoadsWithAction(&fixture, std::string("[") + step + "]", 0)) << step;
	}
}

TEST(AgentRunnerCheckpoint, RequiresPinnedReferencesThatDistinguishTheResponse)
{
	ScopedTestDirectory directory("agent-runner-checkpoint");
	CheckpointFixture   fixture(directory.Path());
	SceneCheckpoint     checkpoint;
	EXPECT_TRUE(fixture.Load(&checkpoint));

	fixture.contract.Set("before", Json::MakeObject().Set("image", "before.png").Set("sha256", std::string(64, '0')));
	fixture.Write();
	EXPECT_FALSE(fixture.Load(&checkpoint));

	fixture.contract.Set("before", fixture.Reference("before")).Set("after", fixture.Reference("before"));
	fixture.Write();
	EXPECT_FALSE(fixture.Load(&checkpoint));
}

TEST(AgentRunnerCheckpoint, BindsCapturesToTheirHashAndInputOrder)
{
	ScopedTestDirectory directory("agent-runner-checkpoint");
	CheckpointFixture   fixture(directory.Path());
	SceneCheckpoint     checkpoint;
	ASSERT_TRUE(fixture.Load(&checkpoint));
	EXPECT_TRUE(AllTrue(fixture.Evaluate()["checks"]));

	Json action = fixture.Action(true);
	Json before = fixture.Record("before", 10);
	before.Set("sha256", std::string(64, '0'));
	EXPECT_FALSE(checkpoint.Evaluate(before, fixture.Record("after", 30), action)["checks"]["before_scene"].AsBool());

	action.Set("start_present", 5);
	EXPECT_FALSE(
	    checkpoint.Evaluate(fixture.Record("before", 10), fixture.Record("after", 30), action)["checks"]["capture_input_order"].AsBool());
}

TEST(AgentRunnerGates, SignalsAndRuntimeFailuresFailAnOtherwiseValidScene)
{
	AcceptanceGates gates;
	for (const auto& gate: EvaluateGates(gates.base))
	{
		EXPECT_TRUE(gate.passed) << gate.name << ": " << gate.detail;
	}
	for (int rc: {-6, -11, -4, -8, -7, -15, -9, 139, 132, 134, 136})
	{
		for (bool timed_out: {false, true})
		{
			GateInputs inputs = gates.base;
			inputs.shutdown   = ClassifyExit(rc, timed_out, false);
			inputs.timed_out  = timed_out;
			const auto result = EvaluateGates(inputs);
			EXPECT_FALSE(AcceptanceGates::Passed(result, "no_host_crash")) << rc;
			EXPECT_FALSE(AcceptanceGates::Passed(result, "expected_exit")) << rc;
		}
	}
	GateInputs inputs  = gates.base;
	inputs.first_error = "VK_ERROR_DEVICE_LOST after presentation";
	EXPECT_FALSE(gates.Passed(inputs, "no_runtime_failure"));
}

TEST(AgentRunnerGates, UnexpectedReturnsUnknownExitsAndTimeoutsFail)
{
	AcceptanceGates gates;
	for (const std::optional<int> rc: {std::optional<int>(1), std::optional<int>(7), std::optional<int>(143), std::optional<int>(137),
	                                   std::optional<int>()})
	{
		GateInputs inputs = gates.base;
		inputs.shutdown   = ClassifyExit(rc, false, false);
		EXPECT_FALSE(gates.Passed(inputs, "expected_exit"));
	}
	EXPECT_EQ(ClassifyExit(-6, false, false), "host_crash");
	EXPECT_EQ(ClassifyExit(0, false, true), "coordinator_stop");
	EXPECT_EQ(ClassifyExit(-15, true, true), "controlled_timeout");
	for (int rc: {-11, -4, -8, 7})
	{
		EXPECT_NE(ClassifyExit(rc, true, true), "controlled_timeout") << rc;
	}

	GateInputs inputs = gates.base;
	inputs.shutdown   = "controlled_timeout";
	EXPECT_FALSE(gates.Passed(inputs, "within_deadline"));
	inputs           = gates.base;
	inputs.timed_out = true;
	EXPECT_FALSE(gates.Passed(inputs, "within_deadline"));
	inputs                      = gates.base;
	inputs.final_error_observed = false;
	EXPECT_FALSE(gates.Passed(inputs, "final_error_observed"));
}

TEST(AgentRunnerGates, MaterialHealthAndAcceptedFlagsCannotCertifyGameplay)
{
	AcceptanceGates gates;
	for (const Json& checkpoint: {Json(), Object(R"({"accepted": true, "pass": true})")})
	{
		GateInputs inputs       = gates.base;
		inputs.checkpoint       = checkpoint;
		inputs.baseline_missing = true;
		inputs.create_baseline  = true;
		const auto result       = EvaluateGates(inputs);
		EXPECT_TRUE(AcceptanceGates::Passed(result, "visual_compare"));
		EXPECT_FALSE(AcceptanceGates::Passed(result, "scene_checkpoint"));
		EXPECT_FALSE(AcceptanceGates::Passed(result, "action_response"));
	}
	gates.fixture.contract.Set("kind", "menu");
	gates.fixture.Write();
	GateInputs inputs = gates.base;
	inputs.checkpoint = gates.fixture.Evaluate();
	EXPECT_FALSE(gates.Passed(inputs, "scene_checkpoint"));
}

TEST(AgentRunnerGates, OnlyTheExpectedResponseToConsumedInputPasses)
{
	AcceptanceGates gates;
	for (const char* role: {"before", "wrong", "menu"})
	{
		GateInputs inputs = gates.base;
		inputs.checkpoint = gates.fixture.Evaluate(role);
		EXPECT_FALSE(gates.Passed(inputs, "action_response")) << role;
	}
	GateInputs inputs = gates.base;
	inputs.checkpoint = gates.fixture.Evaluate("after", false);
	EXPECT_FALSE(gates.Passed(inputs, "action_response"));
}

TEST(AgentRunnerGates, InputGatesRejectPartialSequencesAndFailedClears)
{
	AcceptanceGates gates;
	GateInputs      inputs = gates.base;
	inputs.input_after     = Object(R"({"delivered_taps": 2, "guest_read_state_samples": 100})");
	EXPECT_FALSE(gates.Passed(inputs, "input_delivered"));

	inputs                   = gates.base;
	inputs.input_sequence_ok = false;
	const auto result        = EvaluateGates(inputs);
	EXPECT_FALSE(AcceptanceGates::Passed(result, "input_sequence_complete"));
	EXPECT_FALSE(AcceptanceGates::Passed(result, "input_delivered"));

	const Json milestones = Object(R"({"min_pad_taps": 0, "min_guest_read_state_samples": 0, "min_guest_read_samples": 1})");
	const Json empty      = Json::MakeObject();
	EXPECT_TRUE(AllTrue(InputDeliveryChecks(empty, Object(R"({"guest_read_samples": 3})"), milestones, true)));
	EXPECT_FALSE(AllTrue(InputDeliveryChecks(empty, empty, milestones, true)));
	EXPECT_FALSE(AllTrue(InputDeliveryChecks(empty, Object(R"({"guest_read_samples": 3, "delivered_taps": 1})"), milestones, true)));
	EXPECT_FALSE(
	    AllTrue(InputDeliveryChecks(Object(R"({"guest_read_samples": 4})"), Object(R"({"guest_read_samples": 3})"), milestones, true)));
}

TEST(AgentRunnerErrors, FindsActionableEventsAndFatalLogDiagnostics)
{
	EXPECT_EQ(FirstActionableError(Object(R"([{"kind": "fatal", "message": "device lost"}])"), Json::MakeObject()), "device lost");
	EXPECT_EQ(FirstActionableError(Object(R"([{"kind": "info", "code": "first_present"}])"), Json::MakeObject()), "");

	ScopedTestDirectory directory("agent-runner-log");
	const fs::path      log = directory.Path() / "child.log";
	WriteFile(log, "--- Fatal Error ---\nNot implemented (shader format != 4)\n in /tmp/source.cpp:1\n");
	EXPECT_EQ(FirstActionableFromLog(log.string()), "Not implemented (shader format != 4)");
}

TEST(AgentRunnerErrors, SanitizedReportsHideTheGuestRootAndHome)
{
	const Json report = Json::MakeObject()
	                        .Set("notes", Json::MakeArray().Push("/home/user/secret/game"))
	                        .Set("nested", Json::MakeObject().Set("path", "/home/user/secret/game/eboot.bin"));
	const std::string text = SanitizeJson(report, "/home/user/secret/game").Dump();
	EXPECT_EQ(text.find("/home/user/secret/game"), std::string::npos);
	EXPECT_EQ(text.find("/home/"), std::string::npos);
}

TEST(AgentRunnerPad, EachHoldRequiresItsOwnConsumedGuestSample)
{
	int          downs = 0;
	int64_t      reads = 0;
	ScriptedHost host([&](const std::string& tool, const Json&, double timeout_s) {
		EXPECT_GT(timeout_s, 0.0);
		downs += tool == "pad_down" ? 1 : 0;
		if (tool != "status")
		{
			return Ok();
		}
		reads += downs == 1 ? 1 : 0;
		return Ok(Json::MakeObject().Set("pad", Json::MakeObject().Set("guest_read_samples", reads)));
	});
	const auto result = Deliver(&host, R"([{"tool": "pad_down", "button": "right", "hold_s": 0.1},
	                                       {"tool": "pad_up", "button": "right"},
	                                       {"tool": "pad_down", "button": "left", "hold_s": 0.1}])");
	EXPECT_FALSE(result.ok);
	std::vector<bool> holds;
	for (const Json& event: result.events.Items())
	{
		if (event["event"].AsString() == "hold_observed")
		{
			holds.push_back(event["ok"].AsBool());
		}
	}
	EXPECT_EQ(holds, (std::vector<bool> {true, false}));
	EXPECT_EQ(EventNames(result.events).back(), "pad_clear");
}

TEST(AgentRunnerPad, DeliversSpacedTapsThenAlwaysClears)
{
	std::vector<double> tap_times;
	ScriptedHost*       self = nullptr;
	ScriptedHost        host([&](const std::string& tool, const Json&, double) {
        if (tool == "pad_tap")
        {
            tap_times.push_back(self->now);
        }
        return tool == "status" ? Ok(Object(R"({"pad": {"tap_pending": false}})")) : Ok();
	});
	self = &host;
	const char* taps = R"([{"tool": "pad_tap", "button": "cross"}, {"tool": "pad_tap", "button": "cross"},
	                       {"tool": "pad_tap", "button": "cross"}])";
	const auto  result = Deliver(&host, taps, std::nullopt, 1.0);
	EXPECT_TRUE(result.ok);
	EXPECT_EQ(tap_times, (std::vector<double> {0.0, 1.0, 2.0}));
	std::vector<std::string> mutating;
	for (const auto& tool: host.Tools())
	{
		if (tool.rfind("pad_", 0) == 0)
		{
			mutating.push_back(tool);
		}
	}
	EXPECT_EQ(mutating, (std::vector<std::string> {"pad_tap", "pad_tap", "pad_tap", "pad_clear"}));
	EXPECT_EQ(EventNames(result.events).back(), "pad_clear");
}

TEST(AgentRunnerPad, ObservesLoadingDuringInterTapSettle)
{
	ScriptedHost* self = nullptr;
	ScriptedHost  host([&](const std::string& tool, const Json&, double) {
        if (tool != "status")
        {
            return Ok();
        }
        return Ok(Json::MakeObject()
                      .Set("phase", self->now >= 0.25 ? "loading" : "interactive")
                      .Set("present", static_cast<int64_t>(self->now * 100))
                      .Set("pad", Json::MakeObject().Set("tap_pending", false)));
	});
	self = &host;
	std::vector<std::string> phases;
	PostInputWaitState       wait;
	const auto               result = Deliver(&host, R"([{"tool": "pad_tap", "button": "cross"}, {"tool": "pad_tap", "button": "cross"}])",
	                                          std::nullopt, 0.5, [&](const Json& status) {
                                const std::string phase = status["phase"].IsString() ? status["phase"].AsString() : "";
                                phases.push_back(phase);
                                (void)RecordInputPhaseObservation(&wait, phase);
                            });
	EXPECT_TRUE(result.ok);
	EXPECT_NE(std::find(phases.begin(), phases.end(), "loading"), phases.end());
	EXPECT_TRUE(wait.loading_seen);
	EXPECT_FALSE(wait.interactive_since.has_value());
	EXPECT_FALSE(wait.interactive_start_present.has_value());
	EXPECT_FALSE(AdvancePostInputWait(&wait, 1000, host.now, 240, 15.0));
}

TEST(AgentRunnerPad, ClearsWhenATapFailsOrTheDeadlinePasses)
{
	ScriptedHost failing([](const std::string& tool, const Json&, double) { return tool == "pad_tap" ? Failed(1) : Ok(); });
	EXPECT_FALSE(Deliver(&failing, R"([{"tool": "pad_tap", "button": "cross"}])").ok);
	EXPECT_EQ(failing.Tools(), (std::vector<std::string> {"pad_tap", "pad_clear"}));

	ScriptedHost* self = nullptr;
	ScriptedHost  pending([&](const std::string& tool, const Json&, double timeout_s) {
        if (tool != "status")
        {
            return Ok();
        }
        self->now += timeout_s;
        return Ok(Object(R"({"pad": {"tap_pending": true}})"));
	});
	self = &pending;
	EXPECT_FALSE(Deliver(&pending, R"([{"tool": "pad_tap", "button": "cross"}])", 1.0).ok);
	EXPECT_EQ(pending.Tools(), (std::vector<std::string> {"pad_tap", "status", "pad_clear"}));
	EXPECT_LE(pending.calls[1].timeout_s, 1.0);
	EXPECT_GE(pending.calls.back().timeout_s, 0.1);
}

TEST(AgentRunnerPad, AxisStepsSendExactArgumentsAndRequireAConsumedHold)
{
	bool         axis_started = false;
	int64_t      reads        = 0;
	ScriptedHost host([&](const std::string& tool, const Json&, double timeout_s) {
		EXPECT_GT(timeout_s, 0.0);
		axis_started = axis_started || tool == "pad_axis";
		if (tool != "status")
		{
			return Ok();
		}
		reads += axis_started ? 1 : 0;
		return Ok(Json::MakeObject().Set("pad", Json::MakeObject().Set("delivered_taps", 0).Set("guest_read_samples", reads)));
	});
	const auto result = Deliver(&host, R"([{"tool": "pad_axis", "axis": "left_y", "value": 210, "hold_s": 0.1}])");
	EXPECT_TRUE(result.ok);
	const auto axis = std::find_if(host.calls.begin(), host.calls.end(), [](const auto& call) { return call.tool == "pad_axis"; });
	ASSERT_NE(axis, host.calls.end());
	EXPECT_EQ(axis->args, Object(R"({"axis": "left_y", "value": 210})"));
	EXPECT_EQ(EventNames(result.events), (std::vector<std::string> {"pad_axis", "hold_observed", "pad_clear"}));
	EXPECT_TRUE(result.events[1]["ok"].AsBool());
	const auto tools = host.Tools();
	EXPECT_EQ(std::find(tools.begin(), tools.end(), "pad_tap"), tools.end());
}

TEST(AgentRunnerPad, InvalidOrFailedAxisStepsStillClearInput)
{
	ScriptedHost invalid([](const std::string&, const Json&, double) { return Ok(); });
	auto         result = Deliver(&invalid, R"([{"tool": "pad_axis", "axis": "left_x", "value": 256}])");
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(invalid.Tools(), (std::vector<std::string> {"pad_clear"}));
	EXPECT_EQ(result.events[0]["error"].AsString(), "invalid_axis");
	EXPECT_EQ(EventNames(result.events).back(), "pad_clear");

	ScriptedHost failing([](const std::string& tool, const Json&, double) { return tool == "pad_axis" ? Failed(1) : Ok(); });
	result = Deliver(&failing, R"([{"tool": "pad_axis", "axis": "right_x", "value": 255}])");
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(failing.Tools(), (std::vector<std::string> {"pad_axis", "pad_clear"}));
	EXPECT_TRUE(result.events[result.events.Size() - 1]["ok"].AsBool());

	ScriptedHost unread([](const std::string& tool, const Json&, double) {
		return tool == "status" ? Ok(Object(R"({"pad": {"guest_read_samples": 0}})")) : Ok();
	});
	EXPECT_FALSE(Deliver(&unread, R"([{"tool": "pad_axis", "axis": "right_y", "value": 100, "hold_s": 5}])", 1.0).ok);
	const auto tools = unread.Tools();
	EXPECT_NE(std::find(tools.begin(), tools.end(), "pad_axis"), tools.end());
	EXPECT_EQ(tools.back(), "pad_clear");
	EXPECT_LE(unread.calls.back().timeout_s, 1.0);
}

TEST(AgentRunnerPostInput, WaitsForPresentsAndSettleTimeOnly)
{
	PostInputWaitState settle;
	EXPECT_FALSE(AdvancePostInputWait(&settle, 100, 0.0, 120, 5.0));
	EXPECT_FALSE(AdvancePostInputWait(&settle, 219, 5.0, 120, 5.0));
	EXPECT_TRUE(AdvancePostInputWait(&settle, 220, 5.0, 120, 5.0));

	PostInputWaitState reset;
	reset.loading_seen = true;
	EXPECT_FALSE(AdvancePostInputWait(&reset, 100, 1.0, 10, 2.0));
	EXPECT_FALSE(AdvancePostInputWait(&reset, 109, 2.9, 10, 2.0));
	EXPECT_TRUE(AdvancePostInputWait(&reset, 110, 3.0, 10, 2.0));
	// A present counter reset restarts the window.
	EXPECT_FALSE(AdvancePostInputWait(&reset, 1, 4.0, 10, 2.0));
	EXPECT_TRUE(AdvancePostInputWait(&reset, 11, 6.0, 10, 2.0));
}

TEST(AgentRunnerSession, SceneActionAndCoordinatorStopPass)
{
	SessionHarness session;
	EXPECT_EQ(session.Run(), 0) << session.Summary().Dump(2);
	EXPECT_EQ(session.Summary()["shutdown"].AsString(), "coordinator_stop");
	EXPECT_TRUE(session.Summary()["coordinator_stop_requested"].AsBool());
	EXPECT_EQ(session.guest.captures, 2);
	EXPECT_EQ(session.guest.clears, 2);
	EXPECT_TRUE(AllTrue(session.Checks()));
	EXPECT_TRUE(fs::is_regular_file(session.baseline));
	EXPECT_EQ(session.guest.launched.env.count("KYTY_STUB_MISSING"), 0u);
}

TEST(AgentRunnerSession, ExistingBaselineStillRequiresAnIndependentCheckpoint)
{
	SessionHarness session;
	session.RetainBaselineOf("after");
	const std::string retained = session.BaselineText();
	EXPECT_EQ(session.Run(false, false), 1);
	EXPECT_TRUE(session.GatePassed("visual_compare"));
	EXPECT_FALSE(session.GatePassed("scene_checkpoint"));
	EXPECT_EQ(session.BaselineText(), retained);
}

TEST(AgentRunnerSession, ExistingBaselineWithSceneAndActionPasses)
{
	SessionHarness session;
	session.RetainBaselineOf("after");
	const std::string retained = session.BaselineText();
	EXPECT_EQ(session.Run(true, false), 0) << session.Summary().Dump(2);
	EXPECT_TRUE(session.HasNote("visual_compare_done"));
	EXPECT_EQ(session.BaselineText(), retained);
}

TEST(AgentRunnerSession, ZeroTapStartupHoldNeedsGuestReads)
{
	SessionHarness session;
	Json           milestones = session.profile["milestones"];
	milestones.Set("min_pad_taps", 0).Set("min_guest_read_state_samples", 0).Set("min_guest_read_samples", 1);
	session.profile.Set("pad_sequence", session.fixture.contract["action"]["pad_sequence"]).Set("milestones", milestones);
	EXPECT_EQ(session.Run(), 0) << session.Summary().Dump(2);
	EXPECT_EQ(session.Summary()["input_after"]["delivered_taps"].AsInt(), 0);
	EXPECT_GT(session.Summary()["input_after"]["guest_read_samples"].AsInt(), session.Summary()["input_before"]["guest_read_samples"].AsInt());
}

TEST(AgentRunnerSession, ExitsAreClassifiedFromTheRealReturn)
{
	{
		SessionHarness session;
		session.guest.natural_exit = true;
		EXPECT_EQ(session.Run(), 0) << session.Summary().Dump(2);
		EXPECT_EQ(session.Summary()["shutdown"].AsString(), "guest_exit_ok");
		EXPECT_FALSE(session.Summary()["coordinator_stop_requested"].AsBool());
		EXPECT_EQ(session.Summary()["stop_reason"].AsString(), "natural_exit");
	}
	{
		SessionHarness session;
		session.guest.stop_return = -15;
		EXPECT_EQ(session.Run(), 0) << session.Summary().Dump(2);
		EXPECT_EQ(session.Summary()["shutdown"].AsString(), "coordinator_stop");
		EXPECT_EQ(session.Summary()["child_exit"].AsInt(), -15);
	}
	{
		SessionHarness session;
		session.guest.stop_return = -8;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_EQ(session.Summary()["shutdown"].AsString(), "host_crash");
		EXPECT_FALSE(fs::exists(session.baseline));
	}
	{
		SessionHarness session;
		session.guest.stop_return = 7;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_EQ(session.Summary()["shutdown"].AsString(), "exit_7");
	}
}

TEST(AgentRunnerSession, DeadlinesNeverMaskARealSignalOrPass)
{
	{
		SessionHarness session;
		session.guest.timeout_on_capture = true;
		session.guest.crash_on_capture   = -11;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_TRUE(session.Summary()["timed_out"].AsBool());
		EXPECT_EQ(session.Summary()["shutdown"].AsString(), "host_crash");
		EXPECT_EQ(session.Summary()["child_exit"].AsInt(), -11);
		EXPECT_FALSE(session.Summary()["coordinator_stop_requested"].AsBool());
	}
	{
		SessionHarness session;
		session.guest.timeout_on_capture = true;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_TRUE(session.Summary()["timed_out"].AsBool());
		EXPECT_EQ(session.Summary()["child_exit"].AsInt(), 0);
		EXPECT_EQ(session.Summary()["shutdown"].AsString(), "controlled_timeout");
		EXPECT_EQ(session.Summary()["stop_reason"].AsString(), "deadline");
	}
}

TEST(AgentRunnerSession, LateErrorsAreRetainedAndNeverBecomeABaseline)
{
	{
		SessionHarness session;
		session.guest.error_stage = "post_input";
		WriteFile(session.baseline, "retained baseline");
		EXPECT_EQ(session.Run(), 1);
		EXPECT_EQ(session.Summary()["first_error"].AsString(), "late_input_failure");
		EXPECT_EQ(session.guest.captures, 2);
		EXPECT_EQ(session.BaselineText(), "retained baseline");
	}
	{
		SessionHarness session;
		session.guest.error_stage = "post_capture";
		session.guest.events_only = true;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_EQ(session.Summary()["first_error"].AsString(), "late_capture_failure");
		EXPECT_FALSE(fs::exists(session.baseline));
	}
	{
		SessionHarness session;
		session.guest.error_on_final_observation = true;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_EQ(session.Summary()["first_error"].AsString(), "after_score_failure");
	}
	{
		SessionHarness session;
		session.guest.log_error_on_capture = true;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_EQ(session.Summary()["first_error"].AsString(), "late log failure");
	}
	{
		SessionHarness session;
		session.guest.unavailable_after_capture = true;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_FALSE(session.GatePassed("final_error_observed"));
	}
}

TEST(AgentRunnerSession, WrongScenesAndUnprovenActionsFail)
{
	{
		SessionHarness session;
		session.guest.before_role = "menu";
		EXPECT_EQ(session.Run(false), 1);
		EXPECT_TRUE(session.GatePassed("visual_compare"));
		EXPECT_FALSE(session.GatePassed("scene_checkpoint"));
		EXPECT_FALSE(fs::exists(session.baseline));
	}
	{
		SessionHarness session;
		session.guest.before_role = "menu";
		EXPECT_EQ(session.Run(), 1);
		EXPECT_EQ(session.guest.clears, 1);
		EXPECT_FALSE(session.Checks()["before_scene"].AsBool());
	}
	{
		SessionHarness session;
		session.fixture.contract.Set("after", Json::MakeObject().Set("image", "after.png").Set("sha256", std::string(64, '0')));
		session.fixture.Write();
		EXPECT_EQ(session.Run(), 1);
		EXPECT_TRUE(session.HasNote("checkpoint_invalid"));
		EXPECT_FALSE(fs::exists(session.baseline));
	}
	{
		SessionHarness session;
		session.guest.fail_action_clear = true;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_FALSE(session.Checks()["action_delivered"].AsBool());
	}
	{
		SessionHarness session;
		session.guest.consume_hold = false;
		EXPECT_EQ(session.Run(), 1);
		EXPECT_TRUE(session.Checks()["expected_response"].AsBool());
		EXPECT_FALSE(session.Checks()["action_delivered"].AsBool());
	}
	{
		SessionHarness session;
		session.guest.after_role = "wrong";
		EXPECT_EQ(session.Run(), 1);
		EXPECT_FALSE(session.Checks()["expected_response"].AsBool());
	}
}

#if !defined(_WIN32)
TEST(AgentRunnerHost, StopsAnIsolatedProcessGroupAndKeepsItsOutput)
{
	ScopedTestDirectory directory("agent-runner-host");
	auto                host = CreateSystemRunnerHost();
	ASSERT_NE(host, nullptr);
	LaunchSpec spec;
	spec.argv     = {"/bin/sh", "-c", "echo out; echo err >&2; exec sleep 30"};
	spec.cwd      = directory.Path().string();
	spec.env      = {{"PATH", "/usr/bin:/bin"}};
	spec.log_path = (directory.Path() / "child.log").string();
	std::string error;
	ASSERT_TRUE(host->Launch(spec, &error)) << error;
	const double deadline = host->Now() + 5.0;
	while (ReadFile(spec.log_path).find("err") == std::string::npos && host->Now() < deadline)
	{
		host->Sleep(0.02);
	}
	EXPECT_EQ(getpgid(static_cast<pid_t>(host->ChildPid())), static_cast<pid_t>(host->ChildPid()));
	EXPECT_FALSE(host->PollChild().has_value());

	std::vector<std::string> notes;
	host->TerminateChild(&notes);
	EXPECT_NE(std::find(notes.begin(), notes.end(), "killed_sigterm"), notes.end());
	EXPECT_EQ(host->PollChild().value_or(0), -SIGTERM);
	const std::string log = ReadFile(spec.log_path);
	EXPECT_NE(log.find("out\n"), std::string::npos);
	EXPECT_NE(log.find("err\n"), std::string::npos);
}

TEST(AgentRunnerHost, LeaderExitStopsWhatItLeftInItsGroup)
{
	ScopedTestDirectory directory("agent-runner-host");
	auto                host = CreateSystemRunnerHost();
	ASSERT_NE(host, nullptr);
	LaunchSpec spec;
	spec.argv     = {"/bin/sh", "-c", "sleep 30 & echo $!; exit 0"};
	spec.cwd      = directory.Path().string();
	spec.env      = {{"PATH", "/usr/bin:/bin"}};
	spec.log_path = (directory.Path() / "child.log").string();
	std::string error;
	ASSERT_TRUE(host->Launch(spec, &error)) << error;
	double deadline = host->Now() + 5.0;
	while (!host->PollChild().has_value() && host->Now() < deadline)
	{
		host->Sleep(0.02);
	}
	ASSERT_EQ(host->PollChild().value_or(-1), 0);
	const auto left_behind = static_cast<pid_t>(std::stol(ReadFile(spec.log_path)));
	ASSERT_GT(left_behind, 0);
	// Killed, then reaped by its new parent.
	deadline = host->Now() + 5.0;
	while (::kill(left_behind, 0) == 0 && host->Now() < deadline)
	{
		host->Sleep(0.02);
	}
	EXPECT_NE(::kill(left_behind, 0), 0);
}

TEST(AgentRunnerHost, MissingAgentSocketIsATransportFailure)
{
	ScopedTestDirectory directory("agent-runner-host");
	auto                host = CreateSystemRunnerHost();
	ASSERT_NE(host, nullptr);
	EXPECT_EQ(host->CallAgent((directory.Path() / "missing.sock").string(), "ping", Json::MakeObject(), 1.0).code, 125);
	EXPECT_EQ(host->CallAgent((directory.Path() / "missing.sock").string(), "ping", Json::MakeObject(), 0.0).code, 125);
}

TEST(AgentRunnerCli, MissingGuestRootIsAnHonestSkip)
{
	ScopedTestDirectory directory("agent-runner-cli");
	const std::string   scratch = directory.Path().string();
	EXPECT_EQ(PlayableRegressionMain({"--scratch", scratch, "--allow-missing-guest", "--guest-root", ""}), 0);
	const Json skip = ParseOrNull(ReadFile(directory.Path() / "summary_skip.json"));
	EXPECT_TRUE(skip["skipped"].AsBool());
	EXPECT_FALSE(skip["passed"].AsBool());
}
#endif

UT_END();
