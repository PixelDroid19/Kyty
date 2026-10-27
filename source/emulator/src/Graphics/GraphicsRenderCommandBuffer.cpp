#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Agent/Json.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Agent/EventRing.h"
#include "Emulator/Graphics/CommandProcessorSubmissionSlots.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/DiagnosticDump.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/GpuMemoryTransientBuffer.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/SampleLocations.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderProgramSnapshot.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"
#include "Emulator/Ports/ProgressPort.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// CommandBuffer methods + TransientBufferPool

namespace {

// One transport set holds the five host transport buffers; a recording needs
// one set per draw tile, and tiles are bounded per draw.
constexpr uint32_t kTransportBindingsMax = 5;
constexpr uint32_t kTransportSetsMax     = 1024;

VulkanSubmitAttemptTrail g_submit_fault_trail;

const char* VulkanSubmitKindName(VulkanSubmitKind kind)
{
	switch (kind)
	{
		case VulkanSubmitKind::CommandBuffer: return "command_buffer";
		case VulkanSubmitKind::SemaphoreCommandBuffer: return "semaphore_command_buffer";
		case VulkanSubmitKind::TileDetile: return "tile_detile";
	}
	return "unknown";
}

std::string OptionalUnsigned(bool known, uint64_t value)
{
	return known ? std::to_string(value) : std::string("null");
}

std::string SubmitFaultContextJson(const VulkanSubmitAttempt& context)
{
	const bool guest = context.has_guest_context;
	return "\"queue\":" + std::to_string(context.queue) + ",\"slot\":" + std::to_string(context.command_buffer_slot) +
	       ",\"host_sequence\":" + OptionalUnsigned(context.has_host_submission, context.host_submission_sequence) +
	       ",\"guest_submit\":" + OptionalUnsigned(guest, context.guest_submit) +
	       ",\"presented_frame\":" + std::to_string(context.presented_frame) +
	       ",\"pm4_op\":" + OptionalUnsigned(guest, context.pm4_op) + ",\"pm4_dw\":" + OptionalUnsigned(guest, context.pm4_dw);
}

std::string SubmitFaultAttemptJson(const VulkanSubmitAttempt& entry)
{
	return "{\"attempt\":" + std::to_string(entry.attempt) + ",\"kind\":" +
	       ::Kyty::Agent::JsonString(VulkanSubmitKindName(entry.kind)) + ',' + SubmitFaultContextJson(entry) +
	       ",\"semaphore\":" + (entry.signals_semaphore ? "true" : "false") +
	       ",\"completed\":" + (entry.completed ? "true" : "false") +
	       ",\"result\":" + std::to_string(static_cast<int>(entry.result)) + '}';
}

std::string SubmitFaultReportJson(const char* stage, VkResult result, const VulkanSubmitAttemptSnapshot& snapshot,
                                 const VulkanSubmitAttempt* immediate, const VulkanSubmitAttempt* context)
{
	char bounded_stage[65] {};
	std::snprintf(bounded_stage, sizeof(bounded_stage), "%.64s", stage != nullptr ? stage : "unknown");
	std::string report = "{\"schema\":\"vulkan_submit_fault\",\"version\":2,\"stage\":" +
	                     ::Kyty::Agent::JsonString(bounded_stage) + ",\"result\":" + std::to_string(static_cast<int>(result)) +
	                     ",\"count\":" + std::to_string(snapshot.count) + ",\"dropped\":" + std::to_string(snapshot.dropped) +
	                     ",\"completed_means\":\"submit_return_not_fence\",\"immediate_tracked\":" +
	                     (immediate != nullptr && immediate->attempt != 0u ? "true" : "false") +
	                     ",\"immediate\":" + (immediate != nullptr ? SubmitFaultAttemptJson(*immediate) : "null") +
	                     ",\"command_buffer_context\":" + (context != nullptr ? '{' + SubmitFaultContextJson(*context) + '}' : "null") +
	                     ",\"entries\":[";
	for (uint32_t i = 0; i < snapshot.count; ++i)
	{
		if (i != 0u)
		{
			report += ',';
		}
		report += SubmitFaultAttemptJson(snapshot.entries[i]);
	}
	return report + "]}\n";
}

const char* WriteSubmitFaultReport(const char* stage, VkResult result, const VulkanSubmitAttemptSnapshot& snapshot,
                                  const VulkanSubmitAttempt* immediate, const VulkanSubmitAttempt* context)
{
	const char* path = std::getenv("KYTY_SUBMIT_FAULT_REPORT");
	if (path == nullptr || path[0] == '\0')
	{
		return "disabled";
	}
	const auto report = SubmitFaultReportJson(stage, result, snapshot, immediate, context);
	if (report.size() > 8192u)
	{
		return "size_limit";
	}
	// Explicit process-local scratch path only. Never overwrite earlier evidence
	// or follow an existing FIFO/symlink, and never retry the fatal operation.
	auto* file = std::fopen(path, "wbx");
	if (file == nullptr)
	{
		return "open_failed";
	}
	const bool written = std::fwrite(report.data(), 1, report.size(), file) == report.size();
	const bool closed = std::fclose(file) == 0;
	return written && closed ? "written" : "write_failed";
}

void PublishSubmitFaultEvent(VkResult result, const VulkanSubmitAttemptSnapshot& snapshot, const char* report_status,
                             const char* draws_status, const VulkanSubmitAttempt* context)
{
	char message[Emulator::Agent::kAgentEventMessageMax] {};
	if (context != nullptr)
	{
		const auto sequence = OptionalUnsigned(context->has_host_submission, context->host_submission_sequence);
		std::snprintf(message, sizeof(message), "result=%d report=%s queue=%" PRIu32 " slot=%" PRIu32 " host_sequence=%s draws=%s",
		              static_cast<int>(result), report_status, context->queue, context->command_buffer_slot, sequence.c_str(),
		              draws_status);
	} else
	{
		std::snprintf(message, sizeof(message), "result=%d report=%s count=%" PRIu32 " dropped=%" PRIu64 " draws=%s",
		              static_cast<int>(result), report_status, snapshot.count, snapshot.dropped, draws_status);
	}
	Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Fatal, "device_lost", message);
}

constexpr uint32_t kRecentDrawDefaultCapacity = 64;

std::atomic<uint64_t> g_skipped_invalid_vertex_shader {0};
std::atomic<uint64_t> g_skipped_unsupported_ge_state {0};

std::string WriteProgramSnapshotReport(const char* directory, const char* name, const ShaderProgramSnapshot& snapshot,
                                      DiagnosticDumpWriter& writer)
{
	const auto status = snapshot.words.empty() ? DiagnosticDumpStatus::InvalidData :
	    writer.Write(directory, name, snapshot.words.data(), snapshot.words.size() * sizeof(uint32_t));
	return "{\"snapshot\":" + ::Kyty::Agent::JsonString(ShaderProgramSnapshotStatusName(snapshot.status)) +
	       ",\"mapped_bytes\":" + std::to_string(snapshot.mapped_bytes) +
	       ",\"captured_bytes\":" + std::to_string(snapshot.words.size() * sizeof(uint32_t)) +
	       ",\"file\":" + ::Kyty::Agent::JsonString(name) +
	       ",\"write\":" + ::Kyty::Agent::JsonString(DiagnosticDumpStatusName(status)) + '}';
}

std::string WriteSkippedProgramReport(const char* directory, const char* name, uint64_t address)
{
	return WriteProgramSnapshotReport(directory, name, ShaderSnapshotMappedProgram(address), DiagnosticDumpProcessWriter());
}

// Opt-in KYTY_SKIPPED_DRAW_REPORT=<prefix>: the first skip of each reason is
// written once to <prefix>-<reason>.json with exclusive creation, so the
// evidence survives an event ring that later rolls over.
void WriteSkippedDrawReport(const char* reason, const char* detail, const GraphicsSkippedGeState* ge_state)
{
	const char* prefix = std::getenv("KYTY_SKIPPED_DRAW_REPORT");
	if (prefix == nullptr || prefix[0] == '\0')
	{
		return;
	}
	const std::string path = std::string(prefix) + "-" + reason + ".json";
	if (path.size() > 1023u)
	{
		return;
	}
	auto* file = std::fopen(path.c_str(), "wbx");
	if (file == nullptr)
	{
		return;
	}
	const auto presented_frame = WindowGetPresentedFrameNum();
	char bounded_detail[161] {};
	std::snprintf(bounded_detail, sizeof(bounded_detail), "%.160s", detail != nullptr ? detail : "");
	std::string programs = "null";
	const char* program_directory = std::getenv("KYTY_SKIPPED_SHADER_DUMP");
	if (ge_state != nullptr && program_directory != nullptr && program_directory[0] != '\0')
	{
		programs = "{\"es\":" + WriteSkippedProgramReport(program_directory, "skipped-ge-es.bin", ge_state->es_program) +
		           ",\"gs_back\":" + WriteSkippedProgramReport(program_directory, "skipped-ge-gs-back.bin", ge_state->gs_back_program) + '}';
	}
	const std::string report = "{\"schema\":\"guest_draw_skipped\",\"version\":2,\"reason\":" + ::Kyty::Agent::JsonString(reason) +
	                           ",\"detail\":" + ::Kyty::Agent::JsonString(bounded_detail) +
	                           ",\"ge_state\":" + (ge_state != nullptr ? GraphicsSkippedGeStateJson(*ge_state) : "null") +
	                           ",\"program_snapshots\":" + programs +
	                           ",\"presented_frame\":" + std::to_string(presented_frame) + "}\n";
	if (report.size() <= 8192u)
	{
		(void)std::fwrite(report.data(), 1, report.size(), file);
	}
	(void)std::fclose(file);
}

thread_local VulkanRecentDrawPacket g_recent_draw_packet;

const char* VulkanRecentDrawKindName(VulkanRecentDrawKind kind)
{
	switch (kind)
	{
		case VulkanRecentDrawKind::DrawIndexed: return "draw_indexed";
		case VulkanRecentDrawKind::Draw: return "draw";
		case VulkanRecentDrawKind::Dispatch: return "dispatch";
	}
	return "unknown";
}

std::string OptionalChecksum(bool known, uint64_t value)
{
	if (!known)
	{
		return "null";
	}
	char data[24] {};
	std::snprintf(data, sizeof(data), "\"0x%016" PRIx64 "\"", value);
	return data;
}

bool NativeWaveInputSensitive(const ShaderNativeWaveInfo& wave)
{
	switch (wave.proof)
	{
		case ShaderNativeWaveProof::ExactSubgroup:
		case ShaderNativeWaveProof::QuadLocal:
		case ShaderNativeWaveProof::FragmentNeutral32:
		case ShaderNativeWaveProof::FragmentPartialWave: return true;
		case ShaderNativeWaveProof::Unclassified:
		case ShaderNativeWaveProof::LaneLocal: return false;
	}
	return false;
}

std::string NativeWaveDrawJson(const GraphicsNativeWaveDrawInfo& draw)
{
	return "{\"count\":" + OptionalUnsigned(draw.count.has_value(), draw.count.value_or(0)) +
	       ",\"indexed\":" + (draw.indexed.has_value() ? (*draw.indexed ? "true" : "false") : "null") +
	       ",\"index_type\":" + OptionalUnsigned(draw.index_type.has_value(), draw.index_type.value_or(0)) +
	       ",\"first_instance\":" + OptionalUnsigned(draw.first_instance.has_value(), draw.first_instance.value_or(0)) +
	       ",\"instance_count\":" + OptionalUnsigned(draw.instance_count.has_value(), draw.instance_count.value_or(0)) +
	       ",\"index_address\":" + OptionalChecksum(draw.index_address.has_value(), draw.index_address.value_or(0)) +
	       ",\"vertex_offset_add\":" + (draw.vertex_offset_add ? std::to_string(*draw.vertex_offset_add) : "null") +
	       ",\"draw_modifier\":" + OptionalChecksum(draw.draw_modifier.has_value(), draw.draw_modifier.value_or(0)) +
	       ",\"primitive_type\":" + OptionalUnsigned(draw.primitive_type.has_value(), draw.primitive_type.value_or(0)) +
	       ",\"index_offset\":" + OptionalUnsigned(draw.index_offset.has_value(), draw.index_offset.value_or(0)) + '}';
}

std::string NativeWaveInputMetadataJson(const GraphicsSkippedGeState& state, const ShaderNativeWaveInfo& wave,
                                        uint32_t requested_subgroup_size, const GraphicsNativeWaveDrawInfo& draw)
{
	char bounded_reason[162] {};
	std::snprintf(bounded_reason, sizeof(bounded_reason), "%.161s", wave.refusal_reason != nullptr ? wave.refusal_reason : "");
	const bool reason_truncated = std::strlen(bounded_reason) > 160u;
	bounded_reason[160] = '\0';
	return "{\"schema\":\"native_wave_input\",\"version\":1,\"stage\":\"vertex\","
	       "\"recorded_means\":\"input_observed_not_admitted_or_executed\",\"native_wave\":{\"guest_wave_size\":" +
	       std::to_string(wave.guest_wave_size) + ",\"proof\":" + std::to_string(static_cast<uint32_t>(wave.proof)) +
	       ",\"requested_subgroup_size\":" + std::to_string(requested_subgroup_size) +
	       ",\"refusal_pc\":" + std::to_string(wave.refusal_pc) +
	       ",\"refusal_reason\":" + (wave.refusal_reason != nullptr ? ::Kyty::Agent::JsonString(bounded_reason) : "null") +
	       ",\"refusal_reason_truncated\":" + (reason_truncated ? "true" : "false") +
	       "},\"draw\":" + NativeWaveDrawJson(draw) + ",\"ge_state\":" + GraphicsSkippedGeStateJson(state);
}

std::string RecentDrawArgumentsJson(const VulkanRecentDraw& draw)
{
	const bool dispatch = draw.kind == VulkanRecentDrawKind::Dispatch;
	if (dispatch)
	{
		return ",\"primitive\":null,\"count\":null,\"instances\":null,\"vertex_offset\":null,\"first_instance\":null,\"groups\":[" +
		       std::to_string(draw.groups[0]) + ',' + std::to_string(draw.groups[1]) + ',' + std::to_string(draw.groups[2]) + ']';
	}
	return ",\"primitive\":" + std::to_string(draw.primitive_type) + ",\"count\":" + std::to_string(draw.count) +
	       ",\"instances\":" + std::to_string(draw.instances) + ",\"vertex_offset\":" + std::to_string(draw.vertex_offset) +
	       ",\"first_instance\":" + std::to_string(draw.first_instance) + ",\"groups\":null";
}

