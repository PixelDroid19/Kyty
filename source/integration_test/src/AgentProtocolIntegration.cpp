// Process-isolated checks for the single agent protocol surface:
// version consistency, malformed fail-closed, ring overflow visibility,
// lifecycle event emission + sanitize. No second debug framework.

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Agent/EventRing.h"
#include "Emulator/Agent/Protocol.h"
#include "Emulator/Agent/StallWatch.h"
#include "Emulator/Loader/ModuleLoad.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace Kyty;
using namespace Kyty::Emulator::Agent;

namespace {

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "agent protocol integration failure: %s\n", message);
	std::fflush(stderr);
	std::_Exit(1);
}

void Expect(bool cond, const char* message)
{
	if (!cond)
	{
		Die(message);
	}
}

int InitializeBringUp()
{
	Core::BringUp::ConfigError error {};
	if (!Core::BringUp::InitializeFromEnvironment(&error))
	{
		std::fprintf(stderr, "bring-up configuration error: %s\n", error.message);
		return 125;
	}
	return 0;
}

bool HasProtocolVersion(const std::string& json)
{
	const std::string needle = "\"protocol_version\":" + std::to_string(Kyty::Agent::kProtocolVersion);
	return json.find(needle) != std::string::npos;
}

int ScenarioProtocolVersionConsistent()
{
	const std::string diag = FormatOk(3, BuildDiagnosticsResult(Core::BringUp::GetConfig(), Core::BringUp::GetDiagnostics(),
	                                                            Loader::RuntimeLinker::GetGlobalMissingImportDiagnostics(),
	                                                            Loader::ModuleLifecycleCoordinator::GetDiagnostics()));
	char              ping_body[128];
	std::snprintf(ping_body, sizeof(ping_body), "{\"alive\":true,\"protocol_version\":%u,\"uptime_ms\":0}", Kyty::Agent::kProtocolVersion);
	const std::string ping = FormatOk(4, ping_body);
	const std::string err  = FormatErr(5, "malformed", "bad");

	Expect(HasProtocolVersion(diag), "diagnostics envelope protocol_version");
	Expect(HasProtocolVersion(ping), "ping envelope protocol_version");
	Expect(HasProtocolVersion(err), "error envelope protocol_version");
	// event_ring is top-level in diagnostics (not nested under bring_up only).
	const std::string diag_body = BuildDiagnosticsResult(Core::BringUp::GetConfig(), Core::BringUp::GetDiagnostics(),
	                                                     Loader::RuntimeLinker::GetGlobalMissingImportDiagnostics(),
	                                                     Loader::ModuleLifecycleCoordinator::GetDiagnostics());
	Expect(diag_body.find("\"event_ring\"") != std::string::npos, "diagnostics has event_ring");
	Expect(diag_body.find("\"performance\"") != std::string::npos, "diagnostics has bounded performance snapshot");
	Expect(diag_body.find("\"command_buffers\"") != std::string::npos, "performance has command buffer count");
	Expect(diag_body.find("\"acquire_max_ns\"") != std::string::npos, "performance has bounded acquire timing");
	Expect(diag_body.find("\"present_max_ns\"") != std::string::npos, "performance has bounded present timing");
	Expect(diag_body.find("\"wait_reg_mem_max_ns\"") != std::string::npos, "performance has bounded WaitRegMem timing");
	Expect(diag_body.find("\"wait_flip_done_max_ns\"") != std::string::npos, "performance has bounded WaitFlipDone timing");
	Expect(diag_body.find("\"in_flight_current\"") != std::string::npos, "performance has current submissions in flight");
	Expect(diag_body.find("\"in_flight_max\"") != std::string::npos, "performance has bounded peak submissions in flight");
	Expect(diag_body.find("\"frame_samples\"") != std::string::npos, "performance has bounded frame sample count");
	Expect(diag_body.find("\"frame_time_p99_us\"") != std::string::npos, "performance has bounded p99 frame time");
	Expect(diag_body.find("\"frame_time_max_us\"") != std::string::npos, "performance has bounded maximum frame time");
	Expect(diag_body.find("\"frames_over_250ms\"") != std::string::npos, "performance has cold-frame threshold count");
	Expect(diag_body.find("\"hash_max_ns\"") != std::string::npos, "performance has bounded hash timing");
	Expect(diag_body.find("\"detile_max_ns\"") != std::string::npos, "performance has bounded detile timing");
	Expect(diag_body.find("\"upload_max_ns\"") != std::string::npos, "performance has bounded upload timing");
	Expect(diag_body.find("\"writeback_max_ns\"") != std::string::npos, "performance has bounded writeback timing");
	Expect(diag_body.find("\"gfx_pipeline_lookup_max_ns\"") != std::string::npos, "performance has bounded graphics lookup timing");
	Expect(diag_body.find("\"compute_pipeline_lookup_max_ns\"") != std::string::npos, "performance has bounded compute lookup timing");
	Expect(diag_body.find("\"pipeline_evictions\"") != std::string::npos, "performance has pipeline eviction count");
	Expect(diag_body.find("\"gfx_pipeline_miss_max_ns\"") != std::string::npos, "performance has bounded graphics miss timing");
	Expect(diag_body.find("\"compute_pipeline_miss_max_ns\"") != std::string::npos, "performance has bounded compute miss timing");
	Expect(diag_body.find("\"shader_ir_input_analysis_max_ns\"") != std::string::npos,
	       "performance has bounded shader input-analysis timing");
	Expect(diag_body.find("\"shader_ir_pipeline_miss_max_ns\"") != std::string::npos,
	       "performance has bounded shader miss-parse timing");
	Expect(diag_body.find("\"spirv_source_max_ns\"") != std::string::npos, "performance has bounded SPIR-V source timing");
	Expect(diag_body.find("\"spirv_compile_max_ns\"") != std::string::npos, "performance has bounded SPIR-V compile timing");
	Expect(diag_body.find("\"vk_graphics_pipeline_create_max_ns\"") != std::string::npos,
	       "performance has bounded Vulkan graphics pipeline timing");
	Expect(diag_body.find("\"vk_compute_pipeline_create_max_ns\"") != std::string::npos,
	       "performance has bounded Vulkan compute pipeline timing");
	Expect(diag_body.find("\"shader_translation_cache_hits\"") != std::string::npos, "performance has shader translation cache hits");
	Expect(diag_body.find("\"shader_translation_cache_misses\"") != std::string::npos, "performance has shader translation cache misses");
	Expect(diag_body.find("\"shader_translation_cache_evictions\"") != std::string::npos,
	       "performance has bounded shader translation cache evictions");
	Expect(diag_body.find("\"gpu_memory\"") != std::string::npos, "performance has bounded GPU memory outcomes");
	Expect(diag_body.find("\"create_max_ns\"") != std::string::npos,
	       "performance has bounded GPU memory create timing");
	const char* gpu_memory_types[] = {"video_out_buffer", "depth_stencil_buffer", "label",         "index_buffer", "vertex_buffer",
	                                  "storage_buffer",   "texture",              "render_texture", "storage_texture"};
	for (const char* type: gpu_memory_types)
	{
		const std::string needle = std::string("\"type\":\"") + type + "\"";
		Expect(diag_body.find(needle) != std::string::npos, "performance has every stable GPU memory type row");
	}
	Expect(diag_body.find("\"reclaim_new\"") != std::string::npos, "GPU memory rows contain the final create outcome");
	Expect(diag_body.find("\"logical_free\"") != std::string::npos, "GPU memory rows contain logical frees");
	Expect(diag_body.find("\"live\"") != std::string::npos, "GPU memory rows contain live objects");
	// ensure bring_up closes before event_ring appears as sibling: look for pattern
	const auto bring_pos = diag_body.find("\"bring_up\"");
	const auto ring_pos  = diag_body.find("\"event_ring\"");
	Expect(bring_pos != std::string::npos && ring_pos != std::string::npos && ring_pos > bring_pos, "event_ring after bring_up");
	Expect(Kyty::Agent::kProtocolVersion == 4u, "live constant is 4");
	Expect(ParseTool("perf_snapshot") == Tool::PerfSnapshot, "perf_snapshot is part of protocol v4");
	std::printf("PROTOCOL_VERSION=%u\n", Kyty::Agent::kProtocolVersion);
	return 0;
}

