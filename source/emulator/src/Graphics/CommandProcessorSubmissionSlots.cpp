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
	const auto result = m_coordinator->MarkSubmitted(m_slots[slot].id);
	if (result == GpuSubmissionResult::Success)
	{
		m_submitted_fifo.push_back(slot);
	}
	return result;
}

GpuSubmissionResult CommandProcessorSubmissionSlots::RegisterProducer(uint32_t slot, uint64_t address, uint32_t size_bytes, uint64_t value)
{
	if (m_coordinator == nullptr || slot >= SlotCount)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	if (!m_slots[slot].active)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}
	return m_coordinator->RegisterProducer(m_slots[slot].id, address, size_bytes, value);
}

GpuSubmissionResult CommandProcessorSubmissionSlots::MarkFenceCompleted(uint32_t slot)
{
	if (m_coordinator == nullptr || slot >= SlotCount)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	if (!m_slots[slot].active)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}
	if (m_submitted_fifo.empty() || m_submitted_fifo.front() != slot)
	{
		return GpuSubmissionResult::InvalidTransition;
	}

	const auto completed = m_coordinator->MarkCompletedWithoutActions(m_slots[slot].id);
	if (completed != GpuSubmissionResult::Success)
	{
		return completed;
	}

	m_slots[slot].active = false;
	m_submitted_fifo.pop_front();
	return GpuSubmissionResult::Success;
}

GpuSubmissionResult CommandProcessorSubmissionSlots::RetirePublished(SubmissionId id)
{
	if (m_coordinator == nullptr || id.queue != m_queue || id.sequence == 0)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	return m_coordinator->RetireCompleted(id);
}

GpuSubmissionResult CommandProcessorSubmissionSlots::FindSlot(SubmissionId id, uint32_t* slot) const
{
	if (slot == nullptr)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	for (uint32_t index = 0; index < SlotCount; index++)
	{
		if (m_slots[index].active && m_slots[index].id == id)
		{
			*slot = index;
			return GpuSubmissionResult::Success;
		}
	}
	return GpuSubmissionResult::UnknownSubmission;
}

GpuSubmissionResult CommandProcessorSubmissionSlots::GetOldestSubmitted(uint32_t* slot, SubmissionId* id) const
{
	if (slot == nullptr || id == nullptr)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	if (m_submitted_fifo.empty())
	{
		return GpuSubmissionResult::UnknownSubmission;
	}

	*slot = m_submitted_fifo.front();
	*id   = m_slots[*slot].id;
	return GpuSubmissionResult::Success;
}

GpuSubmissionResult CommandProcessorSubmissionSlots::GetState(SubmissionId id, GpuSubmissionState* state) const
{
	if (m_coordinator == nullptr)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	return m_coordinator->GetState(id, state);
}

GpuSubmissionResult CommandProcessorSubmissionSlots::FindPendingProducer(
    uint64_t address, uint32_t size_bytes, uint64_t reference, uint64_t mask, SubmissionDependency* dependency) const
{
	if (m_coordinator == nullptr)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	return m_coordinator->FindPendingProducer(address, size_bytes, reference, mask, dependency);
}

GpuSubmissionResult CommandProcessorSubmissionSlots::CompleteFenceThenBeginRecording(
    uint32_t completed_slot, uint32_t recording_slot, SubmissionId* id, SubmissionDependency* blocking_dependency)
{
	if (id == nullptr || completed_slot >= SlotCount || recording_slot >= SlotCount)
	{
		return GpuSubmissionResult::InvalidArgument;
	}

	const auto completed = MarkFenceCompleted(completed_slot);
	if (completed != GpuSubmissionResult::Success)
	{
		return completed;
	}
	return BeginRecording(recording_slot, id, blocking_dependency);
}

} // namespace Kyty::Libs::Graphics
