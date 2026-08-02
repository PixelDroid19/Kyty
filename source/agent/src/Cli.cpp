#include "Kyty/Agent/Cli.h"

#include "Kyty/Agent/Json.h"
#include "Kyty/Agent/LocalTransport.h"
#include "Kyty/Agent/WireContract.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

namespace Kyty::AgentCli {
namespace {

namespace Transport = Kyty::Agent::LocalTransport;

using Kyty::Agent::JsonEscape;
using Kyty::Agent::kProtocolVersion;
using Kyty::Agent::kResponseLineMax;

void PrintUsage()
{
	std::fprintf(stderr, "kyty_agent - native realtime Kyty emulator tools\n"
	                     "\n"
	                     "Usage:\n"
	                     "  kyty_agent --endpoint ENDPOINT help|doctor|ping|status|diagnostics\n"
	                     "  kyty_agent crash-context --path ABS.json\n"
	                     "  kyty_agent --endpoint ENDPOINT perf-snapshot [--reset]\n"
	                     "  kyty_agent --endpoint ENDPOINT sync-waits|threads|last-error\n"
	                     "  kyty_agent --endpoint ENDPOINT events [--last N] [--after-seq N]\n"
	                     "  kyty_agent --endpoint ENDPOINT snapshot [--events N] [--after-seq N]\n"
	                     "  kyty_agent --endpoint ENDPOINT capture [--timeout-ms N] [--no-score]\n"
	                     "  kyty_agent --endpoint ENDPOINT score\n"
	                     "  kyty_agent --endpoint ENDPOINT pad down|up|tap BUTTON\n"
	                     "  kyty_agent --endpoint ENDPOINT pad hold BUTTON --delta N [--timeout-ms N]\n"
	                     "  kyty_agent --endpoint ENDPOINT pad axis AXIS VALUE|clear\n"
	                     "  kyty_agent --endpoint ENDPOINT wait-ready [--timeout-ms N]\n"
	                     "  kyty_agent --endpoint ENDPOINT wait-present (--min N|--delta N) [--timeout-ms N]\n"
	                     "  kyty_agent --endpoint ENDPOINT wait-frame (--min N|--delta N) [--timeout-ms N]\n"
	                     "  kyty_agent --endpoint ENDPOINT wait-phase WANT [--min-fps N] [--stable-ms N] [--timeout-ms N]\n"
	                     "  kyty_agent --endpoint ENDPOINT wait-event --kind KIND [--code CODE] [--after-seq N] [--timeout-ms N]\n"
	                     "  kyty_agent --endpoint ENDPOINT watch [--window-ms N|--seconds N] [--present-stall-ms N] [--frame-stall-ms N] "
	                     "[--min-fps N] [--no-capture]\n"
	                     "\n"
	                     "Windows endpoint: \\\\.\\pipe\\NAME. Linux/macOS endpoint: /absolute/socket/path.\n"
	                     "Start the emulator with KYTY_AGENT_ENDPOINT=ENDPOINT.\n"
	                     "Pad input is diagnostic_input, not gameplay acceptance.\n"
	                     "status.phase: not_ready|booting|loading|interactive|stalled (use wait-phase).\n"
	                     "Prefer wait-ready → wait-phase / wait-present --delta over absolute --min with long sleeps.\n"
	                     "Exit 125 = transport (guest dead / stale endpoint); do not retry with longer host sleeps.\n"
	                     "watch exits 1 when present/frame/fps look stalled (loading hang).\n"
	                     "capture/score exit 1 when frame metrics look corrupted (healthy:false).\n");
}

// Stable machine-readable CLI failure codes (stdout JSON when applicable).
// Exit 125 = transport/usage; exit 1 = tool/runtime failure.
// Codes: transport | usage | timeout | tool_error | unhealthy
void PrintCliFailure(const char* code, const char* message)
{
	// Keep local transport errors on the same public wire version as the server.
	std::printf("{\"ok\":false,\"protocol_version\":%u,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}\n", kProtocolVersion,
	            code != nullptr ? code : "error", message != nullptr ? message : "");
}

bool Connect(const char* endpoint, bool quiet, Transport::Connection* connection)
{
	if (!Transport::IsValidEndpoint(endpoint))
	{
		if (!quiet)
		{
			std::fprintf(stderr, "kyty_agent: invalid %s endpoint\n", Transport::EndpointKind());
			PrintCliFailure("usage", "invalid_endpoint");
		}
		return false;
	}
	const auto result = Transport::Connect(endpoint, connection);
	if (result != Transport::Result::Ok)
	{
		if (!quiet)
		{
			std::fprintf(stderr, "kyty_agent: connect(%s) failed: %s\n", endpoint, Transport::ResultName(result));
			if (result == Transport::Result::NotFound || result == Transport::Result::Busy)
			{
				std::fprintf(stderr, "kyty_agent: guest agent endpoint is not live; use wait-ready after relaunch\n");
				PrintCliFailure("transport", "endpoint_not_live");
			} else
			{
				PrintCliFailure("transport", "connect_failed");
			}
		}
		return false;
	}
	return true;
}

uint64_t MonotonicMs()
{
	using Clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

int CallImpl(const char* endpoint, const std::string& request_line, bool print_response, std::string* response_out)
{
	Transport::Connection connection {};
	if (!Connect(endpoint, false, &connection))
	{
		return 125;
	}
	std::string payload = request_line;
	payload.push_back('\n');
	if (Transport::WriteAll(&connection, payload.data(), payload.size()) != Transport::Result::Ok)
	{
		std::fprintf(stderr, "kyty_agent: write failed\n");
		PrintCliFailure("transport", "write_failed");
		Transport::Close(&connection);
		return 125;
	}
	std::string response;
	if (Transport::ReadLine(&connection, &response, kResponseLineMax) != Transport::Result::Ok)
	{
		std::fprintf(stderr, "kyty_agent: read failed\n");
		PrintCliFailure("transport", "read_failed");
		Transport::Close(&connection);
		return 125;
	}
	Transport::Close(&connection);
	if (response_out != nullptr)
	{
		*response_out = response;
	}
	if (print_response)
	{
		std::puts(response.c_str());
	}
	if (response.find("\"ok\":false") != std::string::npos)
	{
		return 1; // tool_error — server error object already has code
	}
	if (response.find("\"healthy\":false") != std::string::npos)
	{
		return 1; // unhealthy
	}
	return 0;
}

int Call(const char* endpoint, const std::string& request_line)
{
	return CallImpl(endpoint, request_line, true, nullptr);
}

// Poll until the emulator agent accepts a connection (boot / relaunch).
int WaitReady(const char* endpoint, uint64_t timeout_ms)
{
	const uint64_t start_ms = MonotonicMs();
	uint64_t       attempts = 0;
	for (;;)
	{
		if (MonotonicMs() - start_ms >= timeout_ms)
		{
			std::fprintf(stderr, "kyty_agent: wait-ready timed out after %llu ms (%llu attempts)\n",
			             static_cast<unsigned long long>(timeout_ms), static_cast<unsigned long long>(attempts));
			std::fprintf(stderr, "kyty_agent: guest agent endpoint is not live; relaunch with KYTY_AGENT_ENDPOINT set\n");
			PrintCliFailure("timeout", "wait_ready_timeout");
			return 1;
		}
		++attempts;
		Transport::Connection connection {};
		if (Connect(endpoint, true, &connection))
		{
			Transport::Close(&connection);
			char req[96];
			std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"ping\",\"args\":{}}");
			const int rc = CallImpl(endpoint, req, false, nullptr);
			if (rc == 0)
			{
				std::printf("{\"id\":1,\"ok\":true,\"protocol_version\":%u,"
				            "\"result\":{\"ready\":true,\"attempts\":%llu,\"waited_ms\":%llu}}\n",
				            kProtocolVersion, static_cast<unsigned long long>(attempts),
				            static_cast<unsigned long long>(MonotonicMs() - start_ms));
				return 0;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

int Doctor(const char* endpoint)
{
	const int ping_rc = Call(endpoint, "{\"id\":1,\"tool\":\"ping\",\"args\":{}}");
	if (ping_rc != 0)
	{
		return ping_rc;
	}
	return Call(endpoint, "{\"id\":2,\"tool\":\"status\",\"args\":{}}");
}

const char* RequireArg(int argc, char** argv, int* index, const char* flag)
{
	if (*index + 1 >= argc)
	{
		std::fprintf(stderr, "kyty_agent: missing value for %s\n", flag);
		return nullptr;
	}
	++(*index);
	return argv[*index];
}

bool ParseDecimalUnsigned(const char* text, uint64_t* out)
{
	if (text == nullptr || text[0] == '\0' || out == nullptr)
	{
		return false;
	}
	uint64_t value = 0;
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != '\0'; ++p)
	{
		if (*p < static_cast<unsigned char>('0') || *p > static_cast<unsigned char>('9'))
		{
			return false;
		}
		const uint64_t digit = static_cast<uint64_t>(*p - static_cast<unsigned char>('0'));
		if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10u)
		{
			return false;
		}
		value = value * 10u + digit;
	}
	*out = value;
	return true;
}

struct WatchOptions
{
	uint64_t window_ms        = 10000;
	uint64_t present_stall_ms = 5000;
	uint64_t frame_stall_ms   = 5000;
	uint64_t min_fps          = 2;
	bool     capture          = true;
};

enum class WatchOptionKind
{
	Number,
	Seconds,
	DisableCapture,
};

struct WatchOptionSpec
{
	const char*     flag;
	WatchOptionKind kind;
	uint64_t WatchOptions::* target;
};

const WatchOptionSpec* FindWatchOption(const char* flag)
{
	static constexpr WatchOptionSpec kOptions[] = {
	    {"--window-ms", WatchOptionKind::Number, &WatchOptions::window_ms},
	    {"--seconds", WatchOptionKind::Seconds, &WatchOptions::window_ms},
	    {"--present-stall-ms", WatchOptionKind::Number, &WatchOptions::present_stall_ms},
	    {"--frame-stall-ms", WatchOptionKind::Number, &WatchOptions::frame_stall_ms},
	    {"--min-fps", WatchOptionKind::Number, &WatchOptions::min_fps},
	    {"--no-capture", WatchOptionKind::DisableCapture, nullptr},
	};
	for (const auto& option: kOptions)
	{
		if (std::strcmp(flag, option.flag) == 0)
		{
			return &option;
		}
	}
	return nullptr;
}

bool ParseWatchOptions(int argc, char** argv, int* index, WatchOptions* out)
{
	for (; *index < argc; ++(*index))
	{
		const char*            flag = argv[*index];
		const WatchOptionSpec* spec = FindWatchOption(flag);
		if (spec == nullptr)
		{
			std::fprintf(stderr, "kyty_agent: unknown watch flag %s\n", flag);
			return false;
		}
		if (spec->kind == WatchOptionKind::DisableCapture)
		{
			out->capture = false;
			continue;
		}
		const char* value = RequireArg(argc, argv, index, flag);
		if (value == nullptr)
		{
			return false;
		}
		const uint64_t parsed = std::strtoull(value, nullptr, 10);
		out->*(spec->target)  = spec->kind == WatchOptionKind::Seconds ? parsed * 1000ull : parsed;
	}
	return true;
}

} // namespace

int Main(int argc, char** argv)
{
	if (argc < 2)
	{
		PrintUsage();
		return 125;
	}

	const char* endpoint = nullptr;
	int         i        = 1;
	for (; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--endpoint") == 0)
		{
			const char* flag = argv[i];
			endpoint         = RequireArg(argc, argv, &i, flag);
			if (endpoint == nullptr)
			{
				return 125;
			}
			continue;
		}
		if (std::strncmp(argv[i], "--endpoint=", 11) == 0)
		{
			endpoint = argv[i] + 11;
			continue;
		}
		break;
	}

	if (endpoint == nullptr)
	{
		endpoint = std::getenv("KYTY_AGENT_ENDPOINT");
	}
	if (endpoint == nullptr || endpoint[0] == '\0')
	{
		std::fprintf(stderr, "kyty_agent: provide --endpoint ENDPOINT or KYTY_AGENT_ENDPOINT\n");
		PrintUsage();
		return 125;
	}
	if (std::strcmp(argv[1], "crash-context") == 0)
	{
		const char* path = nullptr;
		for (int index = 2; index < argc; ++index)
		{
			if (std::strcmp(argv[index], "--path") == 0 && index + 1 < argc)
			{
				path = argv[++index];
			} else if (std::strncmp(argv[index], "--path=", 7) == 0)
			{
				path = argv[index] + 7;
			} else
			{
				std::fprintf(stderr, "kyty_agent: unknown crash-context flag %s\n", argv[index]);
				return 125;
			}
		}
		if (path == nullptr || path[0] == '\0')
		{
			std::fprintf(stderr, "kyty_agent: crash-context requires --path ABS.json\n");
			return 125;
		}
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			std::printf("{\"ok\":false,\"error\":{\"code\":\"not_found\",\"path\":\"%s\"}}\n", JsonEscape(path).c_str());
			return 1;
		}
		std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if (content.size() > 1024u * 1024u)
		{
			std::fprintf(stderr, "kyty_agent: crash-context exceeds 1 MiB\n");
			return 1;
		}
		std::fwrite(content.data(), 1, content.size(), stdout);
		if (content.empty() || content.back() != '\n')
		{
			std::fputc('\n', stdout);
		}
		return 0;
	}
	if (i >= argc)
	{
		PrintUsage();
		return 125;
	}

	const char* cmd = argv[i++];
	if (std::strcmp(cmd, "help") == 0)
	{
		return Call(endpoint, "{\"id\":1,\"tool\":\"help\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "doctor") == 0)
	{
		return Doctor(endpoint);
	}
	if (std::strcmp(cmd, "ping") == 0)
	{
		return Call(endpoint, "{\"id\":1,\"tool\":\"ping\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "status") == 0)
	{
		return Call(endpoint, "{\"id\":1,\"tool\":\"status\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "diagnostics") == 0)
	{
		return Call(endpoint, "{\"id\":1,\"tool\":\"diagnostics\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "perf-snapshot") == 0)
	{
		bool reset = false;
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--reset") == 0)
			{
				reset = true;
				continue;
			}
			std::fprintf(stderr, "kyty_agent: unknown perf-snapshot flag %s\n", argv[i]);
			return 125;
		}
		return Call(endpoint, reset ? "{\"id\":1,\"tool\":\"perf_snapshot\",\"args\":{\"reset\":true}}"
		                            : "{\"id\":1,\"tool\":\"perf_snapshot\",\"args\":{\"reset\":false}}");
	}
	if (std::strcmp(cmd, "sync-waits") == 0)
	{
		return Call(endpoint, "{\"id\":1,\"tool\":\"sync_waits\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "threads") == 0)
	{
		return Call(endpoint, "{\"id\":1,\"tool\":\"threads\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "last-error") == 0)
	{
		return Call(endpoint, "{\"id\":1,\"tool\":\"last_error\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "events") == 0)
	{
		uint64_t last      = 50;
		uint64_t after_seq = 0;
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--last") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--last");
				if (v == nullptr)
				{
					return 125;
				}
				last = std::strtoull(v, nullptr, 10);
				continue;
			}
			if (std::strcmp(argv[i], "--after-seq") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--after-seq");
				if (v == nullptr)
				{
					return 125;
				}
				after_seq = std::strtoull(v, nullptr, 10);
				continue;
			}
			std::fprintf(stderr, "kyty_agent: unknown events flag %s\n", argv[i]);
			return 125;
		}
		char req[256];
		std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"events\",\"args\":{\"last\":%llu,\"after_seq\":%llu}}",
		              static_cast<unsigned long long>(last), static_cast<unsigned long long>(after_seq));
		return Call(endpoint, req);
	}
	if (std::strcmp(cmd, "snapshot") == 0)
	{
		uint64_t events_last      = 50;
		uint64_t events_after_seq = 0;
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--events") == 0)
			{
				const char* value = RequireArg(argc, argv, &i, "--events");
				if (value == nullptr || !ParseDecimalUnsigned(value, &events_last) || events_last < 1 || events_last > 128)
				{
					std::fprintf(stderr, "kyty_agent: --events must be a decimal unsigned integer in [1,128]\n");
					return 125;
				}
				continue;
			}
			if (std::strcmp(argv[i], "--after-seq") == 0)
			{
				const char* value = RequireArg(argc, argv, &i, "--after-seq");
				if (value == nullptr || !ParseDecimalUnsigned(value, &events_after_seq))
				{
					std::fprintf(stderr, "kyty_agent: --after-seq must be a decimal unsigned integer\n");
					return 125;
				}
				continue;
			}
			std::fprintf(stderr, "kyty_agent: unknown snapshot flag %s\n", argv[i]);
			return 125;
		}
		char req[256];
		std::snprintf(req, sizeof(req),
		              "{\"id\":1,\"tool\":\"debug_snapshot\",\"args\":{\"events_last\":%llu,\"events_after_seq\":%llu}}",
		              static_cast<unsigned long long>(events_last), static_cast<unsigned long long>(events_after_seq));
		return Call(endpoint, req);
	}
	if (std::strcmp(cmd, "capture") == 0)
	{
		uint64_t timeout_ms = 10000;
		bool     score      = true;
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--timeout-ms") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--timeout-ms");
				if (v == nullptr)
				{
					return 125;
				}
				timeout_ms = std::strtoull(v, nullptr, 10);
				continue;
			}
			if (std::strcmp(argv[i], "--no-score") == 0)
			{
				score = false;
				continue;
			}
			std::fprintf(stderr, "kyty_agent: unknown capture flag %s\n", argv[i]);
			return 125;
		}
		char req[256];
		std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"capture\",\"args\":{\"timeout_ms\":%llu,\"score\":%s}}",
		              static_cast<unsigned long long>(timeout_ms), score ? "true" : "false");
		return Call(endpoint, req);
	}
	if (std::strcmp(cmd, "score") == 0)
	{
		if (i != argc)
		{
			std::fprintf(stderr, "kyty_agent: score does not accept arguments; capture first\n");
			return 125;
		}
		return Call(endpoint, "{\"id\":1,\"tool\":\"score\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "pad") == 0)
	{
		if (i >= argc)
		{
			std::fprintf(stderr, "kyty_agent: pad requires an action\n");
			return 125;
		}
		const char* action = argv[i++];
		if (std::strcmp(action, "clear") == 0)
		{
			return Call(endpoint, "{\"id\":1,\"tool\":\"pad_clear\",\"args\":{}}");
		}
		if (std::strcmp(action, "axis") == 0)
		{
			if (i + 1 >= argc)
			{
				std::fprintf(stderr, "kyty_agent: pad axis requires AXIS VALUE\n");
				return 125;
			}
			const char* axis  = argv[i++];
			const char* value = argv[i++];
			char        req[256];
			std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"pad_axis\",\"args\":{\"axis\":\"%s\",\"value\":%s}}",
			              JsonEscape(axis).c_str(), value);
			return Call(endpoint, req);
		}
		if (std::strcmp(action, "hold") == 0)
		{
			// Client-side hold: pad_down → wait_present(delta) → pad_up.
			// Use for UI prompts like "(HOLD) Skip" without host sleep minutes.
			if (i >= argc)
			{
				std::fprintf(stderr, "kyty_agent: pad hold requires BUTTON\n");
				return 125;
			}
			const char* button     = argv[i++];
			uint64_t    delta      = 0;
			uint64_t    timeout_ms = 15000;
			bool        have_delta = false;
			for (; i < argc; ++i)
			{
				if (std::strcmp(argv[i], "--delta") == 0)
				{
					const char* v = RequireArg(argc, argv, &i, "--delta");
					if (v == nullptr)
					{
						return 125;
					}
					delta      = std::strtoull(v, nullptr, 10);
					have_delta = true;
					continue;
				}
				if (std::strcmp(argv[i], "--timeout-ms") == 0)
				{
					const char* v = RequireArg(argc, argv, &i, "--timeout-ms");
					if (v == nullptr)
					{
						return 125;
					}
					timeout_ms = std::strtoull(v, nullptr, 10);
					continue;
				}
				std::fprintf(stderr, "kyty_agent: unknown pad hold flag %s\n", argv[i]);
				return 125;
			}
			if (!have_delta || delta == 0)
			{
				std::fprintf(stderr, "kyty_agent: pad hold requires --delta N (presents to hold across)\n");
				return 125;
			}
			char req[256];
			std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"pad_down\",\"args\":{\"button\":\"%s\"}}", JsonEscape(button).c_str());
			const int down_rc = Call(endpoint, req);
			if (down_rc != 0)
			{
				return down_rc;
			}
			std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"wait_present\",\"args\":{\"delta\":%llu,\"timeout_ms\":%llu}}",
			              static_cast<unsigned long long>(delta), static_cast<unsigned long long>(timeout_ms));
			const int wait_rc = Call(endpoint, req);
			std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"pad_up\",\"args\":{\"button\":\"%s\"}}", JsonEscape(button).c_str());
			const int up_rc = Call(endpoint, req);
			if (wait_rc != 0)
			{
				return wait_rc;
			}
			return up_rc;
		}
		if (std::strcmp(action, "down") == 0 || std::strcmp(action, "up") == 0 || std::strcmp(action, "tap") == 0)
		{
			if (i >= argc)
			{
				std::fprintf(stderr, "kyty_agent: pad %s requires BUTTON\n", action);
				return 125;
			}
			const char* button = argv[i++];
			if (i != argc)
			{
				std::fprintf(stderr, "kyty_agent: unexpected pad argument %s\n", argv[i]);
				return 125;
			}
			char req[256];
			if (std::strcmp(action, "tap") == 0)
			{
				std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"pad_tap\",\"args\":{\"button\":\"%s\"}}", JsonEscape(button).c_str());
			} else
			{
				std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"pad_%s\",\"args\":{\"button\":\"%s\"}}", action,
				              JsonEscape(button).c_str());
			}
			return Call(endpoint, req);
		}
		std::fprintf(stderr, "kyty_agent: unknown pad action %s\n", action);
		return 125;
	}
	if (std::strcmp(cmd, "wait-ready") == 0)
	{
		uint64_t timeout_ms = 30000;
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--timeout-ms") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--timeout-ms");
				if (v == nullptr)
				{
					return 125;
				}
				timeout_ms = std::strtoull(v, nullptr, 10);
				continue;
			}
			std::fprintf(stderr, "kyty_agent: unknown wait-ready flag %s\n", argv[i]);
			return 125;
		}
		return WaitReady(endpoint, timeout_ms);
	}
	if (std::strcmp(cmd, "wait-present") == 0 || std::strcmp(cmd, "wait-frame") == 0)
	{
		uint64_t min_value  = 0;
		uint64_t delta      = 0;
		uint64_t timeout_ms = 15000;
		bool     have_min   = false;
		bool     have_delta = false;
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--min") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--min");
				if (v == nullptr)
				{
					return 125;
				}
				min_value = std::strtoull(v, nullptr, 10);
				have_min  = true;
				continue;
			}
			if (std::strcmp(argv[i], "--delta") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--delta");
				if (v == nullptr)
				{
					return 125;
				}
				delta      = std::strtoull(v, nullptr, 10);
				have_delta = true;
				continue;
			}
			if (std::strcmp(argv[i], "--timeout-ms") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--timeout-ms");
				if (v == nullptr)
				{
					return 125;
				}
				timeout_ms = std::strtoull(v, nullptr, 10);
				continue;
			}
			std::fprintf(stderr, "kyty_agent: unknown wait flag %s\n", argv[i]);
			return 125;
		}
		if (!have_min && !have_delta)
		{
			std::fprintf(stderr, "kyty_agent: %s requires --min or --delta\n", cmd);
			return 125;
		}
		char req[320];
		if (have_delta)
		{
			std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"%s\",\"args\":{\"delta\":%llu,\"timeout_ms\":%llu}}",
			              std::strcmp(cmd, "wait-present") == 0 ? "wait_present" : "wait_frame", static_cast<unsigned long long>(delta),
			              static_cast<unsigned long long>(timeout_ms));
		} else
		{
			std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"%s\",\"args\":{\"min\":%llu,\"timeout_ms\":%llu}}",
			              std::strcmp(cmd, "wait-present") == 0 ? "wait_present" : "wait_frame", static_cast<unsigned long long>(min_value),
			              static_cast<unsigned long long>(timeout_ms));
		}
		return Call(endpoint, req);
	}
	if (std::strcmp(cmd, "wait-phase") == 0)
	{
		const char* want       = nullptr;
		uint64_t    timeout_ms = 45000;
		uint64_t    stable_ms  = 400;
		uint64_t    min_fps    = 5;
		if (i < argc && argv[i][0] != '-')
		{
			want = argv[i++];
		}
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--want") == 0)
			{
				want = RequireArg(argc, argv, &i, "--want");
				if (want == nullptr)
				{
					return 125;
				}
				continue;
			}
			if (std::strcmp(argv[i], "--timeout-ms") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--timeout-ms");
				if (v == nullptr)
				{
					return 125;
				}
				timeout_ms = std::strtoull(v, nullptr, 10);
				continue;
			}
			if (std::strcmp(argv[i], "--stable-ms") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--stable-ms");
				if (v == nullptr)
				{
					return 125;
				}
				stable_ms = std::strtoull(v, nullptr, 10);
				continue;
			}
			if (std::strcmp(argv[i], "--min-fps") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--min-fps");
				if (v == nullptr)
				{
					return 125;
				}
				min_fps = std::strtoull(v, nullptr, 10);
				continue;
			}
			std::fprintf(stderr, "kyty_agent: unknown wait-phase flag %s\n", argv[i]);
			return 125;
		}
		if (want == nullptr)
		{
			std::fprintf(stderr, "kyty_agent: wait-phase requires WANT (interactive|loading|booting|stalled|not_ready)\n");
			return 125;
		}
		char req[384];
		std::snprintf(req, sizeof(req),
		              "{\"id\":1,\"tool\":\"wait_phase\",\"args\":{\"want\":\"%s\",\"timeout_ms\":%llu,"
		              "\"stable_ms\":%llu,\"min_fps\":%llu}}",
		              JsonEscape(want).c_str(), static_cast<unsigned long long>(timeout_ms), static_cast<unsigned long long>(stable_ms),
		              static_cast<unsigned long long>(min_fps));
		return Call(endpoint, req);
	}
	if (std::strcmp(cmd, "wait-event") == 0)
	{
		const char* kind       = nullptr;
		const char* code       = nullptr;
		uint64_t    after_seq  = 0;
		uint64_t    timeout_ms = 5000;
		bool        have_after_seq = false;
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--kind") == 0)
			{
				kind = RequireArg(argc, argv, &i, "--kind");
				if (kind == nullptr)
				{
					return 125;
				}
				continue;
			}
			if (std::strcmp(argv[i], "--code") == 0)
			{
				code = RequireArg(argc, argv, &i, "--code");
				if (code == nullptr || code[0] == '\0' || std::strlen(code) > 31u)
				{
					std::fprintf(stderr, "kyty_agent: --code must contain 1-31 bytes\n");
					return 125;
				}
				continue;
			}
			if (std::strcmp(argv[i], "--after-seq") == 0)
			{
				const char* value = RequireArg(argc, argv, &i, "--after-seq");
				if (value == nullptr || !ParseDecimalUnsigned(value, &after_seq))
				{
					std::fprintf(stderr, "kyty_agent: --after-seq must be a decimal unsigned integer\n");
					return 125;
				}
				have_after_seq = true;
				continue;
			}
			if (std::strcmp(argv[i], "--timeout-ms") == 0)
			{
				const char* v = RequireArg(argc, argv, &i, "--timeout-ms");
				if (v == nullptr || !ParseDecimalUnsigned(v, &timeout_ms))
				{
					std::fprintf(stderr, "kyty_agent: --timeout-ms must be a decimal unsigned integer\n");
					return 125;
				}
				continue;
			}
			std::fprintf(stderr, "kyty_agent: unknown wait-event flag %s\n", argv[i]);
			return 125;
		}
		if (kind == nullptr)
		{
			std::fprintf(stderr, "kyty_agent: wait-event requires --kind\n");
			return 125;
		}
		std::string req = "{\"id\":1,\"tool\":\"wait_event\",\"args\":{\"kind\":\"";
		req += JsonEscape(kind);
		req += '"';
		if (code != nullptr)
		{
			req += ",\"code\":\"";
			req += JsonEscape(code);
			req += '"';
		}
		if (have_after_seq)
		{
			req += ",\"after_seq\":";
			req += std::to_string(after_seq);
		}
		req += ",\"timeout_ms\":";
		req += std::to_string(timeout_ms);
		req += "}}";
		return Call(endpoint, req);
	}
	if (std::strcmp(cmd, "watch") == 0)
	{
		WatchOptions options {};
		if (!ParseWatchOptions(argc, argv, &i, &options))
		{
			return 125;
		}
		char req[512];
		std::snprintf(req, sizeof(req),
		              "{\"id\":1,\"tool\":\"watch\",\"args\":{\"window_ms\":%llu,\"present_stall_ms\":%llu,"
		              "\"frame_stall_ms\":%llu,\"min_fps\":%llu,\"capture\":%s}}",
		              static_cast<unsigned long long>(options.window_ms), static_cast<unsigned long long>(options.present_stall_ms),
		              static_cast<unsigned long long>(options.frame_stall_ms), static_cast<unsigned long long>(options.min_fps),
		              options.capture ? "true" : "false");
		return Call(endpoint, req);
	}

	std::fprintf(stderr, "kyty_agent: unknown command %s\n", cmd);
	PrintUsage();
	return 125;
}

} // namespace Kyty::AgentCli
