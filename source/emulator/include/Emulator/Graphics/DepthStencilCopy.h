#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILCOPY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILCOPY_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Graphics/GraphicContext.h"

#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct DepthStencilCopyRequest
{
	DepthStencilVulkanImage* source         = nullptr;
	VkRenderPass             render_pass    = nullptr;
	uint64_t                 render_pass_id = 0;
	VkExtent2D               extent         = {};
};

// Expands fixed-function depth/stencil copy output into a color attachment.
// The caller owns render-pass lifetime and releases its source and pass keys
// before the corresponding Vulkan resources are destroyed.
class DepthStencilCopyRenderer
{
public:
	DepthStencilCopyRenderer() = default;
	~DepthStencilCopyRenderer() = default;
	KYTY_CLASS_NO_COPY(DepthStencilCopyRenderer);

	void Render(GraphicContext* context, VkCommandBuffer command_buffer, const DepthStencilCopyRequest& request);
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
		uint64_t   render_pass_id = 0;
		VkPipeline pipeline       = nullptr;
	};

	void            Initialize(GraphicContext* context);
	VkDescriptorSet GetSourceDescriptor(DepthStencilVulkanImage* source);
	VkPipeline      GetRenderPipeline(GraphicContext* context, VkRenderPass render_pass, uint64_t render_pass_id);

	VkDevice              m_device                = nullptr;
	VkSampler             m_sampler               = nullptr;
	VkDescriptorPool      m_descriptor_pool       = nullptr;
	VkDescriptorSetLayout m_descriptor_set_layout = nullptr;
	VkPipelineLayout      m_pipeline_layout       = nullptr;
	std::vector<SourceDescriptor> m_sources;
	std::vector<RenderPipeline>   m_pipelines;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILCOPY_H_
