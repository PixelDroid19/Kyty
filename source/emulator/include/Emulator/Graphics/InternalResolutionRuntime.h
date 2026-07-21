#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_INTERNALRESOLUTIONRUNTIME_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_INTERNALRESOLUTIONRUNTIME_H_

#include "Emulator/Graphics/AttachmentResolutionCohort.h"

#include <mutex>

namespace Kyty::Libs::Graphics {

struct InternalResolutionRuntimeSnapshot
{
	ResolutionExtent   target_extent;
	ResolutionExtent   guest_display_extent;
	ResolutionDecision candidate_decision;
	bool               guest_registered = false;
	// This phase records policy only. Vulkan resources remain at guest extent.
	bool scaling_applied = false;
};

enum class InternalResolutionDisplaySelectionStatus : uint8_t
{
	Selected,
	StickyMatch,
	StickyMismatch,
	InvalidExtent,
	UnregisteredDisplay,
	UnauthorizedExtent,
};

// Single owner for internal-resolution policy state. Public operations are
// serialized because VideoOut registration and agent telemetry use different
// host threads.
class InternalResolutionRuntime final
{
public:
	explicit InternalResolutionRuntime(ResolutionExtent target_extent = {1280, 720});

	ResolutionPolicyStatus ConfigureTarget(ResolutionExtent target_extent);
	ResolutionPolicyStatus RegisterGuestDisplayExtent(ResolutionExtent guest_extent);

	[[nodiscard]] ResolutionDecision                Evaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource) const;
	[[nodiscard]] ResolutionCohortDecision          EvaluateCohort(const ResolutionCohortInput& input) const;
	InternalResolutionDisplaySelectionStatus SelectDisplayHostExtent(ResolutionExtent guest_extent, ResolutionExtent requested_host_extent,
	                                                                 const ResolutionCohortDecision* authorization,
	                                                                 ResolutionExtent* selected_host_extent);
	bool                                             MarkScalingApplied(const ResolutionCohortDecision& decision);
	[[nodiscard]] InternalResolutionRuntimeSnapshot GetSnapshot() const;

private:
	mutable std::mutex                m_mutex;
	InternalResolutionPolicy          m_policy;
	InternalResolutionRuntimeSnapshot m_snapshot;
	ResolutionExtent                  m_selected_host_extent;
	bool                              m_display_selection_set = false;
};

ResolutionPolicyStatus           InternalResolutionRuntimeInitialize(ResolutionExtent target_extent);
ResolutionPolicyStatus           InternalResolutionRuntimeRegisterGuestDisplayExtent(ResolutionExtent guest_extent);
[[nodiscard]] ResolutionDecision InternalResolutionRuntimeEvaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource);
[[nodiscard]] ResolutionCohortDecision InternalResolutionRuntimeEvaluateCohort(const ResolutionCohortInput& input);
InternalResolutionDisplaySelectionStatus InternalResolutionRuntimeSelectDisplayHostExtent(ResolutionExtent guest_extent,
                                                                                           ResolutionExtent requested_host_extent,
                                                                                           const ResolutionCohortDecision* authorization,
                                                                                           ResolutionExtent* selected_host_extent);
bool InternalResolutionRuntimeMarkScalingApplied(const ResolutionCohortDecision& decision);
[[nodiscard]] InternalResolutionRuntimeSnapshot InternalResolutionRuntimeGetSnapshot();

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_INTERNALRESOLUTIONRUNTIME_H_ */
