#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/GpuSubmissionTracker.h"
#include "Emulator/Graphics/GraphicsGeState.h"
#include "Emulator/Kernel/EventQueue.h"

#include <vulkan/vulkan_core.h>

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

class CommandBuffer;
class CommandProcessor;
class TransientBufferPool;
class DiagnosticDumpWriter;
struct ShaderNativeWaveInfo;
struct ShaderProgramSnapshot;
struct VideoOutVulkanImage;
struct DepthStencilVulkanImage;
struct TextureVulkanImage;
struct StorageTextureVulkanImage;
struct RenderTextureVulkanImage;
struct VulkanCommandPool;
struct VulkanBuffer;
struct VulkanFramebuffer;
struct RenderDepthInfo;
struct RenderColorInfo;
struct GraphicContext;
struct VulkanSampleLocationState;

template <typename OperationFunc, typename PublishFunc>
[[nodiscard]] VkResult VulkanCallAndPublishOnSuccess(OperationFunc&& operation, PublishFunc&& publish)
{
	const VkResult result = operation();
	if (result == VK_SUCCESS)
	{
		publish();
	}
	return result;
}

enum class VulkanSubmitKind: uint8_t
{
	CommandBuffer,
	SemaphoreCommandBuffer,
	TileDetile,
};

struct VulkanSubmitAttempt
{
	uint64_t attempt                 = 0;
	// Unknown unless the command buffer carries a host submission id.
	bool     has_host_submission     = false;
	uint64_t host_submission_sequence = 0;
	// Unknown unless the command processor stamped the guest submit and packet.
	bool     has_guest_context       = false;
	uint64_t guest_submit            = 0;
	VulkanSubmitKind kind            = VulkanSubmitKind::CommandBuffer;
	uint32_t queue                   = 0;
	uint32_t command_buffer_slot     = 0;
	// Presented VideoOut frames when the attempt was made.
	int32_t  presented_frame         = 0;
	uint32_t pm4_op                  = 0;
	uint32_t pm4_dw                  = 0;
	VkResult result                  = VK_NOT_READY;
	bool     signals_semaphore       = false;
	bool     completed               = false;
};

struct VulkanSubmitAttemptSnapshot
{
	std::array<VulkanSubmitAttempt, 8> entries {};
	uint32_t                           count = 0;
	uint64_t                           dropped = 0;
};

enum class RenderTargetLifetimeTraceArmResult: uint8_t
{
	Armed,
	NotReady,
	TraceDisabled,
	AgentArmDisabled,
	AlreadyPending,
	AlreadyOpen,
};

// Requests one bounded render-thread opening of the opt-in lifetime trace.
// Immutable trace configuration is initialized by the render thread. The
// agent thread only publishes an atomic request; it never touches trace
// targets, counters, files, capture state, or Vulkan objects.
[[nodiscard]] RenderTargetLifetimeTraceArmResult GraphicsRequestRenderTargetLifetimeTraceArm();

class VulkanSubmitAttemptTrail
{
public:
	[[nodiscard]] uint64_t Begin(VulkanSubmitAttempt attempt)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		VulkanSubmitAttempt* slot = nullptr;
		for (auto& entry: m_entries)
		{
			if (entry.attempt == 0u)
			{
				slot = &entry;
				break;
			}
		}
		if (slot == nullptr)
		{
			for (auto& entry: m_entries)
			{
				if (entry.completed && (slot == nullptr || entry.attempt < slot->attempt))
				{
					slot = &entry;
				}
			}
		}
		if (slot == nullptr)
		{
			m_dropped++;
			return 0u;
		}
		if (slot->attempt != 0u)
		{
			m_dropped++;
		}
		attempt.attempt   = m_next_attempt++;
		attempt.completed = false;
		*slot             = attempt;
		return attempt.attempt;
	}

	[[nodiscard]] bool Finish(uint64_t attempt, VkResult result)
	{
		if (attempt == 0u)
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto& entry: m_entries)
		{
			if (entry.attempt == attempt)
			{
				entry.result    = result;
				entry.completed = true;
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] VulkanSubmitAttemptSnapshot Snapshot() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return SnapshotLocked();
	}

	[[nodiscard]] bool LatchDeviceLost(VkResult result, VulkanSubmitAttemptSnapshot* snapshot)
	{
		if (result != VK_ERROR_DEVICE_LOST || snapshot == nullptr)
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_failure_latched)
		{
			return false;
		}
		m_failure_latched = true;
		*snapshot         = SnapshotLocked();
		return true;
	}

