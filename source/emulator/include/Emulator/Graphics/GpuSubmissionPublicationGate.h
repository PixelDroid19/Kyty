#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUSUBMISSIONPUBLICATIONGATE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUSUBMISSIONPUBLICATIONGATE_H_

#include "Emulator/Graphics/GpuSubmissionTracker.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace Kyty::Libs::Graphics {

enum class GpuSubmissionPublicationResult : uint8_t
{
	Success,
	InvalidArgument,
	UnknownSubmission,
	InvalidTransition,
	NotReady,
};

// Separates Vulkan fence completion from guest-visible publication. The
// dispatcher acquires one ready submission at a time, executes callbacks with
// no gate lock held, then marks it published and wakes exact waiters.
class GpuSubmissionPublicationGate
{
public:
	explicit GpuSubmissionPublicationGate(GpuQueueId queue): m_queue(queue) {}
	~GpuSubmissionPublicationGate() = default;

	GpuSubmissionPublicationGate(const GpuSubmissionPublicationGate&)            = delete;
	GpuSubmissionPublicationGate& operator=(const GpuSubmissionPublicationGate&) = delete;
	GpuSubmissionPublicationGate(GpuSubmissionPublicationGate&&)                 = delete;
	GpuSubmissionPublicationGate& operator=(GpuSubmissionPublicationGate&&)      = delete;

	GpuSubmissionPublicationResult RegisterSubmitted(SubmissionId submission);
	GpuSubmissionPublicationResult MarkFenceComplete(SubmissionId submission);
	GpuSubmissionPublicationResult TryAcquireNextForPublication(SubmissionId* submission);
	GpuSubmissionPublicationResult MarkPublished(SubmissionId submission);
	GpuSubmissionPublicationResult WaitUntilPublished(SubmissionId submission);
	[[nodiscard]] bool              IsPublishingOnCurrentThread();

private:
	enum class State : uint8_t
	{
		Submitted,
		FenceComplete,
		Publishing,
	};

	struct Entry
	{
		SubmissionId submission;
		State        state = State::Submitted;
	};

	[[nodiscard]] bool IsValid(SubmissionId submission) const;
	Entry*             Find(SubmissionId submission);

	GpuQueueId              m_queue;
	std::mutex              m_mutex;
	std::condition_variable m_condition;
	std::vector<Entry>      m_entries;
	uint64_t                m_published_sequence = 0;
	std::thread::id         m_publisher_thread;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUSUBMISSIONPUBLICATIONGATE_H_ */
