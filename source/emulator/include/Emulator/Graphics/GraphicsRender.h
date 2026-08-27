#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/GpuSubmissionTracker.h"
#include "Emulator/Kernel/EventQueue.h"

#include <vulkan/vulkan_core.h>

#include <array>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

class CommandProcessor;
class TransientBufferPool;
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
	uint64_t host_submission_sequence = 0;
	uint64_t guest_submit            = 0;
	VulkanSubmitKind kind            = VulkanSubmitKind::CommandBuffer;
	uint32_t queue                   = 0;
	uint32_t command_buffer_slot     = 0;
	int32_t  frame                   = 0;
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

[[nodiscard]] bool                      VulkanSubmitFaultTraceEnabled();
[[nodiscard]] VulkanSubmitAttemptTrail* VulkanSubmitFaultTraceTrail();
void VulkanSubmitFaultReport(const char* stage, VkResult result, const VulkanSubmitAttempt* immediate = nullptr);

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
		m_guest_submit = guest_submit;
		m_pm4_op       = pm4_op;
		m_pm4_dw       = pm4_dw;
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
	uint64_t           m_guest_submit = 0;
	uint32_t           m_pm4_op       = 0;
	uint32_t           m_pm4_dw       = 0;

	void WaitForFence(bool drain_label_callbacks, bool reset_command_buffer);
	[[nodiscard]] VulkanSubmitAttempt MakeSubmitAttempt(VulkanSubmitKind kind, bool signals_semaphore) const;
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
void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr,
                                                            uint64_t value);
void GraphicsRenderWriteAtEndOfPipeWithInterrupt64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value);
void GraphicsRenderWriteAtEndOfPipeWithInterrupt32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value);
// Records completion of a driver graphics submission. The notification is
// delivered only after the containing command buffer fence has completed.
void GraphicsRenderQueueQueuedGraphicsInterrupt(CommandBuffer* buffer);
// Records a completion-only write-back action in the current command buffer.
// The caller submits after releasing its CommandProcessor mutex so publication
// cannot precede GPU -> CPU materialization.
void GraphicsRenderPrepareWriteBack(CommandBuffer* buffer);
void GraphicsRenderDispatchDirect(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::Shader* sh_ctx, uint32_t thread_group_x,
                                  uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode);
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

void GraphicsRenderClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
void GraphicsRenderReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