std::string RecentDrawJson(const VulkanRecentDraw& draw)
{
	const bool dispatch = draw.kind == VulkanRecentDrawKind::Dispatch;
	// recorded is the only stage this record proves by itself; later stages are
	// joined from the same host submission and stay null until observed.
	return "{\"record\":" + std::to_string(draw.record) + ",\"kind\":" + ::Kyty::Agent::JsonString(VulkanRecentDrawKindName(draw.kind)) +
	       ",\"queue\":" + std::to_string(draw.queue) + ",\"slot\":" + std::to_string(draw.command_buffer_slot) +
	       ",\"host_sequence\":" + OptionalUnsigned(draw.has_host_submission, draw.host_submission_sequence) +
	       ",\"guest_submit\":" + std::to_string(draw.guest_submit) + ",\"pm4_op\":" + OptionalUnsigned(draw.has_pm4, draw.pm4_op) +
	       ",\"pm4_dw\":" + OptionalUnsigned(draw.has_pm4, draw.pm4_dw) + ",\"vs\":" + OptionalChecksum(!dispatch, draw.vs_checksum) +
	       ",\"ps\":" + OptionalChecksum(!dispatch, draw.ps_checksum) + ",\"cs\":" + OptionalChecksum(dispatch, draw.cs_checksum) +
	       RecentDrawArgumentsJson(draw) + ",\"host_commands\":" + std::to_string(draw.host_commands) +
	       ",\"recorded\":true,\"submit_called\":" + (draw.submit_called ? "true" : "null") +
	       ",\"submit_result\":" + (draw.submit_returned ? std::to_string(static_cast<int>(draw.submit_result)) : "null") +
	       ",\"gpu_completed\":" + (draw.fence_completed ? "true" : "null") + '}';
}

uint32_t RecentDrawCapacityFromEnvironment()
{
	const char* value = std::getenv("KYTY_RECENT_DRAW_CAPACITY");
	if (value == nullptr || value[0] == '\0')
	{
		return kRecentDrawDefaultCapacity;
	}
	char*      end    = nullptr;
	const auto parsed = std::strtoul(value, &end, 10);
	const bool valid  = value[0] >= '0' && value[0] <= '9' && *end == '\0' && parsed >= 1u &&
	                   parsed <= VulkanRecentDrawTrail::CAPACITY_MAX;
	return valid ? static_cast<uint32_t>(parsed) : 0u;
}

VulkanRecentDrawFault MakeRecentDrawFault(const char* stage, VkResult result, const VulkanSubmitAttempt* context)
{
	VulkanRecentDrawFault fault;
	fault.stage  = stage;
	fault.result = result;
	if (context != nullptr)
	{
		fault.has_context         = true;
		fault.queue               = context->queue;
		fault.command_buffer_slot = context->command_buffer_slot;
		fault.has_host_submission = context->has_host_submission;
		fault.host_sequence       = context->host_submission_sequence;
	}
	return fault;
}

const char* WriteRecentDrawFaultReport(const char* stage, VkResult result, const VulkanSubmitAttempt* context,
                                      const VulkanFaultSnapshot& snapshot)
{
	if (snapshot.draw_capacity == 0u)
	{
		return "disabled";
	}
	const auto report = VulkanRecentDrawReportJson(MakeRecentDrawFault(stage, result, context), snapshot.draw_capacity,
	                                               snapshot.draws.data(), snapshot.draw_count, snapshot.draw_dropped, snapshot.skipped);
	return VulkanWriteRecentDrawReport(std::getenv("KYTY_RECENT_DRAW_REPORT"), report);
}

} // namespace

std::string VulkanRecentDrawReportJson(const VulkanRecentDrawFault& fault, uint32_t capacity, const VulkanRecentDraw* draws,
                                       uint32_t count, uint64_t dropped, const GraphicsSkippedDrawCounts& skipped)
{
	char bounded_stage[65] {};
	std::snprintf(bounded_stage, sizeof(bounded_stage), "%.64s", fault.stage != nullptr ? fault.stage : "unknown");
	const std::string context =
	    fault.has_context ? "{\"queue\":" + std::to_string(fault.queue) + ",\"slot\":" + std::to_string(fault.command_buffer_slot) +
	                            ",\"host_sequence\":" + OptionalUnsigned(fault.has_host_submission, fault.host_sequence) + '}'
	                      : std::string("null");
	std::string report = "{\"schema\":\"vulkan_recent_draws\",\"version\":1,\"recorded_means\":\"command_recorded_not_executed\""
	                     ",\"gpu_completed_means\":\"command_buffer_fence_observed_signaled\",\"stage\":" +
	                     ::Kyty::Agent::JsonString(bounded_stage) + ",\"result\":" + std::to_string(static_cast<int>(fault.result)) +
	                     ",\"capacity\":" + std::to_string(capacity) + ",\"count\":" + std::to_string(count) +
	                     ",\"dropped\":" + std::to_string(dropped) + ",\"skipped_draws\":{\"invalid_vertex_shader\":" +
	                     std::to_string(skipped.invalid_vertex_shader) +
	                     ",\"unsupported_ge_state\":" + std::to_string(skipped.unsupported_ge_state) +
	                     "},\"command_buffer_context\":" + context + ",\"draws\":[";
	for (uint32_t i = 0; i < count && draws != nullptr; ++i)
	{
		if (i != 0u)
		{
			report += ',';
		}
		report += RecentDrawJson(draws[i]);
		if (report.size() > RECENT_DRAW_REPORT_BYTES_MAX)
		{
			return {};
		}
	}
	report += "]}\n";
	return report.size() <= RECENT_DRAW_REPORT_BYTES_MAX ? report : std::string {};
}

const char* VulkanWriteRecentDrawReport(const char* path, const std::string& report)
{
	if (path == nullptr || path[0] == '\0')
	{
		return "disabled";
	}
	if (report.empty())
	{
		return "size_limit";
	}
	// Same evidence policy as the submit-fault report: exclusive creation, no
	// overwrite, no retry and no effect on the original fatal path.
	auto* file = std::fopen(path, "wbx");
	if (file == nullptr)
	{
		return "open_failed";
	}
	const bool written = std::fwrite(report.data(), 1, report.size(), file) == report.size();
	const bool closed  = std::fclose(file) == 0;
	return written && closed ? "written" : "write_failed";
}

VulkanRecentDrawTrail* VulkanRecentDrawTraceTrail()
{
	static VulkanRecentDrawTrail* trail = []() -> VulkanRecentDrawTrail*
	{
		const char* path = std::getenv("KYTY_RECENT_DRAW_REPORT");
		if (!VulkanSubmitFaultTraceEnabled() || path == nullptr || path[0] == '\0')
		{
			return nullptr;
		}
		const uint32_t capacity = RecentDrawCapacityFromEnvironment();
		if (capacity == 0u)
		{
			return nullptr;
		}
		static VulkanRecentDrawTrail instance(capacity);
		return &instance;
	}();
	return trail;
}

VulkanRecentDrawPacket VulkanRecentDrawSetPacket(VulkanRecentDrawPacket packet)
{
	const auto previous  = g_recent_draw_packet;
	g_recent_draw_packet = packet;
	return previous;
}

void VulkanRecentDrawRecord(const CommandBuffer* buffer, VulkanRecentDraw draw)
{
	auto* trail = VulkanRecentDrawTraceTrail();
	if (trail == nullptr || buffer == nullptr)
	{
		return;
	}
	buffer->DescribeRecentDrawRecording(&draw);
	draw.has_pm4 = g_recent_draw_packet.valid;
	draw.pm4_op  = g_recent_draw_packet.valid ? g_recent_draw_packet.pm4_op : 0u;
	draw.pm4_dw  = g_recent_draw_packet.valid ? g_recent_draw_packet.pm4_dw : 0u;
	(void)trail->Record(draw);
}

std::string GraphicsSkippedGeStateJson(const GraphicsSkippedGeState& s)
{
	const auto raw_register = [](const GraphicsGeRawRegister& raw)
	{
		return "{\"value\":" + OptionalUnsigned(raw.known, raw.value) +
		       ",\"written\":" + (raw.written ? "true" : "false") +
		       ",\"known\":" + (raw.known ? "true" : "false") + '}';
	};
	std::string report = "{\"stages\":" + std::to_string(s.stages) +
	                     ",\"input_primitive\":" + std::to_string(s.input_primitive) +
	                     ",\"raw_registers\":{\"stages\":" + raw_register(s.stages_raw) +
	                     ",\"ge_control\":" + raw_register(s.ge_control_raw) +
	                     ",\"ge_user_vgpr_en\":" + raw_register(s.ge_user_vgpr_raw) +
	                     ",\"gs_resource1\":" + raw_register(s.gs_resource1_raw) +
	                     ",\"gs_resource2\":" + raw_register(s.gs_resource2_raw) +
	                     ",\"gs_resource3\":" + raw_register(s.gs_resource3_raw) + '}' +
	                     ",\"es_program\":" + OptionalChecksum(true, s.es_program) +
	                     ",\"gs_back_program\":" + OptionalChecksum(true, s.gs_back_program) +
	                     ",\"legacy_gs_program\":" + OptionalChecksum(true, s.legacy_gs_program) +
	                     ",\"gs_checksum\":" + OptionalChecksum(true, s.gs_checksum) +
	                     ",\"gs_user_data_address\":" + OptionalChecksum(true, s.gs_user_data_address) +
	                     ",\"es_resource1\":" + std::to_string(s.es_resource1) +
	                     ",\"gs_resource3\":" + std::to_string(s.gs_resource3) +
	                     ",\"gs_vgprs\":" + std::to_string(s.gs_vgprs) +
	                     ",\"gs_sgprs\":" + std::to_string(s.gs_sgprs) +
	                     ",\"float_mode\":" + std::to_string(s.float_mode) +
	                     ",\"lds_size\":" + std::to_string(s.lds_size) +
	                     ",\"es_vgpr_components\":" + std::to_string(s.es_vgpr_components) +
	                     ",\"gs_vgpr_components\":" + std::to_string(s.gs_vgpr_components) +
	                     ",\"user_sgpr_count\":" + std::to_string(s.user_sgpr_count) +
	                     ",\"max_vertex_out\":" + std::to_string(s.max_vertex_out) +
	                     ",\"output_primitive\":" + std::to_string(s.output_primitive) +
	                     ",\"ngg_subgroup_control\":" + std::to_string(s.ngg_subgroup_control) +
	                     ",\"max_output_per_subgroup\":" + std::to_string(s.max_output_per_subgroup) +
	                     ",\"gs_instance_count\":" + std::to_string(s.gs_instance_count) +
	                     ",\"gs_onchip_control\":" + std::to_string(s.gs_onchip_control) +
	                     ",\"esgs_ring_item_size\":" + std::to_string(s.esgs_ring_item_size) +
	                     ",\"index_format\":" + std::to_string(s.index_format) +
	                     ",\"primitive_group_size\":" + std::to_string(s.primitive_group_size) +
	                     ",\"vertex_group_size\":" + std::to_string(s.vertex_group_size) + ",\"user_sgprs\":[";
	for (size_t i = 0; i < s.user_sgprs.size(); ++i)
	{
		if (i != 0u)
		{
			report += ',';
		}
		report += std::to_string(s.user_sgprs[i]);
	}
	report += "],\"output_state\":";
	if (!s.output_state)
	{
		return report + "null}";
	}
	const auto& output = *s.output_state;
	report += "{\"effective_state_means\":\"emulator_state_not_raw_assignment\",\"raw_registers\":{\"vs_out_config\":" +
	          raw_register(output.vs_out_config_raw) + ",\"position_format\":" + raw_register(output.position_format_raw) +
	          ",\"output_control\":" + raw_register(output.output_control_raw) +
	          ",\"output_primitive\":" + raw_register(output.output_primitive_raw) +
	          "},\"pixel_program\":" + OptionalChecksum(true, output.pixel_program) +
	          ",\"pixel_checksum\":" + OptionalChecksum(true, output.pixel_checksum) +
	          ",\"pixel_embedded\":" + (output.pixel_embedded ? "true" : "false") +
	          ",\"pixel_embedded_id\":" + std::to_string(output.pixel_embedded_id) +
	          ",\"effective\":{\"vs_out_config\":" + std::to_string(output.vs_out_config) +
	          ",\"position_format\":" + std::to_string(output.position_format) +
	          ",\"output_control\":" + std::to_string(output.output_control) +
	          ",\"output_primitive\":" + std::to_string(output.output_primitive) +
	          ",\"pixel_input_enable\":" + std::to_string(output.pixel_input_enable) +
	          ",\"pixel_input_address\":" + std::to_string(output.pixel_input_address) +
	          ",\"pixel_input_control\":" + std::to_string(output.pixel_input_control) +
	          ",\"barycentric_control\":" + std::to_string(output.barycentric_control) +
	          ",\"interpolator_written_mask\":" + std::to_string(output.interpolator_written_mask) + ",\"interpolators\":[";
	for (size_t i = 0; i < output.interpolators.size(); ++i)
	{
		if (i != 0u) { report += ','; }
		report += std::to_string(output.interpolators[i]);
	}
	constexpr std::array<const char*, 11> mode_names = {"cull_front", "cull_back", "face", "poly_mode", "polymode_front_ptype",
	                                                  "polymode_back_ptype", "poly_offset_front_enable", "poly_offset_back_enable",
	                                                  "vtx_window_offset_enable", "provoking_vtx_last", "persp_corr_dis"};
	constexpr std::array<const char*, 12> clip_names = {"user_clip_planes", "user_clip_plane_mode", "dx_clip_space", "vertex_kill_any",
	                                                  "min_z_clip_disable", "max_z_clip_disable", "user_clip_plane_negate_y", "clip_disable",
	                                                  "user_clip_plane_cull_only", "cull_on_clipping_error_disable",
	                                                  "linear_attribute_clip_enable", "force_viewport_index_from_vs_enable"};
	const auto named_values = [&report](const auto& names, const auto& values)
	{
		for (size_t i = 0; i < names.size(); ++i)
		{
			if (i != 0u) { report += ','; }
			report += ::Kyty::Agent::JsonString(names[i]) + ':' + std::to_string(values[i]);
		}
	};
	report += "],\"raster_mode\":{";
	named_values(mode_names, output.raster_mode);
	report += "},\"clip_control\":{";
	named_values(clip_names, output.clip_control);
	return report + "}}}}";
}

const char* GraphicsNativeWaveInputReporter::Report(const char* prefix, const char* program_directory,
                                                   const GraphicsSkippedGeState& state, const ShaderNativeWaveInfo& wave,
                                                   uint32_t requested_subgroup_size, const GraphicsNativeWaveDrawInfo& draw)
{
	if (prefix == nullptr || prefix[0] == '\0')
	{
		return "disabled";
	}
	if (!NativeWaveInputSensitive(wave))
	{
		return "not_wave_sensitive";
	}
	if (m_claimed.exchange(true, std::memory_order_relaxed))
	{
		return "already_reported";
	}
	char path[1024] {};
	const int length = std::snprintf(path, sizeof(path), "%s-native-wave-input.json", prefix);
	if (length < 0 || static_cast<size_t>(length) >= sizeof(path))
	{
		return "path_too_long";
	}

	// Own the scalar metadata and both bounded program copies before filesystem
	// work. No memory lease is held across a write, and a back program is never
	// replaced with the legacy pseudo-stage address or an unbounded scan.
	std::string report = NativeWaveInputMetadataJson(state, wave, requested_subgroup_size, draw);
	const uint64_t es_address = state.es_program;
	const uint64_t gs_back_address = state.gs_back_program;
	std::string programs = "null";
	if (program_directory != nullptr && program_directory[0] != '\0')
	{
		const auto capture = [this](uint64_t address)
		{
			return m_capture != nullptr ? m_capture(address, kShaderProgramSnapshotBytesMax, m_capture_context) :
			                              ShaderSnapshotMappedProgram(address, kShaderProgramSnapshotBytesMax);
		};
		const auto es = capture(es_address);
		const auto gs_back = capture(gs_back_address);
		const auto es_report = WriteProgramSnapshotReport(program_directory, "native-wave-es.bin", es, m_writer);
		const auto gs_back_report = WriteProgramSnapshotReport(program_directory, "native-wave-gs-back.bin", gs_back, m_writer);
		programs = "{\"es\":" + es_report + ",\"gs_back\":" + gs_back_report + '}';
	}
	report += ",\"program_snapshots\":" + programs + "}\n";
	if (report.size() > NATIVE_WAVE_REPORT_BYTES_MAX)
	{
		return "size_limit";
	}
	// The shared report writer creates exclusively and returns the actual write
	// and close outcome. This does not count a skipped draw or publish an event.
	return VulkanWriteRecentDrawReport(path, report);
}

