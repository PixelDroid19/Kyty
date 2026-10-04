#include "Emulator/Graphics/ShaderComputeWaveLayout.h"

#include <limits>

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint32_t kComputeShaderEnable = 0x0001u;
constexpr uint32_t kPartialThreadgroup  = 0x0002u;
constexpr uint32_t kForceStartAtZero    = 0x0004u;
constexpr uint32_t kOrderedAppendEnable = 0x0008u;
constexpr uint32_t kOrderedAppendMode   = 0x0010u;
constexpr uint32_t kUseThreadDimensions = 0x0020u;
constexpr uint32_t kOrderMode           = 0x0040u;
constexpr uint32_t kWave32Enable        = 0x8000u;
constexpr uint32_t kKnownDispatchBits   = kComputeShaderEnable | kPartialThreadgroup | kForceStartAtZero | kOrderedAppendEnable |
                                          kOrderedAppendMode | kUseThreadDimensions | kOrderMode | kWave32Enable;
constexpr uint32_t kDeferredDispatchControl =
    kPartialThreadgroup | kForceStartAtZero | kOrderedAppendEnable | kOrderedAppendMode | kUseThreadDimensions;
constexpr uint32_t kGuestWaveSize      = 64u;
constexpr uint32_t kNativeSubgroupSize = 32u;
constexpr uint32_t kLdsDwordBytes      = 4u;

[[nodiscard]] bool MultiplyU32(uint32_t a, uint32_t b, uint32_t* product) noexcept
{
	if (product == nullptr || (a != 0 && b > std::numeric_limits<uint32_t>::max() / a))
	{
		return false;
	}
	*product = a * b;
	return true;
}

} // namespace

ShaderComputeWaveLayoutStatus ShaderBuildPairedComputeWaveLayout(const ShaderComputeWaveRequest&      request,
                                                                 const ShaderComputeWaveCapabilities& capabilities,
                                                                 ShaderComputeWaveLayout*             layout)
{
	if (layout == nullptr)
	{
		return ShaderComputeWaveLayoutStatus::InvalidArgument;
	}

	// An empty grid is a no-op even when it carries controls this layout cannot lower.
	if (request.groups[0] == 0 || request.groups[1] == 0 || request.groups[2] == 0)
	{
		return ShaderComputeWaveLayoutStatus::NoWork;
	}

	const uint32_t mode = request.dispatch_mode;
	if ((mode & ~kKnownDispatchBits) != 0 || (mode & kDeferredDispatchControl) != 0 || (mode & kComputeShaderEnable) == 0)
	{
		return ShaderComputeWaveLayoutStatus::UnsupportedDispatchMode;
	}
	if ((mode & kWave32Enable) != 0)
	{
		return ShaderComputeWaveLayoutStatus::UnsupportedWidth;
	}

	// GFX10.3's ORDER_MODE selects ordered versus out-of-order wave launch. Both
	// values preserve the dispatch grid and do not alter this layout decision.

	if (!capabilities.size_control_enabled || !capabilities.full_subgroups_enabled || !capabilities.compute_required_size_supported ||
	    !capabilities.compute_ballot_shuffle_supported || capabilities.min_subgroup_size > kNativeSubgroupSize ||
	    capabilities.max_subgroup_size < kNativeSubgroupSize)
	{
		return ShaderComputeWaveLayoutStatus::MissingHostCapability;
	}

	if (request.lane_order != ShaderGuestLaneOrder::LinearXFirst)
	{
		return ShaderComputeWaveLayoutStatus::UnverifiedLaneOrder;
	}

	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		if (request.groups[axis] > capabilities.max_group_count[axis])
		{
			return ShaderComputeWaveLayoutStatus::HostLimitExceeded;
		}
	}

	uint32_t guest_invocations = 1;
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		if (request.local[axis] == 0)
		{
			return ShaderComputeWaveLayoutStatus::InvalidLocalSize;
		}
		if (!MultiplyU32(guest_invocations, request.local[axis], &guest_invocations))
		{
			return ShaderComputeWaveLayoutStatus::InvalidLocalSize;
		}
	}
	// A trailing partial wave keeps its missing lanes permanently inactive
	// (see the paired prolog's valid-lane mask).
	// Quotient/remainder avoids overflowing the rounding addition for a valid
	// uint32_t local product near UINT32_MAX.
	const uint32_t waves            = guest_invocations / kGuestWaveSize + (guest_invocations % kGuestWaveSize != 0u ? 1u : 0u);
	uint32_t       physical_local_x = 0;
	if (!MultiplyU32(waves, kNativeSubgroupSize, &physical_local_x))
	{
		return ShaderComputeWaveLayoutStatus::HostLimitExceeded;
	}

	uint32_t lds_bytes = 0;
	if (!MultiplyU32(request.lds_dwords, kLdsDwordBytes, &lds_bytes))
	{
		return ShaderComputeWaveLayoutStatus::HostLimitExceeded;
	}

	if (physical_local_x > capabilities.max_local_size[0] || 1u > capabilities.max_local_size[1] || 1u > capabilities.max_local_size[2] ||
	    physical_local_x > capabilities.max_invocations || waves > capabilities.max_subgroups || lds_bytes > capabilities.max_shared_bytes)
	{
		return ShaderComputeWaveLayoutStatus::HostLimitExceeded;
	}

	ShaderComputeWaveLayout candidate {};
	candidate.strategy             = ShaderComputeWaveStrategy::Paired64On32;
	candidate.guest_local[0]       = request.local[0];
	candidate.guest_local[1]       = request.local[1];
	candidate.guest_local[2]       = request.local[2];
	candidate.physical_local[0]    = physical_local_x;
	candidate.physical_local[1]    = 1;
	candidate.physical_local[2]    = 1;
	candidate.guest_wave_size      = kGuestWaveSize;
	candidate.native_subgroup_size = kNativeSubgroupSize;
	candidate.banks                = 2;
	candidate.waves                = waves;
	candidate.lds_dwords           = request.lds_dwords;
	*layout                        = candidate;

	return ShaderComputeWaveLayoutStatus::Supported;
}

} // namespace Kyty::Libs::Graphics
