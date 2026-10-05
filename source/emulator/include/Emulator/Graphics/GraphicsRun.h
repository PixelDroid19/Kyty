#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/GpuSubmissionTracker.h"
#include "Emulator/Graphics/Pm4.h"

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

constexpr uint32_t GraphicsDecodeIndirectCxRegisterOffset(uint32_t raw_offset)
{
	constexpr uint32_t selector_mask = 0x70000000u;
	const uint32_t     selector      = raw_offset & selector_mask;
	const uint32_t     offset        = raw_offset & ~selector_mask;

	if (selector == Pm4::CX_PS_SHADER_USAGE_BASE && offset < 32u)
	{
		return Pm4::SPI_PS_INPUT_CNTL_0 + offset;
	}
	return offset;
}

constexpr bool GraphicsIsDefaultIndirectRegisterPair(uint32_t offset, uint32_t value)
{
	return offset == 0x10000000u + value;
}

constexpr bool GraphicsAgcFullTargetBarrierGcrSupported(uint32_t gcr_cntl)
{
	const uint32_t invalidate_mode = gcr_cntl & ~0x8000u;
	return invalidate_mode == 0x280u || invalidate_mode == 0x300u;
}

constexpr bool GraphicsDrawIndexAutoFlagsSupported(uint32_t flags)
{
	return (flags & ~0x22u) == 0;
}

struct GraphicsAgcReleaseMemControl
{
	uint16_t gcr_cntl  = 0;
	uint8_t  data_sel  = 0;
	uint8_t  interrupt = 0;
};

enum class GraphicsSubmissionCompletion
{
	None,
	QueuedGraphicsInterrupt,
};

// ACB handles name independent ordered queues. Bind each live handle to one
// host compute command processor so a graphics producer can run while an ACB
// waits for its label. The console exposes more compute queues than the host
// has processors (a fighting title probes nine at startup), so once every one
// has a handle, a new handle shares the processor with the fewest handles: its
// submissions stay in order there instead of failing.
class GraphicsAgcAsyncQueueSlots
{
public:
	static constexpr int Capacity       = 8;
	static constexpr int HandleCapacity = 64;

	int Find(uint32_t handle) const
	{
		for (int i = 0; i < m_handles; i++)
		{
			if (m_handle[i] == handle)
			{
				return m_slot[i];
			}
		}
		return -1;
	}

	int Bind(uint32_t handle, const bool (&unavailable)[Capacity])
	{
		const int existing = Find(handle);
		if (existing >= 0)
		{
			return existing;
		}
		if (m_handles == HandleCapacity)
		{
			return -1;
		}
		int chosen = -1;
		int fewest = HandleCapacity + 1;
		for (int slot = Capacity - 1; slot >= 0; slot--)
		{
			if (unavailable[slot])
			{
				continue;
			}
			int bound = 0;
			for (int i = 0; i < m_handles; i++)
			{
				bound += m_slot[i] == slot ? 1 : 0;
			}
			if (bound < fewest)
			{
				fewest = bound;
				chosen = slot;
			}
		}
		if (chosen >= 0)
		{
			m_handle[m_handles] = handle;
			m_slot[m_handles]   = chosen;
			m_handles++;
		}
		return chosen;
	}

private:
	uint32_t m_handle[HandleCapacity] = {};
	int      m_slot[HandleCapacity]   = {};
	int      m_handles                = 0;
};

GraphicsAgcReleaseMemControl GraphicsDecodeAgcReleaseMemControl(uint32_t control_dw);
uint32_t GraphicsAgcReleaseMemCacheAction(uint16_t gcr_cntl);
// Interrupt context id of a custom ReleaseMem body (dwords after the header).
// Only the 8-dword envelope carries one; the other forms deliver 0.
uint32_t GraphicsAgcReleaseMemInterruptContextId(uint32_t cmd_id, const uint32_t* body);

void     GraphicsRunSubmit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw,
                           GraphicsSubmissionCompletion completion);
bool     GraphicsRunSubmitAgcAsync(uint32_t queue_handle, uint32_t* cmd_buffer, uint32_t num_dw);
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
