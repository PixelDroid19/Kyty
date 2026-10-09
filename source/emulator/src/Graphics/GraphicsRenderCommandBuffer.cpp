#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Agent/Json.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Agent/EventRing.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/DiagnosticDump.h"
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
#include "Emulator/Profiler.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// CommandBuffer methods + TransientBufferPool

namespace {

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
		case ShaderNativeWaveProof::FragmentNeutral32: return true;
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
		    entry.gs_back_program == state.gs_back_program && entry.stages == state.stages)
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
	                                              state.max_vertex_out, state.output_primitive, state.primitive_group_size,
	                                              state.vertex_group_size, 1u};
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
	std::string report = "{\"schema\":\"guest_draw_skipped_census\",\"version\":1,\"programs\":[";
	{
		std::lock_guard<std::mutex> lock(g_skipped_census_mutex);
		for (size_t i = 0; i < g_skipped_census_size; ++i)
		{
			const auto& e = g_skipped_census[i];
			char        row[384] {};
			std::snprintf(row, sizeof(row),
			              "%s{\"gs_checksum\":\"0x%016llx\",\"es\":\"0x%llx\",\"gs_back\":\"0x%llx\",\"stages\":%u,"
			              "\"max_vertex_out\":%u,\"output_primitive\":%u,\"primitive_group\":%u,\"vertex_group\":%u,\"draws\":%llu}",
			              i == 0 ? "" : ",", static_cast<unsigned long long>(e.gs_checksum), static_cast<unsigned long long>(e.es_program),
			              static_cast<unsigned long long>(e.gs_back_program), e.stages, e.max_vertex_out, e.output_primitive,
			              e.primitive_group, e.vertex_group, static_cast<unsigned long long>(e.draws));
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

static void ReportCriticalTransientPoolCapacityReject(uint64_t requested_size, uint32_t usage, uint32_t usage_entries,
                                                      uint32_t total_entries, uint64_t total_bytes)
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
	             " max_bytes=%" PRIu64 " usage_cap_failed=%d entry_cap_failed=%d byte_cap_failed=%d\n",
	             report_number, static_cast<unsigned>(critical_class), requested_size, usage, usage_entries, total_entries,
	             total_bytes, max_bytes, usage_cap_failed ? 1 : 0, entry_cap_failed ? 1 : 0, byte_cap_failed ? 1 : 0);
}

