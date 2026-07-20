#include "Emulator/Graphics/GpuSubmissionTracker.h"

#include <limits>

namespace Kyty::Libs::Graphics {

GpuSubmissionTracker::Submission* GpuSubmissionTracker::FindSubmission(SubmissionId id)
{
	for (auto& submission: m_submissions)
	{
		if (submission.id == id)
		{
			return &submission;
		}
	}
	return nullptr;
}

const GpuSubmissionTracker::Submission* GpuSubmissionTracker::FindSubmission(SubmissionId id) const
{
	for (const auto& submission: m_submissions)
	{
		if (submission.id == id)
		{
			return &submission;
		}
	}
	return nullptr;
}

bool GpuSubmissionTracker::IsValidPhase(GpuCompletionPhase phase)
{
	return phase == GpuCompletionPhase::WriteBack || phase == GpuCompletionPhase::GuestStore || phase == GpuCompletionPhase::Notify;
}

bool GpuSubmissionTracker::IsValidRange(uint64_t address, uint32_t size_bytes)
{
	if (size_bytes == 0 || size_bytes > sizeof(uint64_t))
	{
		return false;
	}
	return address <= std::numeric_limits<uint64_t>::max() - (size_bytes - 1u);
}

GpuSubmissionResult GpuSubmissionTracker::BeginRecording(GpuQueueId queue, uint32_t slot, SubmissionId* id,
                                                         SubmissionDependency* blocking_dependency)
{
	if (id == nullptr)
	{
		return GpuSubmissionResult::InvalidArgument;
	}

	for (const auto& submission: m_submissions)
	{
		if (submission.id.queue == queue && submission.slot == slot && submission.state != GpuSubmissionState::Completed)
		{
			if (blocking_dependency != nullptr)
			{
				blocking_dependency->producer = submission.id;
			}
			return GpuSubmissionResult::SlotBusy;
		}
	}

	uint64_t* next = nullptr;
	for (auto& sequence: m_queue_sequences)
	{
		if (sequence.queue == queue)
		{
			next = &sequence.next;
			break;
		}
	}
	if (next == nullptr)
	{
		m_queue_sequences.push_back({queue, 1});
		next = &m_queue_sequences.back().next;
	}

	*id = {queue, *next};
	(*next)++;
	m_submissions.push_back({*id, slot, GpuSubmissionState::Recording, {}, {}});
	return GpuSubmissionResult::Success;
}

GpuSubmissionResult GpuSubmissionTracker::AddCompletionAction(SubmissionId id, GpuCompletionPhase phase, uint64_t token)
{
	if (!IsValidPhase(phase))
	{
		return GpuSubmissionResult::InvalidArgument;
	}

	auto* submission = FindSubmission(id);
	if (submission == nullptr)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}
	if (submission->state != GpuSubmissionState::Recording)
	{
		return GpuSubmissionResult::SubmissionFrozen;
	}

	submission->actions.push_back({phase, token});
	return GpuSubmissionResult::Success;
}

GpuSubmissionResult GpuSubmissionTracker::RegisterProducer(SubmissionId id, uint64_t address, uint32_t size_bytes, uint64_t value)
{
	if (!IsValidRange(address, size_bytes))
	{
		return GpuSubmissionResult::InvalidArgument;
	}

	auto* submission = FindSubmission(id);
	if (submission == nullptr)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}
	if (submission->state != GpuSubmissionState::Recording)
	{
		return GpuSubmissionResult::SubmissionFrozen;
	}

	submission->producers.push_back({address, size_bytes, value, m_next_producer_registration++});
	return GpuSubmissionResult::Success;
}

GpuSubmissionResult GpuSubmissionTracker::MarkSubmitted(SubmissionId id)
{
	auto* submission = FindSubmission(id);
	if (submission == nullptr)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}
	if (submission->state != GpuSubmissionState::Recording)
	{
		return GpuSubmissionResult::InvalidTransition;
	}

	submission->state = GpuSubmissionState::Submitted;
	return GpuSubmissionResult::Success;
}

GpuSubmissionResult GpuSubmissionTracker::MarkCompleted(SubmissionId id, GpuCompletionActionSink* sink)
{
	if (sink == nullptr)
	{
		return GpuSubmissionResult::InvalidArgument;
	}

	auto* submission = FindSubmission(id);
	if (submission == nullptr)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}
	if (submission->state == GpuSubmissionState::Completed)
	{
		return GpuSubmissionResult::AlreadyCompleted;
	}
	if (submission->state != GpuSubmissionState::Submitted)
	{
		return GpuSubmissionResult::InvalidTransition;
	}

	submission->state                     = GpuSubmissionState::Completed;
	constexpr GpuCompletionPhase phases[] = {
	    GpuCompletionPhase::WriteBack,
	    GpuCompletionPhase::GuestStore,
	    GpuCompletionPhase::Notify,
	};
	for (const auto phase: phases)
	{
		for (const auto& action: submission->actions)
		{
			if (action.phase == phase)
			{
				sink->Execute(action.phase, action.token);
			}
		}
	}
	return GpuSubmissionResult::Success;
}

