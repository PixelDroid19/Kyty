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

void SetEnvironment(const char* name, const char* value)
{
#if defined(_WIN32)
	Expect(_putenv_s(name, value) == 0, "test environment variable must be set");
#else
	Expect(::setenv(name, value, 1) == 0, "test environment variable must be set");
#endif
}

void RestoreEnvironment(const char* name, bool was_set, const std::string& previous_value)
{
#if defined(_WIN32)
	Expect(_putenv_s(name, was_set ? previous_value.c_str() : "") == 0, "test environment variable must be restored");
#else
	const int result = was_set ? ::setenv(name, previous_value.c_str(), 1) : ::unsetenv(name);
	Expect(result == 0, "test environment variable must be restored");
#endif
}

void RunAgentServerStopWithIdleClient(const std::string& endpoint)
{
	const char* const previous = std::getenv("KYTY_AGENT_ENDPOINT");
	const bool        was_set  = previous != nullptr;
	const std::string previous_value = was_set ? previous : "";
	SetEnvironment("KYTY_AGENT_ENDPOINT", endpoint.c_str());
	Expect(!Kyty::Emulator::Agent::Active(), "agent server starts inactive");
	Expect(Kyty::Emulator::Agent::StartFromEnv(), "agent server starts for shutdown integration");

	Transport::Connection client {};
	for (uint32_t attempt = 0; attempt < 100; ++attempt)
	{
		if (Transport::Connect(endpoint.c_str(), &client) == Transport::Result::Ok)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	Expect(client.native != 0, "connect agent server shutdown client");
	const std::string requests = R"({"id":1,"tool":"ping","args":{}})" "\n{";
	Expect(Transport::WriteAll(&client, requests.data(), requests.size()) == Transport::Result::Ok,
	       "send first request and partial next request");
	std::string response;
	Expect(Transport::ReadLine(&client, &response, 4096) == Transport::Result::Ok, "read first response before shutdown");
	Expect(response.find(R"("ok":true)") != std::string::npos, "first request reaches the accepted client handler");

	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	std::atomic<bool> stop_finished {false};
	std::thread stopper([&]() {
		Kyty::Emulator::Agent::Stop();
		stop_finished = true;
	});
	for (uint32_t attempt = 0; attempt < 200 && !stop_finished.load(); ++attempt)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (!stop_finished.load())
	{
		Die("agent Stop returns with an idle accepted client");
	}
	stopper.join();
	Expect(!Kyty::Emulator::Agent::Active(), "agent server becomes inactive after Stop");
	Transport::Close(&client);
	RestoreEnvironment("KYTY_AGENT_ENDPOINT", was_set, previous_value);
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
		const bool expected_request =
		    request == R"({"id":1,"tool":"pad_tap","args":{"button":"cross","at_present":8000,"repeat":2,"present_delta":40}})";
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

void RunCliImmediatePadTapPressSamplesOne(const std::string& endpoint)
{
	using Kyty::Libs::Controller::AgentPadClear;

	AgentPadClear();
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen immediate one-sample tap");

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
		server_ok = request == R"({"id":1,"tool":"pad_tap","args":{"button":"cross","press_samples":1}})" &&
		            response.find("\"ok\":true") != std::string::npos && wrote;
		Transport::Close(&connection);
	});

	char arg0[] = "kyty_agent";
	char arg1[] = "--endpoint";
	char arg3[] = "pad";
	char arg4[] = "tap";
	char arg5[] = "cross";
	char arg6[] = "--press-samples";
	char arg7[] = "1";
	char* argv[] = {arg0, arg1, const_cast<char*>(endpoint.c_str()), arg3, arg4, arg5, arg6, arg7};
	const int rc = Kyty::AgentCli::Main(8, argv);
	if (rc != 0)
	{
		Transport::Interrupt(&listener);
	}
	server.join();
	Expect(rc == 0, "cli immediate one-sample tap exits successfully");
	Expect(server_ok.load(), "cli sends exact immediate press_samples request and receives success");
	Transport::Close(&listener);
	AgentPadClear();
}

