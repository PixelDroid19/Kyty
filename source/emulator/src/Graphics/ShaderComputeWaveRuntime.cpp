#include "Emulator/Graphics/ShaderComputeWaveRuntime.h"

#include <limits>

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint32_t kComputeShaderEnable   = 0x0001u;
constexpr uint32_t kUseThreadDimensions   = 0x0020u;
constexpr uint32_t kOrderMode             = 0x0040u;
constexpr uint32_t kWave32Enable          = 0x8000u;
constexpr uint32_t kGen5AllowedNativeBits = kComputeShaderEnable | kOrderMode | kWave32Enable;

[[nodiscard]] ShaderComputeWavePreflightResult Rejected(ShaderComputeWavePreflightReason reason) noexcept
{
	return {ShaderComputeWavePreflightStatus::Rejected, reason};
}

[[nodiscard]] ShaderComputeWavePreflightResult LayoutResult(ShaderComputeWaveLayoutStatus status) noexcept
{
	switch (status)
	{
		case ShaderComputeWaveLayoutStatus::Supported:
			return {ShaderComputeWavePreflightStatus::Supported, ShaderComputeWavePreflightReason::None};
		case ShaderComputeWaveLayoutStatus::NoWork:
			return {ShaderComputeWavePreflightStatus::NoWork, ShaderComputeWavePreflightReason::None};
		case ShaderComputeWaveLayoutStatus::UnsupportedWidth: return Rejected(ShaderComputeWavePreflightReason::UnsupportedWidth);
		case ShaderComputeWaveLayoutStatus::UnsupportedDispatchMode:
			return Rejected(ShaderComputeWavePreflightReason::UnsupportedDispatchMode);
		case ShaderComputeWaveLayoutStatus::InvalidLocalSize: return Rejected(ShaderComputeWavePreflightReason::InvalidLocalSize);
		case ShaderComputeWaveLayoutStatus::UnverifiedLaneOrder: return Rejected(ShaderComputeWavePreflightReason::UnverifiedLaneOrder);
		case ShaderComputeWaveLayoutStatus::MissingHostCapability: return Rejected(ShaderComputeWavePreflightReason::MissingHostCapability);
		case ShaderComputeWaveLayoutStatus::HostLimitExceeded: return Rejected(ShaderComputeWavePreflightReason::HostLimitExceeded);
		case ShaderComputeWaveLayoutStatus::InvalidArgument: return Rejected(ShaderComputeWavePreflightReason::InvalidArgument);
	}
	return Rejected(ShaderComputeWavePreflightReason::InvalidArgument);
}

[[nodiscard]] bool HasNoWork(const uint32_t (&raw_count)[3]) noexcept
{
	return raw_count[0] == 0u || raw_count[1] == 0u || raw_count[2] == 0u;
}

[[nodiscard]] ShaderComputeWavePreflightReason ValidateNativeResources(const ShaderComputeWavePreflightRequest& request,
                                                                       const ShaderComputeWaveCapabilities&     capabilities) noexcept
{
	uint64_t invocations = 1u;
	for (uint32_t axis = 0; axis < 3u; ++axis)
	{
		if (request.local_size[axis] == 0u)
		{
			return ShaderComputeWavePreflightReason::InvalidLocalSize;
		}
		if (request.local_size[axis] > capabilities.max_local_size[axis])
		{
			return ShaderComputeWavePreflightReason::HostLimitExceeded;
		}
		if (invocations > std::numeric_limits<uint64_t>::max() / request.local_size[axis])
		{
			return ShaderComputeWavePreflightReason::HostLimitExceeded;
		}
		invocations *= request.local_size[axis];
	}

	const uint64_t lds_bytes = static_cast<uint64_t>(request.lds_dwords) * sizeof(uint32_t);
	if (invocations > capabilities.max_invocations || lds_bytes > capabilities.max_shared_bytes)
	{
		return ShaderComputeWavePreflightReason::HostLimitExceeded;
	}
	return ShaderComputeWavePreflightReason::None;
}

[[nodiscard]] bool CheckedCeilDivide(uint32_t value, uint32_t divisor, uint32_t* quotient) noexcept
{
	if (divisor == 0u || quotient == nullptr)
	{
		return false;
	}

	const uint64_t widened_value   = value;
	const uint64_t widened_divisor = divisor;
	const uint64_t adjustment      = widened_divisor - 1u;
	if (widened_value > std::numeric_limits<uint64_t>::max() - adjustment)
	{
		return false;
	}

	const uint64_t rounded = widened_value + adjustment;
	const uint64_t result  = rounded / widened_divisor;
	if (result > std::numeric_limits<uint32_t>::max())
	{
		return false;
	}
	*quotient = static_cast<uint32_t>(result);
	return true;
}

