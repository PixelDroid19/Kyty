#pragma once

#include <cstddef>
#include <cstdint>

namespace Kyty::Libs::Graphics {

struct GraphicContext;
struct ShaderBindResources;

// Each entry describes one distinct stage. The optional probe set is shared by
// both graphics stages, but contributes only once to pipeline-wide set limits.
void ValidatePipelineDescriptorLimits(GraphicContext* context, const ShaderBindResources* const* stages,
                                      size_t stage_count, bool graphics_probe, uint32_t color_attachments = 0,
                                      uint32_t fragment_outputs = 0);

void ValidateDepthCopyDescriptorLimits(GraphicContext* context, const ShaderBindResources* vertex_bind, bool expand_to_color);

} // namespace Kyty::Libs::Graphics