int ScenarioMalformedFailClosed()
{
	Request   req {};
	ErrorInfo error {};
	// Missing id — must not crash; typed error; no mutation of EventRing.
	EventRing::Instance().ResetForTests();
	const uint64_t seq_before = EventRing::Instance().NextSeq();
	Expect(!ParseRequestLine("{\"tool\":\"status\"}", &req, &error), "parse must fail");
	Expect(!error.code.empty(), "error code set");
	// The protocol adapter maps domain errors into the public error object.
	Expect(error.code == "invalid_args" || error.code == "malformed", "stable error code class");
	Expect(EventRing::Instance().NextSeq() == seq_before, "malformed parse must not push events");

	const auto vr = ParseAndValidateRequestLine("not-json", &req);
	Expect(!vr.Ok(), "non-json fails");
	Expect(std::strcmp(vr.error.code, "malformed") == 0, "malformed code");
	const auto trailing = ParseAndValidateRequestLine(R"({"id":1,"tool":"pad_clear","args":{}} trailing)", &req);
	Expect(!trailing.Ok(), "trailing data after request object fails");
	Expect(std::strcmp(trailing.error.code, "malformed") == 0, "trailing data is malformed");
	Expect(EventRing::Instance().NextSeq() == seq_before, "trailing data must not mutate state");

	const auto nested_button = ParseAndValidateRequestLine(R"({"id":2,"tool":"pad_down","args":{"metadata":{"button":"cross"}}})", &req);
	Expect(!nested_button.Ok(), "nested button must not satisfy top-level argument policy");
	Expect(std::strcmp(nested_button.error.code, "policy_denied") == 0, "nested button is rejected by agent policy");

	const auto text_button = ParseAndValidateRequestLine(R"({"id":3,"tool":"pad_down","args":{"note":"button"}})", &req);
	Expect(!text_button.Ok(), "button text must not satisfy top-level argument policy");
	Expect(std::strcmp(text_button.error.code, "policy_denied") == 0, "button text is rejected by agent policy");

	const auto typed_button = ParseAndValidateRequestLine(R"({"id":4,"tool":"pad_down","args":{"button":"cross"}})", &req);
	Expect(typed_button.Ok(), "top-level string button is accepted");
	Expect(req.kind == Tool::PadDown, "request carries parsed tool identity");

	const std::string err_json = FormatErr(0, error.code.c_str(), error.message.c_str());
	Expect(HasProtocolVersion(err_json), "err has protocol_version");
	Expect(err_json.find("\"ok\":false") != std::string::npos, "ok false");
	return 0;
}