const char* GraphicsReportNativeWaveInput(const GraphicsSkippedGeState& state, const ShaderNativeWaveInfo& wave,
                                          uint32_t requested_subgroup_size, const GraphicsNativeWaveDrawInfo& draw)
{
	const char* prefix = std::getenv("KYTY_NATIVE_WAVE_REPORT");
	if (prefix == nullptr || prefix[0] == '\0')
	{
		return "disabled";
	}
	static GraphicsNativeWaveInputReporter reporter(DiagnosticDumpProcessWriter());
	return reporter.Report(prefix, std::getenv("KYTY_NATIVE_WAVE_SHADER_DUMP"), state, wave, requested_subgroup_size, draw);
}

namespace {

// Distinct skipped GE programs with their draw counts. The table is bounded:
// draws of further programs are counted in `overflow`.
struct SkippedProgramCensusEntry
{
	uint64_t gs_checksum     = 0;
	uint64_t es_program      = 0;
	uint64_t gs_back_program = 0;
	uint32_t stages          = 0;
	uint32_t input_primitive = 0;
	uint32_t max_vertex_out  = 0;
	uint32_t output_primitive = 0;
	uint32_t primitive_group = 0;
	uint32_t vertex_group    = 0;
	uint64_t draws           = 0;
};

constexpr size_t kSkippedProgramCensusMax = 32;

std::mutex                                                             g_skipped_census_mutex;
std::array<SkippedProgramCensusEntry, kSkippedProgramCensusMax> g_skipped_census {};
size_t                                                                 g_skipped_census_size = 0;
uint64_t                                                               g_skipped_census_overflow = 0;

void CountSkippedProgram(const GraphicsSkippedGeState& state)
{
	std::lock_guard<std::mutex> lock(g_skipped_census_mutex);
	for (size_t i = 0; i < g_skipped_census_size; ++i)
	{
		auto& entry = g_skipped_census[i];
		if (entry.gs_checksum == state.gs_checksum && entry.es_program == state.es_program &&
		    entry.gs_back_program == state.gs_back_program && entry.stages == state.stages &&
		    entry.input_primitive == state.input_primitive)
		{
			++entry.draws;
			return;
		}
	}
	if (g_skipped_census_size == kSkippedProgramCensusMax)
	{
		++g_skipped_census_overflow;
		return;
	}
	g_skipped_census[g_skipped_census_size++] = {state.gs_checksum, state.es_program, state.gs_back_program, state.stages,
	                                              state.input_primitive, state.max_vertex_out, state.output_primitive,
	                                              state.primitive_group_size, state.vertex_group_size, 1u};
}

// Opt-in KYTY_SKIPPED_DRAW_REPORT=<prefix>: <prefix>-skipped-census.json is
// rewritten at every milestone with the per-program draw counts seen so far.
void WriteSkippedProgramCensus()
{
	const char* prefix = std::getenv("KYTY_SKIPPED_DRAW_REPORT");
	if (prefix == nullptr || prefix[0] == '\0')
	{
		return;
	}
	const std::string path = std::string(prefix) + "-skipped-census.json";
	if (path.size() > 1023u)
	{
		return;
	}
	std::string report = "{\"schema\":\"guest_draw_skipped_census\",\"version\":2,\"programs\":[";
	{
		std::lock_guard<std::mutex> lock(g_skipped_census_mutex);
		for (size_t i = 0; i < g_skipped_census_size; ++i)
		{
			const auto& e = g_skipped_census[i];
			char        row[384] {};
			std::snprintf(row, sizeof(row),
			              "%s{\"gs_checksum\":\"0x%016llx\",\"es\":\"0x%llx\",\"gs_back\":\"0x%llx\",\"stages\":%u,"
			              "\"input_primitive\":%u,\"max_vertex_out\":%u,\"output_primitive\":%u,\"primitive_group\":%u,"
			              "\"vertex_group\":%u,\"draws\":%llu}",
			              i == 0 ? "" : ",", static_cast<unsigned long long>(e.gs_checksum), static_cast<unsigned long long>(e.es_program),
			              static_cast<unsigned long long>(e.gs_back_program), e.stages, e.input_primitive, e.max_vertex_out,
			              e.output_primitive, e.primitive_group, e.vertex_group, static_cast<unsigned long long>(e.draws));
			report += row;
		}
		report += "],\"overflow\":" + std::to_string(g_skipped_census_overflow) + "}\n";
	}
	if (auto* file = std::fopen(path.c_str(), "wb"); file != nullptr)
	{
		(void)std::fwrite(report.data(), 1, report.size(), file);
		(void)std::fclose(file);
	}
}

} // namespace

void GraphicsRecordSkippedDraw(GraphicsSkippedDrawReason reason, const char* detail, const GraphicsSkippedGeState* ge_state)
{
	const bool invalid_vs = reason == GraphicsSkippedDrawReason::InvalidVertexShader;
	auto&      counter    = invalid_vs ? g_skipped_invalid_vertex_shader : g_skipped_unsupported_ge_state;
	const uint64_t count  = counter.fetch_add(1, std::memory_order_relaxed) + 1u;
	if (ge_state != nullptr)
	{
		CountSkippedProgram(*ge_state);
	}
	if (!GraphicsSkippedDrawEventDue(count))
	{
		return;
	}
	WriteSkippedProgramCensus();
	const char* reason_name = invalid_vs ? "invalid_vertex_shader" : "unsupported_ge_state";
	char        message[Emulator::Agent::kAgentEventMessageMax] {};
	if (count == 1u)
	{
		std::snprintf(message, sizeof(message), "reason=%s %s", reason_name, detail != nullptr ? detail : "");
	} else
	{
		std::snprintf(message, sizeof(message), "reason=%s count=%llu", reason_name, static_cast<unsigned long long>(count));
	}
	Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Warn, "draw_skipped", message);
	if (count == 1u)
	{
		WriteSkippedDrawReport(reason_name, detail, ge_state);
	}
}

GraphicsSkippedDrawCounts GraphicsGetSkippedDrawCounts()
{
	GraphicsSkippedDrawCounts counts;
	counts.invalid_vertex_shader = g_skipped_invalid_vertex_shader.load(std::memory_order_relaxed);
	counts.unsupported_ge_state  = g_skipped_unsupported_ge_state.load(std::memory_order_relaxed);
	return counts;
}

