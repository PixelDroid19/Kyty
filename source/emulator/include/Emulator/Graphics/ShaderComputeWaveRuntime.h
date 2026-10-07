#pragma once

#include "Emulator/Graphics/ShaderComputeWaveLayout.h"

#include <cstdint>

namespace Kyty::Libs::Graphics {

enum class ShaderComputeWavePreflightStatus : uint8_t
{
	Supported,
	NoWork,
	Rejected,
};

enum class ShaderComputeWavePreflightReason : uint8_t
{
	None,
	InvalidArgument,
	InvalidLocalSize,
	UnsupportedDispatchMode,
	UnsupportedWidth,
	UnverifiedLaneOrder,
	MissingHostCapability,
	HostLimitExceeded,
	ArithmeticOverflow,
	UnsupportedSystemSgpr,
};

struct ShaderComputeWavePreflightResult
{
	ShaderComputeWavePreflightStatus status = ShaderComputeWavePreflightStatus::Rejected;
	ShaderComputeWavePreflightReason reason = ShaderComputeWavePreflightReason::InvalidArgument;
};

struct ShaderComputeWavePreflightRequest
{
	bool                 is_next_gen   = false;
	uint32_t             dispatch_mode = 0;
	uint32_t             raw_dispatch_count[3] {};
	uint32_t             local_size[3] {};
	uint32_t             lds_dwords = 0;
	ShaderGuestLaneOrder lane_order = ShaderGuestLaneOrder::Unverified;
	bool                 tg_size_en = false;
};

struct ShaderComputeWaveDispatchPlan
{
	uint32_t                dispatch_mode = 0;
	uint32_t                group_count[3] {};
	ShaderComputeWaveLayout wave_layout {};
	// Wave64 only: a one-lane-per-invocation layout over the original guest
	// workgroup that passed native resource limits. It may replace the paired
	// layout only after the program itself is proven wave-width independent.
	bool                    native_equivalent_valid = false;
	ShaderComputeWaveLayout native_equivalent_layout {};
	// No paired layout exists; wave_layout is the per-lane route and
	// admission must prove the program before it runs.
	bool native_equivalence_required = false;
	// USE_THREAD_DIMENSIONS: lanes with a global ID past these are inactive.
	bool     thread_limits_used = false;
	uint32_t thread_limits[3] {};
};

[[nodiscard]] ShaderComputeWavePreflightResult ShaderBuildComputeWaveDispatchPlan(const ShaderComputeWavePreflightRequest& request,
                                                                                  const ShaderComputeWaveCapabilities&     capabilities,
                                                                                  ShaderComputeWaveDispatchPlan*           plan) noexcept;

[[nodiscard]] const char* ShaderComputeWavePreflightReasonName(ShaderComputeWavePreflightReason reason) noexcept;

} // namespace Kyty::Libs::Graphics
