#include "Emulator/Graphics/CommandProcessorSubmissionSlots.h"

namespace Kyty::Libs::Graphics {

CommandProcessorSubmissionSlots::CommandProcessorSubmissionSlots(GpuSubmissionCoordinator* coordinator, GpuQueueId queue)
    : m_coordinator(coordinator), m_queue(queue)
{
}

GpuSubmissionResult CommandProcessorSubmissionSlots::BeginRecording(uint32_t slot, SubmissionId* id,
                                                                    SubmissionDependency* blocking_dependency)
{
	if (m_coordinator == nullptr || id == nullptr || slot >= SlotCount)
	{
		return GpuSubmissionResult::InvalidArgument;
	}

	SubmissionId recorded;
	const auto   result = m_coordinator->BeginRecording(m_queue, slot, &recorded, blocking_dependency);
	if (result == GpuSubmissionResult::Success)
	{
		m_slots[slot].id     = recorded;
		m_slots[slot].active = true;
		*id                  = recorded;
	}
	return result;
}

GpuSubmissionResult CommandProcessorSubmissionSlots::MarkSubmitted(uint32_t slot)
{
	if (m_coordinator == nullptr || slot >= SlotCount)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	if (!m_slots[slot].active)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}
	return m_coordinator->MarkSubmitted(m_slots[slot].id);
}

GpuSubmissionResult CommandProcessorSubmissionSlots::MarkCompletedWithoutActionsAndRetire(uint32_t slot)
{
	if (m_coordinator == nullptr || slot >= SlotCount)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	if (!m_slots[slot].active)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}

	const auto completed = m_coordinator->MarkCompletedWithoutActions(m_slots[slot].id);
	if (completed != GpuSubmissionResult::Success)
	{
		return completed;
	}

	const auto retired = m_coordinator->RetireCompleted(m_slots[slot].id);
	if (retired == GpuSubmissionResult::Success)
	{
		m_slots[slot].active = false;
	}
	return retired;
}

GpuSubmissionResult CommandProcessorSubmissionSlots::CompleteAndRetireThenBeginRecording(
    uint32_t completed_slot, uint32_t recording_slot, SubmissionId* id, SubmissionDependency* blocking_dependency)
{
	if (id == nullptr || completed_slot >= SlotCount || recording_slot >= SlotCount)
	{
		return GpuSubmissionResult::InvalidArgument;
	}

	const auto completed = MarkCompletedWithoutActionsAndRetire(completed_slot);
	if (completed != GpuSubmissionResult::Success)
	{
		return completed;
	}
	return BeginRecording(recording_slot, id, blocking_dependency);
}

} // namespace Kyty::Libs::Graphics
