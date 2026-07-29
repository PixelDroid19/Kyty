#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SAMPLELOCATIONS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SAMPLELOCATIONS_H_

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace Kyty::Libs::Graphics {

// The guest encodes sample positions as signed four-bit offsets in units of
// one sixteenth of a pixel. Keep that integer representation in cache keys and
// only convert it to Vulkan coordinates while recording Vulkan structures.
constexpr uint32_t kVulkanSampleLocationMaxSamples   = 16;
constexpr uint32_t kVulkanSampleLocationMaxGridCells = 4;
constexpr uint32_t kVulkanSampleLocationMaxCount     = kVulkanSampleLocationMaxSamples * kVulkanSampleLocationMaxGridCells;

struct VulkanSampleLocationCapabilities
{
	uint32_t           extension_enabled   = 0;
	VkSampleCountFlags sample_counts       = 0;
	VkExtent2D         max_grid_size       = {0, 0};
	float              coordinate_range[2] = {};
	uint32_t           subpixel_bits       = 0;
	uint32_t           variable_locations  = 0;
};

struct VulkanSampleLocationState
{
	VkSampleCountFlagBits sample_count                              = VK_SAMPLE_COUNT_1_BIT;
	VkExtent2D            grid_size                                 = {1, 1};
	uint32_t              location_count                            = 0;
	uint32_t              enabled                                   = 0;
	int8_t                offsets[kVulkanSampleLocationMaxCount][2] = {};
};

enum class VulkanSampleLocationStatus : uint8_t
{
	Success,
	InvalidArgument,
	UnsupportedExtension,
	UnsupportedSampleCount,
	UnsupportedGrid,
	UnsupportedCoordinateRange,
	UnsupportedPrecision,
	UnsupportedCentroidPriority,
};

[[nodiscard]] VulkanSampleLocationStatus BuildVulkanSampleLocationState(const uint32_t guest_locations[16], uint64_t centroid_priority,
                                                                        VkSampleCountFlagBits                   sample_count,
                                                                        const VulkanSampleLocationCapabilities& capabilities,
                                                                        VulkanSampleLocationState*              state);

[[nodiscard]] bool        VulkanSampleLocationsEnabled(const VulkanSampleLocationState& state);
[[nodiscard]] bool        VulkanSampleLocationsEqual(const VulkanSampleLocationState& lhs, const VulkanSampleLocationState& rhs);
[[nodiscard]] bool        VulkanSampleLocationsPopulateInfo(const VulkanSampleLocationState& state,
                                                            VkSampleLocationEXT              locations[kVulkanSampleLocationMaxCount],
                                                            VkSampleLocationsInfoEXT*        info);
[[nodiscard]] const char* VulkanSampleLocationStatusName(VulkanSampleLocationStatus status);

} // namespace Kyty::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SAMPLELOCATIONS_H_