int ScenarioJsonEscapingComplete()
{
	const char        raw[]   = {'a', '\b', '\f', '\n', '\r', '\t', '\x01', '"', '\\', '\0'};
	const std::string escaped = JsonString(raw);
	Expect(escaped == R"("a\b\f\n\r\t\u0001\"\\")", "all JSON control characters escaped");
	for (const unsigned char ch: escaped)
	{
		Expect(ch >= 0x20, "serialized JSON contains no raw control byte");
	}
	return 0;
}

int ScenarioDiagnosticsResponseBounded()
{
	Loader::ModuleLoadPlanDiagnostics load_plan {};
	load_plan.entry_count           = Loader::kModuleLoadPlanMaxEntries;
	load_plan.rejection_count       = Loader::kModuleLoadPlanMaxRejections;
	load_plan.export_conflict_count = Loader::kModuleLoadPlanMaxConflicts;
	std::snprintf(load_plan.primary_identity, sizeof(load_plan.primary_identity), "%s", "primary");
	for (uint32_t i = 0; i < load_plan.entry_count; ++i)
	{
		std::memset(load_plan.entries[i], 'e', sizeof(load_plan.entries[i]) - 1);
	}
	for (uint32_t i = 0; i < load_plan.rejection_count; ++i)
	{
		std::memset(load_plan.rejections[i], 'r', sizeof(load_plan.rejections[i]) - 1);
	}
	for (uint32_t i = 0; i < load_plan.export_conflict_count; ++i)
	{
		std::memset(load_plan.export_conflicts[i], 'c', sizeof(load_plan.export_conflicts[i]) - 1);
	}
	const std::string response = FormatOk(1, BuildDiagnosticsResult(Core::BringUp::GetConfig(), Core::BringUp::GetDiagnostics(),
	                                                                Loader::MissingImportDiagnostics {}, load_plan));
	Expect(response.size() + 1 <= Kyty::Agent::kResponseLineMax, "maximum diagnostics response fits wire contract");
	Expect(response.find("\"truncated\":true") != std::string::npos, "bounded diagnostics reports truncation");
	return 0;
}

