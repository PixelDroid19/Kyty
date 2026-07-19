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

InternalResolutionRuntimeSnapshot InternalResolutionRuntimeGetSnapshot()
{
	return Runtime().GetSnapshot();
}

} // namespace Kyty::Libs::Graphics