private:
	[[nodiscard]] VulkanSubmitAttemptSnapshot SnapshotLocked() const
	{
		VulkanSubmitAttemptSnapshot snapshot;
		snapshot.dropped = m_dropped;
		for (const auto& entry: m_entries)
		{
			if (entry.attempt != 0u)
			{
				snapshot.entries[snapshot.count++] = entry;
			}
		}
		for (uint32_t i = 1; i < snapshot.count; ++i)
		{
			auto     entry = snapshot.entries[i];
			uint32_t pos   = i;
			while (pos > 0u && snapshot.entries[pos - 1u].attempt > entry.attempt)
			{
				snapshot.entries[pos] = snapshot.entries[pos - 1u];
				--pos;
			}
			snapshot.entries[pos] = entry;
		}
		return snapshot;
	}

	mutable std::mutex                  m_mutex;
	std::array<VulkanSubmitAttempt, 8> m_entries {};
	uint64_t                            m_next_attempt   = 1;
	uint64_t                            m_dropped        = 0;
	bool                                m_failure_latched = false;
};

template <typename OperationFunc>
[[nodiscard]] VkResult VulkanTraceSubmitAttempt(VulkanSubmitAttemptTrail* trail, VulkanSubmitAttempt attempt,
	                                                OperationFunc&& operation, VulkanSubmitAttempt* observed = nullptr)
{
	if (trail == nullptr)
	{
		return operation();
	}
	const uint64_t attempt_id = trail->Begin(attempt);
	const VkResult result     = operation();
	attempt.attempt           = attempt_id;
	attempt.result            = result;
	attempt.completed         = true;
	if (observed != nullptr)
	{
		*observed = attempt;
	}
	if (attempt_id != 0u)
	{
		(void)trail->Finish(attempt_id, result);
	}
	return result;
}

enum class VulkanRecentDrawKind: uint8_t
{
	DrawIndexed,
	Draw,
	Dispatch,
};

// One recorded draw or dispatch command. Recording is not GPU execution: the
// submit and fence fields are filled only by the matching host submission's
// observed submit call/return and fence completion; otherwise they stay unknown.
struct VulkanRecentDraw
{
	uint64_t             record                   = 0;
	VulkanRecentDrawKind kind                     = VulkanRecentDrawKind::DrawIndexed;
	uint32_t             queue                    = 0;
	uint32_t             command_buffer_slot      = 0;
	bool                 has_host_submission      = false;
	uint64_t             host_submission_sequence = 0;
	uint64_t             guest_submit             = 0;
	bool                 has_pm4                  = false;
	uint32_t             pm4_op                   = 0;
	uint32_t             pm4_dw                   = 0;
	// Guest shader register checksums: vs/ps for draws, cs for dispatches.
	uint64_t             vs_checksum              = 0;
	uint64_t             ps_checksum              = 0;
	uint64_t             cs_checksum              = 0;
	uint32_t             primitive_type           = 0;
	uint32_t             count                    = 0;
	uint32_t             instances                = 0;
	// Base vertex for indexed draws; first vertex for non-indexed draws.
	int32_t              vertex_offset            = 0;
	uint32_t             first_instance           = 0;
	uint32_t             host_commands            = 0;
	uint32_t             groups[3]                = {};
	bool                 submit_called            = false;
	bool                 submit_returned          = false;
	VkResult             submit_result            = VK_NOT_READY;
	bool                 fence_completed          = false;
};

class VulkanRecentDrawTrail
{
public:
	static constexpr uint32_t CAPACITY_MAX = 256;

	explicit VulkanRecentDrawTrail(uint32_t capacity): m_capacity(capacity > CAPACITY_MAX ? 0u : capacity) {}

