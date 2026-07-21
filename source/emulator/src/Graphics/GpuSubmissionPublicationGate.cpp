#include "Emulator/Graphics/GpuSubmissionPublicationGate.h"

namespace Kyty::Libs::Graphics {

bool GpuSubmissionPublicationGate::IsValid(SubmissionId submission) const
{
	return submission.queue == m_queue && submission.sequence != 0;
}

GpuSubmissionPublicationGate::Entry* GpuSubmissionPublicationGate::Find(SubmissionId submission)
{
	for (auto& entry: m_entries)
	{
		if (entry.submission == submission)
		{
			return &entry;
		}
	}
	return nullptr;
}

GpuSubmissionPublicationResult GpuSubmissionPublicationGate::RegisterSubmitted(SubmissionId submission)
{
	if (!IsValid(submission))
	{
		return GpuSubmissionPublicationResult::InvalidArgument;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	if (submission.sequence <= m_published_sequence || Find(submission) != nullptr ||
	    (!m_entries.empty() && submission.sequence <= m_entries.back().submission.sequence))
	{
		return GpuSubmissionPublicationResult::InvalidTransition;
	}
	m_entries.push_back({submission, State::Submitted});
	return GpuSubmissionPublicationResult::Success;
}

GpuSubmissionPublicationResult GpuSubmissionPublicationGate::MarkFenceComplete(SubmissionId submission)
{
	if (!IsValid(submission))
	{
		return GpuSubmissionPublicationResult::InvalidArgument;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	auto*                       entry = Find(submission);
	if (entry == nullptr)
	{
		return GpuSubmissionPublicationResult::UnknownSubmission;
	}
	if (entry->state != State::Submitted)
	{
		return GpuSubmissionPublicationResult::InvalidTransition;
	}
	entry->state = State::FenceComplete;
	return GpuSubmissionPublicationResult::Success;
}

GpuSubmissionPublicationResult GpuSubmissionPublicationGate::TryAcquireNextForPublication(SubmissionId* submission)
{
	if (submission == nullptr)
	{
		return GpuSubmissionPublicationResult::InvalidArgument;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_entries.empty() || m_entries.front().state != State::FenceComplete)
	{
		return GpuSubmissionPublicationResult::NotReady;
	}
	m_entries.front().state = State::Publishing;
	*submission             = m_entries.front().submission;
	m_publisher_thread      = std::this_thread::get_id();
	return GpuSubmissionPublicationResult::Success;
}

GpuSubmissionPublicationResult GpuSubmissionPublicationGate::MarkPublished(SubmissionId submission)
{
	if (!IsValid(submission))
	{
		return GpuSubmissionPublicationResult::InvalidArgument;
	}

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_entries.empty() || m_entries.front().submission != submission || m_entries.front().state != State::Publishing)
		{
			return GpuSubmissionPublicationResult::InvalidTransition;
		}
		m_published_sequence = submission.sequence;
		m_entries.erase(m_entries.begin());
		m_publisher_thread = {};
	}
	m_condition.notify_all();
	return GpuSubmissionPublicationResult::Success;
}

GpuSubmissionPublicationResult GpuSubmissionPublicationGate::WaitUntilPublished(SubmissionId submission)
{
	if (!IsValid(submission))
	{
		return GpuSubmissionPublicationResult::InvalidArgument;
	}

	std::unique_lock<std::mutex> lock(m_mutex);
	if (submission.sequence <= m_published_sequence)
	{
		return GpuSubmissionPublicationResult::Success;
	}
	if (Find(submission) == nullptr)
	{
		return GpuSubmissionPublicationResult::UnknownSubmission;
	}
	m_condition.wait(lock, [&] { return submission.sequence <= m_published_sequence; });
	return GpuSubmissionPublicationResult::Success;
}

bool GpuSubmissionPublicationGate::IsPublishingOnCurrentThread()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_publisher_thread == std::this_thread::get_id();
}

} // namespace Kyty::Libs::Graphics
