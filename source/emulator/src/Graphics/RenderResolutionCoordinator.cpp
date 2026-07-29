#include "Emulator/Graphics/RenderResolutionCoordinator.h"

namespace Kyty::Libs::Graphics {
namespace {

ResolutionResourceInfo DisplayCandidateInfo()
{
	ResolutionResourceInfo resource {};
	resource.kind = ResolutionResourceKind::ColorAttachment;
	return resource;
}

RenderResolutionCoordinator& Runtime()
{
	static RenderResolutionCoordinator runtime;
	return runtime;
}

} // namespace

RenderResolutionCoordinator::RenderResolutionCoordinator(ResolutionExtent target_extent): m_policy(target_extent)
{
	m_snapshot.target_extent = target_extent;
}

ResolutionPolicyStatus RenderResolutionCoordinator::ConfigureTarget(ResolutionScaleMode mode, ResolutionExtent target_extent)
{
	RenderResolutionPolicy policy;
	const auto               status = policy.SetTargetExtent(target_extent);
	if (status != ResolutionPolicyStatus::Success)
	{
		return status;
	}
	policy.SetScaleMode(mode);

	std::lock_guard<std::mutex> lock(m_mutex);
	m_policy                 = policy;
	m_snapshot               = {};
	m_snapshot.target_extent = target_extent;
	return ResolutionPolicyStatus::Success;
}

ResolutionPolicyStatus RenderResolutionCoordinator::RegisterGuestDisplayExtent(ResolutionExtent guest_extent)
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
		m_snapshot.scaling_applied = false;
	}

	m_snapshot.target_extent        = m_policy.GetTargetExtent();
	m_snapshot.guest_display_extent = guest_extent;
	m_snapshot.candidate_decision   = m_policy.Evaluate(guest_extent, DisplayCandidateInfo());
	m_snapshot.guest_registered     = true;
	m_snapshot.scaling_applied      = false;
	return ResolutionPolicyStatus::Success;
}

ResolutionDecision RenderResolutionCoordinator::Evaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_policy.Evaluate(guest_resource_extent, resource);
}

RenderResolutionPlan RenderResolutionCoordinator::EvaluateCohort(const RenderResolutionPlanInput& input) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return EvaluateRenderResolutionPlan(m_policy, input);
}

RenderDisplaySelectionStatus RenderResolutionCoordinator::SelectDisplayHostExtent(ResolutionExtent guest_extent,
                                                                                             ResolutionExtent requested_host_extent,
                                                                                             const RenderResolutionPlan* authorization,
                                                                                             ResolutionExtent* selected_host_extent)
{
	if (guest_extent.width == 0 || guest_extent.height == 0 || requested_host_extent.width == 0 || requested_host_extent.height == 0 ||
	    selected_host_extent == nullptr)
	{
		return RenderDisplaySelectionStatus::InvalidExtent;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_snapshot.guest_registered || guest_extent != m_snapshot.guest_display_extent)
	{
		return RenderDisplaySelectionStatus::UnregisteredDisplay;
	}
	if (requested_host_extent != guest_extent &&
	    (authorization == nullptr || authorization->classification != ResolutionClassification::Scaled ||
	     authorization->guest_extent != guest_extent || authorization->host_extent != requested_host_extent))
	{
		return RenderDisplaySelectionStatus::UnauthorizedExtent;
	}
	*selected_host_extent = requested_host_extent;
	return RenderDisplaySelectionStatus::Selected;
}

bool RenderResolutionCoordinator::MarkScalingApplied(const RenderResolutionPlan& decision)
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

RenderResolutionSnapshot RenderResolutionCoordinator::GetSnapshot() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_snapshot;
}

ResolutionPolicyStatus RenderResolutionInitialize(ResolutionScaleMode mode, ResolutionExtent target_extent)
{
	return Runtime().ConfigureTarget(mode, target_extent);
}

ResolutionPolicyStatus RenderResolutionRegisterGuestDisplayExtent(ResolutionExtent guest_extent)
{
	return Runtime().RegisterGuestDisplayExtent(guest_extent);
}

ResolutionDecision RenderResolutionEvaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource)
{
	return Runtime().Evaluate(guest_resource_extent, resource);
}

RenderResolutionPlan RenderResolutionEvaluatePlan(const RenderResolutionPlanInput& input)
{
	return Runtime().EvaluateCohort(input);
}

RenderDisplaySelectionStatus RenderResolutionSelectDisplayHostExtent(ResolutionExtent guest_extent,
                                                                                           ResolutionExtent requested_host_extent,
                                                                                           const RenderResolutionPlan* authorization,
                                                                                           ResolutionExtent* selected_host_extent)
{
	return Runtime().SelectDisplayHostExtent(guest_extent, requested_host_extent, authorization, selected_host_extent);
}

bool RenderResolutionMarkScalingApplied(const RenderResolutionPlan& decision)
{
	return Runtime().MarkScalingApplied(decision);
}

RenderResolutionSnapshot RenderResolutionGetSnapshot()
{
	return Runtime().GetSnapshot();
}

} // namespace Kyty::Libs::Graphics
