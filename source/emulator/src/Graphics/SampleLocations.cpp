#include "Emulator/Graphics/SampleLocations.h"

#include <cmath>

namespace Kyty::Libs::Graphics {

namespace {

[[nodiscard]] bool IsSupportedSampleCount(VkSampleCountFlagBits sample_count)
{
	switch (sample_count)
	{
		case VK_SAMPLE_COUNT_1_BIT:
		case VK_SAMPLE_COUNT_2_BIT:
		case VK_SAMPLE_COUNT_4_BIT:
		case VK_SAMPLE_COUNT_8_BIT:
		case VK_SAMPLE_COUNT_16_BIT: return true;
		default: return false;
	}
}

[[nodiscard]] int8_t DecodeSignedNibble(uint32_t value)
{
	const auto nibble = static_cast<int8_t>(value & 0x0fu);
	return static_cast<int8_t>((nibble & 0x08) != 0 ? nibble - 16 : nibble);
}

[[nodiscard]] uint32_t DecodeSampleCount(VkSampleCountFlagBits sample_count)
{
	return static_cast<uint32_t>(sample_count);
}

[[nodiscard]] bool HasCustomGuestState(const uint32_t guest_locations[16], uint32_t sample_count, uint64_t centroid_priority)
{
	if (centroid_priority != 0)
	{
		return true;
	}

	for (uint32_t cell = 0; cell < kVulkanSampleLocationMaxGridCells; cell++)
	{
		for (uint32_t sample = 0; sample < sample_count; sample++)
		{
			const uint32_t word_index = cell * 4u + sample / 4u;
			if (guest_locations[word_index] != 0)
			{
				return true;
			}
		}
	}

	return false;
}

[[nodiscard]] bool IsCoordinateSupported(int8_t offset, const VulkanSampleLocationCapabilities& capabilities)
{
	const float coordinate = 0.5f + static_cast<float>(offset) / 16.0f;
	return std::isfinite(capabilities.coordinate_range[0]) && std::isfinite(capabilities.coordinate_range[1]) &&
	       coordinate >= capabilities.coordinate_range[0] && coordinate <= capabilities.coordinate_range[1];
}

[[nodiscard]] bool IsCanonicalCentroidPriority(const VulkanSampleLocationState& state, uint64_t centroid_priority)
{
	if (centroid_priority == 0)
	{
		return true;
	}

	const uint32_t sample_count                           = DecodeSampleCount(state.sample_count);
	uint32_t       order[kVulkanSampleLocationMaxSamples] = {};
	for (uint32_t sample = 0; sample < sample_count; sample++)
	{
		order[sample] = sample;
	}

	for (uint32_t index = 1; index < sample_count; index++)
	{
		const auto candidate = order[index];
		uint32_t   position  = index;
		while (position > 0)
		{
			const auto previous           = order[position - 1];
			const auto candidate_distance = static_cast<int>(state.offsets[candidate][0]) * state.offsets[candidate][0] +
			                                static_cast<int>(state.offsets[candidate][1]) * state.offsets[candidate][1];
			const auto previous_distance  = static_cast<int>(state.offsets[previous][0]) * state.offsets[previous][0] +
			                                static_cast<int>(state.offsets[previous][1]) * state.offsets[previous][1];
			if (previous_distance < candidate_distance || (previous_distance == candidate_distance && previous < candidate))
			{
				break;
			}
			order[position] = previous;
			position--;
		}
		order[position] = candidate;
	}

	for (uint32_t priority = 0; priority < sample_count; priority++)
	{
		if ((centroid_priority & 0x0fu) != order[priority])
		{
			return false;
		}
		centroid_priority >>= 4u;
	}

	return centroid_priority == 0;
}

} // namespace

VulkanSampleLocationStatus BuildVulkanSampleLocationState(const uint32_t guest_locations[16], uint64_t centroid_priority,
                                                          VkSampleCountFlagBits                   sample_count,
                                                          const VulkanSampleLocationCapabilities& capabilities,
                                                          VulkanSampleLocationState*              state)
{
	if (guest_locations == nullptr || state == nullptr || !IsSupportedSampleCount(sample_count))
	{
		return VulkanSampleLocationStatus::InvalidArgument;
	}

	*state              = {};
	state->sample_count = sample_count;
	state->grid_size    = {1, 1};

	const uint32_t samples = DecodeSampleCount(sample_count);
	if (sample_count == VK_SAMPLE_COUNT_1_BIT || !HasCustomGuestState(guest_locations, samples, centroid_priority))
	{
		return VulkanSampleLocationStatus::Success;
	}
	if (capabilities.extension_enabled == 0)
	{
		return VulkanSampleLocationStatus::UnsupportedExtension;
	}
	if ((capabilities.sample_counts & sample_count) == 0)
	{
		return VulkanSampleLocationStatus::UnsupportedSampleCount;
	}
	if (capabilities.max_grid_size.width == 0 || capabilities.max_grid_size.height == 0)
	{
		return VulkanSampleLocationStatus::UnsupportedGrid;
	}
	if (capabilities.subpixel_bits < 4)
	{
		return VulkanSampleLocationStatus::UnsupportedPrecision;
	}

	int8_t guest_offsets[kVulkanSampleLocationMaxGridCells][kVulkanSampleLocationMaxSamples][2] = {};
	for (uint32_t cell = 0; cell < kVulkanSampleLocationMaxGridCells; cell++)
	{
		for (uint32_t sample = 0; sample < samples; sample++)
		{
			const uint32_t word_index = cell * 4u + sample / 4u;
			const uint32_t lane_shift = (sample % 4u) * 8u;
			const uint32_t encoded    = (guest_locations[word_index] >> lane_shift) & 0xffu;
			const auto     x          = DecodeSignedNibble(encoded);
			const auto     y          = DecodeSignedNibble(encoded >> 4u);
			if (!IsCoordinateSupported(x, capabilities) || !IsCoordinateSupported(y, capabilities))
			{
				return VulkanSampleLocationStatus::UnsupportedCoordinateRange;
			}
			guest_offsets[cell][sample][0] = x;
			guest_offsets[cell][sample][1] = y;
		}
	}

	bool uniform_grid = true;
	for (uint32_t cell = 1; cell < kVulkanSampleLocationMaxGridCells && uniform_grid; cell++)
	{
		for (uint32_t sample = 0; sample < samples; sample++)
		{
			if (guest_offsets[cell][sample][0] != guest_offsets[0][sample][0] ||
			    guest_offsets[cell][sample][1] != guest_offsets[0][sample][1])
			{
				uniform_grid = false;
				break;
			}
		}
	}

	if (uniform_grid)
	{
		state->grid_size      = {1, 1};
		state->location_count = samples;
		for (uint32_t sample = 0; sample < samples; sample++)
		{
			state->offsets[sample][0] = guest_offsets[0][sample][0];
			state->offsets[sample][1] = guest_offsets[0][sample][1];
		}
	} else
	{
		if (capabilities.max_grid_size.width < 2 || capabilities.max_grid_size.height < 2 || centroid_priority != 0)
		{
			return (centroid_priority != 0 ? VulkanSampleLocationStatus::UnsupportedCentroidPriority
			                               : VulkanSampleLocationStatus::UnsupportedGrid);
		}
		state->grid_size      = {2, 2};
		state->location_count = samples * kVulkanSampleLocationMaxGridCells;
		for (uint32_t cell = 0; cell < kVulkanSampleLocationMaxGridCells; cell++)
		{
			for (uint32_t sample = 0; sample < samples; sample++)
			{
				const uint32_t index     = cell * samples + sample;
				state->offsets[index][0] = guest_offsets[cell][sample][0];
				state->offsets[index][1] = guest_offsets[cell][sample][1];
			}
		}
	}

	if (!IsCanonicalCentroidPriority(*state, centroid_priority))
	{
		return VulkanSampleLocationStatus::UnsupportedCentroidPriority;
	}

	state->enabled = 1;
	return VulkanSampleLocationStatus::Success;
}

bool VulkanSampleLocationsEnabled(const VulkanSampleLocationState& state)
{
	return state.enabled != 0;
}

bool VulkanSampleLocationsEqual(const VulkanSampleLocationState& lhs, const VulkanSampleLocationState& rhs)
{
	if (lhs.sample_count != rhs.sample_count || lhs.grid_size.width != rhs.grid_size.width ||
	    lhs.grid_size.height != rhs.grid_size.height || lhs.location_count != rhs.location_count || lhs.enabled != rhs.enabled)
	{
		return false;
	}
	for (uint32_t index = 0; index < kVulkanSampleLocationMaxCount; index++)
	{
		if (lhs.offsets[index][0] != rhs.offsets[index][0] || lhs.offsets[index][1] != rhs.offsets[index][1])
		{
			return false;
		}
	}
	return true;
}

bool VulkanSampleLocationsPopulateInfo(const VulkanSampleLocationState& state, VkSampleLocationEXT locations[kVulkanSampleLocationMaxCount],
                                       VkSampleLocationsInfoEXT* info)
{
	if (locations == nullptr || info == nullptr || !VulkanSampleLocationsEnabled(state) || state.location_count == 0 ||
	    state.location_count > kVulkanSampleLocationMaxCount)
	{
		return false;
	}

	for (uint32_t index = 0; index < state.location_count; index++)
	{
		locations[index].x = 0.5f + static_cast<float>(state.offsets[index][0]) / 16.0f;
		locations[index].y = 0.5f + static_cast<float>(state.offsets[index][1]) / 16.0f;
	}

	*info                         = {};
	info->sType                   = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT;
	info->sampleLocationsPerPixel = state.sample_count;
	info->sampleLocationGridSize  = state.grid_size;
	info->sampleLocationsCount    = state.location_count;
	info->pSampleLocations        = locations;
	return true;
}

const char* VulkanSampleLocationStatusName(VulkanSampleLocationStatus status)
{
	switch (status)
	{
		case VulkanSampleLocationStatus::Success: return "success";
		case VulkanSampleLocationStatus::InvalidArgument: return "invalid_argument";
		case VulkanSampleLocationStatus::UnsupportedExtension: return "unsupported_extension";
		case VulkanSampleLocationStatus::UnsupportedSampleCount: return "unsupported_sample_count";
		case VulkanSampleLocationStatus::UnsupportedGrid: return "unsupported_grid";
		case VulkanSampleLocationStatus::UnsupportedCoordinateRange: return "unsupported_coordinate_range";
		case VulkanSampleLocationStatus::UnsupportedPrecision: return "unsupported_precision";
		case VulkanSampleLocationStatus::UnsupportedCentroidPriority: return "unsupported_centroid_priority";
	}
	return "unknown";
}

} // namespace Kyty::Libs::Graphics