void RunCliScheduledPadTapPressSamplesOne(const std::string& endpoint)
{
	using Kyty::Libs::Controller::AgentPadApplyReadStateSample;
	using Kyty::Libs::Controller::AgentPadClear;
	using Kyty::Libs::Controller::AgentPadGetReadStats;
	using Kyty::Libs::Controller::AgentPadOnPresent;
	using Kyty::Libs::Controller::AgentPadReadStats;
	using Kyty::Libs::Controller::PAD_BUTTON_CROSS;

	AgentPadClear();
	AgentPadReadStats stats_before {};
	AgentPadGetReadStats(&stats_before);
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen scheduled one-sample tap");

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
		server_ok = request ==
		                R"({"id":1,"tool":"pad_tap","args":{"button":"cross","at_present":8200,"repeat":1,"present_delta":0,"press_samples":1}})" &&
		            response.find("\"ok\":true") != std::string::npos && wrote;
		Transport::Close(&connection);
	});

	char arg0[] = "kyty_agent";
	char arg1[] = "--endpoint";
	char arg3[] = "pad";
	char arg4[] = "tap";
	char arg5[] = "cross";
	char arg6[] = "--at-present";
	char arg7[] = "8200";
	char arg8[] = "--press-samples";
	char arg9[] = "1";
	char* argv[] = {arg0, arg1, const_cast<char*>(endpoint.c_str()), arg3, arg4, arg5, arg6, arg7, arg8, arg9};
	const int rc = Kyty::AgentCli::Main(10, argv);
	if (rc != 0)
	{
		Transport::Interrupt(&listener);
	}
	server.join();
	Expect(rc == 0, "cli scheduled one-sample tap exits successfully");
	Expect(server_ok.load(), "cli sends exact scheduled press_samples request and receives success");
	Transport::Close(&listener);

	const std::string scheduled = Kyty::Emulator::Agent::Internal::DispatchLine(R"({"id":1,"tool":"status","args":{}})");
	Expect(scheduled.find("\"scheduled_taps\":1") != std::string::npos &&
	           scheduled.find("\"next_target_present\":8200") != std::string::npos,
	       "status retains the scheduled one-sample tap target");
	Expect(AgentPadOnPresent(8200).started == 1, "one-sample tap starts on its scheduled present");

	uint32_t sample = 0;
	AgentPadApplyReadStateSample(&sample);
	Expect((sample & PAD_BUTTON_CROSS) == 0, "scheduled one-sample tap begins released");
	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	Expect((sample & PAD_BUTTON_CROSS) == PAD_BUTTON_CROSS, "scheduled one-sample press reaches the guest");
	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	Expect((sample & PAD_BUTTON_CROSS) == 0, "scheduled one-sample tap ends released on the next advancing sample");

	AgentPadReadStats stats_after {};
	AgentPadGetReadStats(&stats_after);
	Expect(stats_after.delivered_taps == stats_before.delivered_taps + 1 && !stats_after.tap_pending,
	       "scheduled one-sample tap delivers once and completes");
	AgentPadClear();
}

