#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONCOORDINATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONCOORDINATOR_H_

#include "Emulator/Graphics/RenderResolutionPlanner.h"

#include <mutex>

namespace Kyty::Libs::Graphics {

struct RenderResolutionSnapshot
{
	ResolutionExtent   target_extent;
	ResolutionExtent   guest_display_extent;
	ResolutionDecision candidate_decision;
	bool               guest_registered = false;
	// True only after the selected display cohort has been materialized with
	// the immutable host extent planned for the current registered display.
	bool scaling_applied = false;
};

enum class RenderDisplaySelectionStatus : uint8_t
{
	Selected,
	InvalidExtent,
	UnregisteredDisplay,
	UnauthorizedExtent,
};

// Single owner for render-resolution policy state. Public operations are
// serialized because VideoOut registration and agent telemetry use different
// host threads.
class RenderResolutionCoordinator final
{
public:
	explicit RenderResolutionCoordinator(ResolutionExtent target_extent = {1280, 720});

	ResolutionPolicyStatus ConfigureTarget(RenderResolutionMode mode, ResolutionExtent target_extent);
	ResolutionPolicyStatus RegisterGuestDisplayExtent(ResolutionExtent guest_extent);

	[[nodiscard]] ResolutionDecision                Evaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource) const;
	[[nodiscard]] RenderResolutionPlan          EvaluateCohort(const RenderResolutionPlanInput& input) const;
	RenderDisplaySelectionStatus SelectDisplayHostExtent(ResolutionExtent guest_extent, ResolutionExtent requested_host_extent,
	                                                                 const RenderResolutionPlan* authorization,
	                                                                 ResolutionExtent* selected_host_extent);
	bool                                             MarkScalingApplied(const RenderResolutionPlan& decision);
	[[nodiscard]] RenderResolutionSnapshot GetSnapshot() const;

private:
	mutable std::mutex                m_mutex;
	RenderResolutionPolicy          m_policy;
	RenderResolutionSnapshot m_snapshot;
};

ResolutionPolicyStatus           RenderResolutionInitialize(RenderResolutionMode mode, ResolutionExtent target_extent);
ResolutionPolicyStatus           RenderResolutionRegisterGuestDisplayExtent(ResolutionExtent guest_extent);
[[nodiscard]] ResolutionDecision RenderResolutionEvaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource);
[[nodiscard]] RenderResolutionPlan RenderResolutionEvaluatePlan(const RenderResolutionPlanInput& input);
RenderDisplaySelectionStatus RenderResolutionSelectDisplayHostExtent(ResolutionExtent guest_extent,
                                                                                           ResolutionExtent requested_host_extent,
                                                                                           const RenderResolutionPlan* authorization,
                                                                                           ResolutionExtent* selected_host_extent);
bool RenderResolutionMarkScalingApplied(const RenderResolutionPlan& decision);
[[nodiscard]] RenderResolutionSnapshot RenderResolutionGetSnapshot();

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONCOORDINATOR_H_ */