bool VulkanSubmitFaultTraceEnabled()
{
	static const bool enabled = []
	{
		const char* value = std::getenv("KYTY_SUBMIT_FAULT_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

VulkanSubmitAttemptTrail* VulkanSubmitFaultTraceTrail()
{
	return VulkanSubmitFaultTraceEnabled() ? &g_submit_fault_trail : nullptr;
}

bool VulkanLatchFaultSnapshot(VulkanSubmitAttemptTrail* submits, VulkanRecentDrawTrail* draws, VkResult result,
                              VulkanFaultSnapshot* snapshot)
{
	if (submits == nullptr || snapshot == nullptr || !submits->LatchDeviceLost(result, &snapshot->submits))
	{
		return false;
	}
	snapshot->draw_capacity = draws != nullptr ? draws->Capacity() : 0u;
	snapshot->draw_count    = 0u;
	snapshot->draw_dropped  = 0u;
	if (draws != nullptr)
	{
		snapshot->draw_count = draws->Snapshot(snapshot->draws.data(), snapshot->draw_capacity, &snapshot->draw_dropped);
	}
	snapshot->skipped = GraphicsGetSkippedDrawCounts();
	return true;
}

void VulkanSubmitFaultReport(const char* stage, VkResult result, const VulkanSubmitAttempt* immediate,
                             const VulkanSubmitAttempt* command_buffer_context)
{
	VulkanFaultSnapshot captured;
	if (!VulkanLatchFaultSnapshot(VulkanSubmitFaultTraceTrail(), VulkanRecentDrawTraceTrail(), result, &captured))
	{
		return;
	}

	const auto& snapshot      = captured.submits;
	const auto* context       = command_buffer_context != nullptr ? command_buffer_context : immediate;
	const char* report_status = WriteSubmitFaultReport(stage, result, snapshot, immediate, command_buffer_context);
	const char* draws_status  = WriteRecentDrawFaultReport(stage, result, context, captured);
	PublishSubmitFaultEvent(result, snapshot, report_status, draws_status, context);

	// Console mirror of the JSON report; unknown fields stay null here as well.
	const auto immediate_json = immediate != nullptr ? SubmitFaultAttemptJson(*immediate) : std::string("null");
	KYTY_LOG_ERROR("KYTY_SUBMIT_FAULT stage=%s result=%d count=%" PRIu32 " dropped=%" PRIu64 " immediate=%s\n",
	               stage != nullptr ? stage : "unknown", static_cast<int>(result), snapshot.count, snapshot.dropped,
	               immediate_json.c_str());
	for (uint32_t i = 0; i < snapshot.count; ++i)
	{
		KYTY_LOG_ERROR("KYTY_SUBMIT_FAULT_ENTRY %s\n", SubmitFaultAttemptJson(snapshot.entries[i]).c_str());
	}
}

struct ImageTransitionSource
{
	VkPipelineStageFlags stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkAccessFlags        access = 0;
};

static ImageTransitionSource ResolveImageTransitionSource(VkImageLayout layout)
{
	switch (layout)
	{
		case VK_IMAGE_LAYOUT_UNDEFINED: return {};
		case VK_IMAGE_LAYOUT_PREINITIALIZED: return {VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT};
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
		case VK_IMAGE_LAYOUT_GENERAL:
		default:
			return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
	}
}

// Occupancy of one usage class at a refusal: which entries are still bound in this
// command buffer, how many carry identical bytes, and how many are snapshots.
struct TransientPoolUsageOccupancy
{
	uint32_t used               = 0;
	uint32_t free               = 0;
	uint32_t used_four_byte     = 0;
	uint32_t used_four_byte_zero = 0;
	uint32_t used_snapshots     = 0;
	uint32_t used_duplicates    = 0;
	uint32_t largest_duplicate_group = 0;
};

static void ReportCriticalTransientPoolCapacityReject(uint64_t requested_size, uint32_t usage, uint32_t usage_entries,
                                                      uint32_t total_entries, uint64_t total_bytes,
                                                      const TransientPoolUsageOccupancy& occupancy)
{
	const auto critical_class = GpuMemoryTransientBufferAllocationClass::Critical;
	const bool usage_cap_failed = !GpuMemoryTransientBufferPoolCanAllocate(usage_entries, 0u, 0u, 1u, critical_class);
	const bool entry_cap_failed =
	    !GpuMemoryTransientBufferPoolCanAllocate(0u, total_entries, 0u, 1u, critical_class);
	const bool byte_cap_failed =
	    !GpuMemoryTransientBufferPoolCanAllocate(0u, 0u, total_bytes, requested_size, critical_class);
	const uint64_t max_bytes = kGpuMemoryTransientBufferPoolMaxBytes;
	static std::atomic<uint32_t> reports_remaining {8u};
	uint32_t report_number = 0u;
	uint32_t remaining = reports_remaining.load(std::memory_order_relaxed);
	while (remaining != 0u)
	{
		const uint32_t event_number = 9u - remaining;
		if (reports_remaining.compare_exchange_weak(remaining, remaining - 1u, std::memory_order_relaxed,
		                                          std::memory_order_relaxed))
		{
			report_number = event_number;
			break;
		}
	}
	if (report_number == 0u)
	{
		return;
	}

	std::fprintf(stderr,
	             "KYTY_TRANSIENT_POOL_CAPACITY_REJECT event=%" PRIu32 " class=critical(%u) requested=%" PRIu64
	             " usage=0x%08" PRIx32 " usage_entries=%" PRIu32 " total_entries=%" PRIu32 " total_bytes=%" PRIu64
	             " max_bytes=%" PRIu64 " usage_cap_failed=%d entry_cap_failed=%d byte_cap_failed=%d used=%" PRIu32 " free=%" PRIu32
	             " used_4b=%" PRIu32 " used_4b_zero=%" PRIu32 " used_snapshots=%" PRIu32 " used_duplicates=%" PRIu32
	             " largest_duplicate_group=%" PRIu32 "\n",
	             report_number, static_cast<unsigned>(critical_class), requested_size, usage, usage_entries, total_entries,
	             total_bytes, max_bytes, usage_cap_failed ? 1 : 0, entry_cap_failed ? 1 : 0, byte_cap_failed ? 1 : 0, occupancy.used,
	             occupancy.free, occupancy.used_four_byte, occupancy.used_four_byte_zero, occupancy.used_snapshots,
	             occupancy.used_duplicates, occupancy.largest_duplicate_group);
}

// Entries in release order: the oldest leads.
struct TransientFreeList
{
	struct TransientBufferEntry* oldest = nullptr;
	struct TransientBufferEntry* newest = nullptr;
};

struct TransientBufferEntry
{
	VulkanBuffer buffer;
	void*        mapped         = nullptr;
	uint64_t     size           = 0;
	uint32_t     usage          = 0;
	bool         used           = false;
	bool         scratch        = false;
	bool         snapshot_valid = false;
	// The snapshot is indexed by its base alone and serves any read window it covers.
	bool         snapshot_covering = false;
	uint64_t     snapshot_vaddr = 0;
	uint64_t     snapshot_size  = 0;
	// Snapshot bytes start here; the bytes before it hold no guest data.
	uint64_t snapshot_begin = 0;
	// Bytes at or past this offset are known to be zero.
	uint64_t nonzero_end = 0;
	// The dirty tracker covers the snapshot bytes, which match the guest at this write generation.
	bool     tracked            = false;
	uint64_t tracked_generation = 0;
	// Probes the tracked bytes served without a comparison.
	uint32_t tracked_reuses = 0;
	// Creation order; the free index hands out the earliest-created fitting entry.
	uint64_t creation = 0;
	// Recordings of the shared snapshot pool that bind this entry, and the latest of them.
	uint32_t leases           = 0;
	uint64_t lease_generation = 0;
	// Neighbours among the entries holding the same snapshot key, in commit order.
	TransientBufferEntry* snapshot_older = nullptr;
	TransientBufferEntry* snapshot_newer = nullptr;
	// Position in the free index while the entry is unused, and whether that is
	// the index of entries that still hold a snapshot.
	std::map<std::pair<uint64_t, uint64_t>, TransientBufferEntry*>::iterator free_position;
	bool                                                                     free_indexed  = false;
	bool                                                                     free_snapshot = false;
	// A free snapshot's neighbours in its size's release order and in the
	// pool-wide release order, and the size's list it sits in.
	TransientBufferEntry*     free_older     = nullptr;
	TransientBufferEntry*     free_newer     = nullptr;
	TransientBufferEntry*     released_older = nullptr;
	TransientBufferEntry*     released_newer = nullptr;
	struct TransientFreeList* free_list      = nullptr;
	// Position in the pool's entry list.
	size_t pool_index = 0;
};

// Snapshot entries one recording binds. An entry keeps its bytes until every
// recording that leased it has been reset, so a later command buffer of the
// queue binds the same buffer (and the descriptor sets keyed by it) while the
// guest bytes are unchanged.
struct TransientSnapshotLeases
{
	uint64_t                           generation = 0;
	std::vector<TransientBufferEntry*> entries;
};

class TransientBufferPool
{
	static constexpr uint64_t kMaxSnapshotReadSize = 16u;
	// A matching snapshot this large is tracked for guest writes, so later probes
	// skip the comparison. Smaller ones compare faster than page protection costs.
	static constexpr uint64_t kMinTrackedSnapshotSize = 4096u;
	// A 3D frame records tens of command buffers (about forty in the fishing
	// title). Snapshots leased by the last frame or two are kept for the next
	// probe while the pool can grow; ring constants that return later hold
	// new bytes, so keeping them longer only costs memory.
	static constexpr uint64_t kRetainedRecordings = 64u;
	static constexpr uint64_t kExpiredRecordings  = 4u * kRetainedRecordings;
	// Snapshot sizes are never zero, so this size marks the key of covering snapshots.
	static constexpr uint64_t kCoveringKeySize = 0u;

	using Entry     = TransientBufferEntry;
	using FreeIndex = std::map<std::pair<uint64_t, uint64_t>, Entry*>;
	// Free snapshots by size; each size lists its entries in release order, so
	// the first non-empty size at or above a request holds the answer at its head.
	using FreeSnapshotIndex = std::map<uint64_t, TransientFreeList>;

public:
	VulkanBuffer* Upload(GraphicContext* ctx, const void* data, uint64_t size, uint32_t usage,
	                     GpuMemoryTransientBufferAllocationClass allocation_class)
	{
		EXIT_IF(ctx == nullptr || data == nullptr || size == 0u || usage == 0u);
		auto* entry = Acquire(ctx, size, usage, allocation_class);
		if (entry == nullptr)
		{
			return nullptr;
		}

		const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, size);
		UnindexSnapshot(entry);
		entry->snapshot_valid = false;
		entry->nonzero_end    = std::max(entry->nonzero_end, size);
		std::memcpy(entry->mapped, data, static_cast<size_t>(size));
		Commit(entry, size, true);
		return &entry->buffer;
	}

	// Every empty descriptor recorded into one command buffer reads the same
	// zero dword, so one entry per usage serves them all until the next Reset.
	// A carrier per draw would exhaust the per-usage entry limit.
	VulkanBuffer* ZeroCarrier(GraphicContext* ctx, uint32_t usage)
	{
		EXIT_IF(ctx == nullptr || usage == 0u);
		if (const auto carrier = m_zero_carriers.find(usage); carrier != m_zero_carriers.end())
		{
			return &carrier->second->buffer;
		}
		static constexpr uint32_t kZero = 0;
		auto* buffer = Upload(ctx, &kZero, sizeof(kZero), usage, GpuMemoryTransientBufferAllocationClass::Critical);
		if (buffer == nullptr)
		{
			return nullptr;
		}
		m_zero_carriers.emplace(usage, m_by_buffer.at(buffer));
		return buffer;
	}

	// Shared snapshot pool only: ends the leases of the recording's previous use and starts the next one.
	void BeginLeases(TransientSnapshotLeases* leases)
	{
		ReleaseLeases(leases);
		leases->generation = ++m_lease_generation;
	}

	void ReleaseLeases(TransientSnapshotLeases* leases)
	{
		EXIT_IF(leases == nullptr);
		for (auto* entry: leases->entries)
		{
			EXIT_IF(entry->leases == 0u);
			if (--entry->leases == 0u)
			{
				entry->used = false;
				IndexFree(entry);
			}
		}
		leases->entries.clear();
	}

	VulkanBuffer* Capture(GraphicContext* ctx, uint64_t vaddr, uint64_t size, uint64_t read_begin, uint32_t usage, bool zero_tail,
	                      TransientSnapshotLeases* leases, uint64_t* validation_ns, uint64_t* upload_ns, uint64_t* compare_ns, bool* reused)
	{
		EXIT_IF(ctx == nullptr || validation_ns == nullptr || upload_ns == nullptr || compare_ns == nullptr || reused == nullptr);
		EXIT_IF(leases == nullptr || leases->generation == 0u);
		EXIT_IF(read_begin >= size);
		*validation_ns = 0u;
		*upload_ns     = 0u;
		*compare_ns    = 0u;
		*reused        = false;
		if (!GpuMemoryCanUseTransientReadOnlyBuffer(true, size, true, true) || usage == 0u)
		{
			return nullptr;
		}

		const auto capture_start      = DebugStatsClock::now();
		const auto finish_upload_time = [&]()
		{
			const auto total_ns =
			    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - capture_start).count());
			const uint64_t excluded_ns = *validation_ns > UINT64_MAX - *compare_ns ? UINT64_MAX : *validation_ns + *compare_ns;
			*upload_ns = total_ns > excluded_ns ? total_ns - excluded_ns : 0u;
		};
		// Index fetches read guest bytes that idle writable storage already holds,
		// as do the IndexBuffer objects linked under a surface.
		const auto admission = usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT ? GpuMemorySnapshotAdmission::IndexView
		                                                                  : GpuMemorySnapshotAdmission::ReadOnlyOverlaps;
		// Without a zero tail the GPU reads only [read_begin, size), so any
		// snapshot of the same base that covers that window serves it. Draws
		// that bind one vertex stream with different record counts then share
		// a snapshot instead of each holding the whole prefix of the stream.
		const bool covering = !zero_tail;
		DestroyExpired(ctx);
		SnapshotChain* chain = nullptr;
		Entry*         previous =
		    covering ? FindCovering(vaddr, size, usage, read_begin, &chain) : FindSnapshot(vaddr, size, usage, read_begin, &chain);
		if (previous != nullptr)
		{
			bool     matches             = false;
			bool     admitted            = false;
			uint64_t reuse_validation_ns = 0u;
			// A covering snapshot is compared whole, which also arms its write tracking.
			const uint64_t compare_begin = covering ? previous->snapshot_begin : read_begin;
			const uint64_t compare_end   = covering ? previous->snapshot_size : size;
			if (TrackedSnapshotUnwritten(previous, chain))
			{
				// The bytes still match; only the range's admission is checked again.
				const auto preflight_start = DebugStatsClock::now();
				admitted                   = GpuMemoryCanSnapshotReadOnlyBuffer(vaddr + read_begin, size - read_begin, admission);
				reuse_validation_ns        = static_cast<uint64_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - preflight_start).count());
				matches = admitted;
			} else
			{
				const auto observation = BeginTracking(previous, compare_begin, chain);
				admitted = GpuMemoryCompareSnapshotReadOnlyBuffer(vaddr + compare_begin, compare_end - compare_begin,
				                                                  static_cast<const uint8_t*>(previous->mapped) + compare_begin, &matches,
				                                                  &reuse_validation_ns, compare_ns, admission);
				FinishTracking(previous, chain, observation, admitted && matches);
			}
			*validation_ns += reuse_validation_ns;
			if (!admitted)
			{
				return nullptr;
			}
			if (matches)
			{
				// Recordings still in flight bind at most `size` bytes of this key, so clearing past it is safe.
				if (zero_tail)
				{
					ClearTail(previous, size);
				}
				Lease(previous, leases);
				previous->buffer.descriptor_range = previous->snapshot_size;
				*reused                           = true;
				return &previous->buffer;
			}
		}
		// A covering capture grows to the union with the base's latest snapshot,
		// so the next draws of the stream find their windows in it. A union the
		// snapshot path refuses leaves the draw with its own window.
		uint64_t capture_begin = read_begin;
		uint64_t capture_end   = size;
		if (covering && chain != nullptr)
		{
			const uint64_t union_begin = std::min(capture_begin, chain->newest->snapshot_begin);
			const uint64_t union_end   = std::max(capture_end, chain->newest->snapshot_size);
			const auto     union_start = DebugStatsClock::now();
			if (union_end <= kGpuMemoryMaxTransientReadOnlyBytes && (union_begin != capture_begin || union_end != capture_end) &&
			    GpuMemoryCanSnapshotReadOnlyBuffer(vaddr + union_begin, union_end - union_begin, admission))
			{
				capture_begin = union_begin;
				capture_end   = union_end;
			}
			*validation_ns +=
			    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - union_start).count());
		}
		// A key whose bytes changed recycles its own stale entries first, so a
		// range the guest rewrites every frame keeps its buffers. Snapshots
		// still leased by recent recordings are recycled only when the pool
		// cannot grow.
		bool   created = false;
		Entry* entry   = TakeFree(m_free, usage, capture_end);
		if (entry == nullptr && previous != nullptr)
		{
			entry = TakeStale(chain, capture_end);
		}
		if (entry == nullptr)
		{
			entry = TakeAged(usage, capture_end);
		}
		if (entry == nullptr && CanCreate(capture_end, usage, GpuMemoryTransientBufferAllocationClass::Snapshot))
		{
			const auto preflight_start = DebugStatsClock::now();
			const bool eligible        = GpuMemoryCanSnapshotReadOnlyBuffer(vaddr + capture_begin, capture_end - capture_begin, admission);
			*validation_ns += static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - preflight_start).count());
			if (!eligible)
			{
				finish_upload_time();
				return nullptr;
			}
			entry   = Create(ctx, capture_end, usage);
			created = true;
		}
		if (entry == nullptr)
		{
			entry = TakeFreeSnapshot(usage, capture_end);
		}
		if (entry == nullptr)
		{
			finish_upload_time();
			return nullptr;
		}

		uint64_t atomic_validation_ns = 0u;
		uint64_t copy_ns              = 0u;
		// A refused capture may already have copied part of the range, so the
		// entry's previous snapshot is dropped first.
		UnindexSnapshot(entry);
		entry->snapshot_valid = false;
		entry->nonzero_end    = std::max(entry->nonzero_end, capture_end);
		if (!GpuMemoryCaptureSnapshotReadOnlyBuffer(vaddr + capture_begin, capture_end - capture_begin,
		                                            static_cast<uint8_t*>(entry->mapped) + capture_begin, &atomic_validation_ns, &copy_ns,
		                                            admission))
		{
			*validation_ns += atomic_validation_ns;
			finish_upload_time();
			if (created)
			{
				DiscardNew(ctx, entry);
			}
			return nullptr;
		}
		*validation_ns += atomic_validation_ns;
		DebugStatsRecordUpload(capture_end - capture_begin, copy_ns);
		entry->snapshot_valid    = true;
		entry->snapshot_covering = covering;
		entry->snapshot_vaddr    = vaddr;
		entry->snapshot_size     = capture_end;
		entry->snapshot_begin    = capture_begin;
		IndexSnapshot(entry);
		Commit(entry, capture_end, zero_tail);
		Lease(entry, leases);
		finish_upload_time();
		return &entry->buffer;
	}

	bool ReadSnapshot(const VulkanBuffer* storage_buffer, uint64_t expected_vaddr, uint64_t offset, uint64_t size, void* dst) const
	{
		if (storage_buffer == nullptr || dst == nullptr || size == 0u || size > kMaxSnapshotReadSize)
		{
			return false;
		}

		const auto found = m_by_buffer.find(storage_buffer);
		if (found == m_by_buffer.end())
		{
			return false;
		}
		const auto* entry = found->second;
		if (!entry->used || !entry->snapshot_valid || entry->mapped == nullptr || entry->snapshot_vaddr != expected_vaddr ||
		    offset < entry->snapshot_begin || entry->snapshot_size == 0u || entry->snapshot_size > entry->size ||
		    offset > entry->snapshot_size || size > entry->snapshot_size - offset || offset > storage_buffer->descriptor_range ||
		    size > storage_buffer->descriptor_range - offset || offset > entry->size || size > entry->size - offset)
		{
			return false;
		}

		std::memcpy(dst, static_cast<const uint8_t*>(entry->mapped) + offset, static_cast<size_t>(size));
		return true;
	}

	// A device-local scratch is never mapped: only commands recorded in this buffer read and write it.
	VulkanBuffer* Scratch(GraphicContext* ctx, uint64_t size, uint32_t usage, bool device_local, bool allow_reuse = true)
	{
		if (ctx == nullptr || size == 0u || usage == 0u)
		{
			return nullptr;
		}
		Entry*   best          = nullptr;
		uint32_t usage_entries = 0;
		for (auto* candidate: m_entries)
		{
			if (candidate->usage == usage)
			{
				usage_entries++;
			}
			if (allow_reuse && candidate->scratch && candidate->usage == usage && candidate->size >= size &&
			    (best == nullptr || candidate->size < best->size))
			{
				best = candidate;
			}
		}
		if (best != nullptr)
		{
			best->buffer.descriptor_range = size;
			return &best->buffer;
		}
		if (!GpuMemoryTransientBufferPoolCanAllocate(usage_entries, static_cast<uint32_t>(m_entries.size()), m_total_bytes, size,
		                                               GpuMemoryTransientBufferAllocationClass::Critical))
		{
			return nullptr;
		}
		auto* entry                   = new Entry;
		entry->size                   = size;
		entry->usage                  = usage;
		entry->scratch                = true;
		entry->buffer.usage           = usage;
		entry->buffer.memory.property = device_local ? static_cast<uint32_t>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		                                             : static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
		                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		VulkanCreateBuffer(ctx, size, &entry->buffer);
		if (!device_local)
		{
			VulkanMapMemory(ctx, &entry->buffer.memory, &entry->mapped);
		}
		if (!device_local && entry->mapped == nullptr)
		{
			VulkanDeleteBuffer(ctx, &entry->buffer);
			delete entry;
			return nullptr;
		}
		entry->pool_index = m_entries.size();
		m_entries.push_back(entry);
		m_total_bytes += size;
		return &entry->buffer;
	}

	void Reset()
	{
		m_zero_carriers.clear();
		for (auto* entry: m_entries)
		{
			entry->used = false;
			IndexFree(entry);
		}
	}

	void Destroy(GraphicContext* ctx)
	{
		EXIT_IF(ctx == nullptr);
		for (auto* entry: m_entries)
		{
			EXIT_IF(entry == nullptr || (entry->mapped == nullptr && !entry->scratch));
			// A live lease means a command buffer that binds the entry was not freed.
			EXIT_IF(entry->leases != 0u);
			Untrack(entry);
			if (entry->mapped != nullptr)
			{
				VulkanUnmapMemory(ctx, &entry->buffer.memory);
				entry->mapped = nullptr;
			}
			VulkanDeleteBuffer(ctx, &entry->buffer);
			delete entry;
		}
		m_entries.clear();
		m_snapshots.clear();
		m_free.clear();
		m_free_snapshots.clear();
		m_released = {};
		m_usage_entries.clear();
		m_zero_carriers.clear();
		m_by_buffer.clear();
		m_total_bytes = 0;
	}