[[nodiscard]] bool BuildNativeEquivalentLayout(const ShaderComputeWavePreflightRequest& request,
                                               const ShaderComputeWaveCapabilities& capabilities, ShaderComputeWaveLayout* layout) noexcept
{
	if ((request.dispatch_mode & ~kGen5AllowedNativeBits) != 0u || (request.dispatch_mode & kComputeShaderEnable) == 0u ||
	    request.tg_size_en || ValidateNativeResources(request, capabilities) != ShaderComputeWavePreflightReason::None)
	{
		return false;
	}
	for (uint32_t axis = 0; axis < 3u; ++axis)
	{
		if (request.raw_dispatch_count[axis] > capabilities.max_group_count[axis])
		{
			return false;
		}
	}
	layout->strategy             = ShaderComputeWaveStrategy::Native;
	layout->guest_wave_size      = 64u;
	layout->native_subgroup_size = 0u;
	layout->banks                = 1u;
	layout->waves                = 0u;
	layout->lds_dwords           = request.lds_dwords;
	for (uint32_t axis = 0; axis < 3u; ++axis)
	{
		layout->guest_local[axis]    = request.local_size[axis];
		layout->physical_local[axis] = request.local_size[axis];
	}
	return true;
}

[[nodiscard]] ShaderComputeWavePreflightResult BuildWave64DispatchPlan(const ShaderComputeWavePreflightRequest& request,
                                                                     const ShaderComputeWaveCapabilities&     capabilities,
                                                                     ShaderComputeWaveDispatchPlan*           plan) noexcept
{
	ShaderComputeWaveRequest layout_request {};
	for (uint32_t axis = 0; axis < 3u; ++axis)
	{
		layout_request.local[axis]  = request.local_size[axis];
		layout_request.groups[axis] = request.raw_dispatch_count[axis];
	}
	layout_request.dispatch_mode = request.dispatch_mode;
	layout_request.lds_dwords    = request.lds_dwords;
	layout_request.lane_order    = request.lane_order;

	ShaderComputeWaveLayout paired {};
	auto                    result = LayoutResult(ShaderBuildPairedComputeWaveLayout(layout_request, capabilities, &paired));
	if (result.status == ShaderComputeWavePreflightStatus::Supported && request.tg_size_en)
	{
		result = Rejected(ShaderComputeWavePreflightReason::UnsupportedSystemSgpr);
	}

	ShaderComputeWaveDispatchPlan candidate {};
	candidate.dispatch_mode = request.dispatch_mode;
	for (uint32_t axis = 0; axis < 3u; ++axis)
	{
		candidate.group_count[axis] = request.raw_dispatch_count[axis];
	}
	candidate.native_equivalent_valid = BuildNativeEquivalentLayout(request, capabilities, &candidate.native_equivalent_layout);
	if (result.status == ShaderComputeWavePreflightStatus::Supported)
	{
		candidate.wave_layout = paired;
	} else if (result.status == ShaderComputeWavePreflightStatus::Rejected && candidate.native_equivalent_valid)
	{
		// Only the per-lane route remains; admission must prove the program.
		candidate.wave_layout                 = candidate.native_equivalent_layout;
		candidate.native_equivalence_required = true;
		result = {ShaderComputeWavePreflightStatus::Supported, ShaderComputeWavePreflightReason::None};
	} else
	{
		return result;
	}
	*plan = candidate;
	return result;
}

} // namespace