int ScenarioEventRingOverflowVisible()
{
	EventRing ring;
	for (uint32_t i = 0; i < kAgentEventRingCapacity + 10u; ++i)
	{
		char code[32];
		std::snprintf(code, sizeof(code), "e%u", i);
		ring.Push(EventKind::Info, code, "msg");
	}
	const EventRingStats s = ring.GetStats();
	Expect(s.size == kAgentEventRingCapacity, "bounded size");
	Expect(s.dropped == 10u, "dropped count");
	Expect(s.overflowed, "overflowed flag");
	Expect(s.next_seq == kAgentEventRingCapacity + 10u, "seq continues past capacity");
	// Diagnostics-style snapshot remains useful when ring is full.
	Expect(s.total_pushed == kAgentEventRingCapacity + 10u, "total_pushed");
	std::printf("RING capacity=%u dropped=%llu next_seq=%llu\n", s.capacity, static_cast<unsigned long long>(s.dropped),
	            static_cast<unsigned long long>(s.next_seq));
	return 0;
}

int ScenarioLifecycleEventsAndSanitize()
{
	EventRing::Instance().ResetForTests();
	Lifecycle::InstallHooks();

	Lifecycle::EmitStartupConfig("unsafe", true);
	Lifecycle::EmitExecutableDiscovered("eboot.bin");
	Lifecycle::EmitModuleDiscovery(1, 0, 0);
	Lifecycle::EmitMissingImport("sanitizedNid[lib_v1][mod_v1.0][Func]");
	Lifecycle::EmitSymbolResolved("sanitizedNid[lib_v1][mod_v1.0][Func]", "hle");
	Lifecycle::Emit(EventKind::Info, Lifecycle::kCodeExecutableDiscovered, "path=/home/secret/title/eboot.bin");
	Lifecycle::Emit(EventKind::Info, Lifecycle::kCodeExecutableDiscovered,
	                "linux=/mnt/private/PPSA12345/eboot.bin windows=C:\\Games\\CUSA99999\\eboot.bin");
	// Host fault hook → host_crash event (same path as assert/EXIT handlers).
	Lifecycle::InstallHooks(); // ensure host fault hook registered
	Core::SetHostFaultHook(+[](const char* code, const char* message) noexcept
	                       {
		                       char msg[192];
		                       std::snprintf(msg, sizeof(msg), "code=%s %s", code != nullptr ? code : "",
		                                     message != nullptr ? message : "");
		                       Lifecycle::EmitHostCrash(msg);
	                       });
	// Invoke the installed path via the same hook type (no process abort).
	{
		// Re-get the hook is private; emit host crash directly through the public API
		// that InstallHooks wires to DbgAssert — call EmitHostCrash which production uses.
		Lifecycle::EmitHostCrash("code=assert test_host_fault");
	}

	// BringUp Report should fire hook when unsafe + feature on.
	const auto d = Core::BringUp::Report(Core::BringUp::Feature::NotImplemented, Core::BringUp::Subsystem::Core, "agent-protocol-site",
	                                     __FILE__, __LINE__);
	Expect(d == Core::BringUp::Decision::Continue, "unsafe continue");

	EventRecord    out[32] {};
	const uint32_t n = EventRing::Instance().CopySince(0, out, 32);
	Expect(n >= 5, "lifecycle events present");

	bool saw_startup  = false;
	bool saw_exec     = false;
	bool saw_missing  = false;
	bool saw_symbol   = false;
	bool saw_continue = false;
	bool saw_crash    = false;
	bool sanitized    = true;
	for (uint32_t i = 0; i < n; ++i)
	{
		Expect(out[i].seq > 0, "seq assigned");
		if (std::strcmp(out[i].code, Lifecycle::kCodeStartupConfig) == 0)
		{
			saw_startup = true;
		}
		if (std::strcmp(out[i].code, Lifecycle::kCodeExecutableDiscovered) == 0)
		{
			saw_exec = true;
		}
		if (std::strcmp(out[i].code, Lifecycle::kCodeMissingImport) == 0)
		{
			saw_missing = true;
		}
		if (std::strcmp(out[i].code, Lifecycle::kCodeSymbolResolved) == 0)
		{
			saw_symbol = true;
		}
		if (std::strcmp(out[i].code, Lifecycle::kCodeBringUpContinue) == 0)
		{
			saw_continue = true;
		}
		if (std::strcmp(out[i].code, Lifecycle::kCodeHostCrash) == 0)
		{
			saw_crash = true;
		}
		if (std::strstr(out[i].message, "/home/secret") != nullptr || std::strstr(out[i].message, "/mnt/private") != nullptr ||
		    std::strstr(out[i].message, "C:\\Games") != nullptr || std::strstr(out[i].message, "PPSA12345") != nullptr ||
		    std::strstr(out[i].message, "CUSA99999") != nullptr)
		{
			sanitized = false;
		}
	}
	Expect(saw_startup, "startup_config event");
	Expect(saw_exec, "executable_discovered event");
	Expect(saw_missing, "missing_import event");
	Expect(saw_symbol, "symbol_resolved event");
	Expect(saw_continue, "bringup_continue event");
	Expect(saw_crash, "host_crash event");
	Expect(sanitized, "no private host path in messages");

	// Frontier classification (current state).
	Expect(std::strcmp(Lifecycle::ClassifyFrontier("interactive", false, "", true, 10), "interactive") == 0, "interactive frontier");
	Expect(std::strcmp(Lifecycle::ClassifyFrontier("stalled", false, "", true, 10), "stall") == 0, "stall frontier");
	Expect(std::strcmp(Lifecycle::ClassifyFrontier("booting", true, Lifecycle::kCodeBringUpHalt, false, 0), "unsupported") == 0,
	       "unsupported frontier");
	Expect(std::strcmp(Lifecycle::ClassifyFrontier("interactive", true, "relocation_failure", true, 20), "interactive") == 0,
	       "interactive state recovers from historical nonfatal error");
	Expect(std::strcmp(Lifecycle::ClassifyHostFaultCode("not_implemented_halt"), Lifecycle::kCodeBringUpHalt) == 0,
	       "unsupported host halt is not classified as crash");
	Expect(std::strcmp(Lifecycle::ClassifyHostFaultCode("assert"), Lifecycle::kCodeHostCrash) == 0, "assert host fault remains a crash");
	std::printf("LIFECYCLE events=%u sanitized=1\n", n);
	return 0;
}

