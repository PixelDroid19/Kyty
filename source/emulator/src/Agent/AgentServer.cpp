#include "Emulator/Agent/AgentServer.h"

#include "Emulator/Agent/EventRing.h"
#include "Emulator/Agent/FrameScore.h"
#include "Emulator/Agent/Protocol.h"
#include "Emulator/Agent/StallWatch.h"
#include "Emulator/Controller.h"
#include "Emulator/Graphics/Window.h"

#include "Kyty/Core/Threads.h"
#include "KytyBuildInfo.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {
namespace {

std::atomic<bool> g_active {false};
std::atomic<bool> g_stop {false};
Core::Thread*     g_thread     = nullptr;
int               g_listen_fd  = -1;
std::string       g_sock_path;
std::atomic<bool> g_client_busy {false};
uint64_t          g_start_ms   = 0;
std::string       g_last_capture_path;

uint64_t SteadyMs()
{
	using clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

bool EnvFlagOn(const char* name)
{
	const char* value = std::getenv(name);
	return value != nullptr && value[0] == '1' && value[1] == '\0';
}

std::string HelpResult()
{
	return std::string(
	    "{\"protocol_version\":1,\"diagnostic_input\":true,\"tools\":["
	    "{\"tool\":\"help\"},{\"tool\":\"ping\"},{\"tool\":\"status\"},{\"tool\":\"diagnostics\"},"
	    "{\"tool\":\"events\",\"args\":{\"last\":50,\"after_seq\":0}},"
	    "{\"tool\":\"last_error\"},"
	    "{\"tool\":\"capture\",\"args\":{\"timeout_ms\":10000,\"score\":true}},"
	    "{\"tool\":\"score\",\"args\":{\"path\":\"/abs/last.bmp\"}},"
	    "{\"tool\":\"pad_down\",\"args\":{\"button\":\"cross\"}},"
	    "{\"tool\":\"pad_up\",\"args\":{\"button\":\"cross\"}},"
	    "{\"tool\":\"pad_tap\",\"args\":{\"button\":\"cross\"}},"
	    "{\"tool\":\"pad_axis\",\"args\":{\"axis\":\"left_x\",\"value\":128}},"
	    "{\"tool\":\"pad_clear\"},"
	    "{\"tool\":\"wait_present\",\"args\":{\"min\":1,\"timeout_ms\":30000}},"
	    "{\"tool\":\"wait_frame\",\"args\":{\"min\":1,\"timeout_ms\":30000}},"
	    "{\"tool\":\"wait_event\",\"args\":{\"kind\":\"error\",\"timeout_ms\":5000}},"
	    "{\"tool\":\"watch\",\"args\":{\"window_ms\":10000,\"present_stall_ms\":5000,\"frame_stall_ms\":5000,\"min_fps\":2.0,\"capture\":true}}"
	    "]}");
}

std::string StatusResult()
{
	Libs::Graphics::WindowPresentStats stats {};
	Libs::Graphics::WindowGetPresentStats(&stats);

	uint32_t buttons = 0;
	uint8_t  axes[static_cast<int>(Libs::Controller::Axis::AxisMax)] {};
	Libs::Controller::AgentPadGetState(&buttons, axes);
	Libs::Controller::AgentPadReadStats read_stats {};
	Libs::Controller::AgentPadGetReadStats(&read_stats);

	char buf[1280];
	std::snprintf(
	    buf, sizeof(buf),
	    "{\"protocol_version\":%u,\"frame\":%d,\"present\":%llu,\"fps\":%.3f,\"ms_since_present\":%llu,\"ms_since_frame\":%llu,"
	    "\"capture_ready\":%s,\"capture_dir_set\":%s,"
	    "\"graphic_ready\":%s,\"build_revision\":%s,\"build_dirty\":%s,\"diagnostic_input\":true,"
	    "\"pad\":{\"buttons\":%u,\"left_x\":%u,\"left_y\":%u,\"right_x\":%u,\"right_y\":%u,\"l2\":%u,\"r2\":%u,"
	    "\"guest_read_state_samples\":%llu,\"guest_read_samples\":%llu,\"delivered_taps\":%llu,\"tap_pending\":%s}}",
	    kAgentProtocolVersion, stats.frame, static_cast<unsigned long long>(stats.present), stats.fps,
	    static_cast<unsigned long long>(stats.ms_since_present), static_cast<unsigned long long>(stats.ms_since_frame),
	    stats.capture_ready ? "true" : "false", stats.capture_dir_set ? "true" : "false", stats.graphic_ready ? "true" : "false",
	    JsonString(BuildInfo::Revision).c_str(), BuildInfo::Dirty ? "true" : "false", buttons,
	    static_cast<unsigned>(axes[0]), static_cast<unsigned>(axes[1]), static_cast<unsigned>(axes[2]),
	    static_cast<unsigned>(axes[3]), static_cast<unsigned>(axes[4]), static_cast<unsigned>(axes[5]),
	    static_cast<unsigned long long>(read_stats.read_state_samples), static_cast<unsigned long long>(read_stats.read_samples),
	    static_cast<unsigned long long>(read_stats.delivered_taps), read_stats.tap_pending ? "true" : "false");
	return std::string(buf);
}

std::string DiagnosticsResult()
{
	char buf[512];
	std::snprintf(buf, sizeof(buf),
	              "{\"AUTO_CROSS\":%s,\"STUB_MISSING\":%s,\"GFX_PERMISSIVE\":%s,\"SKIP_UD2\":%s,\"AGENT_SOCK\":true,"
	              "\"NATIVE_CAPTURE_DIR\":%s}",
	              EnvFlagOn("KYTY_AUTO_CROSS") ? "true" : "false", EnvFlagOn("KYTY_STUB_MISSING") ? "true" : "false",
	              EnvFlagOn("KYTY_GFX_PERMISSIVE") ? "true" : "false", EnvFlagOn("KYTY_SKIP_UD2") ? "true" : "false",
	              (std::getenv("KYTY_NATIVE_CAPTURE_DIR") != nullptr && std::getenv("KYTY_NATIVE_CAPTURE_DIR")[0] != '\0') ? "true"
	                                                                                                                     : "false");
	return std::string(buf);
}

std::string EventsResult(uint32_t last, uint64_t after_seq)
{
	if (last == 0 || last > 128)
	{
		last = 50;
	}
	std::vector<EventRecord> records(last);
	const uint32_t           count = EventRing::Instance().CopySince(after_seq, records.data(), last);
	std::string              out   = "{\"events\":[";
	for (uint32_t i = 0; i < count; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		char item[512];
		std::snprintf(item, sizeof(item),
		              "{\"seq\":%llu,\"t_ms\":%llu,\"kind\":%s,\"code\":%s,\"message\":%s}",
		              static_cast<unsigned long long>(records[i].seq), static_cast<unsigned long long>(records[i].t_ms),
		              JsonString(EventKindName(records[i].kind)).c_str(), JsonString(records[i].code).c_str(),
		              JsonString(records[i].message).c_str());
		out += item;
	}
	out += "]}";
	return out;
}

std::string LastErrorResult()
{
	EventRecord rec {};
	if (!EventRing::Instance().LastError(&rec))
	{
		return "{\"event\":null}";
	}
	char buf[512];
	std::snprintf(buf, sizeof(buf),
	              "{\"event\":{\"seq\":%llu,\"t_ms\":%llu,\"kind\":%s,\"code\":%s,\"message\":%s}}",
	              static_cast<unsigned long long>(rec.seq), static_cast<unsigned long long>(rec.t_ms),
	              JsonString(EventKindName(rec.kind)).c_str(), JsonString(rec.code).c_str(), JsonString(rec.message).c_str());
	return std::string(buf);
}

std::string PadStateResult()
{
	uint32_t buttons = 0;
	uint8_t  axes[static_cast<int>(Libs::Controller::Axis::AxisMax)] {};
	Libs::Controller::AgentPadGetState(&buttons, axes);
	Libs::Controller::AgentPadReadStats read_stats {};
	Libs::Controller::AgentPadGetReadStats(&read_stats);
	char buf[384];
	std::snprintf(buf, sizeof(buf),
	              "{\"diagnostic_input\":true,\"buttons\":%u,\"left_x\":%u,\"left_y\":%u,\"right_x\":%u,\"right_y\":%u,\"l2\":%u,\"r2\":%u,"
	              "\"delivered_taps\":%llu,\"tap_pending\":%s}",
	              buttons, static_cast<unsigned>(axes[0]), static_cast<unsigned>(axes[1]), static_cast<unsigned>(axes[2]),
	              static_cast<unsigned>(axes[3]), static_cast<unsigned>(axes[4]), static_cast<unsigned>(axes[5]),
	              static_cast<unsigned long long>(read_stats.delivered_taps), read_stats.tap_pending ? "true" : "false");
	return std::string(buf);
}

std::string Dispatch(const Request& req)
{
	if (req.tool == "help")
	{
		return FormatOk(req.id, HelpResult());
	}
	if (req.tool == "ping")
	{
		char buf[256];
		std::snprintf(buf, sizeof(buf), "{\"alive\":true,\"protocol_version\":%u,\"uptime_ms\":%llu}", kAgentProtocolVersion,
		              static_cast<unsigned long long>(SteadyMs() - g_start_ms));
		return FormatOk(req.id, buf);
	}
	if (req.tool == "status")
	{
		return FormatOk(req.id, StatusResult());
	}
	if (req.tool == "diagnostics")
	{
		return FormatOk(req.id, DiagnosticsResult());
	}
	if (req.tool == "events")
	{
		uint32_t last      = 50;
		uint64_t after_seq = 0;
		(void)ArgsGetU32(req.args_json, "last", &last);
		(void)ArgsGetU64(req.args_json, "after_seq", &after_seq);
		return FormatOk(req.id, EventsResult(last, after_seq));
	}
	if (req.tool == "last_error")
	{
		return FormatOk(req.id, LastErrorResult());
	}
	if (req.tool == "capture")
	{
		uint32_t timeout_ms = 10000;
		bool     do_score   = true;
		(void)ArgsGetU32(req.args_json, "timeout_ms", &timeout_ms);
		(void)ArgsGetBool(req.args_json, "score", &do_score);
		Libs::Graphics::WindowNativeCaptureResult err {};
		uint64_t                                  request_id = 0;
		if (!Libs::Graphics::WindowRequestNativeCapture(&request_id, &err))
		{
			return FormatErr(req.id, err.error_code.c_str(), err.error_message.c_str());
		}
		Libs::Graphics::WindowNativeCaptureResult result {};
		if (!Libs::Graphics::WindowWaitNativeCapture(request_id, timeout_ms, &result))
		{
			return FormatErr(req.id, result.error_code.empty() ? "timeout" : result.error_code.c_str(),
			                 result.error_message.empty() ? "native capture wait failed" : result.error_message.c_str());
		}
		g_last_capture_path = result.path;

		std::string score_json = "null";
		if (do_score)
		{
			FrameScoreMetrics metrics {};
			if (ScoreNativeBmp(result.path.c_str(), &metrics))
			{
				score_json = FrameScoreToJson(metrics, result.path.c_str());
				if (metrics.verdict != FrameVerdict::Healthy)
				{
					EventRing::Instance().Push(EventKind::Warn, metrics.verdict_name, metrics.hint);
				}
			} else
			{
				score_json = "{\"verdict\":\"load_failed\",\"healthy\":false}";
			}
		}

		char buf[1536];
		std::snprintf(buf, sizeof(buf),
		              "{\"path\":%s,\"milestone\":%s,\"format\":%s,\"width\":%u,\"height\":%u,\"present\":%llu,\"frame\":%d,\"score\":%s}",
		              JsonString(result.path.c_str()).c_str(), JsonString(result.milestone.c_str()).c_str(),
		              JsonString(result.format.c_str()).c_str(), result.width, result.height,
		              static_cast<unsigned long long>(result.present), result.frame, score_json.c_str());
		return FormatOk(req.id, buf);
	}
	if (req.tool == "score")
	{
		std::string path;
		if (!ArgsGetString(req.args_json, "path", &path) || path.empty())
		{
			path = g_last_capture_path;
		}
		if (path.empty())
		{
			return FormatErr(req.id, "not_ready", "no capture path; pass path or capture first");
		}
		if (path[0] != '/')
		{
			return FormatErr(req.id, "invalid_args", "score path must be absolute");
		}
		FrameScoreMetrics metrics {};
		if (!ScoreNativeBmp(path.c_str(), &metrics))
		{
			return FormatErr(req.id, "load_failed", "failed to load native BMP");
		}
		if (metrics.verdict != FrameVerdict::Healthy)
		{
			EventRing::Instance().Push(EventKind::Warn, metrics.verdict_name, metrics.hint);
		}
		const std::string body = FrameScoreToJson(metrics, path.c_str());
		return FormatOk(req.id, body);
	}
	if (req.tool == "pad_down" || req.tool == "pad_up" || req.tool == "pad_tap")
	{
		std::string button_name;
		if (!ArgsGetString(req.args_json, "button", &button_name))
		{
			return FormatErr(req.id, "invalid_args", "button is required");
		}
		uint32_t button = 0;
		if (!Libs::Controller::AgentPadButtonFromName(button_name.c_str(), &button))
		{
			return FormatErr(req.id, "pad_unknown_button", "unknown button name");
		}
		if (req.tool == "pad_tap")
		{
			if (!Libs::Controller::AgentPadScheduleTap(button))
			{
				return FormatErr(req.id, "tap_pending", "a tap is already pending or the button is held");
			}
			EventRing::Instance().Push(EventKind::Input, "pad_tap", button_name.c_str());
		} else
		{
			const bool down = req.tool == "pad_down";
			Libs::Controller::AgentPadSetButton(button, down);
			EventRing::Instance().Push(EventKind::Input, down ? "pad_down" : "pad_up", button_name.c_str());
		}
		return FormatOk(req.id, PadStateResult());
	}
	if (req.tool == "pad_axis")
	{
		std::string axis_name;
		uint32_t    value = 0;
		if (!ArgsGetString(req.args_json, "axis", &axis_name) || !ArgsGetU32(req.args_json, "value", &value))
		{
			return FormatErr(req.id, "invalid_args", "axis and value are required");
		}
		if (value > 255)
		{
			return FormatErr(req.id, "invalid_args", "value must be 0..255");
		}
		Libs::Controller::Axis axis {};
		if (!Libs::Controller::AgentPadAxisFromName(axis_name.c_str(), &axis))
		{
			return FormatErr(req.id, "invalid_args", "unknown axis name");
		}
		Libs::Controller::AgentPadSetAxis(axis, static_cast<uint8_t>(value));
		EventRing::Instance().Push(EventKind::Input, "pad_axis", axis_name.c_str());
		return FormatOk(req.id, PadStateResult());
	}
	if (req.tool == "pad_clear")
	{
		Libs::Controller::AgentPadClear();
		EventRing::Instance().Push(EventKind::Input, "pad_clear", "");
		return FormatOk(req.id, PadStateResult());
	}
	if (req.tool == "wait_present" || req.tool == "wait_frame")
	{
		uint64_t min_value  = 0;
		uint32_t timeout_ms = 30000;
		if (!ArgsGetU64(req.args_json, "min", &min_value))
		{
			return FormatErr(req.id, "invalid_args", "min is required");
		}
		(void)ArgsGetU32(req.args_json, "timeout_ms", &timeout_ms);
		const uint64_t deadline = SteadyMs() + timeout_ms;
		while (!g_stop.load())
		{
			Libs::Graphics::WindowPresentStats stats {};
			Libs::Graphics::WindowGetPresentStats(&stats);
			const uint64_t current = req.tool == "wait_present" ? stats.present : static_cast<uint64_t>(stats.frame);
			if (current >= min_value)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf), "{\"%s\":%llu}", req.tool == "wait_present" ? "present" : "frame",
				              static_cast<unsigned long long>(current));
				return FormatOk(req.id, buf);
			}
			if (SteadyMs() >= deadline)
			{
				return FormatErr(req.id, "timeout", "wait timed out");
			}
			Core::Thread::Sleep(10);
		}
		return FormatErr(req.id, "socket_gone", "agent server stopping");
	}
	if (req.tool == "wait_event")
	{
		std::string kind_name;
		uint32_t    timeout_ms = 5000;
		uint64_t    after_seq  = EventRing::Instance().NextSeq();
		if (!ArgsGetString(req.args_json, "kind", &kind_name))
		{
			return FormatErr(req.id, "invalid_args", "kind is required");
		}
		(void)ArgsGetU32(req.args_json, "timeout_ms", &timeout_ms);
		(void)ArgsGetU64(req.args_json, "after_seq", &after_seq);
		EventKind kind {};
		if (!EventKindFromName(kind_name.c_str(), &kind))
		{
			return FormatErr(req.id, "invalid_args", "unknown event kind");
		}
		const uint64_t deadline = SteadyMs() + timeout_ms;
		while (!g_stop.load())
		{
			EventRecord records[16] {};
			const uint32_t count = EventRing::Instance().CopySince(after_seq, records, 16);
			for (uint32_t i = 0; i < count; ++i)
			{
				// CopySince returns newest-first; scan for match.
				if (records[i].kind == kind)
				{
					char buf[512];
					std::snprintf(buf, sizeof(buf),
					              "{\"event\":{\"seq\":%llu,\"t_ms\":%llu,\"kind\":%s,\"code\":%s,\"message\":%s}}",
					              static_cast<unsigned long long>(records[i].seq), static_cast<unsigned long long>(records[i].t_ms),
					              JsonString(EventKindName(records[i].kind)).c_str(), JsonString(records[i].code).c_str(),
					              JsonString(records[i].message).c_str());
					return FormatOk(req.id, buf);
				}
			}
			if (count > 0)
			{
				after_seq = records[0].seq;
			}
			if (SteadyMs() >= deadline)
			{
				return FormatErr(req.id, "timeout", "wait_event timed out");
			}
			Core::Thread::Sleep(10);
		}
		return FormatErr(req.id, "socket_gone", "agent server stopping");
	}
	if (req.tool == "watch")
	{
		StallWatchArgs args {};
		bool           do_capture = true;
		(void)ArgsGetU32(req.args_json, "window_ms", &args.window_ms);
		(void)ArgsGetU32(req.args_json, "present_stall_ms", &args.present_stall_ms);
		(void)ArgsGetU32(req.args_json, "frame_stall_ms", &args.frame_stall_ms);
		uint32_t min_fps_u = 2;
		if (ArgsGetU32(req.args_json, "min_fps", &min_fps_u))
		{
			args.min_fps = static_cast<double>(min_fps_u);
		}
		(void)ArgsGetBool(req.args_json, "capture", &do_capture);
		if (args.window_ms < 500)
		{
			args.window_ms = 500;
		}

		Libs::Graphics::WindowPresentStats start_stats {};
		Libs::Graphics::WindowGetPresentStats(&start_stats);
		StallSample start {};
		start.graphic_ready    = start_stats.graphic_ready;
		start.frame            = start_stats.frame;
		start.present          = start_stats.present;
		start.fps              = start_stats.fps;
		start.ms_since_present = start_stats.ms_since_present;
		start.ms_since_frame   = start_stats.ms_since_frame;

		const uint64_t deadline = SteadyMs() + args.window_ms;
		while (!g_stop.load() && SteadyMs() < deadline)
		{
			Core::Thread::Sleep(50);
		}

		Libs::Graphics::WindowPresentStats end_stats {};
		Libs::Graphics::WindowGetPresentStats(&end_stats);
		StallSample end {};
		end.graphic_ready    = end_stats.graphic_ready;
		end.frame            = end_stats.frame;
		end.present          = end_stats.present;
		end.fps              = end_stats.fps;
		end.ms_since_present = end_stats.ms_since_present;
		end.ms_since_frame   = end_stats.ms_since_frame;

		const StallWatchResult classified = ClassifyStall(start, end, args);
		if (!classified.healthy)
		{
			EventRing::Instance().Push(EventKind::Warn, classified.code_name, classified.hint);
		}

		std::string capture_json = "null";
		std::string score_json   = "null";
		if (do_capture && !classified.healthy)
		{
			Libs::Graphics::WindowNativeCaptureResult err {};
			uint64_t                                  request_id = 0;
			if (Libs::Graphics::WindowRequestNativeCapture(&request_id, &err))
			{
				Libs::Graphics::WindowNativeCaptureResult result {};
				if (Libs::Graphics::WindowWaitNativeCapture(request_id, 15000, &result) && result.ok)
				{
					g_last_capture_path = result.path;
					FrameScoreMetrics metrics {};
					if (ScoreNativeBmp(result.path.c_str(), &metrics))
					{
						score_json = FrameScoreToJson(metrics, result.path.c_str());
						if (metrics.verdict != FrameVerdict::Healthy)
						{
							EventRing::Instance().Push(EventKind::Warn, metrics.verdict_name, metrics.hint);
						}
					} else
					{
						score_json = "{\"verdict\":\"load_failed\",\"healthy\":false}";
					}
					char cap[768];
					std::snprintf(cap, sizeof(cap),
					              "{\"path\":%s,\"frame\":%d,\"present\":%llu,\"format\":%s,\"width\":%u,\"height\":%u}",
					              JsonString(result.path.c_str()).c_str(), result.frame,
					              static_cast<unsigned long long>(result.present), JsonString(result.format.c_str()).c_str(), result.width,
					              result.height);
					capture_json = cap;
				} else
				{
					capture_json = std::string("{\"ok\":false,\"error\":") + JsonString(result.error_code.c_str()) + "}";
				}
			} else
			{
				capture_json = std::string("{\"ok\":false,\"error\":") + JsonString(err.error_code.c_str()) + "}";
			}
		}

		EventRecord last_err {};
		const bool  have_err = EventRing::Instance().LastError(&last_err);
		char        err_json[512];
		if (have_err)
		{
			std::snprintf(err_json, sizeof(err_json),
			              "{\"seq\":%llu,\"kind\":%s,\"code\":%s,\"message\":%s}",
			              static_cast<unsigned long long>(last_err.seq), JsonString(EventKindName(last_err.kind)).c_str(),
			              JsonString(last_err.code).c_str(), JsonString(last_err.message).c_str());
		} else
		{
			std::snprintf(err_json, sizeof(err_json), "null");
		}

		char buf[3072];
		std::snprintf(
		    buf, sizeof(buf),
		    "{\"healthy\":%s,\"stall_code\":%s,\"hint\":%s,\"window_ms\":%u,"
		    "\"start\":{\"frame\":%d,\"present\":%llu,\"fps\":%.3f,\"ms_since_present\":%llu,\"ms_since_frame\":%llu},"
		    "\"end\":{\"frame\":%d,\"present\":%llu,\"fps\":%.3f,\"ms_since_present\":%llu,\"ms_since_frame\":%llu},"
		    "\"frame_delta\":%lld,\"present_delta\":%lld,\"capture\":%s,\"score\":%s,\"last_error\":%s}",
		    classified.healthy ? "true" : "false", JsonString(classified.code_name).c_str(), JsonString(classified.hint).c_str(),
		    args.window_ms, classified.start.frame, static_cast<unsigned long long>(classified.start.present), classified.start.fps,
		    static_cast<unsigned long long>(classified.start.ms_since_present),
		    static_cast<unsigned long long>(classified.start.ms_since_frame), classified.end.frame,
		    static_cast<unsigned long long>(classified.end.present), classified.end.fps,
		    static_cast<unsigned long long>(classified.end.ms_since_present),
		    static_cast<unsigned long long>(classified.end.ms_since_frame), static_cast<long long>(classified.frame_delta),
		    static_cast<long long>(classified.present_delta), capture_json.c_str(), score_json.c_str(), err_json);

		// Transport succeeds; agents/CLI treat healthy=false as a detected hang.
		return FormatOk(req.id, buf);
	}

	return FormatErr(req.id, "unknown_tool", "unknown tool");
}