GpuSubmissionResult GpuSubmissionTracker::RetireCompleted(SubmissionId id)
{
	for (auto it = m_submissions.begin(); it != m_submissions.end(); ++it)
	{
		if (it->id != id)
		{
			continue;
		}
		if (it->state != GpuSubmissionState::Completed)
		{
			return GpuSubmissionResult::InvalidTransition;
		}
		m_submissions.erase(it);
		return GpuSubmissionResult::Success;
	}
	return GpuSubmissionResult::UnknownSubmission;
}

GpuSubmissionResult GpuSubmissionTracker::GetState(SubmissionId id, GpuSubmissionState* state) const
{
	if (state == nullptr)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	const auto* submission = FindSubmission(id);
	if (submission == nullptr)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}
	*state = submission->state;
	return GpuSubmissionResult::Success;
}

GpuSubmissionResult GpuSubmissionTracker::HasCompletionActions(SubmissionId id, bool* has_actions) const
{
	if (has_actions == nullptr)
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	const auto* submission = FindSubmission(id);
	if (submission == nullptr)
	{
		return GpuSubmissionResult::UnknownSubmission;
	}
	*has_actions = !submission->actions.empty();
	return GpuSubmissionResult::Success;
}

bool GpuSubmissionTracker::ProducerMatches(const Producer& producer, uint64_t address, uint32_t size_bytes, uint64_t reference,
                                           uint64_t mask)
{
	if (mask == 0)
	{
		return false;
	}

	const uint64_t producer_end = producer.address + producer.size_bytes;
	for (uint32_t bit = 0; bit < size_bytes * 8u; bit++)
	{
		const uint64_t wait_bit = uint64_t {1} << bit;
		if ((mask & wait_bit) == 0)
		{
			continue;
		}

		const uint64_t byte_address = address + bit / 8u;
		if (byte_address < producer.address || byte_address >= producer_end)
		{
			return false;
		}

		const uint32_t producer_bit = static_cast<uint32_t>((byte_address - producer.address) * 8u) + bit % 8u;
		const bool     produced     = ((producer.value >> producer_bit) & 1u) != 0;
		const bool     expected     = ((reference >> bit) & 1u) != 0;
		if (produced != expected)
		{
			return false;
		}
	}
	return true;
}

bool GpuSubmissionTracker::ProducerTouchesMaskedBits(const Producer& producer, uint64_t address, uint32_t size_bytes, uint64_t mask)
{
	const uint64_t producer_end = producer.address + producer.size_bytes;
	for (uint32_t bit = 0; bit < size_bytes * 8u; bit++)
	{
		const uint64_t wait_bit = uint64_t {1} << bit;
		if ((mask & wait_bit) == 0)
		{
			continue;
		}
		const uint64_t byte_address = address + bit / 8u;
		if (byte_address >= producer.address && byte_address < producer_end)
		{
			return true;
		}
	}
	return false;
}

GpuSubmissionResult GpuSubmissionTracker::FindPendingProducer(uint64_t address, uint32_t size_bytes, uint64_t reference, uint64_t mask,
                                                              SubmissionDependency* dependency) const
{
	if (dependency == nullptr || !IsValidRange(address, size_bytes))
	{
		return GpuSubmissionResult::InvalidArgument;
	}
	if (size_bytes < sizeof(uint64_t) && (mask >> (size_bytes * 8u)) != 0)
	{
		return GpuSubmissionResult::InvalidArgument;
	}

	const Submission* newest       = nullptr;
	uint64_t          newest_order = 0;
	for (const auto& submission: m_submissions)
	{
		for (const auto& producer: submission.producers)
		{
			if (producer.registration_order > newest_order && ProducerTouchesMaskedBits(producer, address, size_bytes, mask))
			{
				newest       = &submission;
				newest_order = producer.registration_order;
			}
		}
	}

	if (newest == nullptr)
	{
		return GpuSubmissionResult::ProducerNotFound;
	}

	for (const auto& producer: newest->producers)
	{
		if (producer.registration_order == newest_order && ProducerMatches(producer, address, size_bytes, reference, mask))
		{
			dependency->producer = newest->id;
			return GpuSubmissionResult::Success;
		}
	}

	dependency->producer = newest->id;
	return GpuSubmissionResult::ProducerValueMismatch;
}

} // namespace Kyty::Libs::Graphics
