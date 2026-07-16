#include "Kyty/Agent/Cli.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
namespace Kyty::AgentCli {

int Main(int /*argc*/, char** /*argv*/)
{
	std::fprintf(stderr, "kyty_agent is POSIX-only in this build\n");
	return 125;
}

} // namespace Kyty::AgentCli
#else

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace Kyty::AgentCli {
namespace {

void PrintUsage()
{
	std::fprintf(stderr,
	             "kyty_agent — realtime Kyty emulator tools (local Unix socket)\n"
	             "\n"
	             "Usage:\n"
	             "  kyty_agent --sock ABS_PATH help\n"
	             "  kyty_agent --sock ABS_PATH doctor\n"
	             "  kyty_agent --sock ABS_PATH ping\n"
	             "  kyty_agent --sock ABS_PATH status\n"
	             "  kyty_agent --sock ABS_PATH diagnostics\n"
	             "  kyty_agent --sock ABS_PATH sync-waits\n"
	             "  kyty_agent --sock ABS_PATH threads\n"
	             "  kyty_agent --sock ABS_PATH events [--last N] [--after-seq N]\n"
	             "  kyty_agent --sock ABS_PATH last-error\n"
	             "  kyty_agent --sock ABS_PATH capture [--timeout-ms N] [--no-score]\n"
	             "  kyty_agent --sock ABS_PATH score [--path ABS.bmp]\n"
	             "  kyty_agent --sock ABS_PATH pad down|up|tap BUTTON\n"
	             "  kyty_agent --sock ABS_PATH pad axis AXIS VALUE\n"
	             "  kyty_agent --sock ABS_PATH pad clear\n"
	             "  kyty_agent --sock ABS_PATH wait-present --min N [--timeout-ms N]\n"
	             "  kyty_agent --sock ABS_PATH wait-frame --min N [--timeout-ms N]\n"
	             "  kyty_agent --sock ABS_PATH wait-event --kind KIND [--timeout-ms N]\n"
	             "  kyty_agent --sock ABS_PATH watch [--window-ms N|--seconds N] [--present-stall-ms N] [--frame-stall-ms N] [--min-fps N] [--no-capture]\n"
	             "\n"
	             "Requires the emulator started with KYTY_AGENT_SOCK=ABS_PATH.\n"
	             "Pad input is diagnostic_input, not gameplay acceptance.\n"
	             "watch exits 1 when present/frame/fps look stalled (loading hang).\n"
	             "capture/score exit 1 when frame metrics look corrupted (healthy:false).\n");
}

std::string JsonEscape(const char* value)
{
	std::string out;
	if (value == nullptr)
	{
		return out;
	}
	for (const char* p = value; *p != '\0'; ++p)
	{
		switch (*p)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\t': out += "\\t"; break;
			default: out.push_back(*p); break;
		}
	}
	return out;
}

bool WriteAll(int fd, const std::string& payload)
{
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
	while (out->size() < 8192)
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

int Connect(const char* path)
{
	if (path == nullptr || path[0] != '/')
	{
		std::fprintf(stderr, "kyty_agent: --sock must be an absolute path\n");
		return -1;
	}
	const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
	{
		std::fprintf(stderr, "kyty_agent: socket failed: %s\n", std::strerror(errno));
		return -1;
	}
	sockaddr_un addr {};
	addr.sun_family = AF_UNIX;
	if (std::strlen(path) >= sizeof(addr.sun_path))
	{
		std::fprintf(stderr, "kyty_agent: socket path too long\n");
		::close(fd);
		return -1;
	}
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
	{
		std::fprintf(stderr, "kyty_agent: connect(%s) failed: %s\n", path, std::strerror(errno));
		::close(fd);
		return -1;
	}
	return fd;
}

int Call(const char* sock, const std::string& request_line)
{
	const int fd = Connect(sock);
	if (fd < 0)
	{
		return 125;
	}
	std::string payload = request_line;
	payload.push_back('\n');
	if (!WriteAll(fd, payload))
	{
		std::fprintf(stderr, "kyty_agent: write failed\n");
		::close(fd);
		return 125;
	}
	std::string response;
	if (!ReadLine(fd, &response))
	{
		std::fprintf(stderr, "kyty_agent: read failed\n");
		::close(fd);
		return 125;
	}
	::close(fd);
	std::puts(response.c_str());
	if (response.find("\"ok\":false") != std::string::npos)
	{
		return 1;
	}
	if (response.find("\"healthy\":false") != std::string::npos)
	{
		return 1;
	}
	return 0;
}

int Doctor(const char* sock)
{
	const int ping_rc = Call(sock, "{\"id\":1,\"tool\":\"ping\",\"args\":{}}");
	if (ping_rc != 0)
	{
		return ping_rc;
	}
	return Call(sock, "{\"id\":2,\"tool\":\"status\",\"args\":{}}");
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

} // namespace

int Main(int argc, char** argv)
{
	if (argc < 2)
	{
		PrintUsage();
		return 125;
	}

	const char* sock = nullptr;
	int         i    = 1;
	for (; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--sock") == 0)
		{
			sock = RequireArg(argc, argv, &i, "--sock");
			if (sock == nullptr)
			{
				return 125;
			}
			continue;
		}
		if (std::strncmp(argv[i], "--sock=", 7) == 0)
		{
			sock = argv[i] + 7;
			continue;
		}
		break;
	}

	if (sock == nullptr)
	{
		sock = std::getenv("KYTY_AGENT_SOCK");
	}
	if (sock == nullptr || sock[0] == '\0')
	{
		std::fprintf(stderr, "kyty_agent: provide --sock ABS_PATH or KYTY_AGENT_SOCK\n");
		PrintUsage();
		return 125;
	}
	if (i >= argc)
	{
		PrintUsage();
		return 125;
	}

	const char* cmd = argv[i++];
	if (std::strcmp(cmd, "help") == 0)
	{
		return Call(sock, "{\"id\":1,\"tool\":\"help\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "doctor") == 0)
	{
		return Doctor(sock);
	}
	if (std::strcmp(cmd, "ping") == 0)
	{
		return Call(sock, "{\"id\":1,\"tool\":\"ping\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "status") == 0)
	{
		return Call(sock, "{\"id\":1,\"tool\":\"status\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "diagnostics") == 0)
	{
		return Call(sock, "{\"id\":1,\"tool\":\"diagnostics\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "sync-waits") == 0)
	{
		return Call(sock, "{\"id\":1,\"tool\":\"sync_waits\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "threads") == 0)
	{
		return Call(sock, "{\"id\":1,\"tool\":\"threads\",\"args\":{}}");
	}
	if (std::strcmp(cmd, "last-error") == 0)
	{
		return Call(sock, "{\"id\":1,\"tool\":\"last_error\",\"args\":{}}");
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
		return Call(sock, req);
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
		return Call(sock, req);
	}
	if (std::strcmp(cmd, "score") == 0)
	{
		const char* path = nullptr;
		for (; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "--path") == 0)
			{
				path = RequireArg(argc, argv, &i, "--path");
				if (path == nullptr)
				{
					return 125;
				}
				continue;
			}
			std::fprintf(stderr, "kyty_agent: unknown score flag %s\n", argv[i]);
			return 125;
		}
		if (path != nullptr)
		{
			char req[1024];
			std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"score\",\"args\":{\"path\":\"%s\"}}", JsonEscape(path).c_str());
			return Call(sock, req);
		}
		return Call(sock, "{\"id\":1,\"tool\":\"score\",\"args\":{}}");
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
			return Call(sock, "{\"id\":1,\"tool\":\"pad_clear\",\"args\":{}}");
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
			return Call(sock, req);
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
				std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"pad_tap\",\"args\":{\"button\":\"%s\"}}",
				              JsonEscape(button).c_str());
			} else
			{
				std::snprintf(req, sizeof(req), "{\"id\":1,\"tool\":\"pad_%s\",\"args\":{\"button\":\"%s\"}}", action,
				              JsonEscape(button).c_str());
			}
			return Call(sock, req);
		}
		std::fprintf(stderr, "kyty_agent: unknown pad action %s\n", action);
		return 125;
	}
	if (std::strcmp(cmd, "wait-present") == 0 || std::strcmp(cmd, "wait-frame") == 0)
	{
		uint64_t min_value  = 0;
		uint64_t timeout_ms = 30000;
		bool     have_min   = false;
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
		if (!have_min)
		{
			std::fprintf(stderr, "kyty_agent: %s requires --min\n", cmd);
			return 125;
		}
		char req[256];
		std::snprintf(req, sizeof(req),
		              "{\"id\":1,\"tool\":\"%s\",\"args\":{\"min\":%llu,\"timeout_ms\":%llu}}",
		              std::strcmp(cmd, "wait-present") == 0 ? "wait_present" : "wait_frame",
		              static_cast<unsigned long long>(min_value), static_cast<unsigned long long>(timeout_ms));
		return Call(sock, req);
	}
	if (std::strcmp(cmd, "wait-event") == 0)
	{
		const char* kind        = nullptr;
		uint64_t    timeout_ms  = 5000;
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
			std::fprintf(stderr, "kyty_agent: unknown wait-event flag %s\n", argv[i]);
			return 125;
		}
		if (kind == nullptr)
		{
			std::fprintf(stderr, "kyty_agent: wait-event requires --kind\n");
			return 125;
		}
		char req[256];
		std::snprintf(req, sizeof(req),
		              "{\"id\":1,\"tool\":\"wait_event\",\"args\":{\"kind\":\"%s\",\"timeout_ms\":%llu}}",
		              JsonEscape(kind).c_str(), static_cast<unsigned long long>(timeout_ms));
		return Call(sock, req);
	}
	if (std::strcmp(cmd, "watch") == 0)
	{
		uint64_t window_ms        = 10000;
		uint64_t present_stall_ms = 5000;
		uint64_t frame_stall_ms   = 5000;
		uint64_t min_fps          = 2;
		bool     capture          = true;
		for (; i < argc; ++i)
		{
			uint64_t* target = nullptr;
			const char* flag = argv[i];
			if (std::strcmp(flag, "--window-ms") == 0)
			{
				target = &window_ms;
			} else if (std::strcmp(flag, "--seconds") == 0)
			{
				const char* value = RequireArg(argc, argv, &i, flag);
				if (value == nullptr)
				{
					return 125;
				}
				window_ms = std::strtoull(value, nullptr, 10) * 1000ull;
				continue;
			} else if (std::strcmp(flag, "--present-stall-ms") == 0)
			{
				target = &present_stall_ms;
			} else if (std::strcmp(flag, "--frame-stall-ms") == 0)
			{
				target = &frame_stall_ms;
			} else if (std::strcmp(flag, "--min-fps") == 0)
			{
				target = &min_fps;
			} else if (std::strcmp(flag, "--no-capture") == 0)
			{
				capture = false;
				continue;
			} else
			{
				std::fprintf(stderr, "kyty_agent: unknown watch flag %s\n", flag);
				return 125;
			}
			const char* value = RequireArg(argc, argv, &i, flag);
			if (value == nullptr)
			{
				return 125;
			}
			*target = std::strtoull(value, nullptr, 10);
		}
		char req[512];
		std::snprintf(req, sizeof(req),
		              "{\"id\":1,\"tool\":\"watch\",\"args\":{\"window_ms\":%llu,\"present_stall_ms\":%llu,"
		              "\"frame_stall_ms\":%llu,\"min_fps\":%llu,\"capture\":%s}}",
		              static_cast<unsigned long long>(window_ms), static_cast<unsigned long long>(present_stall_ms),
		              static_cast<unsigned long long>(frame_stall_ms), static_cast<unsigned long long>(min_fps),
		              capture ? "true" : "false");
		return Call(sock, req);
	}

	std::fprintf(stderr, "kyty_agent: unknown command %s\n", cmd);
	PrintUsage();
	return 125;
}

} // namespace Kyty::AgentCli

#endif // !_WIN32
