#include "GraphicsRunDrawFront.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Pm4.h"

#include "GraphicsRunInternal.h"

#include <new>
#include <thread>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

DrawFrontPacket DrawFrontClassifyPacket(const uint32_t* packet, uint32_t available_dw)
{
	const uint32_t header = packet[0];
	const uint32_t type   = header >> 30u;
	if (type == 2u || header == 0u)
	{
		return DrawFrontPacket::Padding;
	}
	const uint32_t cmd_id = header & ~1u;
	if (type != 3u || available_dw < 2u || Pm4::Pm4SpecialType3PacketDwords(cmd_id) != 0u || KYTY_PM4_LEN(cmd_id) > available_dw)
	{
		return DrawFrontPacket::Other;
	}
	switch ((cmd_id >> 8u) & 0xffu)
	{
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX:
		case Pm4::IT_INDEX_BASE:
		case Pm4::IT_INDEX_BUFFER_SIZE:
		case Pm4::IT_INDEX_TYPE:
		case Pm4::IT_NUM_INSTANCES: return DrawFrontPacket::Registers;
		case Pm4::IT_DRAW_INDEX_2:
		case Pm4::IT_DRAW_INDEX_OFFSET_2: return DrawFrontPacket::IndexedDraw;
		case Pm4::IT_NOP: break;
		default: return DrawFrontPacket::Other;
	}
	const uint32_t r = KYTY_PM4_R(cmd_id);
	if (r == Pm4::R_ZERO)
	{
		// A marker selects the type of the next user data (0x4, 0xd), carries embedded data (0x0) or
		// presents; a body that is no marker has no effect.
		const uint32_t marker = packet[1] & 0xfffu;
		const bool     plain  = (packet[1] & 0xffff0000u) != 0x68750000u;
		return plain || marker == 0x0u || marker == 0x4u || marker == 0xdu ? DrawFrontPacket::Registers : DrawFrontPacket::Other;
	}
	if (r == Pm4::R_DRAW_INDEX)
	{
		return DrawFrontPacket::IndexedDraw;
	}
	const bool register_tables = r == Pm4::R_CX_REGS_INDIRECT || r == Pm4::R_SH_REGS_INDIRECT || r == Pm4::R_UC_REGS_INDIRECT;
	const bool debug_marker    = r == Pm4::R_PUSH_MARKER || r == Pm4::R_POP_MARKER;
	const bool shader_register = g_hw_sh_custom_func[r] != nullptr && g_cp_op_custom_func[r] == nullptr;
	return register_tables || debug_marker || shader_register ? DrawFrontPacket::Registers : DrawFrontPacket::Other;
}

DrawFront::DrawFront(CommandProcessor* shadow): m_shadow(shadow), m_records(new Record[kRecords])
{
	EXIT_IF(shadow == nullptr);
	Core::Thread thread(ThreadRun, this);
	thread.Detach();
}

template <typename Condition>
void DrawFront::Sleep(const Condition& condition)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	for (;;)
	{
		// Set before the condition is read, so a change after the read finds the flag and wakes.
		m_sleeping.store(true);
		if (condition())
		{
			break;
		}
		m_wake.wait(lock);
	}
	m_sleeping.store(false);
}

void DrawFront::Wake()
{
	if (!m_sleeping.load())
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_sleeping.store(false);
	}
	m_wake.notify_one();
}

void DrawFront::Start(const uint32_t* draw_packet, const uint32_t* run_end)
{
	EXIT_IF(m_live);
	m_resume = draw_packet;
	m_end    = run_end;
	m_processor_packet.store(draw_packet, std::memory_order_relaxed);
	// The replay is idle, so the records of the previous generation end here.
	m_taken = m_prepared.load();
	m_released.store(m_taken);
	m_epoch_seen = GpuDirtyPageTracker::HostWriteEpoch();
	m_fresh_from = m_taken;
	const uint64_t generation = m_generation.load() + 1u;
	m_generation.store(generation);
	m_job.store(generation);
	m_live = true;
	Wake();
}

void DrawFront::Stop()
{
	if (!m_live)
	{
		return;
	}
	m_live = false;
	// The replay reads the generation after it declares that it reads, so one side sees the other.
	m_generation.fetch_add(1);
	while (m_reading.load())
	{
		std::this_thread::yield();
	}
}