ShaderComputeWavePreflightResult ShaderBuildComputeWaveDispatchPlan(const ShaderComputeWavePreflightRequest& request,
                                                                    const ShaderComputeWaveCapabilities&     capabilities,
                                                                    ShaderComputeWaveDispatchPlan*           plan) noexcept
{
	if (plan == nullptr)
	{
		return Rejected(ShaderComputeWavePreflightReason::InvalidArgument);
	}

	// Gen5 USE_THREAD_DIMENSIONS: counts are threads. Launch ceil(threads /
	// local) groups; the shader deactivates lanes past the thread limits.
	if (request.is_next_gen && (request.dispatch_mode & kUseThreadDimensions) != 0u)
	{
		if (HasNoWork(request.raw_dispatch_count))
		{
			return {ShaderComputeWavePreflightStatus::NoWork, ShaderComputeWavePreflightReason::None};
		}
		ShaderComputeWavePreflightRequest groups = request;
		groups.dispatch_mode &= ~kUseThreadDimensions;
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (!CheckedCeilDivide(request.raw_dispatch_count[axis], request.local_size[axis], &groups.raw_dispatch_count[axis]))
			{
				return Rejected(ShaderComputeWavePreflightReason::InvalidLocalSize);
			}
		}
		const auto result = ShaderBuildComputeWaveDispatchPlan(groups, capabilities, plan);
		if (result.status == ShaderComputeWavePreflightStatus::Supported)
		{
			plan->thread_limits_used = true;
			for (uint32_t axis = 0; axis < 3u; ++axis)
			{
				plan->thread_limits[axis] = request.raw_dispatch_count[axis];
			}
		}
		return result;
	}

	// Wave64 runs through the paired-lane layout, or one guest lane per
	// invocation once the program is proven wave-width independent.
	if (request.is_next_gen && (request.dispatch_mode & kWave32Enable) == 0u)
	{
		return BuildWave64DispatchPlan(request, capabilities, plan);
	}

	if (HasNoWork(request.raw_dispatch_count))
	{
		return {ShaderComputeWavePreflightStatus::NoWork, ShaderComputeWavePreflightReason::None};
	}

	if (request.is_next_gen)
	{
		// Gen5 currently admits only the ordinary group-count modes whose local
		// lane ordering has a contract. Partial groups, thread counts and other
		// initiator controls remain fail-closed for both native and paired paths.
		if ((request.dispatch_mode & ~kGen5AllowedNativeBits) != 0u || (request.dispatch_mode & kComputeShaderEnable) == 0u)
		{
			return Rejected(ShaderComputeWavePreflightReason::UnsupportedDispatchMode);
		}
	}

	const auto resource_reason = ValidateNativeResources(request, capabilities);
	if (resource_reason != ShaderComputeWavePreflightReason::None)
	{
		return Rejected(resource_reason);
	}

	ShaderComputeWaveDispatchPlan candidate {};
	candidate.dispatch_mode                    = request.dispatch_mode;
	candidate.wave_layout.strategy             = ShaderComputeWaveStrategy::Native;
	candidate.wave_layout.guest_wave_size      = request.is_next_gen ? 32u : 0u;
	candidate.wave_layout.native_subgroup_size = 0u;
	candidate.wave_layout.banks                = 1u;
	candidate.wave_layout.lds_dwords           = request.lds_dwords;
	for (uint32_t axis = 0; axis < 3u; ++axis)
	{
		candidate.wave_layout.guest_local[axis]    = request.local_size[axis];
		candidate.wave_layout.physical_local[axis] = request.local_size[axis];
	}

	const bool thread_dimensions = !request.is_next_gen && (request.dispatch_mode & kUseThreadDimensions) != 0u;
	for (uint32_t axis = 0; axis < 3u; ++axis)
	{
		if (thread_dimensions)
		{
			if (!CheckedCeilDivide(request.raw_dispatch_count[axis], request.local_size[axis], &candidate.group_count[axis]))
			{
				return Rejected(ShaderComputeWavePreflightReason::ArithmeticOverflow);
			}
		} else
		{
			candidate.group_count[axis] = request.raw_dispatch_count[axis];
		}
		if (candidate.group_count[axis] > capabilities.max_group_count[axis])
		{
			return Rejected(ShaderComputeWavePreflightReason::HostLimitExceeded);
		}
	}

	*plan = candidate;
	return {ShaderComputeWavePreflightStatus::Supported, ShaderComputeWavePreflightReason::None};
}

const char* ShaderComputeWavePreflightReasonName(ShaderComputeWavePreflightReason reason) noexcept
{
	switch (reason)
	{
		case ShaderComputeWavePreflightReason::None: return "none";
		case ShaderComputeWavePreflightReason::InvalidArgument: return "invalid_argument";
		case ShaderComputeWavePreflightReason::InvalidLocalSize: return "invalid_local_size";
		case ShaderComputeWavePreflightReason::UnsupportedDispatchMode: return "unsupported_dispatch_mode";
		case ShaderComputeWavePreflightReason::UnsupportedWidth: return "unsupported_width";
		case ShaderComputeWavePreflightReason::UnverifiedLaneOrder: return "unverified_lane_order";
		case ShaderComputeWavePreflightReason::MissingHostCapability: return "missing_host_capability";
		case ShaderComputeWavePreflightReason::HostLimitExceeded: return "host_limit_exceeded";
		case ShaderComputeWavePreflightReason::ArithmeticOverflow: return "arithmetic_overflow";
		case ShaderComputeWavePreflightReason::UnsupportedSystemSgpr: return "unsupported_system_sgpr";
	}
	return "invalid_argument";
}

} // namespace Kyty::Libs::Graphics
