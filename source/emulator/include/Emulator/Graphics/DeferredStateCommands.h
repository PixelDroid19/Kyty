#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEFERREDSTATECOMMANDS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEFERREDSTATECOMMANDS_H_

#include <vulkan/vulkan_core.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace Kyty::Libs::Graphics {

// The state binds, dynamic state and indexed draws of a draw's tail, encoded in recording
// order and replayed into one command buffer. Every argument is copied, so the caller's
// storage may change after the call.
class DeferredStateCommands
{
public:
	static constexpr uint32_t VERTEX_BUFFERS_MAX = 32;
	static constexpr uint32_t PUSH_CONSTANT_BYTES_MAX = 4096;

	[[nodiscard]] bool Empty() const { return m_bytes.empty(); }

	void BindPipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline);
	void BindIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
	void BindVertexBuffers(uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets);
	void BindDescriptorSet(VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t slot, VkDescriptorSet set);
	void PushConstants(VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void* values);
	void SetLineWidth(float width);
	void SetStencilCompareMask(VkStencilFaceFlags faces, uint32_t mask);
	void SetStencilWriteMask(VkStencilFaceFlags faces, uint32_t mask);
	void SetStencilReference(VkStencilFaceFlags faces, uint32_t reference);
	void SetColorWriteEnable(PFN_vkCmdSetColorWriteEnableEXT func, VkBool32 enable);
	void SetViewport(const VkViewport& viewport);
	void SetScissor(const VkRect2D& scissor);
	void SetBlendConstants(const float constants[4]);
	void SetDepthBias(float constant_factor, float clamp, float slope_factor);
	void DrawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset,
	                 uint32_t first_instance);
	void WriteTimestamp(VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query);
	// Moves every command of `other` to the end of this list and empties `other`.
	void Take(DeferredStateCommands* other);

	// Records every encoded command into `vk_buffer` in encoding order and empties the list.
	void Replay(VkCommandBuffer vk_buffer);
	void Clear() { m_bytes.clear(); }

private:
	template <typename T>
	void Append(uint8_t op, const T& args);
	void AppendBytes(const void* data, size_t size);

	std::vector<uint8_t> m_bytes;
};

// Records handed-off command lists on its own thread, in hand-off order, so the command processor
// goes on encoding meanwhile. One per command pool: nothing else may use the pool's command buffers
// until Drain returns.
class DeferredReplayWorker
{
public:
	DeferredReplayWorker();
	~DeferredReplayWorker();
	DeferredReplayWorker(const DeferredReplayWorker&)            = delete;
	DeferredReplayWorker& operator=(const DeferredReplayWorker&) = delete;

	// Takes every command of `commands`, all for `target`, without waiting for earlier ones.
	void HandOff(VkCommandBuffer target, DeferredStateCommands* commands);
	// Returns once every handed-off command is recorded.
	void Drain();

private:
	void Run();

	std::mutex              m_mutex;
	std::condition_variable m_wake;
	std::condition_variable m_idle;
	DeferredStateCommands   m_pending;
	VkCommandBuffer         m_pending_target = VK_NULL_HANDLE;
	DeferredStateCommands   m_running;
	std::atomic<bool>       m_has_pending {false};
	bool                    m_busy     = false;
	bool                    m_sleeping = false;
	bool                    m_stop     = false;
	std::thread             m_thread;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEFERREDSTATECOMMANDS_H_ */