PreparedDrawInputs* DrawFront::Take(const uint32_t* draw_packet, uint32_t instance_count)
{
	EXIT_IF(!m_live);
	m_processor_packet.store(draw_packet, std::memory_order_relaxed);
	const uint64_t prepared = m_prepared.load(std::memory_order_acquire);
	const uint64_t epoch    = GpuDirtyPageTracker::HostWriteEpoch();
	if (epoch != m_epoch_seen)
	{
		// The record in preparation counts as begun before this thread saw the epoch.
		m_epoch_seen = epoch;
		m_fresh_from = prepared + 1u;
	}
	while (m_taken < prepared)
	{
		const bool fresh  = m_taken >= m_fresh_from;
		Record&    record = m_records[m_taken % kRecords];
		if (record.packet > draw_packet)
		{
			return nullptr;
		}
		++m_taken;
		if (record.packet < draw_packet)
		{
			continue;
		}
		if (!fresh || record.guest_write_epoch != epoch || record.instance_count != instance_count)
		{
			return nullptr;
		}
		return &record.inputs;
	}
	return nullptr;
}

void DrawFront::Release()
{
	// Ordered before the flag is read, as the sleeper sets the flag before it reads the count.
	m_released.store(m_taken);
	if (m_sleeping.load() && m_prepared.load(std::memory_order_relaxed) - m_taken <= kRecords / 2u)
	{
		Wake();
	}
}

void DrawFront::Prepare(const HW::Context& ctx, const HW::UserConfig& ucfg, HW::Shader* sh_ctx, uint32_t index_count,
                        uint32_t instance_count)
{
	// The processor analyses the draws it has already reached.
	if (m_replay_packet <= m_processor_packet.load(std::memory_order_relaxed))
	{
		return;
	}
	const uint64_t prepared = m_prepared.load(std::memory_order_relaxed);
	if (prepared - m_released.load(std::memory_order_acquire) >= kRecords)
	{
		// The replay waits outside the packet handler, which may still read the packet.
		m_replay_parked = true;
		return;
	}
	Record& record = m_records[prepared % kRecords];
	// Fresh inputs, like the ones the recording analyses into.
	record.inputs.~PreparedDrawInputs();
	new (&record.inputs) PreparedDrawInputs;
	record.packet            = m_replay_packet;
	record.guest_write_epoch = GpuDirtyPageTracker::HostWriteEpoch();
	record.instance_count    = instance_count;
	if (GraphicsRenderPrepareIndexedDrawInputs(ctx, ucfg, sh_ctx, index_count, instance_count, &record.inputs))
	{
		m_prepared.store(prepared + 1u, std::memory_order_release);
	}
}

const uint32_t* DrawFront::Replay(uint64_t generation, const uint32_t* packet)
{
	const uint32_t* const end = m_end;
	while (packet < end && m_generation.load(std::memory_order_acquire) == generation)
	{
		const auto available = static_cast<uint32_t>(end - packet);
		const auto kind      = DrawFrontClassifyPacket(packet, available);
		if (kind == DrawFrontPacket::Other)
		{
			return nullptr;
		}
		if (kind == DrawFrontPacket::Padding)
		{
			++packet;
			continue;
		}
		const uint32_t cmd_id = packet[0] & ~1u;
		m_replay_packet       = packet;
		const auto dispatch =
		    GraphicsRunDispatchType3Packet(m_shadow, cmd_id, packet + 1, available - 1u, available, g_cp_op_func[(cmd_id >> 8u) & 0xffu]);
		if (m_replay_parked)
		{
			// A draw changes no register of the shadow, so its packet runs again once a record is free.
			m_replay_parked = false;
			return packet;
		}
		if (dispatch.status != GraphicsRunType3DispatchStatus::Ready)
		{
			return nullptr;
		}
		packet += 1u + dispatch.consumed_dw;
	}
	return nullptr;
}

void DrawFront::ThreadRun(void* data)
{
	auto*    front = static_cast<DrawFront*>(data);
	uint64_t done  = 0;
	for (;;)
	{
		front->Sleep([&] { return front->m_job.load() != done; });
		// Declared before the generation is read: Stop advances the generation and then waits for this flag.
		front->m_reading.store(true);
		const uint64_t  job    = front->m_job.load();
		const uint32_t* packet = front->m_resume;
		while (job == front->m_generation.load())
		{
			packet = front->Replay(job, packet);
			if (packet == nullptr)
			{
				break;
			}
			// Parked on a full ring: nothing is read until the processor frees records or stops.
			front->m_reading.store(false);
			front->Sleep([&] {
				return front->m_generation.load() != job ||
				       front->m_prepared.load(std::memory_order_relaxed) - front->m_released.load() <= kRecords / 2u;
			});
			front->m_reading.store(true);
		}
		done = job;
		front->m_reading.store(false);
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
