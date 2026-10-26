#pragma once

#include "Emulator/Graphics/FragmentTransportLayout.h"
#include "Emulator/Graphics/GraphicsRender.h"

namespace Kyty::Libs::Graphics {

struct CommandBuffer;
struct PipelineDynamicParameters;
struct PipelineStaticParameters;
struct ShaderBindResources;
struct ShaderPixelInputInfo;
struct VulkanBuffer;
struct VulkanPipeline;

// Host-owned pipelines of one transported draw state. The resolve pipeline is
// the VulkanPipeline the pipeline cache returns; capture and shade are owned by
// the same cache entry.
struct FragmentTransportPasses
{
	VulkanPipeline*            capture        = nullptr;
	VulkanPipeline*            resolve        = nullptr;
	VkPipeline                 shade          = VK_NULL_HANDLE;
	VkPipelineLayout           shade_layout   = VK_NULL_HANDLE;
	PipelineStaticParameters*  capture_static = nullptr;
	PipelineDynamicParameters* capture_dynamic = nullptr;
	uint32_t                   lane_words     = 0;
	uint32_t                   initial_vgpr   = 0;
};

// Per-draw host transport storage. Every buffer is device-local and lives until
// the recording's fence signals, so tiles of one draw reuse them in order.
struct FragmentTransportResources
{
	FragmentTransport::Layout layout {};
	VulkanBuffer*             records     = nullptr;
	VulkanBuffer*             control     = nullptr;
	VulkanBuffer*             references  = nullptr;
	VulkanBuffer*             wave_input  = nullptr;
	VulkanBuffer*             wave_output = nullptr;
	VkDescriptorSet           set         = VK_NULL_HANDLE;
	uint32_t                  band_height = 0;
	uint64_t                  control_offset      = 0;
	uint64_t                  control_bytes       = 0;
	uint64_t                  references_offset   = 0;
	uint64_t                  references_bytes    = 0;
	VkExtent2D                extent {};
};

// The layout shared by every transport pipeline: bindings 0..4 are the host
// storage buffers. Scan and pack declare them at set 0; capture, resolve and
// the split shade pass at set 2.
[[nodiscard]] VkDescriptorSetLayout FragmentTransportDescriptorSetLayout();

[[nodiscard]] bool FragmentTransportPrepareResources(CommandBuffer* buffer, VkExtent2D extent, uint32_t primitive_count,
                                                     uint32_t lane_words, FragmentTransportResources* resources);
// Invalidates the per-tile control structures and orders them before the
// capture draw.
void FragmentTransportBeginTile(CommandBuffer* buffer, const FragmentTransportResources& resources);
// Push constants and the scan and pack dispatches, ordered after the capture
// draw. The shade dispatch follows with the guest pixel descriptors.
void FragmentTransportDispatchScanPack(CommandBuffer* buffer, const FragmentTransportResources& resources);
void FragmentTransportRecordShade(CommandBuffer* buffer, const FragmentTransportResources& resources,
                                  const FragmentTransportPasses& passes, const ShaderBindResources& ps_bind,
                                  const ShaderPixelInputInfo& pixel);
// Orders the tile's shader writes before the resolve pass and the next tile.
void FragmentTransportEndTile(CommandBuffer* buffer);

void FragmentTransportDestroyPasses(FragmentTransportPasses* passes);

// Host kernels shared by every context.
[[nodiscard]] VkPipeline FragmentTransportScanPipeline();
[[nodiscard]] VkPipeline FragmentTransportPackPipeline();

} // namespace Kyty::Libs::Graphics