	[[nodiscard]] uint32_t Capacity() const { return m_capacity; }

	// Keeps the newest records; the oldest is overwritten and counted as dropped.
	[[nodiscard]] uint64_t Record(VulkanRecentDraw draw)
	{
		if (m_capacity == 0u)
		{
			return 0u;
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		draw.record          = m_next_record++;
		draw.submit_called   = false;
		draw.submit_returned = false;
		draw.submit_result   = VK_NOT_READY;
		draw.fence_completed = false;
		if (m_count == m_capacity)
		{
			m_dropped++;
		} else
		{
			m_count++;
		}
		m_entries[m_next_index] = draw;
		m_next_index            = (m_next_index + 1u) % m_capacity;
		return draw.record;
	}

	void MarkSubmitCalled(uint32_t queue, uint64_t host_sequence)
	{
		Update(queue, host_sequence, [](VulkanRecentDraw& draw) { draw.submit_called = true; });
	}

	void MarkSubmitReturned(uint32_t queue, uint64_t host_sequence, VkResult result)
	{
		Update(queue, host_sequence,
		       [result](VulkanRecentDraw& draw)
		       {
			       draw.submit_returned = true;
			       draw.submit_result   = result;
		       });
	}

	// Call only after the command buffer's fence was observed signaled.
	void MarkFenceCompleted(uint32_t queue, uint64_t host_sequence)
	{
		Update(queue, host_sequence, [](VulkanRecentDraw& draw) { draw.fence_completed = true; });
	}

	// Copies retained records oldest first. Returns the number of records written.
	uint32_t Snapshot(VulkanRecentDraw* out, uint32_t out_capacity, uint64_t* dropped) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const uint32_t count = m_count < out_capacity ? m_count : out_capacity;
		const uint32_t first = (m_next_index + m_capacity - m_count) % (m_capacity == 0u ? 1u : m_capacity);
		const uint32_t skip  = m_count - count;
		for (uint32_t i = 0; i < count; ++i)
		{
			out[i] = m_entries[(first + skip + i) % m_capacity];
		}
		if (dropped != nullptr)
		{
			*dropped = m_dropped + skip;
		}
		return count;
	}

private:
	template <typename UpdateFunc>
	void Update(uint32_t queue, uint64_t host_sequence, UpdateFunc&& update)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (uint32_t i = 0; i < m_count; ++i)
		{
			auto& draw = m_entries[i];
			if (draw.has_host_submission && draw.queue == queue && draw.host_submission_sequence == host_sequence)
			{
				update(draw);
			}
		}
	}

	mutable std::mutex                             m_mutex;
	std::array<VulkanRecentDraw, CAPACITY_MAX>     m_entries {};
	uint32_t                                       m_capacity    = 0;
	uint32_t                                       m_count       = 0;
	uint32_t                                       m_next_index  = 0;
	uint64_t                                       m_next_record = 1;
	uint64_t                                       m_dropped     = 0;
};

// Fault context for the recent-draw report: the failing caller's command buffer.
struct VulkanRecentDrawFault
{
	const char* stage               = nullptr;
	VkResult    result              = VK_SUCCESS;
	bool        has_context         = false;
	uint32_t    queue               = 0;
	uint32_t    command_buffer_slot = 0;
	bool        has_host_submission = false;
	uint64_t    host_sequence       = 0;
};

enum class GraphicsSkippedDrawReason: uint8_t
{
	InvalidVertexShader,
	UnsupportedGeState,
};

struct GraphicsSkippedDrawCounts
{
	uint64_t invalid_vertex_shader = 0;
	uint64_t unsupported_ge_state  = 0;
};

// A skipped guest draw is published as a native event on its first occurrence and whenever the per-reason
// count reaches a power of ten, so the scale of an omission is visible under Silent logging without a
// per-draw event (at most 20 events per reason). `count` is the number of skips including this one.
[[nodiscard]] constexpr bool GraphicsSkippedDrawEventDue(uint64_t count)
{
	if (count == 0u)
	{
		return false;
	}
	while (count % 10u == 0u)
	{
		count /= 10u;
	}
	return count == 1u;
}