class TransientBufferPool
{
	struct Entry
	{
		VulkanBuffer buffer;
		void*        mapped = nullptr;
		uint64_t     size   = 0;
		uint32_t     usage  = 0;
		bool         used    = false;
		bool         scratch = false;
		bool         snapshot_valid = false;
		uint64_t     snapshot_vaddr = 0;
		uint64_t     snapshot_size  = 0;
	};

public:
	VulkanBuffer* Upload(GraphicContext* ctx, const void* data, uint64_t size, uint32_t usage,
	                     GpuMemoryTransientBufferAllocationClass allocation_class)
	{
		EXIT_IF(ctx == nullptr || data == nullptr || size == 0u || usage == 0u);
		auto* entry = Acquire(ctx, size, usage, allocation_class, true, nullptr);
		if (entry == nullptr)
		{
			return nullptr;
		}

		const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, size);
		entry->snapshot_valid = false;
		std::memcpy(entry->mapped, data, static_cast<size_t>(size));
		Commit(entry, size);
		return &entry->buffer;
	}

	VulkanBuffer* Capture(GraphicContext* ctx, uint64_t vaddr, uint64_t size, uint32_t usage, uint64_t* validation_ns,
	                      uint64_t* upload_ns, uint64_t* compare_ns, bool* reused)
	{
		EXIT_IF(ctx == nullptr || validation_ns == nullptr || upload_ns == nullptr || compare_ns == nullptr || reused == nullptr);
		*validation_ns = 0u;
		*upload_ns     = 0u;
		*compare_ns    = 0u;
		*reused        = false;
		if (!GpuMemoryCanUseTransientReadOnlyBuffer(true, size, true, true) || usage == 0u)
		{
			return nullptr;
		}

		const auto capture_start = std::chrono::steady_clock::now();
		const auto finish_upload_time = [&]()
		{
			const auto total_ns = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - capture_start).count());
			const uint64_t excluded_ns = *validation_ns > UINT64_MAX - *compare_ns ? UINT64_MAX : *validation_ns + *compare_ns;
			*upload_ns = total_ns > excluded_ns ? total_ns - excluded_ns : 0u;
		};
		Entry*     previous      = nullptr;
		for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
		{
			auto* candidate = *it;
			if (candidate->used && !candidate->scratch && candidate->snapshot_valid && candidate->usage == usage &&
			    candidate->snapshot_vaddr == vaddr && candidate->snapshot_size == size)
			{
				previous = candidate;
				break;
			}
		}
		if (previous != nullptr)
		{
			bool     matches             = false;
			uint64_t reuse_validation_ns = 0u;
			if (!GpuMemoryCompareSnapshotReadOnlyBuffer(vaddr, size, previous->mapped, &matches, &reuse_validation_ns, compare_ns))
			{
				*validation_ns += reuse_validation_ns;
				return nullptr;
			}
			*validation_ns += reuse_validation_ns;
			if (matches)
			{
				previous->buffer.descriptor_range = size;
				*reused                           = true;
				return &previous->buffer;
			}
		}
		bool       created       = false;
		auto*      entry = Acquire(ctx, size, usage, GpuMemoryTransientBufferAllocationClass::Snapshot, false, nullptr);
		if (entry == nullptr)
		{
			const auto preflight_start = std::chrono::steady_clock::now();
			const bool eligible        = GpuMemoryCanSnapshotReadOnlyBuffer(vaddr, size);
			*validation_ns += static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - preflight_start).count());
			if (!eligible)
			{
				finish_upload_time();
				return nullptr;
			}
			entry = Acquire(ctx, size, usage, GpuMemoryTransientBufferAllocationClass::Snapshot, true, &created);
		}
		if (entry == nullptr)
		{
			finish_upload_time();
			return nullptr;
		}

		uint64_t atomic_validation_ns = 0u;
		uint64_t copy_ns              = 0u;
		if (!GpuMemoryCaptureSnapshotReadOnlyBuffer(vaddr, size, entry->mapped, &atomic_validation_ns, &copy_ns))
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
		DebugStatsRecordUpload(size, copy_ns);
		entry->snapshot_valid = true;
		entry->snapshot_vaddr = vaddr;
		entry->snapshot_size  = size;
		Commit(entry, size);
		finish_upload_time();
		return &entry->buffer;
	}

	VulkanBuffer* Scratch(GraphicContext* ctx, uint64_t size, uint32_t usage)
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
			if (candidate->scratch && candidate->usage == usage && candidate->size >= size &&
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
		entry->buffer.memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
		                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		VulkanCreateBuffer(ctx, size, &entry->buffer);
		VulkanMapMemory(ctx, &entry->buffer.memory, &entry->mapped);
		if (entry->mapped == nullptr)
		{
			VulkanDeleteBuffer(ctx, &entry->buffer);
			delete entry;
			return nullptr;
		}
		m_entries.push_back(entry);
		m_total_bytes += size;
		return &entry->buffer;
	}

	void Reset()
	{
		for (auto* entry: m_entries)
		{
			entry->used = false;
		}
	}

	void Destroy(GraphicContext* ctx)
	{
		EXIT_IF(ctx == nullptr);
		for (auto* entry: m_entries)
		{
			EXIT_IF(entry == nullptr || entry->mapped == nullptr);
			VulkanUnmapMemory(ctx, &entry->buffer.memory);
			entry->mapped = nullptr;
			VulkanDeleteBuffer(ctx, &entry->buffer);
			delete entry;
		}
		m_entries.clear();
		m_total_bytes = 0;
	}

