#include "Emulator/Graphics/GpuSubmissionCoordinator.h"

#include "Kyty/Core/DbgAssert.h"

namespace Kyty::Libs::Graphics {

namespace {

class NoCompletionActionsSink final: public GpuCompletionActionSink
{
public:
	void Execute(GpuCompletionPhase /*phase*/, uint64_t /*token*/) override { m_executed = true; }

	[[nodiscard]] bool Executed() const { return m_executed; }

private:
	bool m_executed = false;
};

} // namespace

GpuSubmissionResult GpuSubmissionCoordinator::BeginRecording(GpuQueueId queue, uint32_t slot, SubmissionId* id,
                                                              SubmissionDependency* blocking_dependency)
{
	Core::LockGuard lock(m_mutex);
	return m_tracker.BeginRecording(queue, slot, id, blocking_dependency);
}

GpuSubmissionResult GpuSubmissionCoordinator::AddCompletionAction(SubmissionId id, GpuCompletionPhase phase, uint64_t token)
{
	Core::LockGuard lock(m_mutex);
	return m_tracker.AddCompletionAction(id, phase, token);
}

GpuSubmissionResult GpuSubmissionCoordinator::RegisterProducer(SubmissionId id, uint64_t address, uint32_t size_bytes, uint64_t value)
{
	Core::LockGuard lock(m_mutex);
	return m_tracker.RegisterProducer(id, address, size_bytes, value);
}

GpuSubmissionResult GpuSubmissionCoordinator::MarkSubmitted(SubmissionId id)
{
	Core::LockGuard lock(m_mutex);
	return m_tracker.MarkSubmitted(id);
}

GpuSubmissionResult GpuSubmissionCoordinator::MarkCompletedWithoutActions(SubmissionId id)
{
	Core::LockGuard lock(m_mutex);

	bool       has_actions = false;
	const auto query       = m_tracker.HasCompletionActions(id, &has_actions);
	if (query != GpuSubmissionResult::Success)
	{
		return query;
	}
	if (has_actions)
	{
		return GpuSubmissionResult::CompletionActionsPending;
	}

	NoCompletionActionsSink sink;
	const auto              result = m_tracker.MarkCompleted(id, &sink);
	EXIT_IF(sink.Executed());
	return result;
}

GpuSubmissionResult GpuSubmissionCoordinator::RetireCompleted(SubmissionId id)
{
	Core::LockGuard lock(m_mutex);
	return m_tracker.RetireCompleted(id);
}

GpuSubmissionResult GpuSubmissionCoordinator::GetState(SubmissionId id, GpuSubmissionState* state)
{
	Core::LockGuard lock(m_mutex);
	return m_tracker.GetState(id, state);
}

GpuSubmissionResult GpuSubmissionCoordinator::FindPendingProducer(uint64_t address, uint32_t size_bytes, uint64_t reference, uint64_t mask,
                                                                  SubmissionDependency* dependency)
{
	Core::LockGuard lock(m_mutex);
	return m_tracker.FindPendingProducer(address, size_bytes, reference, mask, dependency);
}

} // namespace Kyty::Libs::Graphics