private:
	struct SnapshotKey
	{
		uint64_t vaddr = 0;
		uint64_t size  = 0;
		uint32_t usage = 0;

		bool operator==(const SnapshotKey& other) const
		{
			return vaddr == other.vaddr && size == other.size && usage == other.usage;
		}
	};
	struct SnapshotKeyHash
	{
		size_t operator()(const SnapshotKey& key) const
		{
			uint64_t hash = key.vaddr * 0x9e3779b97f4a7c15ull;
			hash ^= key.size + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
			hash ^= key.usage + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
			return static_cast<size_t>(hash);
		}
	};

	// Entries of one snapshot key, linked through the entries themselves so
	// that a key rewritten by every draw unindexes an entry in constant time.
	struct SnapshotChain
	{
		Entry* oldest = nullptr;
		Entry* newest = nullptr;
		// After the guest wrote a tracked snapshot of this key, or tracking was
		// refused, probes compare bytes for a while. The wait doubles with every
		// failure, so a key rewritten each frame rarely pays for page protection
		// and one that settles after loading is tracked again.
		uint32_t track_failures     = 0;
		uint32_t probes_until_track = 0;
	};

	// Latest committed entry first: the one most likely to still match guest memory.
	Entry* FindSnapshot(uint64_t vaddr, uint64_t size, uint32_t usage, uint64_t read_begin, SnapshotChain** chain)
	{
		const auto it = m_snapshots.find(SnapshotKey {vaddr, size, usage});
		if (it == m_snapshots.end())
		{
			return nullptr;
		}
		*chain = &it->second;
		for (auto* entry = it->second.newest; entry != nullptr; entry = entry->snapshot_older)
		{
			if (entry->snapshot_begin <= read_begin)
			{
				return entry;
			}
		}
		return nullptr;
	}

	// Latest committed entry first that holds the whole read window.
	Entry* FindCovering(uint64_t vaddr, uint64_t size, uint32_t usage, uint64_t read_begin, SnapshotChain** chain)
	{
		const auto it = m_snapshots.find(SnapshotKey {vaddr, kCoveringKeySize, usage});
		if (it == m_snapshots.end())
		{
			return nullptr;
		}
		*chain = &it->second;
		for (auto* entry = it->second.newest; entry != nullptr; entry = entry->snapshot_older)
		{
			if (entry->snapshot_begin <= read_begin && entry->snapshot_size >= size)
			{
				return entry;
			}
		}
		return nullptr;
	}

	static SnapshotKey KeyOf(const Entry* entry)
	{
		return {entry->snapshot_vaddr, entry->snapshot_covering ? kCoveringKeySize : entry->snapshot_size, entry->usage};
	}

	void IndexSnapshot(Entry* entry)
	{
		auto& chain           = m_snapshots[KeyOf(entry)];
		entry->snapshot_older = chain.newest;
		entry->snapshot_newer = nullptr;
		if (chain.newest != nullptr)
		{
			chain.newest->snapshot_newer = entry;
		} else
		{
			chain.oldest = entry;
		}
		chain.newest = entry;
	}

	void UnindexSnapshot(Entry* entry)
	{
		if (!entry->snapshot_valid)
		{
			return;
		}
		Untrack(entry);
		const auto it = m_snapshots.find(KeyOf(entry));
		EXIT_IF(it == m_snapshots.end());
		auto& chain = it->second;
		if (entry->snapshot_older != nullptr)
		{
			entry->snapshot_older->snapshot_newer = entry->snapshot_newer;
		} else
		{
			chain.oldest = entry->snapshot_newer;
		}
		if (entry->snapshot_newer != nullptr)
		{
			entry->snapshot_newer->snapshot_older = entry->snapshot_older;
		} else
		{
			chain.newest = entry->snapshot_older;
		}
		entry->snapshot_older = nullptr;
		entry->snapshot_newer = nullptr;
		if (chain.newest == nullptr)
		{
			m_snapshots.erase(it);
		}
	}

	static uint64_t TrackedAddress(const Entry* entry) { return entry->snapshot_vaddr + entry->snapshot_begin; }
	static uint64_t TrackedSize(const Entry* entry) { return entry->snapshot_size - entry->snapshot_begin; }

	// No guest write reached a tracked snapshot since it matched. A written one
	// is untracked; its key is compared until tracking is tried again.
	static bool TrackedSnapshotUnwritten(Entry* entry, SnapshotChain* chain)
	{
		if (!entry->tracked)
		{
			return false;
		}
		if (!GpuDirtyPageTracker::Instance().ChangedSince(TrackedAddress(entry), TrackedSize(entry), entry->tracked_generation))
		{
			entry->tracked_reuses++;
			return true;
		}
		// Protection that served enough probes paid for itself: track the key again at once.
		static constexpr uint32_t kReusesWorthTracking = 16u;
		if (entry->tracked_reuses >= kReusesWorthTracking)
		{
			chain->track_failures = 0u;
		} else
		{
			RefuseTracking(chain);
		}
		Untrack(entry);
		return false;
	}

	static void RefuseTracking(SnapshotChain* chain)
	{
		static constexpr uint32_t kMaxTrackFailureShift = 10u;

		chain->track_failures     = std::min(chain->track_failures + 1u, kMaxTrackFailureShift);
		chain->probes_until_track = 1u << chain->track_failures;
	}

	// Arms write tracking before a large snapshot is compared, so a write after
	// the comparison read the bytes advances the observed generation.
	static GpuDirtyReadObservation BeginTracking(const Entry* entry, uint64_t read_begin, SnapshotChain* chain)
	{
		if (read_begin != entry->snapshot_begin || TrackedSize(entry) < kMinTrackedSnapshotSize)
		{
			return {};
		}
		if (chain->probes_until_track != 0u)
		{
			chain->probes_until_track--;
			return {};
		}
		auto& tracker = GpuDirtyPageTracker::Instance();
		if (!tracker.RegisterRange(TrackedAddress(entry), TrackedSize(entry)))
		{
			RefuseTracking(chain);
			return {};
		}
		const auto observation = tracker.BeginRead(TrackedAddress(entry), TrackedSize(entry));
		if (!observation.tracked)
		{
			(void)tracker.UnregisterRange(TrackedAddress(entry), TrackedSize(entry));
			RefuseTracking(chain);
		}
		return observation;
	}

	static void FinishTracking(Entry* entry, SnapshotChain* chain, const GpuDirtyReadObservation& observation, bool matched)
	{
		if (!observation.tracked)
		{
			return;
		}
		auto& tracker = GpuDirtyPageTracker::Instance();
		if (matched && tracker.ReadObservationIsStable(TrackedAddress(entry), TrackedSize(entry), observation))
		{
			entry->tracked            = true;
			entry->tracked_generation = observation.generation;
			entry->tracked_reuses     = 0u;
			return;
		}
		(void)tracker.UnregisterRange(TrackedAddress(entry), TrackedSize(entry));
		RefuseTracking(chain);
	}

	static void Untrack(Entry* entry)
	{
		if (!entry->tracked)
		{
			return;
		}
		(void)GpuDirtyPageTracker::Instance().UnregisterRange(TrackedAddress(entry), TrackedSize(entry));
		entry->tracked = false;
	}

	// Free entries without a snapshot go first, then snapshots no recent
	// recording leased. One a later frame may probe again is recycled only
	// when the pool cannot grow, and then the one released longest ago. Among
	// equal sizes of the snapshot-free index the earliest-created entry wins,
	// so a frame that repeats the previous one's uploads binds the same
	// buffers (and hits the descriptor sets keyed by them).
	Entry* Acquire(GraphicContext* ctx, uint64_t size, uint32_t usage, GpuMemoryTransientBufferAllocationClass allocation_class)
	{
		if (auto* entry = TakeFree(m_free, usage, size); entry != nullptr)
		{
			return entry;
		}
		if (auto* entry = TakeAged(usage, size); entry != nullptr)
		{
			return entry;
		}
		if (CanCreate(size, usage, allocation_class))
		{
			return Create(ctx, size, usage);
		}
		if (auto* entry = TakeFreeSnapshot(usage, size); entry != nullptr)
		{
			return entry;
		}
		if (allocation_class == GpuMemoryTransientBufferAllocationClass::Critical)
		{
			ReportCriticalTransientPoolCapacityReject(size, usage, m_usage_entries[usage], static_cast<uint32_t>(m_entries.size()),
			                                          m_total_bytes, MeasureOccupancy(usage));
		}
		return nullptr;
	}

	// Prefer exact size; fall back to the smallest free entry that fits.
	// Descriptor writes use an explicit range (not the full VkBuffer size),
	// so a larger free slab is valid and distinct sizes do not fragment the
	// entry ceiling.
	static Entry* TakeFree(std::unordered_map<uint32_t, FreeIndex>& index, uint32_t usage, uint64_t size)
	{
		const auto free = index.find(usage);
		if (free == index.end())
		{
			return nullptr;
		}
		const auto fit = free->second.lower_bound({size, 0u});
		return fit != free->second.end() ? fit->second : nullptr;
	}

	// The smallest fitting size, and within it the snapshot released longest ago.
	// Sizes left empty are dropped on the way.
	Entry* TakeFreeSnapshot(uint32_t usage, uint64_t size)
	{
		const auto free = m_free_snapshots.find(usage);
		if (free == m_free_snapshots.end())
		{
			return nullptr;
		}
		auto& sizes = free->second;
		for (auto fit = sizes.lower_bound(size); fit != sizes.end();)
		{
			if (fit->second.oldest != nullptr)
			{
				return fit->second.oldest;
			}
			fit = sizes.erase(fit);
		}
		return nullptr;
	}

	// Lists link through one pair of an entry's neighbour fields: the free
	// pair for a size's list, the released pair for the pool-wide order.
	static void ListAppend(TransientFreeList* list, Entry* entry, Entry* Entry::* older, Entry* Entry::* newer)
	{
		entry->*older = list->newest;
		entry->*newer = nullptr;
		if (list->newest != nullptr)
		{
			list->newest->*newer = entry;
		} else
		{
			list->oldest = entry;
		}
		list->newest = entry;
	}

	static void ListUnlink(TransientFreeList* list, Entry* entry, Entry* Entry::* older, Entry* Entry::* newer)
	{
		Entry* const previous = entry->*older;
		Entry* const next     = entry->*newer;
		if (previous != nullptr)
		{
			previous->*newer = next;
		} else
		{
			list->oldest = next;
		}
		if (next != nullptr)
		{
			next->*older = previous;
		} else
		{
			list->newest = previous;
		}
		entry->*older = nullptr;
		entry->*newer = nullptr;
	}

	static Entry* TakeStale(const SnapshotChain* chain, uint64_t size)
	{
		for (auto* entry = chain->oldest; entry != nullptr; entry = entry->snapshot_newer)
		{
			if (!entry->used && entry->size >= size)
			{
				return entry;
			}
		}
		return nullptr;
	}

	// The fitting free snapshot released longest ago, unless a recent recording leased it.
	Entry* TakeAged(uint32_t usage, uint64_t size)
	{
		auto* entry = TakeFreeSnapshot(usage, size);
		return entry != nullptr && entry->lease_generation + kRetainedRecordings <= m_lease_generation ? entry : nullptr;
	}

	bool CanCreate(uint64_t size, uint32_t usage, GpuMemoryTransientBufferAllocationClass allocation_class)
	{
		return GpuMemoryTransientBufferPoolCanAllocate(m_usage_entries[usage], static_cast<uint32_t>(m_entries.size()), m_total_bytes, size,
		                                               allocation_class);
	}

	Entry* Create(GraphicContext* ctx, uint64_t size, uint32_t usage)
	{
		auto* entry                   = new Entry;
		entry->size                   = size;
		entry->usage                  = usage;
		entry->buffer.usage           = usage;
		entry->buffer.memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		entry->nonzero_end            = size;
		entry->creation               = ++m_creations;
		VulkanCreateBuffer(ctx, size, &entry->buffer);
		VulkanMapMemory(ctx, &entry->buffer.memory, &entry->mapped);
		EXIT_IF(entry->mapped == nullptr);
		entry->pool_index = m_entries.size();
		m_entries.push_back(entry);
		m_by_buffer.emplace(&entry->buffer, entry);
		m_usage_entries[usage]++;
		IndexFree(entry);
		m_total_bytes += size;
		return entry;
	}

	// Refusal diagnostics only: groups used entries of one usage by size and committed bytes.
	TransientPoolUsageOccupancy MeasureOccupancy(uint32_t usage) const
	{
		TransientPoolUsageOccupancy occupancy {};
		std::unordered_map<uint64_t, uint32_t> groups;
		for (const auto* entry: m_entries)
		{
			if (entry->scratch || entry->usage != usage)
			{
				continue;
			}
			if (!entry->used)
			{
				occupancy.free++;
				continue;
			}
			occupancy.used++;
			occupancy.used_snapshots += entry->snapshot_valid ? 1u : 0u;
			const uint64_t bytes = std::min<uint64_t>(entry->buffer.descriptor_range, entry->size);
			if (bytes == 4u)
			{
				occupancy.used_four_byte++;
				uint32_t word = 0;
				std::memcpy(&word, entry->mapped, sizeof(word));
				occupancy.used_four_byte_zero += word == 0u ? 1u : 0u;
			}
			uint64_t hash = 1469598103934665603ull ^ bytes;
			const auto* data = static_cast<const uint8_t*>(entry->mapped);
			for (uint64_t i = 0; i < std::min<uint64_t>(bytes, 4096u); ++i)
			{
				hash = (hash ^ data[i]) * 1099511628211ull;
			}
			const uint32_t members = ++groups[hash];
			occupancy.used_duplicates += members > 1u ? 1u : 0u;
			occupancy.largest_duplicate_group = std::max(occupancy.largest_duplicate_group, members);
		}
		return occupancy;
	}

	void DiscardNew(GraphicContext* ctx, Entry* entry)
	{
		EXIT_IF(entry == nullptr || m_entries.empty() || m_entries.back() != entry);
		DestroyEntry(ctx, entry);
	}

	// Snapshots no recording leased for this long are destroyed, a few per
	// capture, so bytes held for a finished scene return to the budget.
	void DestroyExpired(GraphicContext* ctx)
	{
		static constexpr uint32_t kMaxDestroyedPerCapture = 4u;
		for (uint32_t i = 0; i < kMaxDestroyedPerCapture && m_released.oldest != nullptr; ++i)
		{
			auto* entry = m_released.oldest;
			if (entry->lease_generation + kExpiredRecordings > m_lease_generation)
			{
				return;
			}
			UnindexSnapshot(entry);
			entry->snapshot_valid = false;
			DestroyEntry(ctx, entry);
		}
	}

	// Destroying an entry leaves no empty size list behind, so the free
	// snapshot index stays bounded by the sizes of live entries.
	void DropEmptyFreeSize(uint32_t usage, uint64_t size)
	{
		const auto free = m_free_snapshots.find(usage);
		if (free == m_free_snapshots.end())
		{
			return;
		}
		if (const auto list = free->second.find(size); list != free->second.end() && list->second.oldest == nullptr)
		{
			free->second.erase(list);
		}
	}

	void DestroyEntry(GraphicContext* ctx, Entry* entry)
	{
		EXIT_IF(ctx == nullptr || entry->used || entry->snapshot_valid || entry->scratch || m_total_bytes < entry->size);
		auto* moved                  = m_entries.back();
		m_entries[entry->pool_index] = moved;
		moved->pool_index            = entry->pool_index;
		m_entries.pop_back();
		UnindexFree(entry);
		DropEmptyFreeSize(entry->usage, entry->size);
		m_by_buffer.erase(&entry->buffer);
		m_usage_entries[entry->usage]--;
		m_total_bytes -= entry->size;
		VulkanUnmapMemory(ctx, &entry->buffer.memory);
		entry->mapped = nullptr;
		VulkanDeleteBuffer(ctx, &entry->buffer);
		delete entry;
	}

	// Bytes past the committed size read as zero; only the part of that tail
	// an earlier write reached needs clearing.
	// An unread tail keeps its stale bytes; nonzero_end still covers them.
	void Commit(Entry* entry, uint64_t size, bool zero_tail)
	{
		EXIT_IF(entry == nullptr || entry->mapped == nullptr || size == 0u || size > entry->size || entry->nonzero_end > entry->size);
		if (zero_tail)
		{
			ClearTail(entry, size);
		}
		entry->buffer.descriptor_range = size;
		entry->used = true;
		UnindexFree(entry);
	}

	void Lease(Entry* entry, TransientSnapshotLeases* leases)
	{
		entry->used = true;
		UnindexFree(entry);
		if (entry->lease_generation == leases->generation)
		{
			return;
		}
		entry->lease_generation = leases->generation;
		entry->leases++;
		leases->entries.push_back(entry);
	}

	void ClearTail(Entry* entry, uint64_t size)
	{
		const uint64_t tail_bytes = GpuMemoryTransientBufferTailBytes(entry->nonzero_end, size);
		if (tail_bytes != 0u)
		{
			std::memset(static_cast<uint8_t*>(entry->mapped) + size, 0, static_cast<size_t>(tail_bytes));
		}
		entry->nonzero_end = size;
	}

	void IndexFree(Entry* entry)
	{
		if (entry->scratch || entry->free_indexed)
		{
			return;
		}
		// Snapshots are ordered by release, so the least recently used one is recycled first.
		entry->free_snapshot = entry->snapshot_valid;
		if (entry->free_snapshot)
		{
			entry->free_list = &m_free_snapshots[entry->usage][entry->size];
			ListAppend(entry->free_list, entry, &Entry::free_older, &Entry::free_newer);
			ListAppend(&m_released, entry, &Entry::released_older, &Entry::released_newer);
		} else
		{
			entry->free_position = m_free[entry->usage].emplace(std::make_pair(entry->size, entry->creation), entry).first;
		}
		entry->free_indexed = true;
	}

	void UnindexFree(Entry* entry)
	{
		if (!entry->free_indexed)
		{
			return;
		}
		if (entry->free_snapshot)
		{
			ListUnlink(entry->free_list, entry, &Entry::free_older, &Entry::free_newer);
			ListUnlink(&m_released, entry, &Entry::released_older, &Entry::released_newer);
			entry->free_list = nullptr;
		} else
		{
			m_free[entry->usage].erase(entry->free_position);
		}
		entry->free_indexed = false;
	}

	std::vector<Entry*> m_entries;
	// Entries holding a valid snapshot, by the guest range and usage they hold.
	std::unordered_map<SnapshotKey, SnapshotChain, SnapshotKeyHash> m_snapshots;
	// Unused entries without a snapshot, by usage, size and creation order.
	std::unordered_map<uint32_t, FreeIndex> m_free;
	// Unused entries holding a snapshot, by usage, size and release order.
	std::unordered_map<uint32_t, FreeSnapshotIndex> m_free_snapshots;
	// The same entries in release order across usages.
	TransientFreeList                                                             m_released;
	std::unordered_map<uint32_t, uint32_t>                                        m_usage_entries;
	// The zero carrier each usage holds until the next Reset.
	std::unordered_map<uint32_t, Entry*>              m_zero_carriers;
	std::unordered_map<const VulkanBuffer*, Entry*>   m_by_buffer;
	uint64_t            m_total_bytes = 0;
	uint64_t                                          m_lease_generation = 0;
	uint64_t                                          m_creations        = 0;
};