// Same-draw output interface. Effective state is observed emulator state, not
// proof of a raw register assignment; only the explicit raw records carry that
// provenance. Pixel program identities are not dereferenced by this snapshot.
struct GraphicsNativeWaveOutputState
{
	GraphicsGeRawRegister vs_out_config_raw;
	GraphicsGeRawRegister position_format_raw;
	GraphicsGeRawRegister output_control_raw;
	GraphicsGeRawRegister output_primitive_raw;
	uint32_t vs_out_config = 0;
	uint32_t position_format = 0;
	uint32_t output_control = 0;
	uint32_t output_primitive = 0;
	uint64_t pixel_program = 0;
	uint64_t pixel_checksum = 0;
	bool pixel_embedded = false;
	uint32_t pixel_embedded_id = 0;
	uint32_t pixel_input_enable = 0;
	uint32_t pixel_input_address = 0;
	uint32_t pixel_input_control = 0;
	uint32_t barycentric_control = 0;
	uint32_t interpolator_written_mask = 0;
	std::array<uint32_t, 32> interpolators {};
	// HW::ModeControl/ClipControl members in declaration order, as decoded
	// values. The JSON serializer gives each member its name, never an ASIC word.
	std::array<uint32_t, 11> raster_mode {};
	std::array<uint32_t, 12> clip_control {};
};

// Explicitly separate the legacy pseudo-stage base from the merged GS back
// program. A zero legacy base does not establish an absent hardware program.
struct GraphicsSkippedGeState
{
	GraphicsGeRawRegister stages_raw;
	GraphicsGeRawRegister ge_control_raw;
	GraphicsGeRawRegister ge_user_vgpr_raw;
	GraphicsGeRawRegister gs_resource1_raw;
	GraphicsGeRawRegister gs_resource2_raw;
	GraphicsGeRawRegister gs_resource3_raw;
	uint32_t stages = 0;
	uint64_t es_program = 0;
	uint64_t gs_back_program = 0;
	uint64_t legacy_gs_program = 0;
	uint64_t gs_checksum = 0;
	uint64_t gs_user_data_address = 0;
	uint32_t es_resource1 = 0;
	uint32_t gs_resource3 = 0;
	uint32_t gs_vgprs = 0;
	uint32_t gs_sgprs = 0;
	uint32_t float_mode = 0;
	uint32_t lds_size = 0;
	uint32_t es_vgpr_components = 0;
	uint32_t gs_vgpr_components = 0;
	uint32_t user_sgpr_count = 0;
	std::array<uint32_t, 32> user_sgprs {};
	uint32_t max_vertex_out = 0;
	uint32_t output_primitive = 0;
	uint32_t ngg_subgroup_control = 0;
	uint32_t max_output_per_subgroup = 0;
	uint32_t gs_instance_count = 0;
	uint32_t gs_onchip_control = 0;
	uint32_t esgs_ring_item_size = 0;
	uint32_t index_format = 0;
	uint32_t primitive_group_size = 0;
	uint32_t vertex_group_size = 0;
	std::optional<GraphicsNativeWaveOutputState> output_state;
};

[[nodiscard]] GraphicsSkippedGeState GraphicsDescribeSkippedGeState(const HW::Context& ctx, const HW::UserConfig& ucfg,
                                                                   const HW::Shader& shader);
[[nodiscard]] std::string GraphicsSkippedGeStateJson(const GraphicsSkippedGeState& state);

// Actual guest draw arguments, before topology conversion. An unset field is
// unknown, including index fields on auto draws; an observed zero stays zero.
// index_type retains the caller's raw index_type_and_size word. Addresses are
// identities only and are never dereferenced by the draw-metadata reporter.
struct GraphicsNativeWaveDrawInfo
{
	std::optional<uint32_t> count;
	std::optional<bool>     indexed;
	std::optional<uint32_t> index_type;
	std::optional<uint32_t> first_instance;
	std::optional<uint32_t> instance_count;
	std::optional<uint64_t> index_address;
	std::optional<int32_t>  vertex_offset_add;
	std::optional<uint64_t> draw_modifier;
	std::optional<uint32_t> primitive_type;
	std::optional<uint32_t> index_offset;
};

