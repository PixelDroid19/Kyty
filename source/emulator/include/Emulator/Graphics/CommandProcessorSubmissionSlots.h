#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMMANDPROCESSORSUBMISSIONSLOTS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMMANDPROCESSORSUBMISSIONSLOTS_H_

#include "Emulator/Graphics/GpuSubmissionCoordinator.h"

#include <array>
#include <cstdint>

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
	GpuSubmissionResult MarkSubmitted(uint32_t slot);
	GpuSubmissionResult MarkCompletedWithoutActionsAndRetire(uint32_t slot);

private:
	struct Slot
	{
		SubmissionId id;
		bool         active = false;
	};

	GpuSubmissionCoordinator*   m_coordinator = nullptr;
	GpuQueueId                  m_queue {0};
	std::array<Slot, SlotCount> m_slots {};
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMMANDPROCESSORSUBMISSIONSLOTS_H_ */
