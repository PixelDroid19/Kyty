#include "Kyty/Agent/Cli.h"
#include "Kyty/Agent/LocalTransport.h"
#include "Kyty/Agent/WireContract.h"

#include "Emulator/Agent/AgentServer.h"
#include "Emulator/Agent/Protocol.h"
#include "Emulator/Controller.h"

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
				const std::string response = "{\"id\":1,\"ok\":true,\"protocol_version\":" +
				                             std::to_string(Kyty::Agent::kProtocolVersion) +
				                             ",\"result\":{\"alive\":true}}\n";
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

			const std::string response = Kyty::Emulator::Agent::Internal::DispatchLine(request);
			Kyty::Emulator::Agent::Request   parsed_request {};
			Kyty::Emulator::Agent::ErrorInfo parse_error {};
			const bool                       parsed = Kyty::Emulator::Agent::ParseRequestLine(request.c_str(), &parsed_request, &parse_error);
			if (!parsed)
			{
				workflow_ok = false;
			} else if (step == 0)
			{
				workflow_ok = workflow_ok && parsed_request.kind == Kyty::Emulator::Agent::Tool::Help &&
				              parsed_request.tool == "help" && parsed_request.args_json == "{}";
				workflow_ok = workflow_ok && response.find("\"capabilities\":") != std::string::npos &&
				              response.find("\"debug_snapshot\":true") != std::string::npos;
			} else
			{
				uint32_t events_last = 0;
				workflow_ok = workflow_ok && parsed_request.kind == Kyty::Emulator::Agent::Tool::DebugSnapshot &&
				              parsed_request.tool == "debug_snapshot" &&
				              Kyty::Emulator::Agent::ArgsGetU32(parsed_request.args_json, "events_last", &events_last) &&
				              events_last == 100;
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

void RunCliTraceRtLifetimeArm(const std::string& endpoint)
{
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen cli lifetime arm");

	std::atomic<bool> server_ok {false};
	std::thread server([&]() {
		Transport::Connection connection {};
		if (Transport::Accept(&listener, &connection) != Transport::Result::Ok)
		{
			return;
		}
		std::string request;
		if (Transport::ReadLine(&connection, &request, 4096) != Transport::Result::Ok)
		{
			Transport::Close(&connection);
			return;
		}
		const std::string response = "{\"id\":1,\"ok\":true,\"protocol_version\":" +
		                             std::to_string(Kyty::Agent::kProtocolVersion) +
		                             ",\"result\":{\"accepted\":true,\"pending\":true,\"one_shot\":true}}\n";
		server_ok = request == R"({"id":1,"tool":"trace_rt_lifetime_arm","args":{}})" &&
		            Transport::WriteAll(&connection, response.data(), response.size()) == Transport::Result::Ok;
		Transport::Close(&connection);
	});

	char  arg0[] = "kyty_agent";
	char  arg1[] = "--endpoint";
	char  arg3[] = "trace-rt-lifetime-arm";
	char* argv[] = {arg0, arg1, const_cast<char*>(endpoint.c_str()), arg3};
	const int rc = Kyty::AgentCli::Main(4, argv);
	if (rc != 0)
	{
		Transport::Interrupt(&listener);
	}
	server.join();
	Expect(rc == 0, "cli lifetime arm exit");
	Expect(server_ok.load(), "cli sends exactly one no-argument lifetime arm request");
	Transport::Close(&listener);
}

void RunCliScheduledPresentTap(const std::string& endpoint)
{
	using Kyty::Libs::Controller::AgentPadApplyReadStateSample;
	using Kyty::Libs::Controller::AgentPadClear;
	using Kyty::Libs::Controller::AgentPadOnPresent;
	using Kyty::Libs::Controller::PAD_BUTTON_CROSS;

	AgentPadClear();
	const std::string help = Kyty::Emulator::Agent::Internal::DispatchLine(R"({"id":1,"tool":"help","args":{}})");
	Expect(help.find("\"scheduled_present_taps\":true") != std::string::npos && help.find("\"at_present\":8000") != std::string::npos,
	       "help advertises the bounded present-addressed tap capability");
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen scheduled tap");

	std::atomic<bool> server_ok {false};
	std::thread server([&]() {
		Transport::Connection connection {};
		if (Transport::Accept(&listener, &connection) != Transport::Result::Ok)
		{
			return;
		}
		std::string request;
		if (Transport::ReadLine(&connection, &request, 4096) != Transport::Result::Ok)
		{
			Transport::Close(&connection);
			return;
		}
		const std::string response = Kyty::Emulator::Agent::Internal::DispatchLine(request);
		const std::string payload  = response + "\n";
		const bool        wrote    = Transport::WriteAll(&connection, payload.data(), payload.size()) == Transport::Result::Ok;
		const bool expected_request = request.find("\"tool\":\"pad_tap\"") != std::string::npos &&
		                              request.find("\"button\":\"cross\"") != std::string::npos &&
		                              request.find("\"at_present\":8000") != std::string::npos &&
		                              request.find("\"repeat\":2") != std::string::npos &&
		                              request.find("\"present_delta\":40") != std::string::npos;
		server_ok = expected_request && response.find("\"ok\":true") != std::string::npos && wrote;
		Transport::Close(&connection);
	});

	char arg0[]  = "kyty_agent";
	char arg1[]  = "--endpoint";
	char arg3[]  = "pad";
	char arg4[]  = "tap";
	char arg5[]  = "cross";
	char arg6[]  = "--at-present";
	char arg7[]  = "8000";
	char arg8[]  = "--repeat";
	char arg9[]  = "2";
	char arg10[] = "--present-delta";
	char arg11[] = "40";
	char* argv[] = {arg0, arg1, const_cast<char*>(endpoint.c_str()), arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
	Expect(Kyty::AgentCli::Main(12, argv) == 0, "cli scheduled tap exits successfully");
	server.join();
	Expect(server_ok.load(), "cli commits one scheduled pad_tap request");
	Transport::Close(&listener);

	// The response commits the fixed schedule: closing the request socket does
	// not own, delay, or cancel either target.
	const std::string status_before = Kyty::Emulator::Agent::Internal::DispatchLine(R"({"id":5,"tool":"status","args":{}})");
	Expect(status_before.find("\"delivered_taps\":0") != std::string::npos &&
	           status_before.find("\"scheduled_taps\":2") != std::string::npos &&
	           status_before.find("\"next_target_present\":8000") != std::string::npos,
	       "status reports the committed schedule before any guest-delivered pulse");
	const std::string duplicate = Kyty::Emulator::Agent::Internal::DispatchLine(
	    R"({"id":10,"tool":"pad_tap","args":{"button":"cross","at_present":8040,"repeat":1,"present_delta":0}})");
	Expect(duplicate.find("\"code\":\"schedule_conflict\"") != std::string::npos,
	       "duplicate present reservation is rejected atomically");
	const std::string unknown = Kyty::Emulator::Agent::Internal::DispatchLine(
	    R"({"id":11,"tool":"pad_tap","args":{"button":"cross","at_present":8060,"repeat":1,"present_delta":0,"extra":1}})");
	Expect(unknown.find("\"code\":\"invalid_args\"") != std::string::npos,
	       "unknown scheduled tap argument is rejected before queue mutation");
	const std::string status_still_two =
	    Kyty::Emulator::Agent::Internal::DispatchLine(R"({"id":12,"tool":"status","args":{}})");
	Expect(status_still_two.find("\"scheduled_taps\":2") != std::string::npos,
	       "rejected duplicate and unknown arguments leave the schedule unchanged");

	const auto before_first = AgentPadOnPresent(7999);
	Expect(before_first.started == 0 && before_first.cancelled == 0, "tap does not start before its absolute present");
	const auto first = AgentPadOnPresent(8000);
	Expect(first.started == 1 && first.cancelled == 0 && first.started_target_present == 8000,
	       "first tap starts on exactly present 8000");
	const std::string status_armed = Kyty::Emulator::Agent::Internal::DispatchLine(R"({"id":6,"tool":"status","args":{}})");
	Expect(status_armed.find("\"delivered_taps\":0") != std::string::npos && status_armed.find("\"tap_pending\":true") != std::string::npos,
	       "scheduled tap waits for guest samples after the target present");

	uint32_t sample = 0;
	AgentPadApplyReadStateSample(&sample);
	Expect((sample & PAD_BUTTON_CROSS) == 0, "scheduled tap begins released");
	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	Expect((sample & PAD_BUTTON_CROSS) == PAD_BUTTON_CROSS, "scheduled tap press reaches the guest");
	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	Expect((sample & PAD_BUTTON_CROSS) == PAD_BUTTON_CROSS, "scheduled tap press remains visible on the next guest sample");
	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	Expect((sample & PAD_BUTTON_CROSS) == 0, "scheduled tap ends released");
	const std::string status_after_first = Kyty::Emulator::Agent::Internal::DispatchLine(R"({"id":7,"tool":"status","args":{}})");
	Expect(status_after_first.find("\"delivered_taps\":1") != std::string::npos,
	       "first scheduled tap is counted only after a guest press sample");

	const auto before_second = AgentPadOnPresent(8039);
	Expect(before_second.started == 0 && before_second.cancelled == 0, "second tap does not start early");
	const auto second = AgentPadOnPresent(8040);
	Expect(second.started == 1 && second.cancelled == 0 && second.started_target_present == 8040,
	       "second tap starts on exactly present 8040");
	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	Expect((sample & PAD_BUTTON_CROSS) == PAD_BUTTON_CROSS, "second scheduled press reaches the guest");
	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	Expect((sample & PAD_BUTTON_CROSS) == PAD_BUTTON_CROSS, "second scheduled press remains visible on the next guest sample");
	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	const std::string status_after_second =
	    Kyty::Emulator::Agent::Internal::DispatchLine(R"({"id":8,"tool":"status","args":{}})");
	Expect(status_after_second.find("\"delivered_taps\":2") != std::string::npos,
	       "two guest-sampled taps are delivered at the requested presents");

	// Essential schedule validation is fail-closed before it mutates the queue.
	const std::string past = Kyty::Emulator::Agent::Internal::DispatchLine(
	    R"({"id":2,"tool":"pad_tap","args":{"button":"cross","at_present":8040,"repeat":1,"present_delta":0}})");
	Expect(past.find("\"ok\":false") != std::string::npos && past.find("\"code\":\"invalid_args\"") != std::string::npos,
	       "already-recorded present is rejected before queue mutation");
	const std::string no_delta = Kyty::Emulator::Agent::Internal::DispatchLine(
	    R"({"id":3,"tool":"pad_tap","args":{"button":"cross","at_present":8100,"repeat":2,"present_delta":0}})");
	Expect(no_delta.find("\"ok\":false") != std::string::npos && no_delta.find("\"code\":\"invalid_args\"") != std::string::npos,
	       "repeated scheduled tap requires a positive present delta");

	const std::string pending = Kyty::Emulator::Agent::Internal::DispatchLine(
	    R"({"id":4,"tool":"pad_tap","args":{"button":"cross","at_present":8100,"repeat":2,"present_delta":1}})");
	Expect(pending.find("\"ok\":true") != std::string::npos, "pending-target cancellation schedule is accepted");
	Expect(AgentPadOnPresent(8100).started == 1, "first pending target starts exactly once");
	const auto cancelled = AgentPadOnPresent(8101);
	Expect(cancelled.started == 0 && cancelled.cancelled == 1 && cancelled.cancelled_target_present == 8101,
	       "next target cancels instead of delaying while the tap FSM is pending");
	const std::string status_cancelled =
	    Kyty::Emulator::Agent::Internal::DispatchLine(R"({"id":9,"tool":"status","args":{}})");
	Expect(status_cancelled.find("\"scheduled_taps\":0") != std::string::npos &&
	           status_cancelled.find("\"cancelled_scheduled_taps\":1") != std::string::npos,
	       "cancelled target is observable and removed from the bounded queue");
	Expect(AgentPadOnPresent(8102).started == 0, "cancelled target is never delivered late");
	AgentPadClear();
}

void RunRawNulLine(const std::string& endpoint)
{
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen raw nul");

	std::atomic<bool> server_ok {false};
	std::thread server([&]() {
		Transport::Connection connection {};
		if (Transport::Accept(&listener, &connection) != Transport::Result::Ok)
		{
			return;
		}
		std::string request;
		if (Transport::ReadLine(&connection, &request, 4096) != Transport::Result::Ok)
		{
			Transport::Close(&connection);
			return;
		}
		const std::string response = Kyty::Emulator::Agent::Internal::DispatchLine(request);
		const bool        wrote    = [&]() {
			const std::string payload = response + "\n";
			return Transport::WriteAll(&connection, payload.data(), payload.size()) == Transport::Result::Ok;
		}();
		server_ok = request.find('\0') != std::string::npos && response.find(R"("ok":false)") != std::string::npos &&
		            response.find(R"("code":"malformed")") != std::string::npos && wrote;
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
	Expect(client.native != 0, "connect raw nul");
	std::string payload = R"({"id":1,"tool":"help","args":{}})";
	payload.push_back('\0');
	payload += "arbitrary-suffix\n";
	Expect(Transport::WriteAll(&client, payload.data(), payload.size()) == Transport::Result::Ok, "write raw nul");
	std::string response;
	Expect(Transport::ReadLine(&client, &response, 4096) == Transport::Result::Ok, "read raw nul response");
	Expect(response.find(R"("ok":false)") != std::string::npos, "raw nul response rejected");
	Expect(response.find(R"("code":"malformed")") != std::string::npos, "raw nul malformed code");
	Transport::Close(&client);
	server.join();
	Expect(server_ok.load(), "raw nul server validation");
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
	RunCliTraceRtLifetimeArm(UniqueEndpoint("lifetime-arm"));
	RunCliScheduledPresentTap(UniqueEndpoint("scheduled-present-tap"));
	RunRawNulLine(UniqueEndpoint("raw-nul"));
	std::puts("agent transport integration passed");
	return 0;
}
