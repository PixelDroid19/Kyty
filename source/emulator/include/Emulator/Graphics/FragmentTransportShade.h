#pragma once

#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Shader.h"

#include <vulkan/vulkan.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Builds the split-Wave64 compute module that shades captured pixel records.
// The module reads ShaderFragmentComputeInfo's lane records and writes its
// 34-word result lane; the caller supplies the guest PS resource SGPR.
[[nodiscard]] bool FragmentTransportBuildShadeModule(const ShaderCode& code, const ShaderPixelInputInfo& pixel,
                                                     uint32_t user_sgpr_count, uint32_t lds_dwords, Vector<uint32_t>* binary,
                                                     String8* error);

// Lane record shape shared by the capture reader and the shade prolog.
[[nodiscard]] uint32_t FragmentTransportInitialVgprs(const ShaderPixelInputInfo& pixel);
[[nodiscard]] uint32_t FragmentTransportLaneWords(const ShaderCode& code, const ShaderPixelInputInfo& pixel);

// Creates the split compute pipeline over the guest pixel resources and the
// transport storage set at index two. The caller owns and destroys both handles.
[[nodiscard]] bool FragmentTransportCreateShadePipeline(VkDevice device, VkPipelineCache pipeline_cache,
                                                        const ShaderBindResources& ps_bind, const Vector<uint32_t>& binary,
                                                        bool require_subgroup_size, VkDescriptorSetLayout transport_layout,
                                                        VkPipelineLayout* layout, VkPipeline* pipeline);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