void RunCliPressSamplesRejectsBeforeConnect(const std::string& endpoint, const char* value)
{
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen invalid press-samples request");

	std::atomic<bool> accept_entered {false};
	std::atomic<bool> connected {false};
	std::thread server([&]() {
		Transport::Connection connection {};
		accept_entered = true;
		if (Transport::Accept(&listener, &connection) != Transport::Result::Ok)
		{
			return;
		}
		connected = true;
		std::string request;
		if (Transport::ReadLine(&connection, &request, 4096) == Transport::Result::Ok)
		{
			const std::string response = "{\"id\":1,\"ok\":true,\"protocol_version\":" +
			                             std::to_string(Kyty::Agent::kProtocolVersion) + ",\"result\":{}}\n";
			(void)Transport::WriteAll(&connection, response.data(), response.size());
		}
		Transport::Close(&connection);
	});
	while (!accept_entered.load())
	{
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	char  arg0[] = "kyty_agent";
	char  arg1[] = "--endpoint";
	char  arg3[] = "pad";
	char  arg4[] = "tap";
	char  arg5[] = "cross";
	char  arg6[] = "--press-samples";
	char* argv[] = {arg0, arg1, const_cast<char*>(endpoint.c_str()), arg3, arg4, arg5, arg6, const_cast<char*>(value)};
	const int rc = Kyty::AgentCli::Main(8, argv);
	Transport::Interrupt(&listener);
	server.join();
	Expect(rc == 125, "cli rejects invalid press-samples value");
	Expect(!connected.load(), "invalid press-samples value is rejected before connecting");
	Transport::Close(&listener);
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

void ExpectRawLineResult(const std::string& endpoint, const std::string& payload, size_t max_size,
	                     Transport::Result expected_result, const std::string& expected_line, const char* message)
{
	Transport::Listener listener {};
	Expect(Transport::Listen(&listener, endpoint.c_str()) == Transport::Result::Ok, "listen raw line limit");

	std::atomic<Transport::Result> result {Transport::Result::IoError};
	std::string                     line_result;
	std::thread server([&]() {
		Transport::Connection connection {};
		if (Transport::Accept(&listener, &connection) != Transport::Result::Ok)
		{
			return;
		}
		std::string line;
		result      = Transport::ReadLine(&connection, &line, max_size);
		line_result = line;
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
	Expect(client.native != 0, "connect raw line limit");
	Expect(Transport::WriteAll(&client, payload.data(), payload.size()) == Transport::Result::Ok, "write CR line test case");
	server.join();
	Expect(result.load() == expected_result, message);
	Expect(line_result == expected_line, "CR stripping preserves the line payload");
	Transport::Close(&client);
	Transport::Close(&listener);
}

void RunRawLineLimit(const std::string& endpoint)
{
	constexpr size_t kLimit = 8;
	ExpectRawLineResult(endpoint + "-boundary", "x" + std::string(kLimit - 2, '\r') + "\n", kLimit,
	                    Transport::Result::Ok, "x", "line at the raw byte budget is accepted");
	ExpectRawLineResult(endpoint + "-over-budget", "x" + std::string(kLimit - 1, '\r') + "\n", kLimit,
	                    Transport::Result::MessageTooLarge, "x", "discarded CR bytes count toward the raw line budget");
}

} // namespace

int main()
{
	Expect(Transport::IsValidEndpoint(UniqueEndpoint("valid").c_str()), "platform endpoint accepted");
	Expect(!Transport::IsValidEndpoint("relative-endpoint"), "relative endpoint rejected");
	RunRoundTrip(UniqueEndpoint("roundtrip"));
	RunInterrupt(UniqueEndpoint("interrupt"));
	RunAgentServerStopWithIdleClient(UniqueEndpoint("shutdown-idle-client"));
	RunCliWaitReady(UniqueEndpoint("wait-ready"));
	RunCliDebugSnapshotWorkflow(UniqueEndpoint("debug-snapshot"));
	RunCliTraceRtLifetimeArm(UniqueEndpoint("lifetime-arm"));
	RunCliImmediatePadTapPressSamplesOne(UniqueEndpoint("immediate-tap-one-sample"));
	RunCliScheduledPresentTap(UniqueEndpoint("scheduled-present-tap"));
	RunCliScheduledPadTapPressSamplesOne(UniqueEndpoint("scheduled-tap-one-sample"));
	RunCliPressSamplesRejectsBeforeConnect(UniqueEndpoint("tap-press-samples-zero"), "0");
	RunCliPressSamplesRejectsBeforeConnect(UniqueEndpoint("tap-press-samples-three"), "3");
	RunRawNulLine(UniqueEndpoint("raw-nul"));
	RunRawLineLimit(UniqueEndpoint("raw-line-limit"));
	std::puts("agent transport integration passed");
	return 0;
}