private:
	Entry* Acquire(GraphicContext* ctx, uint64_t size, uint32_t usage, GpuMemoryTransientBufferAllocationClass allocation_class,
	               bool allow_create, bool* created)
	{
		if (created != nullptr)
		{
			*created = false;
		}
		Entry*   entry              = nullptr;
		Entry*   larger_unused      = nullptr;
		uint32_t usage_entries      = 0;
		for (auto* candidate: m_entries)
		{
			if (candidate->scratch)
			{
				continue;
			}
			if (candidate->usage == usage)
			{
				usage_entries++;
			}
			if (candidate->used || candidate->usage != usage || candidate->size < size)
			{
				continue;
			}
			// Prefer exact size; fall back to the smallest free entry that fits.
			// Descriptor writes use an explicit range (not the full VkBuffer size),
			// so a larger free slab is valid. V# spill UBOs can otherwise fail
			// when many distinct sizes fragment MaxEntriesPerUsage=512.
			if (candidate->size == size)
			{
				entry = candidate;
				break;
			}
			if (larger_unused == nullptr || candidate->size < larger_unused->size)
			{
				larger_unused = candidate;
			}
		}
		if (entry == nullptr)
		{
			entry = larger_unused;
		}

		if (entry == nullptr)
		{
			if (!allow_create)
			{
				return nullptr;
			}
			if (!GpuMemoryTransientBufferPoolCanAllocate(usage_entries, static_cast<uint32_t>(m_entries.size()), m_total_bytes, size,
			                                                   allocation_class))
			{
				if (allocation_class == GpuMemoryTransientBufferAllocationClass::Critical)
				{
					ReportCriticalTransientPoolCapacityReject(size, usage, usage_entries,
					                                          static_cast<uint32_t>(m_entries.size()), m_total_bytes);
				}
				return nullptr;
			}

			entry                         = new Entry;
			entry->size                   = size;
			entry->usage                  = usage;
			entry->buffer.usage           = usage;
			entry->buffer.memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
			                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
			VulkanCreateBuffer(ctx, size, &entry->buffer);
			VulkanMapMemory(ctx, &entry->buffer.memory, &entry->mapped);
			EXIT_IF(entry->mapped == nullptr);
			m_entries.push_back(entry);
			m_total_bytes += size;
			if (created != nullptr)
			{
				*created = true;
			}
		}
		return entry;
	}

	void DiscardNew(GraphicContext* ctx, Entry* entry)
	{
		EXIT_IF(ctx == nullptr || entry == nullptr || entry->used || m_entries.empty() || m_entries.back() != entry ||
		        m_total_bytes < entry->size);
		m_entries.pop_back();
		m_total_bytes -= entry->size;
		VulkanUnmapMemory(ctx, &entry->buffer.memory);
		entry->mapped = nullptr;
		VulkanDeleteBuffer(ctx, &entry->buffer);
		delete entry;
	}

	static void Commit(Entry* entry, uint64_t size)
	{
		EXIT_IF(entry == nullptr || entry->mapped == nullptr || size == 0u || size > entry->size);
		const uint64_t tail_bytes = GpuMemoryTransientBufferTailBytes(entry->size, size);
		if (tail_bytes != 0u)
		{
			std::memset(static_cast<uint8_t*>(entry->mapped) + size, 0, static_cast<size_t>(tail_bytes));
		}
		entry->buffer.descriptor_range = size;
		entry->used = true;
	}

	std::vector<Entry*> m_entries;
	uint64_t            m_total_bytes = 0;
};