#if !defined(_WIN32)

bool WriteAll(int fd, const std::string& line)
{
	std::string payload = line;
	payload.push_back('\n');
	size_t off = 0;
	while (off < payload.size())
	{
		const ssize_t n = ::write(fd, payload.data() + off, payload.size() - off);
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return false;
		}
		off += static_cast<size_t>(n);
	}
	return true;
}

bool ReadLine(int fd, std::string* out)
{
	out->clear();
	char ch = 0;
	while (out->size() < kAgentLineMax)
	{
		const ssize_t n = ::read(fd, &ch, 1);
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return false;
		}
		if (n == 0)
		{
			return !out->empty();
		}
		if (ch == '\n')
		{
			return true;
		}
		if (ch != '\r')
		{
			out->push_back(ch);
		}
	}
	return false;
}

void HandleClient(int client_fd)
{
	bool expected = false;
	if (!g_client_busy.compare_exchange_strong(expected, true))
	{
		(void)WriteAll(client_fd, FormatErr(0, "busy", "only one agent client is supported in v1"));
		::close(client_fd);
		return;
	}

	while (!g_stop.load())
	{
		std::string line;
		if (!ReadLine(client_fd, &line))
		{
			break;
		}
		if (line.empty())
		{
			continue;
		}
		Request   req {};
		ErrorInfo error {};
		if (!ParseRequestLine(line.c_str(), &req, &error))
		{
			if (!WriteAll(client_fd, FormatErr(0, error.code.c_str(), error.message.c_str())))
			{
				break;
			}
			continue;
		}
		const std::string response = Dispatch(req);
		if (!WriteAll(client_fd, response))
		{
			break;
		}
	}

	::close(client_fd);
	g_client_busy.store(false);
}