// A command pool holds one command buffer per submission slot (CommandPool::Create).
static_assert(kGpuMemoryTransientSnapshotPoolRecordings == CommandProcessorSubmissionSlots::SlotCount);

// Snapshots shared by the command buffers of one command pool (see TransientSnapshotLeases).
struct TransientSnapshotPool
{
	std::mutex          mutex;
	TransientBufferPool pool;
};

void TransientSnapshotPoolDestroy(GraphicContext* ctx, VulkanCommandPool* pool)
{
	EXIT_IF(pool == nullptr);
	if (pool->snapshots == nullptr)
	{
		return;
	}
	pool->snapshots->pool.Destroy(ctx);
	delete pool->snapshots;
	pool->snapshots = nullptr;
}

// CommandBuffer methods (uses TransientBufferPool from Internal.h)

bool CommandBuffer::SupportsGraphics() const
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	const auto* ctx = g_render_ctx->GetGraphicCtx();
	return ctx->queues[m_queue].family == ctx->queues[GraphicContext::QUEUE_GFX].family;
}

bool CommandBuffer::IsInvalid() const
{
	EXIT_IF(g_render_ctx == nullptr);

	if (m_pool != nullptr)
	{
		Core::LockGuard lock(m_pool->mutex);

		return (m_index == static_cast<uint32_t>(-1) || m_index >= m_pool->buffers_count);
	}

	return true;
}