inline constexpr size_t NATIVE_WAVE_REPORT_BYTES_MAX = 8u * 1024u;

// The default capture uses ShaderSnapshotMappedProgram's metadata/readable-range
// leases. A replaceable copy boundary lets CPU fixtures verify ordering and the
// disabled path without global resets or a guest. Both copies finish before I/O.
// Capture callbacks must release leases before returning owned bytes bounded by max_bytes.
using GraphicsNativeWaveProgramCapture = ShaderProgramSnapshot (*)(uint64_t address, uint32_t max_bytes, void* context);

class GraphicsNativeWaveInputReporter final
{
public:
	explicit GraphicsNativeWaveInputReporter(DiagnosticDumpWriter& writer, GraphicsNativeWaveProgramCapture capture = nullptr,
	                                        void* capture_context = nullptr)
	    : m_writer(writer), m_capture(capture), m_capture_context(capture_context)
	{
	}

	// One attempted wave-sensitive vertex record per instance, including failed
	// writes. Disabled and lane-local/unclassified inputs do not consume the latch.
	// Returns disabled, not_wave_sensitive, already_reported, path_too_long,
	// size_limit, open_failed, write_failed or written (file persistence only).
	[[nodiscard]] const char* Report(const char* prefix, const char* program_directory, const GraphicsSkippedGeState& state,
	                                 const ShaderNativeWaveInfo& wave, uint32_t requested_subgroup_size,
	                                 const GraphicsNativeWaveDrawInfo& draw = {});

private:
	DiagnosticDumpWriter&            m_writer;
	GraphicsNativeWaveProgramCapture m_capture;
	void*                            m_capture_context;
	std::atomic_bool                 m_claimed {false};
};

// Call only after ShaderGetInputInfoVS, before pipeline admission. Opt-in
// KYTY_NATIVE_WAVE_REPORT=<prefix> creates <prefix>-native-wave-input.json once
// per process. KYTY_NATIVE_WAVE_SHADER_DUMP optionally names an existing private
// directory for exclusive native-wave-es.bin/native-wave-gs-back.bin copies,
// at most 256 KiB each. Refusal PC/reason are verbatim analyzer fields: PC zero
// can be unset and a null reason is not replaced with a pipeline diagnosis.
[[nodiscard]] const char* GraphicsReportNativeWaveInput(const GraphicsSkippedGeState& state, const ShaderNativeWaveInfo& wave,
                                                       uint32_t requested_subgroup_size,
                                                       const GraphicsNativeWaveDrawInfo& draw = {});

// Both trails are copied before any report serialization or filesystem access.
// They use separate short critical sections; this is not an atomic GPU snapshot.
struct VulkanFaultSnapshot
{
	VulkanSubmitAttemptSnapshot                              submits;
	std::array<VulkanRecentDraw, VulkanRecentDrawTrail::CAPACITY_MAX> draws {};
	uint32_t                                                draw_capacity = 0;
	uint32_t                                                draw_count    = 0;
	uint64_t                                                draw_dropped  = 0;
	GraphicsSkippedDrawCounts                               skipped;
};

[[nodiscard]] bool VulkanLatchFaultSnapshot(VulkanSubmitAttemptTrail* submits, VulkanRecentDrawTrail* draws, VkResult result,
                                           VulkanFaultSnapshot* snapshot);

// Counts every guest draw the renderer does not emit and publishes the first of
// each reason as a native Warn event, independent of PrintfDirection. detail is
// bounded by the event message size and may be null.
void GraphicsRecordSkippedDraw(GraphicsSkippedDrawReason reason, const char* detail, const GraphicsSkippedGeState* ge_state = nullptr);
[[nodiscard]] GraphicsSkippedDrawCounts GraphicsGetSkippedDrawCounts();

