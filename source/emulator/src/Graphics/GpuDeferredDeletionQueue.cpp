#include "Emulator/Graphics/GpuDeferredDeletionQueue.h"

#include <algorithm>
#include <utility>

namespace Kyty::Libs::Graphics {

GpuDeferredDeletionResult GpuSubmissionHighWater::RecordUse(SubmissionId submission)
{
	if (submission.sequence == 0)
	{
		return GpuDeferredDeletionResult::InvalidArgument;
	}

	auto dependency = std::find_if(m_dependencies.begin(), m_dependencies.end(),
	                               [&submission](const auto& value) { return value.queue == submission.queue; });
	if (dependency == m_dependencies.end())
	{
		m_dependencies.push_back(submission);
	} else
	{
		dependency->sequence = std::max(dependency->sequence, submission.sequence);
	}
	return GpuDeferredDeletionResult::Success;
}

bool GpuSubmissionHighWater::LatestForQueue(GpuQueueId queue, SubmissionId* submission) const
{
	if (submission == nullptr)
	{
		return false;
	}
	const auto dependency =
	    std::find_if(m_dependencies.begin(), m_dependencies.end(), [queue](const auto& value) { return value.queue == queue; });
	if (dependency == m_dependencies.end())
	{
		return false;
	}
	*submission = *dependency;
	return true;
}

GpuDeferredDeletionResult GpuDeferredDeletionQueue::Enqueue(std::vector<SubmissionId> dependencies, Task task)
{
	if (!task)
	{
		return GpuDeferredDeletionResult::InvalidArgument;
	}

	std::vector<SubmissionId> normalized;
	normalized.reserve(dependencies.size());
	for (const auto& dependency: dependencies)
	{
		if (dependency.sequence == 0)
		{
			return GpuDeferredDeletionResult::InvalidArgument;
		}

		auto existing = std::find_if(normalized.begin(), normalized.end(),
		                             [&dependency](const auto& value) { return value.queue == dependency.queue; });
		if (existing == normalized.end())
		{
			normalized.push_back(dependency);
		} else
		{
			existing->sequence = std::max(existing->sequence, dependency.sequence);
		}
	}

	{
		std::lock_guard lock(m_mutex);
		m_entries.push_back({std::move(normalized), std::move(task)});
	}
	DrainReady();
	return GpuDeferredDeletionResult::Success;
}

GpuDeferredDeletionResult GpuDeferredDeletionQueue::CompleteSubmission(SubmissionId submission)
{
	if (submission.sequence == 0)
	{
		return GpuDeferredDeletionResult::InvalidArgument;
	}

	{
		std::lock_guard lock(m_mutex);
		auto            completed = std::find_if(m_completed.begin(), m_completed.end(),
		                                         [&submission](const auto& value) { return value.queue == submission.queue; });
		if (completed == m_completed.end())
		{
			m_completed.push_back({submission.queue, submission.sequence});
		} else
		{
			completed->sequence = std::max(completed->sequence, submission.sequence);
		}
	}
	DrainReady();
	return GpuDeferredDeletionResult::Success;
}

size_t GpuDeferredDeletionQueue::PendingCount() const
{
	std::lock_guard lock(m_mutex);
	return m_entries.size();
}

bool GpuDeferredDeletionQueue::AreDependenciesComplete(const std::vector<SubmissionId>& dependencies) const
{
	std::lock_guard lock(m_mutex);
	for (const auto& dependency: dependencies)
	{
		if (dependency.sequence == 0 || !IsComplete(dependency))
		{
			return false;
		}
	}
	return true;
}

bool GpuDeferredDeletionQueue::AreDependenciesCompleteForPublication(const std::vector<SubmissionId>& dependencies,
                                                                     SubmissionId publishing) const
{
	if (publishing.sequence == 0)
	{
		return false;
	}

	std::lock_guard lock(m_mutex);
	for (const auto& dependency: dependencies)
	{
		if (dependency.sequence == 0)
		{
			return false;
		}
		if (dependency == publishing)
		{
			continue;
		}
		if (!IsComplete(dependency))
		{
			return false;
		}
	}
	return true;
}

bool GpuDeferredDeletionQueue::IsReady(const Entry& entry) const
{
	for (const auto& dependency: entry.dependencies)
	{
		if (!IsComplete(dependency))
		{
			return false;
		}
	}
	return true;
}

bool GpuDeferredDeletionQueue::IsComplete(SubmissionId submission) const
{
	const auto completed = std::find_if(m_completed.begin(), m_completed.end(),
	                                    [&submission](const auto& value) { return value.queue == submission.queue; });
	return completed != m_completed.end() && completed->sequence >= submission.sequence;
}

void GpuDeferredDeletionQueue::DrainReady()
{
	{
		std::lock_guard lock(m_mutex);
		if (m_draining)
		{
			return;
		}
		m_draining = true;
	}

	for (;;)
	{
		Task task;
		{
			std::lock_guard lock(m_mutex);
			if (m_entries.empty() || !IsReady(m_entries.front()))
			{
				m_draining = false;
				return;
			}
			task = std::move(m_entries.front().task);
			m_entries.erase(m_entries.begin());
		}
		task();
	}
}

} // namespace Kyty::Libs::Graphics