void CommandBuffer::Allocate()
{
	EXIT_IF(!IsInvalid());

	m_pool = g_command_pool.GetPool(m_queue);

	Core::LockGuard lock(m_pool->mutex);

	if (m_pool->snapshots == nullptr)
	{
		m_pool->snapshots = new TransientSnapshotPool;
	}

	for (uint32_t i = 0; i < m_pool->buffers_count; i++)
	{
		if (!m_pool->busy[i])
		{
			m_pool->busy[i] = true;
			MarkCommandBufferResetting();
			const auto reset_result = vkResetCommandBuffer(m_pool->buffers[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
			if (reset_result != VK_SUCCESS)
			{
				VulkanSubmitFaultReport("command_buffer_allocate_reset", reset_result);
				EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(reset_result), m_queue,
				     i);
			}
			ReleasePipelineLeases();
			m_index = i;
			break;
		}
	}

	if (IsInvalid()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: IsInvalid() condition ignored (continuing)\n"); }
}

void CommandBuffer::Free()
{
	EXIT_IF(IsInvalid());

	Core::LockGuard lock(m_pool->mutex);

	WaitForFence();
	if (m_snapshot_leases != nullptr)
	{
		auto*                       shared = m_pool->snapshots;
		std::lock_guard<std::mutex> lock(shared->mutex);
		shared->pool.ReleaseLeases(m_snapshot_leases);
		delete m_snapshot_leases;
		m_snapshot_leases = nullptr;
	}
	if (m_transient_buffers != nullptr)
	{
		m_transient_buffers->Destroy(g_render_ctx->GetGraphicCtx());
		delete m_transient_buffers;
		m_transient_buffers = nullptr;
	}
	if (m_transient_scratch_buffers != nullptr)
	{
		m_transient_scratch_buffers->Destroy(g_render_ctx->GetGraphicCtx());
		delete m_transient_scratch_buffers;
		m_transient_scratch_buffers = nullptr;
	}
	if (m_transient_copy_buffers != nullptr)
	{
		m_transient_copy_buffers->Destroy(g_render_ctx->GetGraphicCtx());
		delete m_transient_copy_buffers;
		m_transient_copy_buffers = nullptr;
	}
	if (m_transient_device_buffers != nullptr)
	{
		m_transient_device_buffers->Destroy(g_render_ctx->GetGraphicCtx());
		delete m_transient_device_buffers;
		m_transient_device_buffers = nullptr;
	}
	if (m_transport_descriptor_pool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(g_render_ctx->GetGraphicCtx()->device, m_transport_descriptor_pool, nullptr);
		m_transport_descriptor_pool = VK_NULL_HANDLE;
	}
	DestroyRecordingUploads();

	m_pool->busy[m_index] = false;
	MarkCommandBufferResetting();
	const auto reset_result = vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	if (reset_result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("command_buffer_free_reset", reset_result);
		EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(reset_result), m_queue,
		     m_index);
	}
	ReleasePipelineLeases();
	m_index = static_cast<uint32_t>(-1);

	if (!IsInvalid()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !IsInvalid() condition ignored (continuing)\n"); }
}

bool CommandBuffer::AttachPipelineLease(VulkanPipeline* pipeline, bool* added)
{
	if (pipeline == nullptr || added == nullptr || m_pool == nullptr || m_index == static_cast<uint32_t>(-1) || m_execute)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(m_pipeline_lease_mutex);
	if (!m_recording || m_resetting)
	{
		return false;
	}
	if (std::find(m_pipeline_leases.begin(), m_pipeline_leases.end(), pipeline) != m_pipeline_leases.end())
	{
		*added = false;
		return true;
	}
	const size_t max_pipeline_leases = PipelineCache::MaxPipelineCount();
	if (m_pipeline_leases.size() >= max_pipeline_leases)
	{
		return false;
	}
	if (m_pipeline_leases.capacity() < max_pipeline_leases)
	{
		m_pipeline_leases.reserve(max_pipeline_leases);
	}

	m_pipeline_leases.push_back(pipeline);
	*added = true;
	return true;
}

void CommandBuffer::MarkCommandBufferResetting()
{
	std::lock_guard<std::mutex> lock(m_pipeline_lease_mutex);
	m_recording = false;
	m_resetting = true;
}

void CommandBuffer::ReleasePipelineLeases()
{
	std::vector<VulkanPipeline*> leases;
	{
		std::lock_guard<std::mutex> lock(m_pipeline_lease_mutex);
		leases.swap(m_pipeline_leases);
	}

	if (!leases.empty())
	{
		EXIT_IF(g_render_ctx == nullptr);
		auto* pipeline_cache = g_render_ctx->GetPipelineCache();
		EXIT_IF(pipeline_cache == nullptr);
		for (auto* pipeline: leases)
		{
			pipeline_cache->ReleasePipelineLease(pipeline);
		}
		leases.clear();
	}

	// Keep the bounded pointer-vector capacity with the command buffer for its
	// next recording instead of allocating again on every reset.
	{
		std::lock_guard<std::mutex> lock(m_pipeline_lease_mutex);
		if (m_pipeline_leases.empty())
		{
			m_pipeline_leases.swap(leases);
		}
		m_recording = false;
		m_resetting = false;
	}
}

void CommandBuffer::Begin()
{
	EXIT_IF(IsInvalid());
	m_open_render_pass.reset();
	{
		std::lock_guard<std::mutex> lock(m_pipeline_lease_mutex);
		EXIT_IF(m_recording || m_resetting);
	}
	if (m_snapshot_leases != nullptr)
	{
		auto*                       shared = m_pool->snapshots;
		std::lock_guard<std::mutex> lock(shared->mutex);
		shared->pool.BeginLeases(m_snapshot_leases);
	}
	if (m_transient_buffers != nullptr)
	{
		m_transient_buffers->Reset();
	}
	if (m_transient_scratch_buffers != nullptr)
	{
		m_transient_scratch_buffers->Reset();
	}
	if (m_transient_copy_buffers != nullptr)
	{
		m_transient_copy_buffers->Reset();
	}
	if (m_transient_device_buffers != nullptr)
	{
		m_transient_device_buffers->Reset();
	}
	if (m_transport_descriptor_pool != VK_NULL_HANDLE)
	{
		vkResetDescriptorPool(g_render_ctx->GetGraphicCtx()->device, m_transport_descriptor_pool, 0);
	}
	DestroyRecordingUploads();

	auto* buffer = m_pool->buffers[m_index];

	VkCommandBufferBeginInfo begin_info {};
	begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext            = nullptr;
	begin_info.flags            = 0;
	begin_info.pInheritanceInfo = nullptr;

	const auto result = vkBeginCommandBuffer(buffer, &begin_info);
	if (result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("command_buffer_begin", result);
		EXIT("vkBeginCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(result), m_queue, m_index);
	}
	{
		std::lock_guard<std::mutex> lock(m_pipeline_lease_mutex);
		m_recording = true;
	}
}

VulkanBuffer* CommandBuffer::UploadTransientBuffer(const void* data, uint64_t size, uint32_t usage)
{
	EXIT_IF(IsInvalid());
	if (m_transient_buffers == nullptr)
	{
		m_transient_buffers = new TransientBufferPool;
	}
	return m_transient_buffers->Upload(g_render_ctx->GetGraphicCtx(), data, size, usage,
	                                   GpuMemoryTransientBufferAllocationClass::Critical);
}

VulkanBuffer* CommandBuffer::UploadTransientZeroCarrier(uint32_t usage)
{
	EXIT_IF(IsInvalid());
	if (m_transient_buffers == nullptr)
	{
		m_transient_buffers = new TransientBufferPool;
	}
	return m_transient_buffers->ZeroCarrier(g_render_ctx->GetGraphicCtx(), usage);
}

VulkanBuffer* CommandBuffer::CaptureTransientSnapshotBuffer(uint64_t vaddr, uint64_t size, uint64_t read_begin, uint32_t usage,
	                                                         bool zero_tail, uint64_t* validation_ns, uint64_t* upload_ns, uint64_t* compare_ns,
	                                                         bool* reused)
{
	EXIT_IF(IsInvalid());
	auto*                       shared = m_pool->snapshots;
	std::lock_guard<std::mutex> lock(shared->mutex);
	if (m_snapshot_leases == nullptr)
	{
		m_snapshot_leases = new TransientSnapshotLeases;
		shared->pool.BeginLeases(m_snapshot_leases);
	}
	return shared->pool.Capture(g_render_ctx->GetGraphicCtx(), vaddr, size, read_begin, usage, zero_tail, m_snapshot_leases, validation_ns,
	                            upload_ns, compare_ns, reused);
}

bool CommandBuffer::ReadTransientSnapshotBuffer(const VulkanBuffer* storage_buffer, uint64_t expected_vaddr, uint64_t offset,
	                                               uint64_t size, void* dst) const
{
	if (m_snapshot_leases == nullptr)
	{
		return false;
	}
	auto*                       shared = m_pool->snapshots;
	std::lock_guard<std::mutex> lock(shared->mutex);
	return shared->pool.ReadSnapshot(storage_buffer, expected_vaddr, offset, size, dst);
}

VulkanBuffer* CommandBuffer::AllocateTransientScratchBuffer(uint64_t size, uint32_t usage)
{
	EXIT_IF(IsInvalid());
	if (m_transient_scratch_buffers == nullptr)
	{
		m_transient_scratch_buffers = new TransientBufferPool;
	}
	return m_transient_scratch_buffers->Scratch(g_render_ctx->GetGraphicCtx(), size, usage, false);
}

VulkanBuffer* CommandBuffer::AllocateTransientCopyBuffer(uint64_t size)
{
	EXIT_IF(IsInvalid());
	if (m_transient_copy_buffers == nullptr)
	{
		m_transient_copy_buffers = new TransientBufferPool;
	}
	return m_transient_copy_buffers->Scratch(g_render_ctx->GetGraphicCtx(), size,
	                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
}

VulkanBuffer* CommandBuffer::AllocateTransientDeviceBuffer(uint64_t size, uint32_t usage, bool allow_reuse)
{
	EXIT_IF(IsInvalid());
	if (m_transient_device_buffers == nullptr)
	{
		m_transient_device_buffers = new TransientBufferPool;
	}
	return m_transient_device_buffers->Scratch(g_render_ctx->GetGraphicCtx(), size, usage, true, allow_reuse);
}

VulkanBuffer* CommandBuffer::TransportStorage(uint64_t size, uint32_t usage)
{
	EXIT_IF(IsInvalid());
	EXIT_IF(size == 0u);
	Core::LockGuard lock(m_pool->mutex);
	if (m_pool->transport_storage == nullptr)
	{
		auto* storage            = new VulkanBuffer;
		storage->usage           = usage;
		storage->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VulkanCreateBuffer(g_render_ctx->GetGraphicCtx(), size, storage);
		storage->descriptor_range = size;
		m_pool->transport_storage = storage;
	}
	auto* storage = m_pool->transport_storage;
	EXIT_IF(storage->usage != usage);
	return size <= storage->descriptor_range ? storage : nullptr;
}

VkDescriptorSet CommandBuffer::AllocateTransportDescriptorSet(VkDescriptorSetLayout layout)
{
	EXIT_IF(IsInvalid());
	EXIT_IF(layout == VK_NULL_HANDLE);
	if (m_transport_descriptor_pool == VK_NULL_HANDLE)
	{
		VkDescriptorPoolSize size {};
		size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		size.descriptorCount = kTransportSetsMax * kTransportBindingsMax;
		VkDescriptorPoolCreateInfo info {};
		info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		info.maxSets       = kTransportSetsMax;
		info.poolSizeCount = 1;
		info.pPoolSizes    = &size;
		const auto result  = vkCreateDescriptorPool(g_render_ctx->GetGraphicCtx()->device, &info, nullptr, &m_transport_descriptor_pool);
		if (result != VK_SUCCESS)
		{
			m_transport_descriptor_pool = VK_NULL_HANDLE;
			return VK_NULL_HANDLE;
		}
	}
	VkDescriptorSetAllocateInfo info {};
	info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorPool     = m_transport_descriptor_pool;
	info.descriptorSetCount = 1;
	info.pSetLayouts        = &layout;
	VkDescriptorSet set     = VK_NULL_HANDLE;
	return vkAllocateDescriptorSets(g_render_ctx->GetGraphicCtx()->device, &info, &set) == VK_SUCCESS ? set : VK_NULL_HANDLE;
}

VulkanBuffer* CommandBuffer::UploadRecordingBuffer(const void* data, uint64_t size, uint32_t usage)
{
	EXIT_IF(IsInvalid() || data == nullptr || size == 0u || usage == 0u);
	auto*                      ctx = g_render_ctx->GetGraphicCtx();
	const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, size);
	auto*                      upload = new VulkanBuffer;
	upload->usage                     = usage;
	upload->memory.property           = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VulkanCreateBuffer(ctx, size, upload);
	void* mapped = nullptr;
	VulkanMapMemory(ctx, &upload->memory, &mapped);
	EXIT_IF(mapped == nullptr);
	std::memcpy(mapped, data, static_cast<size_t>(size));
	VulkanUnmapMemory(ctx, &upload->memory);
	m_recording_uploads.push_back(upload);
	return upload;
}

// The previous recording's fence has signalled (Begin) or been waited for (Free).
void CommandBuffer::DestroyRecordingUploads()
{
	for (auto* upload: m_recording_uploads)
	{
		VulkanDeleteBuffer(g_render_ctx->GetGraphicCtx(), upload);
		delete upload;
	}
	m_recording_uploads.clear();
}

void CommandBuffer::DeferDepthPlaneStoreBack(DepthStencilVulkanImage* depth)
{
	EXIT_IF(depth == nullptr || depth->storage_plane == nullptr);
	if (std::find(m_depth_plane_store_backs.begin(), m_depth_plane_store_backs.end(), depth) == m_depth_plane_store_backs.end())
	{
		m_depth_plane_store_backs.push_back(depth);
	}
}

void CommandBuffer::StoreBackDepthPlanes()
{
	for (auto* depth: m_depth_plane_store_backs)
	{
		if (!UtilCopyTexelBits(this, depth->storage_plane, depth, static_cast<uint64_t>(depth->layout)))
		{
			EXIT("depth storage plane store-back failed: extent=%ux%u format=%u\n", depth->extent.width, depth->extent.height,
			     static_cast<uint32_t>(depth->format));
		}
		depth->content_stamp = VulkanImageNextStamp();
	}
	m_depth_plane_store_backs.clear();
}

void CommandBuffer::End()
{
	EXIT_IF(IsInvalid());

	auto* buffer = RecordOutsideRenderPass();

	const auto result = vkEndCommandBuffer(buffer);
	if (result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("command_buffer_end", result);
		EXIT("vkEndCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(result), m_queue, m_index);
	}
	{
		std::lock_guard<std::mutex> lock(m_pipeline_lease_mutex);
		m_recording = false;
	}
	DebugStatsRecordCommandBuffer();
}

VulkanSubmitAttempt CommandBuffer::MakeSubmitAttempt(VulkanSubmitKind kind, bool signals_semaphore) const
{
	VulkanSubmitAttempt attempt;
	attempt.kind                     = kind;
	attempt.has_host_submission      = m_has_submission;
	attempt.host_submission_sequence = m_has_submission ? m_submission.sequence : 0u;
	attempt.has_guest_context        = m_has_guest_context;
	attempt.guest_submit             = m_guest_submit;
	attempt.queue                    = static_cast<uint32_t>(m_queue);
	attempt.command_buffer_slot      = m_index;
	attempt.presented_frame          = WindowGetPresentedFrameNum();
	attempt.pm4_op                   = m_pm4_op;
	attempt.pm4_dw                   = m_pm4_dw;
	attempt.signals_semaphore        = signals_semaphore;
	return attempt;
}

void CommandBuffer::ReportSubmitFault(const char* stage, VkResult result) const
{
	if (result != VK_ERROR_DEVICE_LOST || !VulkanSubmitFaultTraceEnabled())
	{
		return;
	}
	const auto context = MakeSubmitAttempt(VulkanSubmitKind::CommandBuffer, false);
	VulkanSubmitFaultReport(stage, result, nullptr, &context);
}

void CommandBuffer::MarkRecentDrawsSubmitCalled() const
{
	if (auto* trail = VulkanRecentDrawTraceTrail(); trail != nullptr && m_has_submission)
	{
		trail->MarkSubmitCalled(static_cast<uint32_t>(m_queue), m_submission.sequence);
	}
}

void CommandBuffer::MarkRecentDrawsSubmitReturned(VkResult result) const
{
	if (auto* trail = VulkanRecentDrawTraceTrail(); trail != nullptr && m_has_submission)
	{
		trail->MarkSubmitReturned(static_cast<uint32_t>(m_queue), m_submission.sequence, result);
	}
}

void CommandBuffer::MarkRecentDrawsFenceCompleted() const
{
	if (auto* trail = VulkanRecentDrawTraceTrail(); trail != nullptr && m_has_submission)
	{
		trail->MarkFenceCompleted(static_cast<uint32_t>(m_queue), m_submission.sequence);
	}
}

// Signals of one submission: the caller's binary semaphore and, for a
// command-processor submission, the queue timeline its ring waits on.
struct SubmitSignals
{
	VkSemaphore                   semaphores[2] {};
	uint64_t                      values[2] {};
	VkTimelineSemaphoreSubmitInfo timeline {};
};

// Call with the queue lock held: timeline values rise in submission order.
static void AttachSubmitSignals(VulkanQueueInfo* queue, VkSemaphore binary, bool timeline, SubmitSignals* signals,
                                VkSubmitInfo* submit_info, uint64_t* timeline_value)
{
	uint32_t count  = 0;
	*timeline_value = 0;
	if (binary != nullptr)
	{
		signals->semaphores[count++] = binary;
	}
	if (timeline)
	{
		*timeline_value                             = ++queue->timeline_value;
		signals->semaphores[count]                  = queue->timeline;
		signals->values[count++]                    = *timeline_value;
		signals->timeline.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		signals->timeline.signalSemaphoreValueCount = count;
		signals->timeline.pSignalSemaphoreValues    = signals->values;
		submit_info->pNext                          = &signals->timeline;
	}
	submit_info->signalSemaphoreCount = count;
	submit_info->pSignalSemaphores    = count != 0 ? signals->semaphores : nullptr;
}

bool CommandBuffer::GetTimeline(VkDevice* device, VkSemaphore* semaphore, uint64_t* value) const
{
	EXIT_IF(device == nullptr || semaphore == nullptr || value == nullptr);
	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);
	if (m_timeline_value == 0)
	{
		return false;
	}
	const auto* ctx = g_render_ctx->GetGraphicCtx();
	*device         = ctx->device;
	*semaphore      = ctx->queues[m_queue].timeline;
	*value          = m_timeline_value;
	return true;
}

void CommandBuffer::Execute()
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 0;
	submit_info.pWaitSemaphores      = nullptr;
	submit_info.pWaitDstStageMask    = nullptr;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 0;
	submit_info.pSignalSemaphores    = nullptr;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	auto&         queue    = g_render_ctx->GetGraphicCtx()->queues[m_queue];
	auto*         trail    = VulkanSubmitFaultTraceTrail();
	SubmitSignals signals;
	const auto  sequence = m_has_submission ? m_submission.sequence : 0u;
	VulkanSubmitAttempt attempt {};
	VulkanSubmitAttempt observed {};
	if (trail != nullptr)
	{
		attempt = MakeSubmitAttempt(VulkanSubmitKind::CommandBuffer, false);
	}

	const VkResult result = VulkanCallAndPublishOnSuccess(
	    [&]() -> VkResult
	    {
		    EXIT_IF(queue.mutex == nullptr);
		    Core::LockGuard queue_lock(*queue.mutex);
		    AttachSubmitSignals(&queue, nullptr, m_has_submission, &signals, &submit_info, &m_timeline_value);
		    MarkRecentDrawsSubmitCalled();
		    return VulkanTraceSubmitAttempt(trail, attempt, [&] { return vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence); },
		                                    &observed);
	    },
	    [&]
	    {
		    DebugStatsRecordSubmit();
		    m_execute = true;
	    });
	MarkRecentDrawsSubmitReturned(result);
	if (result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("queue_submit", result, trail != nullptr ? &observed : nullptr);
		EXIT("vkQueueSubmit failed: result=%d queue=%d slot=%" PRIu32 " sequence=%" PRIu64 "\n", static_cast<int>(result),
		     m_queue, m_index, sequence);
	}
}

void CommandBuffer::ExecuteWithSemaphore(VkSemaphore signal_semaphore)
{
	EXIT_IF(IsInvalid());
	EXIT_IF(signal_semaphore == nullptr);

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 0;
	submit_info.pWaitSemaphores      = nullptr;
	submit_info.pWaitDstStageMask    = nullptr;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 0;
	submit_info.pSignalSemaphores    = nullptr;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	auto&         queue    = g_render_ctx->GetGraphicCtx()->queues[m_queue];
	auto*         trail    = VulkanSubmitFaultTraceTrail();
	SubmitSignals signals;
	const auto  sequence = m_has_submission ? m_submission.sequence : 0u;
	VulkanSubmitAttempt attempt {};
	VulkanSubmitAttempt observed {};
	if (trail != nullptr)
	{
		attempt = MakeSubmitAttempt(VulkanSubmitKind::SemaphoreCommandBuffer, true);
	}

	const VkResult result = VulkanCallAndPublishOnSuccess(
	    [&]() -> VkResult
	    {
		    EXIT_IF(queue.mutex == nullptr);
		    Core::LockGuard queue_lock(*queue.mutex);
		    AttachSubmitSignals(&queue, signal_semaphore, m_has_submission, &signals, &submit_info, &m_timeline_value);
		    MarkRecentDrawsSubmitCalled();
		    return VulkanTraceSubmitAttempt(trail, attempt, [&] { return vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence); },
		                                    &observed);
	    },
	    [&]
	    {
		    DebugStatsRecordSubmit();
		    m_execute = true;
	    });
	MarkRecentDrawsSubmitReturned(result);
	if (result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("queue_submit_semaphore", result, trail != nullptr ? &observed : nullptr);
		EXIT("vkQueueSubmit failed: result=%d queue=%d slot=%" PRIu32 " sequence=%" PRIu64 "\n", static_cast<int>(result),
		     m_queue, m_index, sequence);
	}
}

void CommandBuffer::WaitForFence()
{
	WaitForFence(true, false);
}

void CommandBuffer::WaitForFenceWithoutLabelCallbacks()
{
	WaitForFence(false, false);
}

void CommandBuffer::WaitForFenceAndReset()
{
	WaitForFence(true, true);
}

void CommandBuffer::WaitForFenceAndResetWithoutLabelCallbacks()
{
	WaitForFence(false, true);
}

bool CommandBuffer::TryCompleteFenceAndResetWithoutLabelCallbacks()
{
	EXIT_IF(IsInvalid());
	if (!m_execute)
	{
		return true;
	}

	auto* device = g_render_ctx->GetGraphicCtx()->device;
	const auto status = VulkanCallAndPublishOnSuccess(
	    [&] { return vkGetFenceStatus(device, m_pool->fences[m_index]); },
	    [&]
	    {
		    DebugStatsRecordSubmissionComplete();
		    MarkRecentDrawsFenceCompleted();
		    g_render_ctx->GetVertexClipProbeRenderer()->Complete(this);
		    const auto fence_reset_result = vkResetFences(device, 1, &m_pool->fences[m_index]);
		    if (fence_reset_result != VK_SUCCESS)
		    {
			    ReportSubmitFault("fence_status_reset", fence_reset_result);
			    EXIT("vkResetFences failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(fence_reset_result), m_queue,
			         m_index);
		    }
		    MarkCommandBufferResetting();
		    const auto command_reset_result =
		        vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
		    if (command_reset_result != VK_SUCCESS)
		    {
			    ReportSubmitFault("fence_status_command_reset", command_reset_result);
			    EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(command_reset_result),
			         m_queue, m_index);
		    }
		    ReleasePipelineLeases();
		    m_execute = false;
	    });
	if (status == VK_NOT_READY)
	{
		return false;
	}
	if (status != VK_SUCCESS)
	{
		ReportSubmitFault("fence_status", status);
		EXIT("vkGetFenceStatus failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(status), m_queue, m_index);
	}
	return true;
}

