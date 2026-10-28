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
	Split64On32,
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

// Native wave32 runs one host invocation per guest lane in whole 32-lane
// subgroups: the guest group is laid out linearly along X, so its trailing
// wave may be partial and the padding lanes start with EXEC clear.
[[nodiscard]] bool ShaderNativeWave32PhysicalLocal(const uint32_t (&guest_local)[3], uint32_t (&physical_local)[3]) noexcept;

// Workgroup dwords each split guest wave reserves to exchange wave-wide values
// between its host subgroups of <part_lanes> lanes: a sequence and two words
// per subgroup, and with 16-lane subgroups one dword per guest lane for the
// lane exchanges that cross them.
[[nodiscard]] constexpr uint32_t ShaderSplitWaveExchangeDwords(uint32_t part_lanes) noexcept
{
	return 64u / part_lanes * 3u + (part_lanes == 16u ? 64u : 0u);
}

// Host subgroup width of each split guest wave's parts: 16 lanes where the host
// allows exact width 16, else 32. At width 16 each guest value takes half the
// host registers; on Intel a large kernel at width 32 spends minutes in
// register allocation and spills, at width 16 seconds.
[[nodiscard]] uint32_t ShaderSplitWavePartLanes(const ShaderComputeWaveCapabilities& capabilities) noexcept;

// Split wave64 runs each guest wave as whole host subgroups of 16 lanes where
// the host allows exact width 16, else of 32 lanes (SubgroupId P*w .. P*w+P-1
// with P = 64 / lanes); the workgroup keeps guest lane order, padded to whole
// guest waves.
[[nodiscard]] ShaderComputeWaveLayoutStatus ShaderBuildSplitComputeWaveLayout(const ShaderComputeWaveRequest&      request,
                                                                              const ShaderComputeWaveCapabilities& capabilities,
                                                                              ShaderComputeWaveLayout*             layout);

} // namespace Kyty::Libs::Graphics
