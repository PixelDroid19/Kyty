#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMMANDPROCESSORSUBMISSIONSLOTS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMMANDPROCESSORSUBMISSIONSLOTS_H_

#include "Emulator/Graphics/GpuSubmissionCoordinator.h"

#include <array>
#include <cstdint>
#include <deque>

namespace Kyty::Libs::Graphics {

// Associates each CommandProcessor command-buffer slot with the exact logical
// submission recorded into that slot. Fence waits remain owned by the caller.
class CommandProcessorSubmissionSlots
{
public:
	static constexpr uint32_t SlotCount = 4;

	CommandProcessorSubmissionSlots(GpuSubmissionCoordinator* coordinator, GpuQueueId queue);
	~CommandProcessorSubmissionSlots() = default;

	CommandProcessorSubmissionSlots(const CommandProcessorSubmissionSlots&)            = delete;
	CommandProcessorSubmissionSlots& operator=(const CommandProcessorSubmissionSlots&) = delete;
	CommandProcessorSubmissionSlots(CommandProcessorSubmissionSlots&&)                 = delete;
	CommandProcessorSubmissionSlots& operator=(CommandProcessorSubmissionSlots&&)      = delete;

	GpuSubmissionResult BeginRecording(uint32_t slot, SubmissionId* id, SubmissionDependency* blocking_dependency);
	GpuSubmissionResult RegisterProducer(uint32_t slot, uint64_t address, uint32_t size_bytes, uint64_t value);
	GpuSubmissionResult MarkSubmitted(uint32_t slot);
	GpuSubmissionResult MarkFenceCompleted(uint32_t slot);
	GpuSubmissionResult RetirePublished(SubmissionId id);
	GpuSubmissionResult FindSlot(SubmissionId id, uint32_t* slot) const;
	GpuSubmissionResult GetState(SubmissionId id, GpuSubmissionState* state) const;
	GpuSubmissionResult GetOldestSubmitted(uint32_t* slot, SubmissionId* id) const;
	GpuSubmissionResult FindPendingProducer(uint64_t address, uint32_t size_bytes, uint64_t reference, uint64_t mask,
	                                        SubmissionDependency* dependency) const;
	GpuSubmissionResult CompleteFenceThenBeginRecording(uint32_t completed_slot, uint32_t recording_slot, SubmissionId* id,
	                                                     SubmissionDependency* blocking_dependency);

private:
	struct Slot
	{
		SubmissionId id;
		bool         active = false;
	};

	GpuSubmissionCoordinator*   m_coordinator = nullptr;
	GpuQueueId                  m_queue {0};
	std::array<Slot, SlotCount> m_slots {};
	std::deque<uint32_t>        m_submitted_fifo;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMMANDPROCESSORSUBMISSIONSLOTS_H_ */