void CommandBuffer::WaitForFence(bool drain_label_callbacks, bool reset_command_buffer)
{
	EXIT_IF(IsInvalid());

	if (m_execute)
	{
		auto* device = g_render_ctx->GetGraphicCtx()->device;

		const ::Kyty::Emulator::Ports::ScopedProgress progress(::Kyty::Emulator::Ports::ProgressLane::FenceWait);
		const auto wait_start  = DebugStatsClock::now();
		const auto wait_result = VulkanCallAndPublishOnSuccess(
		    [&]
		    { return vkWaitForFences(device, 1, &m_pool->fences[m_index], VK_TRUE, 10000000000ULL); },
		    [&]
		    {
			    DebugStatsRecordSubmissionComplete();
			    MarkRecentDrawsFenceCompleted();
			    g_render_ctx->GetVertexClipProbeRenderer()->Complete(this);
			    if (drain_label_callbacks)
			    {
				    LabelDrainCompleted();
			    }
			    const auto fence_reset_result = vkResetFences(device, 1, &m_pool->fences[m_index]);
			    if (fence_reset_result != VK_SUCCESS)
			    {
				    ReportSubmitFault("fence_wait_reset", fence_reset_result);
				    EXIT("vkResetFences failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(fence_reset_result),
				         m_queue, m_index);
			    }
			    if (reset_command_buffer)
			    {
				    MarkCommandBufferResetting();
				    const auto command_reset_result =
				        vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
				    if (command_reset_result != VK_SUCCESS)
				    {
					    ReportSubmitFault("fence_wait_command_reset", command_reset_result);
					    EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n",
					         static_cast<int>(command_reset_result), m_queue, m_index);
				    }
				    ReleasePipelineLeases();
			    }
			    m_execute = false;
		    });
		const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - wait_start).count();
		DebugStatsRecordFenceWait(static_cast<uint64_t>(wait_ns));
		if (wait_result != VK_SUCCESS)
		{
			const uint64_t sequence = m_has_submission ? m_submission.sequence : 0u;
			ReportSubmitFault("fence_wait", wait_result);
			EXIT("vkWaitForFences failed: result=%d queue=%d slot=%" PRIu32 " sequence=%" PRIu64 " after=%" PRId64 "ns\n",
			     static_cast<int>(wait_result), m_queue, m_index, sequence, static_cast<int64_t>(wait_ns));
		}
	}
}

// A new instance on the same framebuffer would start from exactly the state the
// open one holds when no attachment is cleared, discarded or transitioned.
static bool RenderPassContinuesAttachments(const VulkanFramebuffer& framebuffer, const RenderColorInfo& color,
                                           const RenderDepthInfo& depth, bool with_color, bool with_depth)
{
	if (with_depth && (depth.depth_clear_enable || depth.stencil_clear_enable ||
	                   framebuffer.depth_load_op != VK_ATTACHMENT_LOAD_OP_LOAD ||
	                   depth.vulkan_buffer->layout != framebuffer.depth_initial_layout))
	{
		return false;
	}
	const uint32_t color_count = with_color ? std::min(color.targets_num, VulkanFramebuffer::TARGETS_MAX) : 0u;
	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		const auto* image = color.attachment[slot].vulkan_buffer;
		if (image != nullptr &&
		    (framebuffer.color_load_op[slot] != VK_ATTACHMENT_LOAD_OP_LOAD || image->layout != framebuffer.color_initial_layout[slot]))
		{
			return false;
		}
	}
	return true;
}

void CommandBuffer::BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
                                    const VulkanSampleLocationState* sample_locations)
{
	EXIT_IF(IsInvalid());
	EXIT_IF(framebuffer == nullptr);

	bool       with_depth     = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	const bool with_color     = (RenderColorHasActiveTarget(*color) && RenderColorFirstActiveImage(*color) != nullptr);
	const bool custom_samples = sample_locations != nullptr && VulkanSampleLocationsEnabled(*sample_locations);
	if (m_open_render_pass.has_value() && m_open_render_pass->continuable && !custom_samples &&
	    m_open_render_pass->render_pass == framebuffer->render_pass && m_open_render_pass->framebuffer == framebuffer->framebuffer &&
	    RenderPassContinuesAttachments(*framebuffer, *color, *depth, with_color, with_depth))
	{
		DebugStatsRecordRenderPassContinuation();
		return;
	}
	CloseRenderPass();

	auto* buffer = m_pool->buffers[m_index];

	if (!with_depth && !with_color) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !with_depth && !with_color condition ignored (continuing)\n"); }

	auto* depth_image = (with_depth ? depth->vulkan_buffer : nullptr);
	// Custom sample locations apply only when the draw actually carries them.
	// A multisampled depth image reused by a 1x draw (or a copy whose guest
	// state did not program AA) has no location state; render it with the
	// driver default positions instead of rejecting the pass.
	const bool custom_depth_locations =
	    (sample_locations != nullptr && VulkanSampleLocationsEnabled(*sample_locations) && depth_image != nullptr &&
	     depth_image->sample_locations_compatible && depth_image->samples != VK_SAMPLE_COUNT_1_BIT);
	if (custom_depth_locations)
	{
		if (sample_locations == nullptr || !VulkanSampleLocationsEnabled(*sample_locations)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sample_locations == nullptr || !VulkanSampleLocationsEnabled(*sample_locations) condition ignored (continuing)\n"); }
		if (sample_locations->sample_count != depth_image->samples) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sample_locations->sample_count != depth_image->samples condition ignored (continuing)\n"); }
	} else if (sample_locations != nullptr && VulkanSampleLocationsEnabled(*sample_locations) && with_depth)
	{
		// MSAA draw against a depth image that was not created
		// SAMPLE_LOCATIONS_COMPATIBLE: drop the custom locations for this pass
		// (default sample positions) instead of rejecting the draw. Possible
		// edge artifacts on the depth test; visual output is preserved.
	}

	const uint32_t color_count = (with_color ? color->targets_num : (with_depth ? 1u : 0u));
	VkClearValue   clears[RenderColorInfo::TARGETS_MAX + 1] {};
	uint32_t       clear_attachment = 0;
	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		if (with_color && !RenderColorSlotActive(*color, slot))
		{
			continue;
		}
		if (with_color)
		{
			// Clear values belong to VkRenderPassBeginInfo, not the render-pass or
			// framebuffer identity. Keeping them in the cache key creates a fresh
			// render pass/pipeline for every animated clear color and stalls loading
			// on Metal pipeline compilation.
			const auto& attachment = color->attachment[slot];
			// The clear value must be decoded with the same format the render
			// pass/framebuffer bind the attachment as — attachment_format — not
			// the backing image format. They differ for compatible-view binds
			// (UNORM image viewed as sRGB, R32 display buffers).
			const auto clear = ResolveColorAttachmentLoadOps(attachment.vulkan_buffer->layout, attachment.cmask_fast_clear_enable,
			                                                  attachment.clear_word0, attachment.clear_word1, attachment.attachment_format);
			clears[clear_attachment].color = {{clear.clear_r, clear.clear_g, clear.clear_b, clear.clear_a}};
		} else
		{
			clears[clear_attachment].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
		}
		clear_attachment++;
	}
	if (with_depth)
	{
		if (framebuffer->depth_attachment_index >= framebuffer->attachment_count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: framebuffer->depth_attachment_index >= framebuffer->attachment_count condition ignored (continuing)\n"); }
		clears[framebuffer->depth_attachment_index].depthStencil = {depth->depth_clear_value, depth->stencil_clear_value};
	}

	const VkExtent2D extent = framebuffer->extent;
	if (extent.width == 0 || extent.height == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: extent.width == 0 || extent.height == 0 condition ignored (continuing)\n"); }

	VkRenderPassBeginInfo render_pass_info {};
	render_pass_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render_pass_info.pNext             = nullptr;
	render_pass_info.renderPass        = framebuffer->render_pass;
	render_pass_info.framebuffer       = framebuffer->framebuffer;
	render_pass_info.renderArea.offset = {0, 0};
	render_pass_info.renderArea.extent = extent;
	render_pass_info.clearValueCount   = framebuffer->attachment_count;
	render_pass_info.pClearValues      = clears;

	// Draw-side lifetime events cannot observe a clear-only render pass. Record
	// the contract before barriers mutate the emulator-side image layout.
	TraceRenderTargetLifetimePassBegin(m_guest_submit, *color, *framebuffer);

	VkSampleLocationEXT current_sample_location_values[kVulkanSampleLocationMaxCount] = {};
	VkSampleLocationEXT previous_sample_location_values[kVulkanSampleLocationMaxCount] = {};
	VkSampleLocationsInfoEXT current_sample_location_info {};
	VkSampleLocationsInfoEXT previous_sample_location_info {};
	VkAttachmentSampleLocationsEXT attachment_initial_sample_locations {};
	VkSubpassSampleLocationsEXT post_subpass_sample_locations {};
	VkRenderPassSampleLocationsBeginInfoEXT render_pass_sample_locations {};
	if (custom_depth_locations)
	{
		if (!VulkanSampleLocationsPopulateInfo(*sample_locations, current_sample_location_values,
		                                                       &current_sample_location_info)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanSampleLocationsPopulateInfo(*sample_locations, current_sample_location_va condition ignored (continuing)\n"); }
		post_subpass_sample_locations.subpassIndex         = 0;
		post_subpass_sample_locations.sampleLocationsInfo  = current_sample_location_info;
		render_pass_sample_locations.sType                 = VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT;
		render_pass_sample_locations.postSubpassSampleLocationsCount = 1;
		render_pass_sample_locations.pPostSubpassSampleLocations     = &post_subpass_sample_locations;

		if (depth_image->layout != VK_IMAGE_LAYOUT_UNDEFINED)
		{
			if (!VulkanSampleLocationsEnabled(depth_image->last_sample_locations)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanSampleLocationsEnabled(depth_image->last_sample_locations) condition ignored (continuing)\n"); }
			if (!VulkanSampleLocationsPopulateInfo(depth_image->last_sample_locations,
			                                                       previous_sample_location_values, &previous_sample_location_info)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanSampleLocationsPopulateInfo(depth_image->last_sample_locations, condition ignored (continuing)\n"); }
			attachment_initial_sample_locations.attachmentIndex       = framebuffer->depth_attachment_index;
			attachment_initial_sample_locations.sampleLocationsInfo   = previous_sample_location_info;
			render_pass_sample_locations.attachmentInitialSampleLocationsCount = 1;
			render_pass_sample_locations.pAttachmentInitialSampleLocations     = &attachment_initial_sample_locations;
		}
		render_pass_info.pNext = &render_pass_sample_locations;
	}

	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		if (!with_color || color->attachment[slot].vulkan_buffer == nullptr ||
		    color->attachment[slot].vulkan_buffer->layout == framebuffer->color_initial_layout[slot])
		{
			continue;
		}
		auto* image = color->attachment[slot].vulkan_buffer;
		const auto source = ResolveImageTransitionSource(image->layout);
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext                           = nullptr;
		image_memory_barrier.srcAccessMask                   = source.access;
		image_memory_barrier.dstAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		image_memory_barrier.oldLayout                       = image->layout;
		image_memory_barrier.newLayout                       = framebuffer->color_initial_layout[slot];
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = image->image;
		image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

		vkCmdPipelineBarrier(buffer, source.stages, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
		                     nullptr, 1, &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}

	const auto depth_stencil_layout = framebuffer->depth_stencil_layout;
	if (with_depth && depth_stencil_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
	                     depth_stencil_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
	                     depth_stencil_layout != VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: with_depth && depth_stencil_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_O condition ignored (continuing)\n"); }
	// Transition to the render-pass initial layout, not the subpass layout.
	// First-use CLEAR keeps UNDEFINED so vkCmdBeginRenderPass can discard+clear;
	// a pre-pass UNDEFINED→ATTACHMENT would define nothing and then LOAD garbage.
	if (with_depth && framebuffer->depth_initial_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
	    depth->vulkan_buffer->layout != framebuffer->depth_initial_layout)
	{
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext =
		    (custom_depth_locations && depth_image->layout != VK_IMAGE_LAYOUT_UNDEFINED ? &previous_sample_location_info : nullptr);
		image_memory_barrier.srcAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.dstAccessMask =
		    (depth_stencil_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
		         ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT
		         : (depth_stencil_layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
		                ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
		                      VK_ACCESS_SHADER_READ_BIT
		                : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
		image_memory_barrier.oldLayout                       = depth->vulkan_buffer->layout;
		image_memory_barrier.newLayout                       = framebuffer->depth_initial_layout;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = depth->vulkan_buffer->image;
		image_memory_barrier.subresourceRange.aspectMask     = DepthFormatAspectMask(depth->vulkan_buffer->format);
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

		vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &image_memory_barrier);

		depth->vulkan_buffer->layout = image_memory_barrier.newLayout;
	}

	vkCmdBeginRenderPass(buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
	DebugStatsRecordRenderPassBegin();
	m_open_render_pass = OpenRenderPass {framebuffer->render_pass, framebuffer->framebuffer, !custom_samples};
	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		if (!with_color || color->attachment[slot].vulkan_buffer == nullptr ||
		    color->attachment[slot].vulkan_buffer->type != VulkanImageType::RenderTexture)
		{
			continue;
		}
		auto* image = static_cast<RenderTextureVulkanImage*>(color->attachment[slot].vulkan_buffer);
		if (framebuffer->color_load_op[slot] == VK_ATTACHMENT_LOAD_OP_CLEAR &&
		    extent.width == image->extent.width && extent.height == image->extent.height && image->mip_levels == 1u &&
		    color->attachment[slot].base_array_layer == 0u && color->attachment[slot].layer_count == 1u)
		{
			image->fully_defined_from_clear = true;
		} else if (framebuffer->color_load_op[slot] == VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		{
			image->fully_defined_from_clear = false;
		}
	}

	// The render pass final layout is COLOR_ATTACHMENT_OPTIMAL. Keep the
	// emulator-side tracker in sync so a later sampled use emits the required
	// attachment-to-shader-read barrier instead of treating the image as new.
	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		if (with_color && color->attachment[slot].vulkan_buffer != nullptr)
		{
			color->attachment[slot].vulkan_buffer->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
	}
	if (with_depth)
	{
		depth->vulkan_buffer->layout = framebuffer->depth_final_layout;
		if (custom_depth_locations)
		{
			depth_image->last_sample_locations = *sample_locations;
		}
	}
}

void CommandBuffer::CloseRenderPass()
{
	EXIT_IF(IsInvalid());
	if (!m_open_render_pass.has_value())
	{
		return;
	}
	m_open_render_pass.reset();

	auto* buffer = m_pool->buffers[m_index];
	vkCmdEndRenderPass(buffer);

	// Without this dependency hosts can hang when a later draw samples a color
	// target written earlier in the same command buffer.
	VkMemoryBarrier memory_barrier {};
	memory_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
	vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
	                     0, 1, &memory_barrier, 0, nullptr, 0, nullptr);
}

VkCommandBuffer CommandBuffer::RecordOutsideRenderPass()
{
	EXIT_IF(IsInvalid());
	CloseRenderPass();
	return m_pool->buffers[m_index];
}

VkCommandBuffer CommandBuffer::RecordState() const
{
	EXIT_IF(IsInvalid());
	return m_pool->buffers[m_index];
}

VkCommandBuffer CommandBuffer::RecordInsideRenderPass() const
{
	EXIT_IF(IsInvalid());
	EXIT_IF(!m_open_render_pass.has_value());
	return m_pool->buffers[m_index];
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