// Bounded JSON for a recent-draw snapshot. Returns an empty string when the
// result would exceed RECENT_DRAW_REPORT_BYTES_MAX.
inline constexpr size_t RECENT_DRAW_REPORT_BYTES_MAX = 128u * 1024u;
[[nodiscard]] std::string VulkanRecentDrawReportJson(const VulkanRecentDrawFault& fault, uint32_t capacity,
                                                     const VulkanRecentDraw* draws, uint32_t count, uint64_t dropped,
                                                     const GraphicsSkippedDrawCounts& skipped = {});
// Writes the report with exclusive creation. Returns a status token:
// written, size_limit, open_failed or write_failed.
[[nodiscard]] const char* VulkanWriteRecentDrawReport(const char* path, const std::string& report);

// Opt-in: KYTY_RECENT_DRAW_REPORT=<path> with KYTY_SUBMIT_FAULT_TRACE=1;
// KYTY_RECENT_DRAW_CAPACITY selects 1..256 records (default 64). Null when disabled.
[[nodiscard]] VulkanRecentDrawTrail* VulkanRecentDrawTraceTrail();
// Records a command just emitted into buffer, joining its recording identity.
void VulkanRecentDrawRecord(const CommandBuffer* buffer, VulkanRecentDraw draw);
struct VulkanRecentDrawPacket
{
	bool     valid  = false;
	uint32_t pm4_op = 0;
	uint32_t pm4_dw = 0;
};
// Publishes the calling command-processor thread's current PM4 packet (the dword
// offset is relative to the active run, as in the submit-fault context) and
// returns the previous value so nested runs can restore it.
VulkanRecentDrawPacket VulkanRecentDrawSetPacket(VulkanRecentDrawPacket packet);

[[nodiscard]] bool                      VulkanSubmitFaultTraceEnabled();
[[nodiscard]] VulkanSubmitAttemptTrail* VulkanSubmitFaultTraceTrail();
// A command-buffer context identifies the failing caller, not a submit return
// or fence completion. Its kind/result/semaphore/completed fields are unused.
void VulkanSubmitFaultReport(const char* stage, VkResult result, const VulkanSubmitAttempt* immediate = nullptr,
                             const VulkanSubmitAttempt* command_buffer_context = nullptr);

class CommandBuffer
{
public:
	explicit CommandBuffer(int queue): m_queue(queue) { Allocate(); }
	virtual ~CommandBuffer() { Free(); }

	void              SetParent(CommandProcessor* parent) { m_parent = parent; }
	CommandProcessor* GetParent() { return m_parent; }

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const;

	void Allocate();
	void Free();
	void Begin() const;
	void End() const;
	void Execute();
	void ExecuteWithSemaphore(VkSemaphore signal_semaphore);
	void BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
	                     const VulkanSampleLocationState* sample_locations = nullptr) const;
	void EndRenderPass() const;
	void WaitForFence();
	void WaitForFenceWithoutLabelCallbacks();
	void WaitForFenceAndReset();
	void WaitForFenceAndResetWithoutLabelCallbacks();
	[[nodiscard]] bool TryCompleteFenceAndResetWithoutLabelCallbacks();

	[[nodiscard]] uint32_t GetIndex() const { return m_index; }
	VulkanCommandPool*     GetPool() { return m_pool; }
	[[nodiscard]] bool     IsExecute() const { return m_execute; }
	void                   SetSubmissionId(SubmissionId submission)
	{
		m_submission     = submission;
		m_has_submission = true;
	}
	[[nodiscard]] int GetQueueIndex() const { return m_queue; }
	[[nodiscard]] bool GetSubmissionId(SubmissionId* submission) const
	{
		if (!m_has_submission || submission == nullptr)
		{
			return false;
		}
		*submission = m_submission;
		return true;
	}
	void SetSubmitFaultContext(uint64_t guest_submit, uint32_t pm4_op, uint32_t pm4_dw)
	{
		m_has_guest_context = true;
		m_guest_submit      = guest_submit;
		m_pm4_op       = pm4_op;
		m_pm4_dw       = pm4_dw;
	}
	// Fills the recording identity (queue, slot, host submission) of a recent-draw record.
	void DescribeRecentDrawRecording(VulkanRecentDraw* draw) const
	{
		draw->queue                    = static_cast<uint32_t>(m_queue);
		draw->command_buffer_slot      = m_index;
		draw->has_host_submission      = m_has_submission;
		draw->host_submission_sequence = m_has_submission ? m_submission.sequence : 0u;
	}
	VulkanBuffer* UploadTransientBuffer(const void* data, uint64_t size, uint32_t usage);
	VulkanBuffer* CaptureTransientSnapshotBuffer(uint64_t vaddr, uint64_t size, uint32_t usage, uint64_t* validation_ns,
	                                             uint64_t* upload_ns, uint64_t* compare_ns, bool* reused);
	// Reusable scratch for commands recorded in this buffer. Callers must order
	// write/read/write hazards explicitly; lifetime extends through its fence.
	VulkanBuffer* AllocateTransientScratchBuffer(uint64_t size, uint32_t usage);

