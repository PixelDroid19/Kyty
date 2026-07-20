#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/GpuSubmissionTracker.h"

#include <mutex>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class CommandProcessor;
namespace HW {
struct CsStageRegisters;
}

// Serializes command admission with teardown transactions. A quiesced action
// keeps admission closed across both the GPU drain and the caller-owned host
// lifetime change (for example, unmapping guest virtual memory).
class GpuSubmissionAdmissionGate
{
public:
	GpuSubmissionAdmissionGate()  = default;
	~GpuSubmissionAdmissionGate() = default;

	KYTY_CLASS_NO_COPY(GpuSubmissionAdmissionGate);

	template <typename Action>
	decltype(auto) RunAdmitted(Action&& action)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return std::forward<Action>(action)();
	}

	template <typename Drain, typename Action>
	decltype(auto) RunQuiesced(Drain&& drain, Action&& action)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::forward<Drain>(drain)();
		return std::forward<Action>(action)();
	}

private:
	std::mutex m_mutex;
};

using GraphicsRunQuiescedAction = bool (*)(void*);

void GraphicsRunInit();
bool GraphicsDecodeComputeResourceLimits(HW::CsStageRegisters* regs, uint32_t cmd_offset, const uint32_t* values,
                                         uint32_t value_count);
bool GraphicsWriteDataPrecedesMatchingWaitMem64(const uint32_t* write_body, uint32_t write_body_dwords,
                                                const uint32_t* next_packet, uint32_t next_packet_dwords);

void     GraphicsRunSubmit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw);
void     GraphicsRunSubmitAndFlip(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw,
                                  int handle, int index, int flip_mode, int64_t flip_arg);
uint32_t GraphicsRunMapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw,
                                    uint32_t* read_ptr_addr);
void     GraphicsRunUnmapComputeQueue(uint32_t id);
void     GraphicsRunWait();
void     GraphicsRunDone();
void     GraphicsRunDingDong(uint32_t ring_id, uint32_t offset_dw);
int      GraphicsRunGetFrameNum();
bool     GraphicsRunAreSubmitsAllowed();
bool     GraphicsRunWithQuiescedSubmissions(GraphicsRunQuiescedAction action, void* data);

void GraphicsRunCommandProcessorFlush(CommandProcessor* cp);
void GraphicsRunCommandProcessorWait(CommandProcessor* cp);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
