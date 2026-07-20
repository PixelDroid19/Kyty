#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUSUBMISSIONCOORDINATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUSUBMISSIONCOORDINATOR_H_

#include "Kyty/Core/Threads.h"

#include "Emulator/Graphics/GpuSubmissionTracker.h"

namespace Kyty::Libs::Graphics {

// Thread-safe authority for logical GPU submission state. Runtime callbacks
// remain outside this type until they can be dispatched without holding its
// mutex.
class GpuSubmissionCoordinator
{
public:
	GpuSubmissionCoordinator()  = default;
	~GpuSubmissionCoordinator() = default;

	GpuSubmissionCoordinator(const GpuSubmissionCoordinator&)            = delete;
	GpuSubmissionCoordinator& operator=(const GpuSubmissionCoordinator&) = delete;
	GpuSubmissionCoordinator(GpuSubmissionCoordinator&&)                 = delete;
	GpuSubmissionCoordinator& operator=(GpuSubmissionCoordinator&&)      = delete;

	GpuSubmissionResult BeginRecording(GpuQueueId queue, uint32_t slot, SubmissionId* id,
	                                   SubmissionDependency* blocking_dependency);
	GpuSubmissionResult AddCompletionAction(SubmissionId id, GpuCompletionPhase phase, uint64_t token);
	GpuSubmissionResult RegisterProducer(SubmissionId id, uint64_t address, uint32_t size_bytes, uint64_t value);
	GpuSubmissionResult MarkSubmitted(SubmissionId id);
	GpuSubmissionResult MarkCompletedWithoutActions(SubmissionId id);
	GpuSubmissionResult RetireCompleted(SubmissionId id);
	GpuSubmissionResult GetState(SubmissionId id, GpuSubmissionState* state);
	GpuSubmissionResult FindPendingProducer(uint64_t address, uint32_t size_bytes, uint64_t reference, uint64_t mask,
	                                        SubmissionDependency* dependency);

private:
	Core::Mutex          m_mutex;
	GpuSubmissionTracker m_tracker;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUSUBMISSIONCOORDINATOR_H_ */