private:
	VulkanCommandPool* m_pool    = nullptr;
	uint32_t           m_index   = static_cast<uint32_t>(-1);
	int                m_queue   = -1;
	bool               m_execute = false;
	CommandProcessor*  m_parent  = nullptr;
	SubmissionId       m_submission;
	bool               m_has_submission = false;
	TransientBufferPool* m_transient_buffers = nullptr;
	TransientBufferPool* m_transient_scratch_buffers = nullptr;
	bool               m_has_guest_context = false;
	uint64_t           m_guest_submit = 0;
	uint32_t           m_pm4_op       = 0;
	uint32_t           m_pm4_dw       = 0;

	void WaitForFence(bool drain_label_callbacks, bool reset_command_buffer);
	[[nodiscard]] VulkanSubmitAttempt MakeSubmitAttempt(VulkanSubmitKind kind, bool signals_semaphore) const;
	void ReportSubmitFault(const char* stage, VkResult result) const;
	void MarkRecentDrawsSubmitCalled() const;
	void MarkRecentDrawsSubmitReturned(VkResult result) const;
	void MarkRecentDrawsFenceCompleted() const;
};

void GraphicsRenderInit();
void GraphicsRenderCreateContext();

void GraphicsRenderDrawIndex(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                             uint32_t index_type_and_size, uint32_t index_count, const void* index_addr, uint64_t draw_modifier,
                             uint32_t type, uint32_t instance_count, int32_t vertex_offset_add, uint32_t first_instance);
void GraphicsRenderDrawIndexAuto(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
	                             uint32_t index_count, uint64_t draw_modifier, uint32_t instance_count);
void GraphicsRenderWriteAtEndOfPipe64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value);
void GraphicsRenderWriteAtEndOfPipeClockCounter(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr);
void GraphicsRenderWriteAtEndOfPipe32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value);
void GraphicsRenderWriteAtEndOfPipeGds32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t dw_offset,
                                         uint32_t dw_num);
void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBackFlip32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                                                                uint32_t value, int handle, int index, int flip_mode, int64_t flip_arg);
void GraphicsRenderWriteAtEndOfPipeWithFlip32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value, int handle,
                                              int index, int flip_mode, int64_t flip_arg);
void GraphicsRenderWriteAtEndOfPipeOnlyFlip(uint64_t submit_id, CommandBuffer* buffer, int handle, int index, int flip_mode,
                                            int64_t flip_arg);
void GraphicsRenderWriteAtEndOfPipeWithWriteBack64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value);
// The interrupt variants deliver interrupt_context_id (the ReleaseMem
// interrupt context id) as the graphics event data at completion.
void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr,
                                                            uint64_t value, uint32_t interrupt_context_id);
void GraphicsRenderWriteAtEndOfPipeWithInterrupt64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value,
                                                   uint32_t interrupt_context_id);
void GraphicsRenderWriteAtEndOfPipeWithInterrupt32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value,
                                                   uint32_t interrupt_context_id);
