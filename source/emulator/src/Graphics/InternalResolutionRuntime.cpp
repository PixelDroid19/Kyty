#include "Emulator/Graphics/InternalResolutionRuntime.h"

namespace Kyty::Libs::Graphics {
namespace {

ResolutionResourceInfo DisplayCandidateInfo()
{
	ResolutionResourceInfo resource {};
	resource.kind = ResolutionResourceKind::ColorAttachment;
	return resource;
}

InternalResolutionRuntime& Runtime()
{
	static InternalResolutionRuntime runtime;
	return runtime;
}

} // namespace

InternalResolutionRuntime::InternalResolutionRuntime(ResolutionExtent target_extent): m_policy(target_extent)
{
	m_snapshot.target_extent = target_extent;
}

ResolutionPolicyStatus InternalResolutionRuntime::ConfigureTarget(ResolutionExtent target_extent)
{
	InternalResolutionPolicy policy;
	const auto               status = policy.SetTargetExtent(target_extent);
	if (status != ResolutionPolicyStatus::Success)
	{
		return status;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	m_policy                 = policy;
	m_snapshot               = {};
	m_snapshot.target_extent = target_extent;
	m_selected_host_extent   = {};
	m_display_selection_set  = false;
	return ResolutionPolicyStatus::Success;
}

ResolutionPolicyStatus InternalResolutionRuntime::RegisterGuestDisplayExtent(ResolutionExtent guest_extent)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto                  status = m_policy.RegisterGuestDisplayExtent(guest_extent);
	if (status != ResolutionPolicyStatus::Success)
	{
		return status;
	}

	const bool guest_extent_changed = m_snapshot.guest_registered && guest_extent != m_snapshot.guest_display_extent;
	if (guest_extent_changed)
	{
		m_selected_host_extent  = {};
		m_display_selection_set = false;
	}

	m_snapshot.target_extent        = m_policy.GetTargetExtent();
	m_snapshot.guest_display_extent = guest_extent;
	m_snapshot.candidate_decision   = m_policy.Evaluate(guest_extent, DisplayCandidateInfo());
	m_snapshot.guest_registered     = true;
	m_snapshot.scaling_applied      = false;
	return ResolutionPolicyStatus::Success;
}

ResolutionDecision InternalResolutionRuntime::Evaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_policy.Evaluate(guest_resource_extent, resource);
}

ResolutionCohortDecision InternalResolutionRuntime::EvaluateCohort(const ResolutionCohortInput& input) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return EvaluateResolutionCohort(m_policy, input);
}

InternalResolutionDisplaySelectionStatus InternalResolutionRuntime::SelectDisplayHostExtent(ResolutionExtent guest_extent,
                                                                                             ResolutionExtent requested_host_extent,
                                                                                             const ResolutionCohortDecision* authorization,
                                                                                             ResolutionExtent* selected_host_extent)
{
	if (guest_extent.width == 0 || guest_extent.height == 0 || requested_host_extent.width == 0 || requested_host_extent.height == 0 ||
	    selected_host_extent == nullptr)
	{
		return InternalResolutionDisplaySelectionStatus::InvalidExtent;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_snapshot.guest_registered || guest_extent != m_snapshot.guest_display_extent)
	{
		return InternalResolutionDisplaySelectionStatus::UnregisteredDisplay;
	}
	if (requested_host_extent != guest_extent &&
	    (authorization == nullptr || authorization->classification != ResolutionClassification::Scaled ||
	     authorization->guest_extent != guest_extent || authorization->host_extent != requested_host_extent))
	{
		return InternalResolutionDisplaySelectionStatus::UnauthorizedExtent;
	}
	if (!m_display_selection_set)
	{
		m_selected_host_extent  = requested_host_extent;
		m_display_selection_set = true;
		*selected_host_extent   = requested_host_extent;
		return InternalResolutionDisplaySelectionStatus::Selected;
	}
	*selected_host_extent = m_selected_host_extent;
	return requested_host_extent == m_selected_host_extent ? InternalResolutionDisplaySelectionStatus::StickyMatch
	                                                       : InternalResolutionDisplaySelectionStatus::StickyMismatch;
}

bool InternalResolutionRuntime::MarkScalingApplied(const ResolutionCohortDecision& decision)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_snapshot.guest_registered || decision.classification != ResolutionClassification::Scaled ||
	    decision.guest_extent != m_snapshot.guest_display_extent || decision.host_extent != m_snapshot.target_extent)
	{
		return false;
	}
	m_snapshot.scaling_applied = true;
	return true;
}

InternalResolutionRuntimeSnapshot InternalResolutionRuntime::GetSnapshot() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_snapshot;
}

ResolutionPolicyStatus InternalResolutionRuntimeInitialize(ResolutionExtent target_extent)
{
	return Runtime().ConfigureTarget(target_extent);
}

ResolutionPolicyStatus InternalResolutionRuntimeRegisterGuestDisplayExtent(ResolutionExtent guest_extent)
{
	return Runtime().RegisterGuestDisplayExtent(guest_extent);
}

ResolutionDecision InternalResolutionRuntimeEvaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource)
{
	return Runtime().Evaluate(guest_resource_extent, resource);
}

ResolutionCohortDecision InternalResolutionRuntimeEvaluateCohort(const ResolutionCohortInput& input)
{
	return Runtime().EvaluateCohort(input);
}

InternalResolutionDisplaySelectionStatus InternalResolutionRuntimeSelectDisplayHostExtent(ResolutionExtent guest_extent,
                                                                                           ResolutionExtent requested_host_extent,
                                                                                           const ResolutionCohortDecision* authorization,
                                                                                           ResolutionExtent* selected_host_extent)
{
	return Runtime().SelectDisplayHostExtent(guest_extent, requested_host_extent, authorization, selected_host_extent);
}

bool InternalResolutionRuntimeMarkScalingApplied(const ResolutionCohortDecision& decision)
{
	return Runtime().MarkScalingApplied(decision);
}

InternalResolutionRuntimeSnapshot InternalResolutionRuntimeGetSnapshot()
{
	return Runtime().GetSnapshot();
}

} // namespace Kyty::Libs::Graphics
