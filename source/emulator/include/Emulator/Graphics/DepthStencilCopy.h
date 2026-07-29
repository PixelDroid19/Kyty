#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILCOPY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILCOPY_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Shader.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// A fixed-function depth/stencil expansion may still require the guest vertex
// stage to determine its covered pixels. The fragment stage remains internal
// and expands the depth and stencil planes into the color target.
struct DepthStencilCopyVertexStage
{
	const ShaderVertexInputInfo* input                 = nullptr;
	const ShaderBindResources*   bind                  = nullptr;
	const ShaderId*              shader_id             = nullptr;
	const uint32_t*              shader_words          = nullptr;
	size_t                       shader_word_count     = 0;
	VkDescriptorSetLayout        descriptor_set_layout = nullptr;
	VkPrimitiveTopology          topology              = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	bool                         cull_front            = false;
	bool                         cull_back             = false;
	bool                         face                  = false;
	bool                         dx_clip_space         = true;
};

struct DepthStencilCopyDepthTest
{
	bool        enabled      = false;
	bool        write_enable = false;
	VkCompareOp compare_op    = VK_COMPARE_OP_ALWAYS;
};

struct DepthStencilCopyStencilFace
{
	VkStencilOp fail_op       = VK_STENCIL_OP_KEEP;
	VkStencilOp pass_op       = VK_STENCIL_OP_KEEP;
	VkStencilOp depth_fail_op = VK_STENCIL_OP_KEEP;
	VkCompareOp compare_op     = VK_COMPARE_OP_ALWAYS;
	uint32_t    compare_mask   = 0;
	uint32_t    write_mask     = 0;
	uint32_t    reference      = 0;
};

struct DepthStencilCopyStencilTest
{
	bool                        enabled = false;
	DepthStencilCopyStencilFace front;
	DepthStencilCopyStencilFace back;
};

enum class DepthStencilCopyMode
{
	ExpandToColor,
	DepthStencilOnly,
};

struct DepthStencilCopyRequest
{
	DepthStencilCopyMode       mode             = DepthStencilCopyMode::ExpandToColor;
	DepthStencilVulkanImage*   source           = nullptr;
	VkRenderPass               render_pass      = nullptr;
	uint64_t                   render_pass_id   = 0;
	VkExtent2D                 extent           = {};
	VkViewport                 viewport         = {};
	VkRect2D                   scissor          = {};
	uint8_t                    color_write_mask = 0;
	DepthStencilCopyDepthTest   depth_test;
	DepthStencilCopyStencilTest stencil_test;
	const DepthStencilCopyVertexStage* vertex_stage = nullptr;
};

struct DepthStencilCopyPreparedDraw
{
	VkPipeline       pipeline          = nullptr;
	VkPipelineLayout pipeline_layout   = nullptr;
	VkDescriptorSet  source_descriptor = nullptr;
	VkViewport       viewport          = {};
	VkRect2D         scissor           = {};
	float            source_scale[2]  = {};
};

// Executes fixed-function depth/stencil copy work. It either expands source
// planes into a color attachment or performs the matching depth/stencil-only
// draw. The caller owns render-pass lifetime and releases its source and pass
// keys before the corresponding Vulkan resources are destroyed.
class DepthStencilCopyRenderer
{
public:
	DepthStencilCopyRenderer() = default;
	~DepthStencilCopyRenderer() = default;
	KYTY_CLASS_NO_COPY(DepthStencilCopyRenderer);

	[[nodiscard]] DepthStencilCopyPreparedDraw PrepareDraw(GraphicContext* context, const DepthStencilCopyRequest& request);
	void BindPreparedDraw(VkCommandBuffer command_buffer, const DepthStencilCopyPreparedDraw& draw) const;
	void ReleaseSource(GraphicContext* context, uint64_t source_id);
	void ReleaseRenderPass(GraphicContext* context, uint64_t render_pass_id);

private:
	struct SourceDescriptor
	{
		uint64_t        source_id  = 0;
		VkDescriptorSet descriptor = nullptr;
	};

	struct RenderPipeline
	{
		uint64_t              render_pass_id              = 0;
		ShaderId              vertex_shader_id;
		VkDescriptorSetLayout vertex_descriptor_set_layout = nullptr;
		VkPrimitiveTopology   topology                     = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		bool                  guest_vertex_stage           = false;
		bool                  cull_front                   = false;
		bool                  cull_back                    = false;
		bool                  face                         = false;
		bool                  dx_clip_space                = true;
		DepthStencilCopyMode  mode                         = DepthStencilCopyMode::ExpandToColor;
		uint8_t               color_write_mask             = 0;
		bool                  depth_test_enable            = false;
		bool                  depth_write_enable           = false;
		VkCompareOp           depth_compare_op              = VK_COMPARE_OP_ALWAYS;
		DepthStencilCopyStencilTest stencil_test;
		uint32_t              vertex_push_constant_offset  = 0;
		uint32_t              vertex_push_constant_size    = 0;
		bool                  vertex_uses_uniform_buffer   = false;
		VkPipeline            pipeline                     = nullptr;
		VkPipelineLayout      pipeline_layout              = nullptr;
	};

	void            Initialize(GraphicContext* context);
	VkDescriptorSet GetSourceDescriptor(DepthStencilVulkanImage* source);
	RenderPipeline* GetRenderPipeline(GraphicContext* context, const DepthStencilCopyRequest& request);
	void            DestroyRenderPipeline(RenderPipeline* pipeline);

	VkDevice              m_device                       = nullptr;
	VkSampler             m_sampler                      = nullptr;
	VkDescriptorPool      m_descriptor_pool              = nullptr;
	VkDescriptorSetLayout m_empty_descriptor_set_layout  = nullptr;
	VkDescriptorSetLayout m_source_descriptor_set_layout = nullptr;
	std::vector<SourceDescriptor> m_sources;
	std::vector<RenderPipeline>   m_pipelines;
	uint32_t                      m_evict_cursor = 0;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILCOPY_H_