// CommandBuffer methods (uses TransientBufferPool from Internal.h)

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

	for (uint32_t i = 0; i < m_pool->buffers_count; i++)
	{
		if (!m_pool->busy[i])
		{
			m_pool->busy[i] = true;
			const auto reset_result = vkResetCommandBuffer(m_pool->buffers[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
			if (reset_result != VK_SUCCESS)
			{
				VulkanSubmitFaultReport("command_buffer_allocate_reset", reset_result);
				EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(reset_result), m_queue,
				     i);
			}
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

	m_pool->busy[m_index] = false;
	const auto reset_result = vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	if (reset_result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("command_buffer_free_reset", reset_result);
		EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(reset_result), m_queue,
		     m_index);
	}
	m_index = static_cast<uint32_t>(-1);

	if (!IsInvalid()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !IsInvalid() condition ignored (continuing)\n"); }
}

void CommandBuffer::Begin() const
{
	EXIT_IF(IsInvalid());
	if (m_transient_buffers != nullptr)
	{
		m_transient_buffers->Reset();
	}
	if (m_transient_scratch_buffers != nullptr)
	{
		m_transient_scratch_buffers->Reset();
	}

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

VulkanBuffer* CommandBuffer::CaptureTransientSnapshotBuffer(uint64_t vaddr, uint64_t size, uint32_t usage, uint64_t* validation_ns,
	                                                         uint64_t* upload_ns, uint64_t* compare_ns, bool* reused)
{
	EXIT_IF(IsInvalid());
	if (m_transient_buffers == nullptr)
	{
		m_transient_buffers = new TransientBufferPool;
	}
	return m_transient_buffers->Capture(g_render_ctx->GetGraphicCtx(), vaddr, size, usage, validation_ns, upload_ns, compare_ns, reused);
}

VulkanBuffer* CommandBuffer::AllocateTransientScratchBuffer(uint64_t size, uint32_t usage)
{
	EXIT_IF(IsInvalid());
	if (m_transient_scratch_buffers == nullptr)
	{
		m_transient_scratch_buffers = new TransientBufferPool;
	}
	return m_transient_scratch_buffers->Scratch(g_render_ctx->GetGraphicCtx(), size, usage);
}

void CommandBuffer::End() const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	const auto result = vkEndCommandBuffer(buffer);
	if (result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("command_buffer_end", result);
		EXIT("vkEndCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(result), m_queue, m_index);
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

	const auto& queue    = g_render_ctx->GetGraphicCtx()->queues[m_queue];
	auto*       trail    = VulkanSubmitFaultTraceTrail();
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
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores    = &signal_semaphore;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	const auto& queue    = g_render_ctx->GetGraphicCtx()->queues[m_queue];
	auto*       trail    = VulkanSubmitFaultTraceTrail();
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
		    const auto command_reset_result =
		        vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
		    if (command_reset_result != VK_SUCCESS)
		    {
			    ReportSubmitFault("fence_status_command_reset", command_reset_result);
			    EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(command_reset_result),
			         m_queue, m_index);
		    }
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

		const auto wait_start = std::chrono::steady_clock::now();
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
				    const auto command_reset_result =
				        vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
				    if (command_reset_result != VK_SUCCESS)
				    {
					    ReportSubmitFault("fence_wait_command_reset", command_reset_result);
					    EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n",
					         static_cast<int>(command_reset_result), m_queue, m_index);
				    }
			    }
			    m_execute = false;
		    });
		const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_start).count();
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

void CommandBuffer::BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
                                    const VulkanSampleLocationState* sample_locations) const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	EXIT_IF(framebuffer == nullptr);

	bool with_depth = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	bool with_color = (RenderColorHasActiveTarget(*color) && RenderColorFirstActiveImage(*color) != nullptr);

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
	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		if (!with_color || color->attachment[slot].vulkan_buffer == nullptr ||
		    color->attachment[slot].vulkan_buffer->type != VulkanImageType::RenderTexture)
		{
			continue;
		}
		auto* image = static_cast<RenderTextureVulkanImage*>(color->attachment[slot].vulkan_buffer);
		if (framebuffer->color_load_op[slot] == VK_ATTACHMENT_LOAD_OP_CLEAR &&
		    extent.width == image->extent.width && extent.height == image->extent.height &&
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

void CommandBuffer::EndRenderPass() const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	vkCmdEndRenderPass(buffer);
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