int ScenarioSeqMonotonic()
{
	EventRing ring;
	for (int i = 0; i < 20; ++i)
	{
		ring.Push(EventKind::Info, "seq_test", "x");
	}
	EventRecord    out[20] {};
	const uint32_t n = ring.CopySince(0, out, 20);
	Expect(n == 20, "20 events");
	// Newest first: seq should be descending.
	for (uint32_t i = 1; i < n; ++i)
	{
		Expect(out[i].seq < out[i - 1].seq, "newest-first descending seq");
	}
	Expect(out[0].seq == 20, "latest seq is 20");
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: kyty_agent_protocol_integration <scenario>\n");
		return 125;
	}
	const int init = InitializeBringUp();
	if (init != 0)
	{
		return init;
	}
	const std::string scenario(argv[1]);
	if (scenario == "protocol_version_consistent")
	{
		return ScenarioProtocolVersionConsistent();
	}
	if (scenario == "malformed_fail_closed")
	{
		return ScenarioMalformedFailClosed();
	}
	if (scenario == "json_escaping_complete")
	{
		return ScenarioJsonEscapingComplete();
	}
	if (scenario == "diagnostics_response_bounded")
	{
		return ScenarioDiagnosticsResponseBounded();
	}
	if (scenario == "event_ring_overflow_visible")
	{
		return ScenarioEventRingOverflowVisible();
	}
	if (scenario == "lifecycle_events_sanitize")
	{
		return ScenarioLifecycleEventsAndSanitize();
	}
	if (scenario == "seq_monotonic")
	{
		return ScenarioSeqMonotonic();
	}
	std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
	return 125;
}
