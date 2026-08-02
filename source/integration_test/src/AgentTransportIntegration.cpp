#include "Kyty/Agent/Cli.h"
#include "Kyty/Agent/LocalTransport.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace Transport = Kyty::Agent::LocalTransport;

namespace {

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "agent transport integration failure: %s\n", message);
	std::fflush(stderr);
	std::_Exit(1);
}

void Expect(bool condition, const char* message)
{
	if (!condition)
	{
		Die(message);
	}
}

std::string UniqueEndpoint(const char* suffix)
{
	const auto stamp = static_cast<unsigned long long>(
	    std::chrono::steady_clock::now().time_since_epoch().count());
#if defined(_WIN32)
	return "\\\\.\\pipe\\kyty-agent-" + std::to_string(stamp) + "-" + suffix;
#else
	return "/tmp/kyty-agent-" + std::to_string(stamp) + "-" + suffix + ".sock";
#endif
}

void RunRoundTrip(const std::string& endpoint)
{
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen round trip");

	std::atomic<bool> server_ok {false};
	std::thread server([&]() {
		Transport::Connection connection {};
		if (Transport::Accept(&listener, &connection) != Transport::Result::Ok)
		{
			return;
		}
		std::string request;
		if (Transport::ReadLine(&connection, &request, 4096) != Transport::Result::Ok || request != "request")
		{
			Transport::Close(&connection);
			return;
		}
		const std::string response = "response\n";
		server_ok = Transport::WriteAll(&connection, response.data(), response.size()) == Transport::Result::Ok;
		Transport::Close(&connection);
	});

	Transport::Connection client {};
	for (uint32_t attempt = 0; attempt < 100; ++attempt)
	{
		if (Transport::Connect(endpoint.c_str(), &client) == Transport::Result::Ok)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	Expect(client.native != 0, "connect round trip");
	const std::string request = "request\n";
	Expect(Transport::WriteAll(&client, request.data(), request.size()) == Transport::Result::Ok, "write round trip");
	std::string response;
	Expect(Transport::ReadLine(&client, &response, 4096) == Transport::Result::Ok, "read round trip");
	Expect(response == "response", "round trip payload");
	Transport::Close(&client);
	server.join();
	Expect(server_ok.load(), "server completed round trip");
	Transport::Close(&listener);
}

void RunInterrupt(const std::string& endpoint)
{
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen interrupt");

	std::atomic<bool> entered {false};
	std::atomic<bool> interrupted {false};
	std::thread server([&]() {
		entered = true;
		Transport::Connection connection {};
		interrupted = Transport::Accept(&listener, &connection) == Transport::Result::Interrupted;
		Transport::Close(&connection);
	});

	while (!entered.load())
	{
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	Transport::Interrupt(&listener);
	server.join();
	Expect(interrupted.load(), "blocked accept interrupted");
	Transport::Close(&listener);
}

void RunCliWaitReady(const std::string& endpoint)
{
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen cli wait-ready");

	std::atomic<bool> server_ok {false};
	std::thread server([&]() {
		for (uint32_t attempt = 0; attempt < 32; ++attempt)
		{
			Transport::Connection connection {};
			if (Transport::Accept(&listener, &connection) != Transport::Result::Ok)
			{
				return;
			}
			std::string request;
			const auto result = Transport::ReadLine(&connection, &request, 4096);
			if (result == Transport::Result::Ok && !request.empty())
			{
				const std::string response =
				    "{\"id\":1,\"ok\":true,\"protocol_version\":6,\"result\":{\"alive\":true}}\n";
				server_ok = request.find("\"tool\":\"ping\"") != std::string::npos &&
				            Transport::WriteAll(&connection, response.data(), response.size()) == Transport::Result::Ok;
				Transport::Close(&connection);
				return;
			}
			Transport::Close(&connection);
		}
	});

	char arg0[] = "kyty_agent";
	char arg1[] = "--endpoint";
	char arg3[] = "wait-ready";
	char arg4[] = "--timeout-ms";
	char arg5[] = "3000";
	char* argv[] = {arg0, arg1, const_cast<char*>(endpoint.c_str()), arg3, arg4, arg5};
	const int rc = Kyty::AgentCli::Main(6, argv);
	server.join();
	Expect(rc == 0, "cli wait-ready exit");
	Expect(server_ok.load(), "cli sent exactly one valid ping request");
	Transport::Close(&listener);
}

void RunCliDebugSnapshotWorkflow(const std::string& endpoint)
{
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen cli debug snapshot");

	std::atomic<bool> server_ok {false};
	std::thread server([&]() {
		bool workflow_ok = true;
		for (uint32_t step = 0; step < 2; ++step)
		{
			Transport::Connection connection {};
			if (Transport::Accept(&listener, &connection) != Transport::Result::Ok)
			{
				workflow_ok = false;
				break;
			}
			std::string request;
			if (Transport::ReadLine(&connection, &request, 4096) != Transport::Result::Ok)
			{
				workflow_ok = false;
				Transport::Close(&connection);
				break;
			}

			std::string response;
			if (step == 0)
			{
				workflow_ok = workflow_ok && request.find("\"tool\":\"help\"") != std::string::npos;
				response =
				    "{\"id\":1,\"ok\":true,\"protocol_version\":6,\"result\":{\"schema\":\"agent_tools\","
				    "\"capabilities\":{\"debug_snapshot\":true}}}\n";
				workflow_ok = workflow_ok && response.find("\"debug_snapshot\":true") != std::string::npos;
			} else
			{
				workflow_ok = workflow_ok && request.find("\"tool\":\"debug_snapshot\"") != std::string::npos &&
				              request.find("\"events_last\":100") != std::string::npos;
				response =
				    "{\"id\":1,\"ok\":true,\"protocol_version\":6,\"result\":{\"protocol_version\":6,"
				    "\"schema\":\"debug_snapshot\",\"event_seq_start\":8,\"event_seq_end\":8,"
				    "\"stable\":true,\"status\":null,\"diagnostics\":null,\"threads\":null,"
				    "\"sync_waits\":null,\"events\":null,\"last_error\":null}}\n";
				const char* required[] = {"\"schema\":\"debug_snapshot\"", "\"event_seq_start\":", "\"event_seq_end\":"};
				for (const char* field: required)
				{
					workflow_ok = workflow_ok && response.find(field) != std::string::npos;
				}
			}
			const bool write_ok = Transport::WriteAll(&connection, response.data(), response.size()) == Transport::Result::Ok;
			workflow_ok = workflow_ok && write_ok;
			Transport::Close(&connection);
		}
		server_ok = workflow_ok;
	});

	char arg0[] = "kyty_agent";
	char arg1[] = "--endpoint";
	char arg3[] = "help";
	char* help_argv[] = {arg0, arg1, const_cast<char*>(endpoint.c_str()), arg3};
	const int help_rc = Kyty::AgentCli::Main(4, help_argv);
	Expect(help_rc == 0, "cli help exit");

	char arg4[] = "snapshot";
	char arg5[] = "--events";
	char arg6[] = "100";
	char* snapshot_argv[] = {arg0, arg1, const_cast<char*>(endpoint.c_str()), arg4, arg5, arg6};
	const int snapshot_rc = Kyty::AgentCli::Main(6, snapshot_argv);
	Expect(snapshot_rc == 0, "cli snapshot exit");

	server.join();
	Expect(server_ok.load(), "help and debug snapshot workflow");
	Transport::Close(&listener);
}

} // namespace

int main()
{
	Expect(Transport::IsValidEndpoint(UniqueEndpoint("valid").c_str()), "platform endpoint accepted");
	Expect(!Transport::IsValidEndpoint("relative-endpoint"), "relative endpoint rejected");
	RunRoundTrip(UniqueEndpoint("roundtrip"));
	RunInterrupt(UniqueEndpoint("interrupt"));
	RunCliWaitReady(UniqueEndpoint("wait-ready"));
	RunCliDebugSnapshotWorkflow(UniqueEndpoint("debug-snapshot"));
	std::puts("agent transport integration passed");
	return 0;
}
