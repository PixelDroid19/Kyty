#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDEFERREDDELETIONQUEUE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDEFERREDDELETIONQUEUE_H_

#include "Emulator/Graphics/GpuSubmissionTracker.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace Kyty::Libs::Graphics {

enum class GpuDeferredDeletionResult : uint8_t
{
	Success,
	InvalidArgument,
};

class GpuSubmissionHighWater
{
public:
	[[nodiscard]] GpuDeferredDeletionResult        RecordUse(SubmissionId submission);
	[[nodiscard]] const std::vector<SubmissionId>& Dependencies() const { return m_dependencies; }
	[[nodiscard]] bool LatestForQueue(GpuQueueId queue, SubmissionId* submission) const;

private:
	std::vector<SubmissionId> m_dependencies;
};

// Owns self-contained destruction tasks until every queue high-water mark
// protecting the resource has completed. Callbacks are always invoked without
// holding the queue mutex and retain FIFO publication order.
class GpuDeferredDeletionQueue
{
public:
	using Task = std::function<void()>;

	GpuDeferredDeletionQueue()  = default;
	~GpuDeferredDeletionQueue() = default;

	GpuDeferredDeletionQueue(const GpuDeferredDeletionQueue&)            = delete;
	GpuDeferredDeletionQueue& operator=(const GpuDeferredDeletionQueue&) = delete;
	GpuDeferredDeletionQueue(GpuDeferredDeletionQueue&&)                 = delete;
	GpuDeferredDeletionQueue& operator=(GpuDeferredDeletionQueue&&)      = delete;

	GpuDeferredDeletionResult Enqueue(std::vector<SubmissionId> dependencies, Task task);
	GpuDeferredDeletionResult CompleteSubmission(SubmissionId submission);

	[[nodiscard]] size_t PendingCount() const;
	[[nodiscard]] bool   AreDependenciesComplete(const std::vector<SubmissionId>& dependencies) const;
	// Publication runs after this exact fence has completed but before its
	// callbacks and deferred destruction have been fully published.
	[[nodiscard]] bool   AreDependenciesCompleteForPublication(const std::vector<SubmissionId>& dependencies,
	                                                           SubmissionId publishing) const;

private:
	struct CompletedQueue
	{
		GpuQueueId queue {0};
		uint64_t   sequence = 0;
	};

	struct Entry
	{
		std::vector<SubmissionId> dependencies;
		Task                      task;
	};

	[[nodiscard]] bool IsReady(const Entry& entry) const;
	[[nodiscard]] bool IsComplete(SubmissionId submission) const;
	void               DrainReady();

	mutable std::mutex          m_mutex;
	std::vector<CompletedQueue> m_completed;
	std::vector<Entry>          m_entries;
	bool                        m_draining = false;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUDEFERREDDELETIONQUEUE_H_ */
