#include "Emulator/Agent/AgentServer.h"

#include "Kyty/Core/Threads.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Agent/EventRing.h"
#include "Emulator/Agent/FrameScore.h"
#include "Emulator/Agent/Protocol.h"
#include "Emulator/Agent/StallWatch.h"
#include "Emulator/Controller.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Loader/ModuleLoad.h"
#include "Emulator/Loader/RuntimeLinker.h"

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
Core::Thread*     g_thread    = nullptr;
int               g_listen_fd = -1;
std::string       g_sock_path;
std::atomic<bool> g_client_busy {false};
uint64_t          g_start_ms = 0;
std::string       g_last_capture_path;

uint64_t SteadyMs()
{
	using clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

std::string HelpResult()
{
	char buf[2048];
	std::snprintf(buf, sizeof(buf),
	              "{\"protocol_version\":%u,\"diagnostic_input\":true,\"schema\":\"agent_tools\","
	              "\"tools\":["
	              "{\"tool\":\"help\"},{\"tool\":\"ping\"},{\"tool\":\"status\"},{\"tool\":\"diagnostics\"},"
	              "{\"tool\":\"perf_snapshot\",\"args\":{\"reset\":false}},"
	              "{\"tool\":\"sync_waits\"},{\"tool\":\"threads\"},"
	              "{\"tool\":\"events\",\"args\":{\"last\":50,\"after_seq\":0}},"
	              "{\"tool\":\"last_error\"},"
	              "{\"tool\":\"capture\",\"args\":{\"timeout_ms\":10000,\"score\":true}},"
	              "{\"tool\":\"score\",\"args\":{\"path\":\"/abs/last.bmp\"}},"
	              "{\"tool\":\"pad_down\",\"args\":{\"button\":\"cross\"}},"
	              "{\"tool\":\"pad_up\",\"args\":{\"button\":\"cross\"}},"
	              "{\"tool\":\"pad_tap\",\"args\":{\"button\":\"cross\"}},"
	              "{\"tool\":\"pad_axis\",\"args\":{\"axis\":\"left_x\",\"value\":128}},"
	              "{\"tool\":\"pad_clear\"},"
	              "{\"tool\":\"wait_present\",\"args\":{\"min\":1,\"delta\":20,\"timeout_ms\":15000}},"
	              "{\"tool\":\"wait_frame\",\"args\":{\"min\":1,\"delta\":20,\"timeout_ms\":15000}},"
	              "{\"tool\":\"wait_phase\",\"args\":{\"want\":\"interactive\",\"min_fps\":5,\"stable_ms\":400,\"timeout_ms\":45000}},"
	              "{\"tool\":\"wait_event\",\"args\":{\"kind\":\"error\",\"timeout_ms\":5000}},"
	              "{\"tool\":\"watch\",\"args\":{\"window_ms\":10000,\"present_stall_ms\":5000,\"frame_stall_ms\":5000,\"min_fps\":2.0,"
	              "\"capture\":true}}"
	              "]}",
	              Kyty::Agent::kProtocolVersion);
	return std::string(buf);
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

	const PhaseHint phase      = ClassifyPhaseHint(stats.graphic_ready, stats.present, stats.fps, stats.ms_since_present, PhaseHintArgs {});
	const char*     phase_name = PhaseHintName(phase);

	// Current-state frontier (not history): last error + phase → coarse label.
	EventRecord last_err {};
	const bool  have_err = EventRing::Instance().LastError(&last_err);
	const char* frontier =
	    Lifecycle::ClassifyFrontier(phase_name, have_err, have_err ? last_err.code : "", stats.graphic_ready, stats.present);
	const EventRingStats ring = EventRing::Instance().GetStats();
	// Lifecycle first_frame / graphics_init / input_ready / first_present are emitted
	// at Window producers (not here) so events-only consumers observe them.

	char buf[2048];
	std::snprintf(buf, sizeof(buf),
	              "{\"protocol_version\":%u,\"schema\":\"runtime_status\","
	              "\"frame\":%d,\"present\":%llu,\"fps\":%.3f,\"ms_since_present\":%llu,\"ms_since_frame\":%llu,"
	              "\"phase\":%s,\"frontier\":%s,"
	              "\"capture_ready\":%s,\"capture_dir_set\":%s,"
	              "\"graphic_ready\":%s,\"build_revision\":%s,\"build_dirty\":%s,\"diagnostic_input\":true,"
	              "\"event_ring\":{\"capacity\":%u,\"size\":%llu,\"next_seq\":%llu,\"dropped\":%llu,\"overflowed\":%s},"
	              "\"pad\":{\"buttons\":%u,\"left_x\":%u,\"left_y\":%u,\"right_x\":%u,\"right_y\":%u,\"l2\":%u,\"r2\":%u,"
	              "\"guest_read_state_samples\":%llu,\"guest_read_samples\":%llu,\"delivered_taps\":%llu,\"tap_pending\":%s}}",
	              Kyty::Agent::kProtocolVersion, stats.frame, static_cast<unsigned long long>(stats.present), stats.fps,
	              static_cast<unsigned long long>(stats.ms_since_present), static_cast<unsigned long long>(stats.ms_since_frame),
	              JsonString(phase_name).c_str(), JsonString(frontier).c_str(), stats.capture_ready ? "true" : "false",
	              stats.capture_dir_set ? "true" : "false", stats.graphic_ready ? "true" : "false", JsonString(BuildInfo::Revision).c_str(),
	              BuildInfo::Dirty ? "true" : "false", ring.capacity, static_cast<unsigned long long>(ring.size),
	              static_cast<unsigned long long>(ring.next_seq), static_cast<unsigned long long>(ring.dropped),
	              ring.overflowed ? "true" : "false", buttons, static_cast<unsigned>(axes[0]), static_cast<unsigned>(axes[1]),
	              static_cast<unsigned>(axes[2]), static_cast<unsigned>(axes[3]), static_cast<unsigned>(axes[4]),
	              static_cast<unsigned>(axes[5]), static_cast<unsigned long long>(read_stats.read_state_samples),
	              static_cast<unsigned long long>(read_stats.read_samples), static_cast<unsigned long long>(read_stats.delivered_taps),
	              read_stats.tap_pending ? "true" : "false");
	return std::string(buf);
}

std::string DiagnosticsResult()
{
	const auto bringup      = Core::BringUp::GetConfig();
	const auto diagnostics  = Core::BringUp::GetDiagnostics();
	const auto import_stats = Loader::RuntimeLinker::GetGlobalMissingImportDiagnostics();
	const auto load_plan    = Loader::ModuleLifecycleCoordinator::GetDiagnostics();
	return BuildDiagnosticsResult(bringup, diagnostics, import_stats, load_plan);
}

std::string PerformanceResult(bool reset)
{
	const auto stats = Libs::Graphics::DebugStatsGetPerformanceSnapshot(reset);
	char       buf[6144];
	std::snprintf(buf, sizeof(buf),
	              "{\"protocol_version\":%u,\"schema\":\"performance_snapshot\",\"reset\":%s,"
	              "\"interval_ms\":%llu,\"draws\":%llu,\"dispatches\":%llu,\"alloc_bytes\":%llu,\"free_bytes\":%llu,"
	              "\"creates\":%llu,\"frees\":%llu,\"flips\":%llu,\"buffer_flushes\":%llu,\"command_buffers\":%llu,\"submits\":%llu,"
	              "\"fence_waits\":%llu,\"fence_wait_ns\":%llu,\"fence_wait_max_ns\":%llu,"
	              "\"acquires\":%llu,\"acquire_ns\":%llu,\"acquire_max_ns\":%llu,"
	              "\"presents\":%llu,\"present_ns\":%llu,\"present_max_ns\":%llu,"
	              "\"wait_reg_mem\":%llu,\"wait_reg_mem_ns\":%llu,\"wait_reg_mem_max_ns\":%llu,"
	              "\"wait_flip_done\":%llu,\"wait_flip_done_ns\":%llu,\"wait_flip_done_max_ns\":%llu,"
	              "\"in_flight_current\":%llu,\"in_flight_max\":%llu,"
	              "\"frame_samples\":%llu,\"frame_time_p50_us\":%llu,\"frame_time_p95_us\":%llu,"
	              "\"frame_time_p99_us\":%llu,\"frame_time_max_us\":%llu,"
	              "\"frames_over_50ms\":%llu,\"frames_over_100ms\":%llu,\"frames_over_250ms\":%llu,"
	              "\"hash_calls\":%llu,\"hash_bytes\":%llu,\"hash_ns\":%llu,\"hash_max_ns\":%llu,"
	              "\"detile_calls\":%llu,\"detile_bytes\":%llu,\"detile_ns\":%llu,\"detile_max_ns\":%llu,"
	              "\"upload_calls\":%llu,\"upload_bytes\":%llu,\"upload_ns\":%llu,\"upload_max_ns\":%llu,"
	              "\"writeback_calls\":%llu,\"writeback_bytes\":%llu,\"writeback_ns\":%llu,\"writeback_max_ns\":%llu,"
	              "\"gfx_pipeline_lookup_hits\":%llu,\"gfx_pipeline_lookup_misses\":%llu,"
	              "\"gfx_pipeline_lookup_ns\":%llu,\"gfx_pipeline_lookup_max_ns\":%llu,"
	              "\"compute_pipeline_lookup_hits\":%llu,\"compute_pipeline_lookup_misses\":%llu,"
	              "\"compute_pipeline_lookup_ns\":%llu,\"compute_pipeline_lookup_max_ns\":%llu,"
	              "\"pipeline_evictions\":%llu,"
	              "\"gfx_pipeline_miss_count\":%llu,\"gfx_pipeline_miss_ns\":%llu,\"gfx_pipeline_miss_max_ns\":%llu,"
	              "\"compute_pipeline_miss_count\":%llu,\"compute_pipeline_miss_ns\":%llu,\"compute_pipeline_miss_max_ns\":%llu,"
	              "\"shader_ir_input_analysis_count\":%llu,\"shader_ir_input_analysis_ns\":%llu,"
	              "\"shader_ir_input_analysis_max_ns\":%llu,"
	              "\"shader_ir_pipeline_miss_count\":%llu,\"shader_ir_pipeline_miss_ns\":%llu,"
	              "\"shader_ir_pipeline_miss_max_ns\":%llu,"
	              "\"spirv_source_count\":%llu,\"spirv_source_ns\":%llu,\"spirv_source_max_ns\":%llu,"
	              "\"spirv_compile_count\":%llu,\"spirv_compile_ns\":%llu,\"spirv_compile_max_ns\":%llu,"
	              "\"vk_graphics_pipeline_create_count\":%llu,\"vk_graphics_pipeline_create_ns\":%llu,"
	              "\"vk_graphics_pipeline_create_max_ns\":%llu,"
	              "\"vk_compute_pipeline_create_count\":%llu,\"vk_compute_pipeline_create_ns\":%llu,"
	              "\"vk_compute_pipeline_create_max_ns\":%llu,"
	              "\"shader_translation_cache_hits\":%llu,\"shader_translation_cache_misses\":%llu,"
	              "\"shader_translation_cache_evictions\":%llu,"
	              "\"live_objects\":%llu,\"fps\":%.3f,\"frame_time_ms\":%.3f}",
	              Kyty::Agent::kProtocolVersion, reset ? "true" : "false", static_cast<unsigned long long>(stats.interval_ms),
	              static_cast<unsigned long long>(stats.draws), static_cast<unsigned long long>(stats.dispatches),
	              static_cast<unsigned long long>(stats.alloc_bytes), static_cast<unsigned long long>(stats.free_bytes),
	              static_cast<unsigned long long>(stats.creates), static_cast<unsigned long long>(stats.frees),
	              static_cast<unsigned long long>(stats.flips), static_cast<unsigned long long>(stats.buffer_flushes),
	              static_cast<unsigned long long>(stats.command_buffers), static_cast<unsigned long long>(stats.submits),
	              static_cast<unsigned long long>(stats.fence_waits), static_cast<unsigned long long>(stats.fence_wait_ns),
	              static_cast<unsigned long long>(stats.fence_wait_max_ns), static_cast<unsigned long long>(stats.acquires),
	              static_cast<unsigned long long>(stats.acquire_ns), static_cast<unsigned long long>(stats.acquire_max_ns),
	              static_cast<unsigned long long>(stats.presents), static_cast<unsigned long long>(stats.present_ns),
	              static_cast<unsigned long long>(stats.present_max_ns), static_cast<unsigned long long>(stats.wait_reg_mem),
	              static_cast<unsigned long long>(stats.wait_reg_mem_ns), static_cast<unsigned long long>(stats.wait_reg_mem_max_ns),
	              static_cast<unsigned long long>(stats.wait_flip_done), static_cast<unsigned long long>(stats.wait_flip_done_ns),
	              static_cast<unsigned long long>(stats.wait_flip_done_max_ns), static_cast<unsigned long long>(stats.in_flight_current),
	              static_cast<unsigned long long>(stats.in_flight_max), static_cast<unsigned long long>(stats.frame_samples),
	              static_cast<unsigned long long>(stats.frame_time_p50_us), static_cast<unsigned long long>(stats.frame_time_p95_us),
	              static_cast<unsigned long long>(stats.frame_time_p99_us), static_cast<unsigned long long>(stats.frame_time_max_us),
	              static_cast<unsigned long long>(stats.frames_over_50ms), static_cast<unsigned long long>(stats.frames_over_100ms),
	              static_cast<unsigned long long>(stats.frames_over_250ms), static_cast<unsigned long long>(stats.hash_calls),
	              static_cast<unsigned long long>(stats.hash_bytes), static_cast<unsigned long long>(stats.hash_ns),
	              static_cast<unsigned long long>(stats.hash_max_ns), static_cast<unsigned long long>(stats.detile_calls),
	              static_cast<unsigned long long>(stats.detile_bytes), static_cast<unsigned long long>(stats.detile_ns),
	              static_cast<unsigned long long>(stats.detile_max_ns), static_cast<unsigned long long>(stats.upload_calls),
	              static_cast<unsigned long long>(stats.upload_bytes), static_cast<unsigned long long>(stats.upload_ns),
	              static_cast<unsigned long long>(stats.upload_max_ns), static_cast<unsigned long long>(stats.writeback_calls),
	              static_cast<unsigned long long>(stats.writeback_bytes), static_cast<unsigned long long>(stats.writeback_ns),
	              static_cast<unsigned long long>(stats.writeback_max_ns),
	              static_cast<unsigned long long>(stats.gfx_pipeline_lookup_hits),
	              static_cast<unsigned long long>(stats.gfx_pipeline_lookup_misses),
	              static_cast<unsigned long long>(stats.gfx_pipeline_lookup_ns),
	              static_cast<unsigned long long>(stats.gfx_pipeline_lookup_max_ns),
	              static_cast<unsigned long long>(stats.compute_pipeline_lookup_hits),
	              static_cast<unsigned long long>(stats.compute_pipeline_lookup_misses),
	              static_cast<unsigned long long>(stats.compute_pipeline_lookup_ns),
	              static_cast<unsigned long long>(stats.compute_pipeline_lookup_max_ns),
	              static_cast<unsigned long long>(stats.pipeline_evictions),
	              static_cast<unsigned long long>(stats.gfx_pipeline_miss_count),
	              static_cast<unsigned long long>(stats.gfx_pipeline_miss_ns),
	              static_cast<unsigned long long>(stats.gfx_pipeline_miss_max_ns),
	              static_cast<unsigned long long>(stats.compute_pipeline_miss_count),
	              static_cast<unsigned long long>(stats.compute_pipeline_miss_ns),
	              static_cast<unsigned long long>(stats.compute_pipeline_miss_max_ns),
	              static_cast<unsigned long long>(stats.shader_ir_input_analysis_count),
	              static_cast<unsigned long long>(stats.shader_ir_input_analysis_ns),
	              static_cast<unsigned long long>(stats.shader_ir_input_analysis_max_ns),
	              static_cast<unsigned long long>(stats.shader_ir_pipeline_miss_count),
	              static_cast<unsigned long long>(stats.shader_ir_pipeline_miss_ns),
	              static_cast<unsigned long long>(stats.shader_ir_pipeline_miss_max_ns),
	              static_cast<unsigned long long>(stats.spirv_source_count), static_cast<unsigned long long>(stats.spirv_source_ns),
	              static_cast<unsigned long long>(stats.spirv_source_max_ns), static_cast<unsigned long long>(stats.spirv_compile_count),
	              static_cast<unsigned long long>(stats.spirv_compile_ns), static_cast<unsigned long long>(stats.spirv_compile_max_ns),
	              static_cast<unsigned long long>(stats.vk_graphics_pipeline_create_count),
	              static_cast<unsigned long long>(stats.vk_graphics_pipeline_create_ns),
	              static_cast<unsigned long long>(stats.vk_graphics_pipeline_create_max_ns),
	              static_cast<unsigned long long>(stats.vk_compute_pipeline_create_count),
	              static_cast<unsigned long long>(stats.vk_compute_pipeline_create_ns),
	              static_cast<unsigned long long>(stats.vk_compute_pipeline_create_max_ns),
	              static_cast<unsigned long long>(stats.shader_translation_cache_hits),
	              static_cast<unsigned long long>(stats.shader_translation_cache_misses),
	              static_cast<unsigned long long>(stats.shader_translation_cache_evictions),
	              static_cast<unsigned long long>(stats.live_objects), stats.fps, stats.frame_time_ms);
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
	const EventRingStats     ring  = EventRing::Instance().GetStats();
	// History (events) is separate from current state (ring stats / status).
	std::string out = "{\"protocol_version\":";
	out += std::to_string(Kyty::Agent::kProtocolVersion);
	out += ",\"schema\":\"event_history\",\"ring\":{\"capacity\":";
	out += std::to_string(ring.capacity);
	out += ",\"size\":";
	out += std::to_string(ring.size);
	out += ",\"next_seq\":";
	out += std::to_string(ring.next_seq);
	out += ",\"total_pushed\":";
	out += std::to_string(ring.total_pushed);
	out += ",\"dropped\":";
	out += std::to_string(ring.dropped);
	out += ",\"overflowed\":";
	out += ring.overflowed ? "true" : "false";
	out += "},\"events\":[";
	for (uint32_t i = 0; i < count; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		char item[512];
		std::snprintf(item, sizeof(item), "{\"seq\":%llu,\"t_ms\":%llu,\"kind\":%s,\"code\":%s,\"message\":%s}",
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
	std::snprintf(buf, sizeof(buf), "{\"event\":{\"seq\":%llu,\"t_ms\":%llu,\"kind\":%s,\"code\":%s,\"message\":%s}}",
	              static_cast<unsigned long long>(rec.seq), static_cast<unsigned long long>(rec.t_ms),
	              JsonString(EventKindName(rec.kind)).c_str(), JsonString(rec.code).c_str(), JsonString(rec.message).c_str());
	return std::string(buf);
}

std::string SyncWaitsResult()
{
	Libs::LibKernel::PthreadCondWaitDiagnostics diagnostics {};
	const bool                                  available = Libs::LibKernel::PthreadGetCondWaitDiagnostics(&diagnostics);
	std::string                                 out       = "{\"enabled\":";
	out += (available && diagnostics.enabled) ? "true" : "false";
	char summary[384];
	std::snprintf(summary, sizeof(summary),
	              ",\"blocked_count\":%u,\"tracked_cond\":\"0x%016llx\",\"tracked_waits\":%u,\"tracked_signals\":%u,\"blocked\":[",
	              diagnostics.blocked_count, static_cast<unsigned long long>(diagnostics.tracked_cond), diagnostics.tracked_waits,
	              diagnostics.tracked_signals);
	out += summary;
	for (uint32_t i = 0; i < diagnostics.blocked_count; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		const auto& waiter = diagnostics.blocked[i];
		char        item[384];
		std::snprintf(item, sizeof(item),
		              "{\"cond\":\"0x%016llx\",\"mutex\":\"0x%016llx\",\"return_addr\":\"0x%016llx\","
		              "\"cond_handle\":\"0x%016llx\",\"mutex_handle\":\"0x%016llx\",\"signal_count\":%u}",
		              static_cast<unsigned long long>(waiter.cond), static_cast<unsigned long long>(waiter.mutex),
		              static_cast<unsigned long long>(waiter.return_addr), static_cast<unsigned long long>(waiter.cond_handle),
		              static_cast<unsigned long long>(waiter.mutex_handle), waiter.signal_count);
		out += item;
	}
	out += "]}";
	return out;
}

std::string ThreadsResult()
{
	Libs::LibKernel::PthreadThreadDiagnostics diagnostics {};
	const bool                                available = Libs::LibKernel::PthreadGetThreadDiagnostics(&diagnostics);
	std::string                               out       = "{\"available\":";
	out += (available && diagnostics.available) ? "true" : "false";
	char summary[192];
	std::snprintf(summary, sizeof(summary), ",\"allocated_count\":%u,\"active_count\":%u,\"thread_count\":%u,\"threads\":[",
	              diagnostics.allocated_count, diagnostics.active_count, diagnostics.thread_count);
	out += summary;
	for (uint32_t i = 0; i < diagnostics.thread_count; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		const auto& thread = diagnostics.threads[i];
		char        item[256];
		std::snprintf(item, sizeof(item),
		              "{\"entry\":\"0x%016llx\",\"argument\":\"0x%016llx\",\"unique_id\":%d,\"started\":%s,"
		              "\"detached\":%s,\"almost_done\":%s,\"free\":%s}",
		              static_cast<unsigned long long>(thread.entry), static_cast<unsigned long long>(thread.argument), thread.unique_id,
		              thread.started ? "true" : "false", thread.detached ? "true" : "false", thread.almost_done ? "true" : "false",
		              thread.free ? "true" : "false");
		out += item;
	}
	out += "]}";
	return out;
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

using RequestHandler = std::string (*)(const Request&);

std::string HandleHelp(const Request& req)
{
	return FormatOk(req.id, HelpResult());
}

std::string HandlePing(const Request& req)
{
	char buf[256];
	std::snprintf(buf, sizeof(buf), "{\"alive\":true,\"protocol_version\":%u,\"uptime_ms\":%llu}", Kyty::Agent::kProtocolVersion,
	              static_cast<unsigned long long>(SteadyMs() - g_start_ms));
	return FormatOk(req.id, buf);
}

std::string HandleStatus(const Request& req)
{
	return FormatOk(req.id, StatusResult());
}

std::string HandleDiagnostics(const Request& req)
{
	return FormatOk(req.id, DiagnosticsResult());
}

std::string HandlePerfSnapshot(const Request& req)
{
	bool reset = false;
	(void)ArgsGetBool(req.args_json, "reset", &reset);
	return FormatOk(req.id, PerformanceResult(reset));
}

std::string HandleSyncWaits(const Request& req)
{
	return FormatOk(req.id, SyncWaitsResult());
}

std::string HandleThreads(const Request& req)
{
	return FormatOk(req.id, ThreadsResult());
}

std::string HandleEvents(const Request& req)
{
	uint32_t last      = 50;
	uint64_t after_seq = 0;
	(void)ArgsGetU32(req.args_json, "last", &last);
	(void)ArgsGetU64(req.args_json, "after_seq", &after_seq);
	return FormatOk(req.id, EventsResult(last, after_seq));
}

std::string HandleLastError(const Request& req)
{
	return FormatOk(req.id, LastErrorResult());
}

struct HandlerEntry
{
	Tool           tool;
	RequestHandler handler;
};

bool ParsePhaseHint(const std::string& name, PhaseHint* out)
{
	if (out == nullptr)
	{
		return false;
	}
	struct Entry
	{
		const char* name;
		PhaseHint   phase;
	};
	static constexpr Entry kPhases[] = {
	    {"not_ready", PhaseHint::NotReady},      {"booting", PhaseHint::Booting}, {"loading", PhaseHint::Loading},
	    {"interactive", PhaseHint::Interactive}, {"stalled", PhaseHint::Stalled},
	};
	for (const auto& entry: kPhases)
	{
		if (name == entry.name)
		{
			*out = entry.phase;
			return true;
		}
	}
	return false;
}

void ReportUnhealthyScore(const FrameScoreMetrics& metrics)
{
	if (metrics.verdict != FrameVerdict::Healthy)
	{
		EventRing::Instance().Push(EventKind::Warn, metrics.verdict_name, metrics.hint);
	}
}

std::string ScoreCaptureJson(const std::string& path)
{
	FrameScoreMetrics metrics {};
	if (!ScoreNativeBmp(path.c_str(), &metrics))
	{
		return "{\"verdict\":\"load_failed\",\"healthy\":false}";
	}
	ReportUnhealthyScore(metrics);
	return FrameScoreToJson(metrics, path.c_str());
}

StallSample ReadStallSample()
{
	Libs::Graphics::WindowPresentStats stats {};
	Libs::Graphics::WindowGetPresentStats(&stats);
	StallSample sample {};
	sample.graphic_ready    = stats.graphic_ready;
	sample.frame            = stats.frame;
	sample.present          = stats.present;
	sample.fps              = stats.fps;
	sample.ms_since_present = stats.ms_since_present;
	sample.ms_since_frame   = stats.ms_since_frame;
	return sample;
}

struct WatchCaptureArtifacts
{
	std::string capture_json = "null";
	std::string score_json   = "null";
};

WatchCaptureArtifacts CaptureWatchArtifacts()
{
	WatchCaptureArtifacts                     artifacts;
	Libs::Graphics::WindowNativeCaptureResult error {};
	uint64_t                                  request_id = 0;
	if (!Libs::Graphics::WindowRequestNativeCapture(&request_id, &error))
	{
		artifacts.capture_json = std::string("{\"ok\":false,\"error\":") + JsonString(error.error_code.c_str()) + "}";
		return artifacts;
	}

	Libs::Graphics::WindowNativeCaptureResult result {};
	if (!Libs::Graphics::WindowWaitNativeCapture(request_id, 15000, &result) || !result.ok)
	{
		artifacts.capture_json = std::string("{\"ok\":false,\"error\":") + JsonString(result.error_code.c_str()) + "}";
		return artifacts;
	}

	g_last_capture_path  = result.path;
	artifacts.score_json = ScoreCaptureJson(result.path);
	char capture[768];
	std::snprintf(capture, sizeof(capture), "{\"path\":%s,\"frame\":%d,\"present\":%llu,\"format\":%s,\"width\":%u,\"height\":%u}",
	              JsonString(result.path.c_str()).c_str(), result.frame, static_cast<unsigned long long>(result.present),
	              JsonString(result.format.c_str()).c_str(), result.width, result.height);
	artifacts.capture_json = capture;
	return artifacts;
}

std::string LastErrorJson()
{
	EventRecord last_error {};
	if (!EventRing::Instance().LastError(&last_error))
	{
		return "null";
	}
	char json[512];
	std::snprintf(json, sizeof(json), "{\"seq\":%llu,\"kind\":%s,\"code\":%s,\"message\":%s}",
	              static_cast<unsigned long long>(last_error.seq), JsonString(EventKindName(last_error.kind)).c_str(),
	              JsonString(last_error.code).c_str(), JsonString(last_error.message).c_str());
	return json;
}

bool StableFor(uint64_t now, uint32_t stable_ms, uint64_t* match_since)
{
	if (*match_since == 0)
	{
		*match_since = now;
	}
	return now - *match_since >= stable_ms;
}

std::string HandleCapture(const Request& req)
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

	const std::string score_json = do_score ? ScoreCaptureJson(result.path) : "null";

	char buf[1536];
	std::snprintf(buf, sizeof(buf),
	              "{\"path\":%s,\"milestone\":%s,\"format\":%s,\"width\":%u,\"height\":%u,\"present\":%llu,\"frame\":%d,\"score\":%s}",
	              JsonString(result.path.c_str()).c_str(), JsonString(result.milestone.c_str()).c_str(),
	              JsonString(result.format.c_str()).c_str(), result.width, result.height, static_cast<unsigned long long>(result.present),
	              result.frame, score_json.c_str());
	return FormatOk(req.id, buf);
}

std::string HandleScore(const Request& req)
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
	ReportUnhealthyScore(metrics);
	const std::string body = FrameScoreToJson(metrics, path.c_str());
	return FormatOk(req.id, body);
}

std::string HandlePadButton(const Request& req)
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
	if (req.kind != Tool::PadTap)
	{
		const bool down = req.kind == Tool::PadDown;
		Libs::Controller::AgentPadSetButton(button, down);
		EventRing::Instance().Push(EventKind::Input, down ? "pad_down" : "pad_up", button_name.c_str());
		return FormatOk(req.id, PadStateResult());
	}
	if (!Libs::Controller::AgentPadScheduleTap(button))
	{
		return FormatErr(req.id, "tap_pending", "a tap is already pending or the button is held");
	}
	EventRing::Instance().Push(EventKind::Input, "pad_tap", button_name.c_str());
	return FormatOk(req.id, PadStateResult());
}

std::string HandlePadDown(const Request& req)
{
	return HandlePadButton(req);
}

std::string HandlePadUp(const Request& req)
{
	return HandlePadButton(req);
}

std::string HandlePadTap(const Request& req)
{
	return HandlePadButton(req);
}

std::string HandlePadAxis(const Request& req)
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

std::string HandlePadClear(const Request& req)
{
	Libs::Controller::AgentPadClear();
	EventRing::Instance().Push(EventKind::Input, "pad_clear", "");
	return FormatOk(req.id, PadStateResult());
}

std::string HandleWaitCounter(const Request& req)
{
	uint64_t   min_value  = 0;
	uint64_t   delta      = 0;
	uint32_t   timeout_ms = 15000;
	const bool have_min   = ArgsGetU64(req.args_json, "min", &min_value);
	const bool have_delta = ArgsGetU64(req.args_json, "delta", &delta) && delta > 0;
	(void)ArgsGetU32(req.args_json, "timeout_ms", &timeout_ms);
	if (!have_min && !have_delta)
	{
		return FormatErr(req.id, "invalid_args", "min or delta is required");
	}
	if (have_delta)
	{
		Libs::Graphics::WindowPresentStats now {};
		Libs::Graphics::WindowGetPresentStats(&now);
		const uint64_t current = req.kind == Tool::WaitPresent ? now.present : static_cast<uint64_t>(now.frame);
		min_value              = current + delta;
	}
	const uint64_t deadline = SteadyMs() + timeout_ms;
	while (!g_stop.load())
	{
		Libs::Graphics::WindowPresentStats stats {};
		Libs::Graphics::WindowGetPresentStats(&stats);
		const uint64_t current = req.kind == Tool::WaitPresent ? stats.present : static_cast<uint64_t>(stats.frame);
		if (current >= min_value)
		{
			char buf[192];
			std::snprintf(buf, sizeof(buf), "{\"%s\":%llu,\"target\":%llu,\"phase\":%s}",
			              req.kind == Tool::WaitPresent ? "present" : "frame", static_cast<unsigned long long>(current),
			              static_cast<unsigned long long>(min_value),
			              JsonString(PhaseHintName(ClassifyPhaseHint(stats.graphic_ready, stats.present, stats.fps, stats.ms_since_present,
			                                                         PhaseHintArgs {})))
			                  .c_str());
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

std::string HandleWaitPresent(const Request& req)
{
	return HandleWaitCounter(req);
}

std::string HandleWaitFrame(const Request& req)
{
	return HandleWaitCounter(req);
}

std::string HandleWaitPhase(const Request& req)
{
	std::string want_name  = "interactive";
	uint32_t    timeout_ms = 45000;
	uint32_t    stable_ms  = 400;
	uint32_t    min_fps_u  = 5;
	uint32_t    stall_ms   = 2000;
	(void)ArgsGetString(req.args_json, "want", &want_name);
	(void)ArgsGetU32(req.args_json, "timeout_ms", &timeout_ms);
	(void)ArgsGetU32(req.args_json, "stable_ms", &stable_ms);
	(void)ArgsGetU32(req.args_json, "min_fps", &min_fps_u);
	(void)ArgsGetU32(req.args_json, "stall_ms", &stall_ms);
	PhaseHint want {};
	if (!ParsePhaseHint(want_name, &want))
	{
		return FormatErr(req.id, "invalid_args", "want must be not_ready|booting|loading|interactive|stalled");
	}
	PhaseHintArgs phase_args {};
	phase_args.interactive_min_fps = static_cast<double>(min_fps_u);
	phase_args.stall_ms            = stall_ms;
	if (stable_ms < 50)
	{
		stable_ms = 50;
	}
	const uint64_t deadline    = SteadyMs() + timeout_ms;
	uint64_t       match_since = 0;
	PhaseHint      last_phase  = PhaseHint::NotReady;
	while (!g_stop.load())
	{
		Libs::Graphics::WindowPresentStats stats {};
		Libs::Graphics::WindowGetPresentStats(&stats);
		last_phase         = ClassifyPhaseHint(stats.graphic_ready, stats.present, stats.fps, stats.ms_since_present, phase_args);
		const uint64_t now = SteadyMs();
		if (last_phase != want)
		{
			match_since = 0;
		}
		if (last_phase == want && StableFor(now, stable_ms, &match_since))
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), "{\"phase\":%s,\"present\":%llu,\"fps\":%.3f,\"ms_since_present\":%llu,\"stable_ms\":%u}",
			              JsonString(PhaseHintName(last_phase)).c_str(), static_cast<unsigned long long>(stats.present), stats.fps,
			              static_cast<unsigned long long>(stats.ms_since_present), stable_ms);
			return FormatOk(req.id, buf);
		}
		if (now >= deadline)
		{
			char msg[192];
			std::snprintf(msg, sizeof(msg), "wait_phase timed out in phase=%s want=%s", PhaseHintName(last_phase), want_name.c_str());
			return FormatErr(req.id, "timeout", msg);
		}
		Core::Thread::Sleep(20);
	}
	return FormatErr(req.id, "socket_gone", "agent server stopping");
}

std::string HandleWaitEvent(const Request& req)
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
		EventRecord    records[16] {};
		const uint32_t count = EventRing::Instance().CopySince(after_seq, records, 16);
		for (uint32_t i = 0; i < count; ++i)
		{
			// CopySince returns newest-first; scan for match.
			if (records[i].kind == kind)
			{
				char buf[512];
				std::snprintf(buf, sizeof(buf), "{\"event\":{\"seq\":%llu,\"t_ms\":%llu,\"kind\":%s,\"code\":%s,\"message\":%s}}",
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

std::string HandleWatch(const Request& req)
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

	const StallSample start = ReadStallSample();

	const uint64_t deadline = SteadyMs() + args.window_ms;
	while (!g_stop.load() && SteadyMs() < deadline)
	{
		Core::Thread::Sleep(50);
	}

	const StallSample end = ReadStallSample();

	const StallWatchResult classified = ClassifyStall(start, end, args);
	if (!classified.healthy)
	{
		EventRing::Instance().Push(EventKind::Warn, classified.code_name, classified.hint);
	}

	WatchCaptureArtifacts artifacts;
	if (do_capture && !classified.healthy)
	{
		artifacts = CaptureWatchArtifacts();
	}

	const std::string last_error_json = LastErrorJson();

	char buf[3072];
	std::snprintf(buf, sizeof(buf),
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
	              static_cast<long long>(classified.present_delta), artifacts.capture_json.c_str(), artifacts.score_json.c_str(),
	              last_error_json.c_str());

	// Transport succeeds; agents/CLI treat healthy=false as a detected hang.
	return FormatOk(req.id, buf);
}

std::string HandleUnknown(const Request& req)
{
	return FormatErr(req.id, "unknown_tool", "unknown tool");
}

static constexpr HandlerEntry kHandlers[] = {
    {Tool::Help, HandleHelp},           {Tool::Ping, HandlePing},
    {Tool::Status, HandleStatus},       {Tool::Diagnostics, HandleDiagnostics},
    {Tool::PerfSnapshot, HandlePerfSnapshot},
    {Tool::SyncWaits, HandleSyncWaits}, {Tool::Threads, HandleThreads},
    {Tool::Events, HandleEvents},       {Tool::LastError, HandleLastError},
    {Tool::Capture, HandleCapture},     {Tool::Score, HandleScore},
    {Tool::PadDown, HandlePadDown},     {Tool::PadUp, HandlePadUp},
    {Tool::PadTap, HandlePadTap},       {Tool::PadAxis, HandlePadAxis},
    {Tool::PadClear, HandlePadClear},   {Tool::WaitPresent, HandleWaitPresent},
    {Tool::WaitFrame, HandleWaitFrame}, {Tool::WaitPhase, HandleWaitPhase},
    {Tool::WaitEvent, HandleWaitEvent}, {Tool::Watch, HandleWatch},
    {Tool::Unknown, HandleUnknown},
};

std::string Dispatch(const Request& req)
{
	for (const auto& entry: kHandlers)
	{
		if (entry.tool == req.kind)
		{
			return entry.handler(req);
		}
	}
	return HandleUnknown(req);
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
	while (out->size() < Kyty::Agent::kRequestLineMax)
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
		(void)WriteAll(client_fd, FormatErr(0, "busy", "only one agent client is supported"));
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