// Records completion of a driver graphics submission. The notification is
// delivered only after the containing command buffer fence has completed.
void GraphicsRenderQueueQueuedGraphicsInterrupt(CommandBuffer* buffer);
// Records a completion-only write-back action in the current command buffer.
// The caller submits after releasing its CommandProcessor mutex so publication
// cannot precede GPU -> CPU materialization.
void GraphicsRenderPrepareWriteBack(CommandBuffer* buffer);
enum class ComputeDispatchResult
{
	Completed,
	ProcessorWriteBackRequired,
	SubmissionCompletionRequired,
};
// Preparation and recording share one resource analysis. A required write-back
// leaves the command buffer untouched; retry after completing it with fresh inputs.
[[nodiscard]] ComputeDispatchResult GraphicsRenderDispatchDirect(
    uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::Shader* sh_ctx, uint32_t thread_group_x,
    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode, bool processor_writeback_complete,
    SubmissionId* pending_writeback);
void GraphicsRenderMemoryBarrier(CommandBuffer* buffer);
void GraphicsRenderRenderTextureBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size);
void GraphicsRenderDepthStencilBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size);
void GraphicsRenderMemoryFree(uint64_t vaddr, uint64_t size);
void GraphicsRenderDeleteIndexBuffers();
void GraphicsRenderMemoryFlush(uint64_t vaddr, uint64_t size);

// Scratch: dump remembered KYTY_DUMP_RT color targets (paired with VideoOut frame dumps).
void GraphicsDumpRememberedRts(GraphicContext* ctx, const char* path_prefix);
// Opt-in TRACE: one-shot B10G11R11 + depth pixel stats after a present capture.
void GraphicsPeekRememberedSceneTargets(GraphicContext* ctx);

void DeleteFramebuffer(VideoOutVulkanImage* image);
void DeleteFramebuffer(DepthStencilVulkanImage* image);
void DeleteFramebuffer(RenderTextureVulkanImage* image);
void DeleteDescriptor(VulkanBuffer* buffer);
void DeleteDescriptor(TextureVulkanImage* image);
void DeleteDescriptor(StorageTextureVulkanImage* image);
void DeleteDescriptor(RenderTextureVulkanImage* image);

int GraphicsRenderAddEqEvent(Kernel::EventQueue::KernelEqueue eq, int id, void* udata);
int GraphicsRenderDeleteEqEvent(Kernel::EventQueue::KernelEqueue eq, int id);

// GDS transfers are recorded into a command-processor recording at its stream position, so they
// are ordered behind earlier work on that queue and ahead of later work. Anything but Recorded
// recorded nothing. Empty in-window spans record nothing and report Recorded.
enum class GraphicsGdsTransferResult : uint8_t
{
	Recorded,
	// A GDS span is outside the guest window, or a same-buffer copy overlaps.
	InvalidRange,
	// The guest source is unaligned or not a registered GPU mapping.
	InvalidSource,
	// The guest destination is not a registered GPU mapping.
	InvalidDestination,
	// Publication staging is exhausted: complete earlier submissions, then retry.
	StagingBudgetExhausted,
	// The recording owns the source in another form: submit and wait, then retry.
	ProcessorWriteBackRequired,
	// Another queue's incomplete submission owns the source: wait for it, then retry.
	SubmissionCompletionRequired,
	// A texture-type writer owns the source; no byte conversion is established.
	UnsupportedSource,
};

[[nodiscard]] GraphicsGdsTransferResult GraphicsRenderClearGds(CommandBuffer* buffer, uint64_t dw_offset, uint64_t dw_count,
                                                               uint32_t clear_value);
// The source is read where it is current: the guest bytes, or the device buffer whose writes on
// the recording queue are not yet written back.
[[nodiscard]] GraphicsGdsTransferResult GraphicsRenderWriteGdsFromMemory(CommandBuffer* buffer, uint64_t dw_offset, uint64_t src_vaddr,
                                                                         uint64_t dw_count, SubmissionId* dependency);
[[nodiscard]] GraphicsGdsTransferResult GraphicsRenderCopyGds(CommandBuffer* buffer, uint64_t src_dw_offset, uint64_t dst_dw_offset,
                                                              uint64_t dw_count);
// Publishes a GDS span to guest memory when the recording's submission is published.
[[nodiscard]] GraphicsGdsTransferResult GraphicsRenderReadGds(CommandBuffer* buffer, uint32_t* dst, uint64_t dw_offset, uint64_t dw_count);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
