#pragma once

#include <cstdint>

namespace Kyty::Libs::Graphics {

enum class ShaderGuestLaneOrder
{
	Unverified,
	LinearXFirst,
};

enum class ShaderComputeWaveStrategy
{
	Native,
	Paired64On32,
};

enum class ShaderComputeWaveLayoutStatus
{
	Supported,
	NoWork,
	UnsupportedWidth,
	UnsupportedDispatchMode,
	InvalidLocalSize,
	UnverifiedLaneOrder,
	MissingHostCapability,
	HostLimitExceeded,
	InvalidArgument,
};

struct ShaderComputeWaveRequest
{
	uint32_t             local[3];
	uint32_t             groups[3];
	uint32_t             dispatch_mode;
	uint32_t             lds_dwords;
	ShaderGuestLaneOrder lane_order;
};

struct ShaderComputeWaveCapabilities
{
	bool     size_control_enabled;
	bool     full_subgroups_enabled;
	bool     compute_required_size_supported;
	bool     compute_ballot_shuffle_supported;
	uint32_t min_subgroup_size;
	uint32_t max_subgroup_size;
	uint32_t max_local_size[3];
	uint32_t max_group_count[3];
	uint32_t max_invocations;
	uint32_t max_subgroups;
	uint32_t max_shared_bytes;
};

struct ShaderComputeWaveLayout
{
	ShaderComputeWaveStrategy strategy = ShaderComputeWaveStrategy::Native;
	uint32_t                  guest_local[3] {};
	uint32_t                  physical_local[3] {};
	uint32_t                  guest_wave_size      = 0;
	uint32_t                  native_subgroup_size = 0;
	uint32_t                  banks                = 1;
	uint32_t                  waves                = 0;
	uint32_t                  lds_dwords           = 0;
};

[[nodiscard]] ShaderComputeWaveLayoutStatus ShaderBuildPairedComputeWaveLayout(const ShaderComputeWaveRequest&      request,
                                                                               const ShaderComputeWaveCapabilities& capabilities,
                                                                               ShaderComputeWaveLayout*             layout);

} // namespace Kyty::Libs::Graphics