void ServerThread(void* /*arg*/)
{
	while (!g_stop.load())
	{
		const int client = ::accept(g_listen_fd, nullptr, nullptr);
		if (client < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			if (g_stop.load())
			{
				break;
			}
			Core::Thread::Sleep(10);
			continue;
		}
		HandleClient(client);
	}
}

bool ListenUnix(const char* path)
{
	g_listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_listen_fd < 0)
	{
		return false;
	}

	::unlink(path);

	sockaddr_un addr {};
	addr.sun_family = AF_UNIX;
	if (std::strlen(path) >= sizeof(addr.sun_path))
	{
		::close(g_listen_fd);
		g_listen_fd = -1;
		return false;
	}
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (::bind(g_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
	{
		::close(g_listen_fd);
		g_listen_fd = -1;
		return false;
	}
	if (::listen(g_listen_fd, 1) != 0)
	{
		::close(g_listen_fd);
		g_listen_fd = -1;
		::unlink(path);
		return false;
	}
	return true;
}

#endif // !_WIN32

} // namespace

bool StartFromEnv()
{
	if (g_active.load())
	{
		return true;
	}

	const char* path = std::getenv("KYTY_AGENT_SOCK");
	if (path == nullptr || path[0] == '\0')
	{
		return true;
	}
#if defined(_WIN32)
	std::fprintf(stderr, "KYTY_AGENT_SOCK is set but the agent server is POSIX-only in this build\n");
	return true;
#else
	if (path[0] != '/')
	{
		std::fprintf(stderr, "KYTY_AGENT_SOCK must be an absolute path\n");
		EventRing::Instance().Push(EventKind::Error, "invalid_sock", "KYTY_AGENT_SOCK must be absolute");
		return false;
	}

	g_sock_path = path;
	g_stop.store(false);
	g_start_ms = SteadyMs();
	if (!ListenUnix(path))
	{
		std::fprintf(stderr, "KYTY_AGENT failed to listen on %s\n", path);
		EventRing::Instance().Push(EventKind::Error, "listen_failed", path);
		return false;
	}

	g_thread = new Core::Thread(ServerThread, nullptr);
	g_active.store(true);
	EventRing::Instance().Push(EventKind::Info, "agent_start", path);
	std::fprintf(stderr, "KYTY_AGENT listening on %s\n", path);
	return true;
#endif
}

void Stop()
{
	if (!g_active.load())
	{
		return;
	}
	g_stop.store(true);
#if !defined(_WIN32)
	if (g_listen_fd >= 0)
	{
		::shutdown(g_listen_fd, SHUT_RDWR);
		::close(g_listen_fd);
		g_listen_fd = -1;
	}
	if (g_thread != nullptr)
	{
		g_thread->Join();
		delete g_thread;
		g_thread = nullptr;
	}
	if (!g_sock_path.empty())
	{
		::unlink(g_sock_path.c_str());
		g_sock_path.clear();
	}
#endif
	Libs::Controller::AgentPadClear();
	g_active.store(false);
	g_client_busy.store(false);
	EventRing::Instance().Push(EventKind::Info, "agent_stop", "");
}

bool Active()
{
	return g_active.load();
}

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED
