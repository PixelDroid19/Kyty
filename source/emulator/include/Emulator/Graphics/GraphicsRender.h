#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/GpuSubmissionTracker.h"
#include "Emulator/Kernel/EventQueue.h"

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
	void ExecuteWithSemaphore();
	void BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth) const;
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
	VulkanBuffer* UploadTransientBuffer(const void* data, uint64_t size, uint32_t usage);

private:
	VulkanCommandPool* m_pool    = nullptr;
	uint32_t           m_index   = static_cast<uint32_t>(-1);
	int                m_queue   = -1;
	bool               m_execute = false;
	CommandProcessor*  m_parent  = nullptr;
	SubmissionId       m_submission;
	bool               m_has_submission = false;
	TransientBufferPool* m_transient_buffers = nullptr;

	void WaitForFence(bool drain_label_callbacks, bool reset_command_buffer);
};

void GraphicsRenderInit();
void GraphicsRenderCreateContext();

// Resolves the Gen5 rect-list auto-draw expansion without depending on Vulkan
// objects, so the guest primitive contract can be tested deterministically.
bool GraphicsResolveRectListAutoDraw(uint32_t primitive_type, uint32_t index_count, int vertex_buffers_num, uint32_t* vertex_count);

void GraphicsRenderDrawIndex(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                             uint32_t index_type_and_size, uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type);
void GraphicsRenderDrawIndexAuto(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                                 uint32_t index_count, uint32_t flags);
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

void DeleteFramebuffer(VideoOutVulkanImage* image);
void DeleteFramebuffer(DepthStencilVulkanImage* image);
void DeleteFramebuffer(RenderTextureVulkanImage* image);
void DeleteDescriptor(VulkanBuffer* buffer);
void DeleteDescriptor(TextureVulkanImage* image);
void DeleteDescriptor(StorageTextureVulkanImage* image);
void DeleteDescriptor(RenderTextureVulkanImage* image);

int GraphicsRenderAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata);
int GraphicsRenderDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id);

void GraphicsRenderClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
void GraphicsRenderReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
