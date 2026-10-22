#include "GraphicsRunInternal.h"

#include "Kyty/Core/BringUp.h"

#include "Emulator/Config.h"
#include "Emulator/Agent/EventRing.h"
#include "Emulator/Graphics/GdsRange.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/GpuWriteHistory.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Log.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// An indirect register block pair outside its bank is ignored; the first few
// reach the agent so a lost context or shader write stays visible.
static void ReportDroppedIndirectRegister(const char* bank, uint32_t pair, uint32_t offset, uint32_t value, std::atomic_uint32_t* dropped)
{
	if (dropped->fetch_add(1, std::memory_order_relaxed) >= 4u)
	{
		return;
	}
	char message[Emulator::Agent::kAgentEventMessageMax] {};
	std::snprintf(message, sizeof(message), "bank=%s pair=%" PRIu32 " offset=0x%08" PRIx32 " value=0x%08" PRIx32, bank, pair, offset, value);
	Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Warn, "indirect_register_dropped", message);
}

static void RequirePacketBody(uint32_t cmd_id, uint32_t body_dw, uint32_t required_dw)
{
	if (body_dw < required_dw)
	{
		EXIT("short PM4 packet body: cmd_id=0x%08" PRIx32 " required_dw=%" PRIu32 " body_dw=%" PRIu32 "\n", cmd_id,
		     required_dw, body_dw);
	}
}

KYTY_CP_OP_PARSER(cp_op_acquire_mem)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xC0055800 && cmd_id != 0xc0061050)
	{
		EXIT("unsupported ACQUIRE_MEM header: 0x%08" PRIx32 "\n", cmd_id);
	}

	bool custom = (cmd_id == 0xc0061050);
	RequirePacketBody(cmd_id, dw, custom ? 7u : 6u);

	uint32_t                  cache_action = buffer[0] & 0x7fffffffu;
	uint64_t                  size_lo      = buffer[1];
	uint32_t                  size_hi      = buffer[2];
	uint64_t                  base_lo      = buffer[3];
	uint32_t                  base_hi      = buffer[4];
	uint32_t                  poll         = buffer[5];
	[[maybe_unused]] uint32_t gcr_cntl     = (custom ? buffer[6] : 0);

	uint32_t target_mask     = cache_action & 0x00007FC0u;
	uint32_t extended_action = cache_action & 0x2E000000u;
	uint32_t action          = ((cache_action & 0x00C00000u) >> 0x12u) | ((cache_action & 0x00058000u) >> 0xfu);

	// EXIT_NOT_IMPLEMENTED(stall_mode != 1);
	if (size_hi != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: size_hi != 0 condition ignored (continuing)\n"); }
	if (base_hi != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: base_hi != 0 condition ignored (continuing)\n"); }
	if (poll != 10) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: poll != 10 condition ignored (continuing)\n"); }

	switch (cache_action)
	{
		case 0x02c40040:
		case 0x02c43fc0:
		case 0x02c47fc0:
		{
			// target_mask:     0x00000040 (rt0), 0x00003fc0 (all rt), 0x00007fc0 (all rt and depth)
			// extended_action: 0x02000000 (FlushAndInvalidateCbCache)
			// action:          0x38 (WriteBackAndInvalidateL1andL2)
			EXIT_IF(target_mask != 0x00000040 && target_mask != 0x00003FC0 && target_mask != 0x00007FC0);
			EXIT_IF(extended_action != 0x02000000);
			EXIT_IF(action != 0x38);
			if (size_lo == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: size_lo == 0 condition ignored (continuing)\n"); }
			if (base_lo == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: base_lo == 0 condition ignored (continuing)\n"); }

			cp->RenderTextureBarrier(base_lo << 8u, size_lo << 8u);
			cp->WriteBack();
		}
		break;
		case 0x02003fc0:
		{
			// target_mask:     0x00003FC0 (all rt)
			// extended_action: 0x02000000 (FlushAndInvalidateCbCache)
			// action:          0x00 (none)
			EXIT_IF(target_mask != 0x00003FC0);
			EXIT_IF(extended_action != 0x02000000);
			EXIT_IF(action != 0x00);
			if (size_lo == 0)
			{
				cp->MemoryBarrier();
			} else
			{
				if (base_lo == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: base_lo == 0 condition ignored (continuing)\n"); }
				cp->RenderTextureBarrier(base_lo << 8u, size_lo << 8u);
			}
		}
		break;
		case 0x00C40000:
		{
			// target_mask:     0x00000000 (none)
			// extended_action: 0x00000000 (none)
			// action:          0x38 (WriteBackAndInvalidateL1andL2)
			EXIT_IF(target_mask != 0x00000000);
			EXIT_IF(extended_action != 0x00000000);
			EXIT_IF(action != 0x38);
			if (size_lo != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: size_lo != 1 condition ignored (continuing)\n"); }
			if (base_lo != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: base_lo != 0 condition ignored (continuing)\n"); }

			cp->MemoryBarrier();
			cp->WriteBack();
		}
		break;
		case 0x00400000:
		{
			// target_mask:     0x00000000 (none)
			// extended_action: 0x00000000 (none)
			// action:          0x10 (InvalidateL1)
			EXIT_IF(target_mask != 0x00000000);
			EXIT_IF(extended_action != 0x00000000);
			EXIT_IF(action != 0x10);
			if (size_lo != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: size_lo != 1 condition ignored (continuing)\n"); }
			if (base_lo != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: base_lo != 0 condition ignored (continuing)\n"); }

			cp->MemoryBarrier();
		}
		break;
		case 0x04c44000:
		{
			// target_mask:     0x00004000 (Depth Target)
			// extended_action: 0x04000000 (FlushAndInvalidateDbCache)
			// action:          0x38 (WriteBackAndInvalidateL1andL2)
			EXIT_IF(target_mask != 0x00004000);
			EXIT_IF(extended_action != 0x04000000);
			EXIT_IF(action != 0x38);

			cp->DepthStencilBarrier(base_lo << 8u, size_lo << 8u);
			cp->WriteBack();
		}
		break;

		case 0x06007fc0:
		{
			// target_mask:     0x00007fc0 (all rt and depth)
			// extended_action: 0x06000000 (Flush Cb & Db)
			// action:          0x00 (none)
			// Gen5 emits the same GL0/GL1 invalidate bits with an additional
			// 0x8000 cache-control qualifier on this full-target barrier.
			if (!GraphicsAgcFullTargetBarrierGcrSupported(gcr_cntl))
			{
				KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unsupported full-target barrier (continuing)\n");
			}
			// AGC size_bytes == -1 encodes as size_lo == 0 (full range). The base
			// dword is still written from the guest pointer (observed post-Play
			// frontier: base_lo=1, size_lo=0). Only a non-zero size selects a
			// ranged barrier, which is not yet supported for this cache action.
			if (size_lo != 0)
			{
				KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unsupported full-target barrier (continuing)\n");
			}

			EXIT_IF(target_mask != 0x00007fc0);
			EXIT_IF(extended_action != 0x06000000);
			EXIT_IF(action != 0x00);

			cp->MemoryBarrier();
		}
		break;

		case 0x00007fc0:
		{
			// target_mask:     0x00007fc0 (all rt and depth)
			// extended_action: none
			// action:          none
			// Observed immediately after the post-Play WaitMem fence advances
			// into the loading path (cache_action word is only the target mask).
			EXIT_IF(target_mask != 0x00007fc0);
			EXIT_IF(extended_action != 0x00000000);
			EXIT_IF(action != 0x00);

			cp->MemoryBarrier();
		}
		break;

		case 0x02007fc0:
		{
			// target_mask:     0x00007fc0 (all rt and depth)
			// extended_action: 0x02000000 (FlushAndInvalidateCbCache)
			// action:          0x00 (none)
			// Sibling of 0x02003fc0 (all rt only) already handled above.
			EXIT_IF(target_mask != 0x00007fc0);
			EXIT_IF(extended_action != 0x02000000);
			EXIT_IF(action != 0x00);

			if (size_lo == 0)
			{
				cp->MemoryBarrier();
			} else
			{
				if (base_lo == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: base_lo == 0 condition ignored (continuing)\n"); }
				cp->RenderTextureBarrier(base_lo << 8u, size_lo << 8u);
			}
		}
		break;

		case 0x00000000:
			// No-op / simple cache sync (all fields zero) — plain barrier.
			cp->MemoryBarrier();
			break;

		default: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown barrier (continuing)\n");
	}

	// ACQUIRE_MEM is a GPU ordering operation. Keep its Vulkan barrier in the
	// recording buffer; an explicit host submission here would serialize every
	// DCB and turn a stream of barriers into one fence wait per packet. CPU-visible
	// ordering is handled by WAIT_REG_MEM and WriteBack at their actual boundaries.
	return (custom ? 7 : 6);
}

KYTY_CP_OP_PARSER(cp_op_dispatch_direct)
{
	KYTY_PROFILER_FUNCTION();

	const bool standard = (cmd_id == KYTY_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0));
	const bool custom   = (cmd_id == KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_DISPATCH_DIRECT));

	if (!standard && !custom)
	{
		EXIT("unsupported DISPATCH_DIRECT header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, standard ? 4u : 5u);

	uint32_t thread_group_x = buffer[0];
	uint32_t thread_group_y = buffer[1];
	uint32_t thread_group_z = buffer[2];
	uint32_t mode           = buffer[3];

	cp->DispatchDirect(thread_group_x, thread_group_y, thread_group_z, mode);

	return (standard ? 4 : 5);
}

KYTY_CP_OP_PARSER(cp_op_dispatch_reset)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xC0001024)
	{
		EXIT("unsupported DISPATCH_RESET header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 1u);

	cp->Reset();

	return 1;
}

// DMA_DATA with a GDS endpoint. Selector decoding (0/3 memory, 1 GDS, 2 immediate source) is the
// established one. Every other GDS combination, span, count or memory endpoint is refused
// strictly; GDS work is never skipped.
static uint32_t DmaDataWithGds(CommandProcessor* cp, const GraphicsHardwareDmaData& packet, bool destination_is_memory,
                               bool destination_is_gds, bool source_is_memory, bool source_is_gds, bool source_is_immediate)
{
	const auto refuse = [&packet](const char* reason)
	{
		EXIT("DMA_DATA with a GDS endpoint refused (%s): src_sel=%u dst_sel=%u src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " bytes=%" PRIu32 "\n",
		     reason, static_cast<uint32_t>(packet.source), static_cast<uint32_t>(packet.destination), packet.source_address,
		     packet.destination_address, packet.byte_count);
	};
	if (packet.byte_count == 0 || (packet.byte_count & 3u) != 0)
	{
		refuse("byte count");
		return 6;
	}
	const uint64_t dw_count = packet.byte_count / 4u;

	if (destination_is_gds)
	{
		if (!GraphicsGdsByteRangeValid(packet.destination_address, packet.byte_count))
		{
			refuse("GDS destination span");
			return 6;
		}
		const uint64_t dst_dw_offset = packet.destination_address / 4u;
		if (source_is_immediate)
		{
			cp->ClearGds(dst_dw_offset, dw_count, static_cast<uint32_t>(packet.source_address));
			return 6;
		}
		if (source_is_gds)
		{
			if (!GraphicsGdsByteRangeValid(packet.source_address, packet.byte_count))
			{
				refuse("GDS source span");
				return 6;
			}
			// A same-buffer copy needs disjoint spans; overlap has no established contract.
			if (GraphicsGdsDwordRangesOverlap(packet.source_address / 4u, dst_dw_offset, dw_count))
			{
				refuse("overlapping GDS spans");
				return 6;
			}
			cp->CopyGds(packet.source_address / 4u, dst_dw_offset, dw_count);
			return 6;
		}
		if (!source_is_memory || (packet.source_address & 3u) != 0u ||
		    GpuMemoryValidateAllocatedRange(packet.source_address, packet.byte_count) != GpuMemoryRangeValidationStatus::Valid)
		{
			refuse("memory source");
			return 6;
		}
		cp->WriteGds(dst_dw_offset, packet.source_address, dw_count);
		return 6;
	}

	if (!destination_is_memory || !GraphicsGdsByteRangeValid(packet.source_address, packet.byte_count))
	{
		refuse("GDS source to memory");
		return 6;
	}
	if ((packet.destination_address & 3u) != 0u ||
	    GpuMemoryValidateAllocatedRange(packet.destination_address, packet.byte_count) != GpuMemoryRangeValidationStatus::Valid)
	{
		refuse("memory destination");
		return 6;
	}
	// The destination is written when this recording's submission is published, under a host-write lease.
	cp->ReadGds(reinterpret_cast<uint32_t*>(packet.destination_address), packet.source_address / 4u, dw_count);
	GpuWriteHistoryRecord(GpuWriteHistoryKind::DmaData, packet.destination_address, packet.byte_count, 0u, 0u, 0u);
	return 6;
}


KYTY_CP_OP_PARSER(cp_op_dma_data)
{
	KYTY_PROFILER_FUNCTION();

	RequirePacketBody(cmd_id, dw, 6u);
	GraphicsHardwareDmaData packet {};
	if (!GraphicsDecodeHardwareDmaData(cmd_id, buffer, dw, &packet))
	{
		EXIT("unsupported DMA_DATA header or body: cmd_id=0x%08" PRIx32 " body_dw=%" PRIu32 "\n", cmd_id, dw);
	}

	// Reference-clock prefetch packets use a fixed destination register rather
	// than a memory or GDS endpoint.
	if (buffer[0] == 0x60000000u && packet.destination_address == 0x0003022cu && (buffer[5] >> 21u) == 0x141u)
	{
		cp->PrefetchL2(reinterpret_cast<void*>(packet.source_address), buffer[5] & 0x1fffffu);
		return 6;
	}

	const bool destination_is_memory = packet.destination == 0u || packet.destination == 3u;
	const bool destination_is_gds    = packet.destination == 1u;
	const bool source_is_memory      = packet.source == 0u || packet.source == 3u;
	const bool source_is_gds         = packet.source == 1u;
	const bool source_is_immediate   = packet.source == 2u;
	if (destination_is_gds || source_is_gds)
	{
		return DmaDataWithGds(cp, packet, destination_is_memory, destination_is_gds, source_is_memory, source_is_gds, source_is_immediate);
	}

	if (packet.byte_count == 0 || (packet.byte_count & 3u) != 0)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: invalid DMA_DATA byte count ignored\n");
		return 6;
	}
	if (!destination_is_memory || (!source_is_memory && !source_is_immediate))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unsupported DMA_DATA selector ignored\n");
		return 6;
	}

	if (GpuMemoryValidateAllocatedRange(packet.destination_address, packet.byte_count) != GpuMemoryRangeValidationStatus::Valid)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: invalid DMA_DATA destination ignored\n");
		return 6;
	}
	GpuMemoryNotifyHostWrite(packet.destination_address, packet.byte_count);

	if (source_is_immediate)
	{
		const uint32_t value = static_cast<uint32_t>(packet.source_address);
		auto*          dst   = reinterpret_cast<uint8_t*>(packet.destination_address);
		for (uint32_t offset = 0; offset < packet.byte_count; offset += sizeof(value))
		{
			memcpy(dst + offset, &value, sizeof(value));
		}
	} else
	{
		if (GpuMemoryValidateAllocatedRange(packet.source_address, packet.byte_count) != GpuMemoryRangeValidationStatus::Valid)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: invalid DMA_DATA source ignored\n");
			return 6;
		}
		memmove(reinterpret_cast<void*>(packet.destination_address), reinterpret_cast<const void*>(packet.source_address),
		        packet.byte_count);
	}
	GraphicsRenderMemoryFlush(packet.destination_address, packet.byte_count);
	GpuWriteHistoryRecord(GpuWriteHistoryKind::DmaData, packet.destination_address, packet.byte_count, 0u, 0u, 0u);

	return 6;
}

// Custom IT_NOP/R_DMA_DATA from sceAgcDcbDmaData / sceAgcAcbDmaData.
// Layout (8 dwords total including header consumed by the dispatcher):
//   buffer[0]=policies, [1]=controls, [2]=byte_count, [3..4]=dst, [5..6]=src
// relative to post-header body (see GraphicsDcbDmaData).
KYTY_CP_OP_PARSER(cp_op_custom_dma_data)
{
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(cp == nullptr);
	if (cmd_id != KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_DMA_DATA))
	{
		EXIT("unsupported custom DMA_DATA header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 7u);

	const uint32_t byte_count = buffer[2];
	const uint64_t dst        = buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u);
	const uint64_t src        = buffer[5] | (static_cast<uint64_t>(buffer[6]) << 32u);

	if (byte_count == 0 || byte_count > (256u * 1024u * 1024u) || dst == 0 || (byte_count & 3u) != 0)
	{
		return 7;
	}

	if (GpuMemoryValidateAllocatedRange(dst, byte_count) != GpuMemoryRangeValidationStatus::Valid)
	{
		return 7;
	}

	if (src == 0)
	{
		GpuMemoryNotifyHostWrite(dst, byte_count);
		memset(reinterpret_cast<void*>(dst), 0, byte_count);
		GraphicsRenderMemoryFlush(dst, byte_count);
		GpuWriteHistoryRecord(GpuWriteHistoryKind::DmaDataCustom, dst, byte_count, 0u, 0u, 0u);
		return 7;
	}

	if (src == 0 || GpuMemoryValidateAllocatedRange(src, byte_count) != GpuMemoryRangeValidationStatus::Valid)
	{
		return 7;
	}

	GpuMemoryNotifyHostWrite(dst, byte_count);
	memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), byte_count);
	GraphicsRenderMemoryFlush(dst, byte_count);
	GpuWriteHistoryRecord(GpuWriteHistoryKind::DmaDataCustom, dst, byte_count, 0u, 0u, 0u);
	return 7;
}

KYTY_CP_OP_PARSER(cp_op_draw_index)
{
	KYTY_PROFILER_FUNCTION();

	const uint32_t packet_len = KYTY_PM4_LEN(cmd_id);

	if ((cmd_id & ~1u) == KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_DRAW_INDEX))
	{
		if (packet_len != 7) { EXIT("invalid DRAW_INDEX packet length: cmd_id=0x%08" PRIx32 "\n", cmd_id); }
		RequirePacketBody(cmd_id, dw, 6u);

		uint32_t index_count = buffer[0];
		auto*    index_addr  = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
		uint64_t modifier    = buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u);
		if (buffer[5] != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[5] != 0 condition ignored (continuing)\n"); }

		cp->DrawIndex(index_count, index_addr, modifier, 1);

		return 6;
	}

	if (cmd_id == KYTY_PM4(6, Pm4::IT_DRAW_INDEX_2, 0u))
	{
		RequirePacketBody(cmd_id, dw, 5u);
		uint32_t index_count = buffer[0];
		auto*    index_addr  = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));

		if (buffer[3] != index_count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[3] != index_count condition ignored (continuing)\n"); }
		if (buffer[4] != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[4] != 0 condition ignored (continuing)\n"); }

		cp->DrawIndex(index_count, index_addr, 0, 1);

		if (available_dw == 5u)
		{
			return 5;
		}
		const uint32_t lookahead_header = buffer[5];
		if (lookahead_header == 0xc0001000u)
		{
			if (!GraphicsRunType3PacketAt(buffer, available_dw, 5u, 0xc0001000u))
			{
				EXIT("truncated DRAW_INDEX_2 follow-up packet: header=0x%08" PRIx32 " available_dw=%" PRIu32 "\n",
				     lookahead_header, available_dw);
			}
			if (buffer[6] != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[6] != 0 condition ignored (continuing)\n"); }

			return 7;
		}

		if (lookahead_header == 0xc0021000u)
		{
			if (!GraphicsRunType3PacketAt(buffer, available_dw, 5u, 0xc0021000u))
			{
				EXIT("truncated DRAW_INDEX_2 follow-up packet: header=0x%08" PRIx32 " available_dw=%" PRIu32 "\n",
				     lookahead_header, available_dw);
			}
			if (buffer[6] != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[6] != 0 condition ignored (continuing)\n"); }

			return 9;
		}

		return 5;
	}

	EXIT("unsupported DRAW_INDEX_2 header: 0x%08" PRIx32 "\n", cmd_id);
	return dw;
}

// IT_DRAW_INDEX_OFFSET_2 (0x35): draw from IT_INDEX_BASE + index_offset.
// Body: index_count, index_offset, index_count, flags.
KYTY_CP_OP_PARSER(cp_op_draw_index_offset)
{
	KYTY_PROFILER_FUNCTION();
	if ((cmd_id & ~1u) != KYTY_PM4(5, Pm4::IT_DRAW_INDEX_OFFSET_2, 0u))
	{
		EXIT("unsupported DRAW_INDEX_OFFSET_2 header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 4u);
	if (dw != 4u)
	{
		EXIT("invalid DRAW_INDEX_OFFSET_2 body size: cmd_id=0x%08" PRIx32 " body_dw=%" PRIu32 "\n", cmd_id, dw);
	}
	if (buffer[0] != buffer[2]) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[0] != buffer[2] condition ignored (continuing)\n"); }
	if ((buffer[3] & ~0xE0000001u) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (buffer[3] & ~0xE0000001u) != 0 condition ignored (continuing)\n"); }
	cp->DrawIndexOffset(buffer[1], buffer[0], buffer[3] & 0xE0000001u);
	return 4;
}

KYTY_CP_OP_PARSER(cp_op_draw_index_auto)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xC0051010 && cmd_id != 0xc0012d00)
	{
		EXIT("unsupported DRAW_INDEX_AUTO header: 0x%08" PRIx32 "\n", cmd_id);
	}

	if (cmd_id == 0xC0051010)
	{
		RequirePacketBody(cmd_id, dw, 6u);
		uint32_t index_count    = buffer[0];
		uint64_t draw_modifier = static_cast<uint64_t>(buffer[1]) | (static_cast<uint64_t>(buffer[2]) << 32u);

		cp->DrawIndexAuto(index_count, draw_modifier);

		return 6;
	}

	if (cmd_id == 0xc0012d00)
	{
		RequirePacketBody(cmd_id, dw, 2u);
		uint32_t index_count = buffer[0];
		uint32_t flags       = buffer[1];

		if (!GraphicsDrawIndexAutoFlagsSupported(flags)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !GraphicsDrawIndexAutoFlagsSupported(flags) condition ignored (continuing)\n"); }

		cp->DrawIndexAuto(index_count, 0);

		// The Type3 packet has exactly two body dwords. CommandProcessor::Run
		// dispatches the following packet on the next iteration.
		return 2;
	}

	return dw;
}

KYTY_CP_OP_PARSER(cp_op_draw_reset)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	if (cmd_id != 0xc0001014) { EXIT("unsupported DRAW_RESET header: 0x%08" PRIx32 "\n", cmd_id); }

	cp->Reset();

	return 1;
}

// Gen5 IT_SET_BASE from GraphicsDcbSetBaseIndirectArgs: header + 3 body dwords.
// Indirect-arg base is recorded for later draw/dispatch; no guest-visible
// side effect beyond stream progress is required yet.
KYTY_CP_OP_PARSER(cp_op_set_base)
{
	KYTY_PROFILER_FUNCTION();
	if ((cmd_id & ~3u) != KYTY_PM4(4, Pm4::IT_SET_BASE, 0u))
	{
		EXIT("unsupported SET_BASE header: 0x%08" PRIx32 "\n", cmd_id);
	}

	RequirePacketBody(cmd_id, dw, 3u);
	if (dw != 3u) { EXIT("invalid SET_BASE body size: cmd_id=0x%08" PRIx32 " body_dw=%" PRIu32 "\n", cmd_id, dw); }
	if (buffer[0] != 1u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[0] != 1u condition ignored (continuing)\n"); }

	const uint32_t base_index = (cmd_id >> 1u) & 0x1u;
	const uint64_t address    = (buffer[1] & ~7ull) | (static_cast<uint64_t>(buffer[2]) << 32u);
	cp->SetIndirectArgsBaseAddress(base_index, address);

	return 3;
}

// DCB uses a base-relative offset; ACB carries an absolute 64-bit address.
KYTY_CP_OP_PARSER(cp_op_dispatch_indirect)
{
	KYTY_PROFILER_FUNCTION();

	if (Config::IsNextGen() && cmd_id == KYTY_PM4(4, Pm4::IT_DISPATCH_INDIRECT, 0u))
	{
		RequirePacketBody(cmd_id, dw, 3u);
		const uint64_t address = static_cast<uint64_t>(buffer[0]) | (static_cast<uint64_t>(buffer[1]) << 32u);
		cp->DispatchIndirectAbsolute(address, buffer[2]);
		return 3;
	}
	if (cmd_id != KYTY_PM4(3, Pm4::IT_DISPATCH_INDIRECT, 0u))
	{
		EXIT("unsupported dispatch-indirect header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 2u);
	cp->DispatchIndirect(buffer[0], buffer[1]);

	return 2;
}

// Gen5 IT_DRAW_INDEX_INDIRECT: header + 4 body dwords (offset, patch lo/hi, initiator).
KYTY_CP_OP_PARSER(cp_op_draw_index_indirect)
{
	KYTY_PROFILER_FUNCTION();
	if (cmd_id != KYTY_PM4(5, Pm4::IT_DRAW_INDEX_INDIRECT, 0u))
	{
		EXIT("unsupported DRAW_INDEX_INDIRECT header: 0x%08" PRIx32 "\n", cmd_id);
	}

	RequirePacketBody(cmd_id, dw, 4u);
	cp->DrawIndexIndirect(buffer[0], buffer[3]);

	return 4;
}

// Gen5 IT_DRAW_INDIRECT: header + 4 body dwords (offset, patch lo/hi, initiator).
KYTY_CP_OP_PARSER(cp_op_draw_indirect)
{
	KYTY_PROFILER_FUNCTION();
	if (cmd_id != KYTY_PM4(5, Pm4::IT_DRAW_INDIRECT, 0u))
	{
		EXIT("unsupported DRAW_INDIRECT header: 0x%08" PRIx32 "\n", cmd_id);
	}

	RequirePacketBody(cmd_id, dw, 4u);
	cp->DrawIndirect(buffer[0], buffer[3]);

	return 4;
}

// Gen5 IT_CLEAR_STATE from GraphicsDcbResetQueue: header + 4-bit state body.
KYTY_CP_OP_PARSER(cp_op_clear_state)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	if (cmd_id != 0xc0001200) { EXIT("unsupported CLEAR_STATE header: 0x%08" PRIx32 "\n", cmd_id); }
	if ((buffer[0] & ~0xfu) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (buffer[0] & ~0xfu) != 0 condition ignored (continuing)\n"); }

	// This packet executes inside CommandProcessor::Run. Calling Reset here
	// would wait on the command buffer currently being decoded and recursively
	// re-enter the same packet. CLEAR_STATE restores context-register defaults;
	// shader and UCONFIG state belong to separate register files.
	cp->GetCtx()->Reset();

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_context_state)
{
	KYTY_PROFILER_FUNCTION();

	RequirePacketBody(cmd_id, dw, 1u);
	const auto operation = buffer[0];
	const uint32_t packet_dw = operation == 2 ? 3 : 5;
	if (operation > 3 || dw < packet_dw - 1u || cmd_id != KYTY_PM4(packet_dw, Pm4::IT_NOP, Pm4::R_CONTEXT_STATE))
	{
		EXIT("invalid CONTEXT_STATE packet: cmd_id=0x%08" PRIx32 " operation=%" PRIu32 " body_dw=%" PRIu32 "\n", cmd_id,
		     operation, dw);
	}
	for (uint32_t i = 1; i < packet_dw - 1; i++)
	{
		EXIT_IF(buffer[i] != 0);
	}
	cp->ApplyContextState(operation);
	return packet_dw - 1;
}

KYTY_CP_OP_PARSER(cp_op_dump_const_ram)
{
	KYTY_PROFILER_FUNCTION();

	RequirePacketBody(cmd_id, dw, 4u);
	if (cmd_id != 0xC0038300) { EXIT("unsupported DUMP_CONST_RAM header: 0x%08" PRIx32 "\n", cmd_id); }

	auto  offset = buffer[0];
	auto  dw_num = buffer[1];
	auto* dst    = reinterpret_cast<uint32_t*>(buffer[2] | (static_cast<uint64_t>(buffer[3]) << 32u));

	if (dw_num >= 0x3000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dw_num >= 0x3000 condition ignored (continuing)\n"); }
	if (offset > 0xbffc) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: offset > 0xbffc condition ignored (continuing)\n"); }
	if ((offset & 0x3u) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (offset & 0x3u) != 0 condition ignored (continuing)\n"); }

	cp->DumpConstRam(dst, offset, dw_num);

	return 4;
}

KYTY_CP_OP_PARSER(cp_op_event_write)
{
	KYTY_PROFILER_FUNCTION();

	const uint32_t packet_dw = KYTY_PM4_LEN(cmd_id);
	RequirePacketBody(cmd_id, dw, 1u);
	if (((cmd_id >> 8u) & 0xffu) != Pm4::IT_EVENT_WRITE || (packet_dw != 2u && packet_dw != 4u))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: malformed EVENT_WRITE packet (continuing)\n");
		return packet_dw > 0u ? packet_dw - 1u : 0u;
	}

	uint32_t event_index = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_type  = (buffer[0]) & 0x3fu;
	uint64_t event_address = 0;

	if (packet_dw == 4u)
	{
		event_address = buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u);
	}

	cp->TriggerEvent(event_type, event_index, event_address);

	return packet_dw - 1u;
}

KYTY_CP_OP_PARSER(cp_op_event_write_eop)
{
	KYTY_PROFILER_FUNCTION();

	RequirePacketBody(cmd_id, dw, 5u);
	if (cmd_id != 0xC0044700) { EXIT("unsupported EVENT_WRITE_EOP header: 0x%08" PRIx32 "\n", cmd_id); }

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t event_write_dest   = ((buffer[0] >> 23u) & 0x10u) | ((buffer[2] >> 16u) & 0x01u);
	uint32_t eop_event_type     = (buffer[0]) & 0x3fu;
	uint32_t cache_action       = (buffer[0] >> 12u) & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_write_source = ((buffer[2] >> 29u) & 0x7u);
	uint32_t interrupt_selector = (buffer[2] >> 24u) & 0x7u;
	auto*    dst_gpu_addr       = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2] & 0xffffu) << 32u));
	uint64_t value              = (buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u));

	// EVENT_WRITE_EOP carries no interrupt context id.
	cp->WriteAtEndOfPipe64(cache_policy, event_write_dest, eop_event_type, cache_action, event_index, event_write_source, dst_gpu_addr,
	                       value, interrupt_selector, 0);

	return 5;
}

KYTY_CP_OP_PARSER(cp_op_event_write_eos)
{
	KYTY_PROFILER_FUNCTION();

	RequirePacketBody(cmd_id, dw, 4u);
	if (cmd_id != 0xC0034802) { EXIT("unsupported EVENT_WRITE_EOS header: 0x%08" PRIx32 "\n", cmd_id); }

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t event_write_dest   = 0;
	uint32_t eop_event_type     = (buffer[0]) & 0x3fu;
	uint32_t cache_action       = (buffer[0] >> 12u) & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_write_source = ((buffer[2] >> 29u) & 0x7u);
	uint32_t interrupt_selector = (buffer[2] >> 24u) & 0x7u;

	auto*    dst_gpu_addr = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2] & 0xffffu) << 32u));
	uint32_t value        = buffer[3];

	cp->WriteAtEndOfPipe32(cache_policy, event_write_dest, eop_event_type, cache_action, event_index, event_write_source, dst_gpu_addr,
	                       value, interrupt_selector);

	return 4;
}

KYTY_CP_OP_PARSER(cp_op_flip)
{
	KYTY_PROFILER_FUNCTION();

	RequirePacketBody(cmd_id, dw, 5u);
	if (cmd_id != 0xc004105c) { EXIT("unsupported FLIP header: 0x%08" PRIx32 "\n", cmd_id); }

	CommandProcessor::FlipInfo f;

	f.handle    = static_cast<int>(buffer[0]);
	f.index     = static_cast<int>(buffer[1]);
	f.flip_mode = static_cast<int>(buffer[2]);
	f.flip_arg  = static_cast<int64_t>(buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u));

	cp->SetFlip(f);
	cp->Flip();

	return 5;
}

KYTY_CP_OP_PARSER(cp_op_increment_ce_counter)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	if (cmd_id != 0xC0008400) { EXIT("unsupported INCREMENT_CE_COUNTER header: 0x%08" PRIx32 "\n", cmd_id); }
	if (buffer[0] != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[0] != 1 condition ignored (continuing)\n"); }

	cp->IncremenetCe();

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_increment_de_counter)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	if (cmd_id != 0xC0008500) { EXIT("unsupported INCREMENT_DE_COUNTER header: 0x%08" PRIx32 "\n", cmd_id); }
	if (buffer[0] != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[0] != 0 condition ignored (continuing)\n"); }

	cp->IncremenetDe();

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_index_type)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	if (cmd_id != 0xC0002A00) { EXIT("unsupported INDEX_TYPE header: 0x%08" PRIx32 "\n", cmd_id); }

	cp->SetIndexType(buffer[0]);

	return 1;
}

// IT_INDEX_BASE (0x26): two dwords absolute GPU VA of the index buffer.
// Emitted by sceAgcDcbSetIndexBuffer / GraphicsDcbSetIndexBuffer; required
// before DRAW_INDEX_OFFSET / DRAW_INDEX_INDIRECT that resolve against base.
KYTY_CP_OP_PARSER(cp_op_index_base)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 2u);

	if (cmd_id != 0xc0012600) { EXIT("unsupported INDEX_BASE header: 0x%08" PRIx32 "\n", cmd_id); }

	const auto index_base_addr = buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u);
	cp->SetIndexBaseAddress(index_base_addr);

	return 2;
}

// IT_INDEX_BUFFER_SIZE (0x13): index count cap for indirect/offset draws.
KYTY_CP_OP_PARSER(cp_op_index_buffer_size)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	if (cmd_id != 0xc0001300) { EXIT("unsupported INDEX_BUFFER_SIZE header: 0x%08" PRIx32 "\n", cmd_id); }

	cp->SetIndexBufferSize(buffer[0]);

	return 1;
}

// COND_INDIRECT_BUFFER shares the opcode: mode (1 if-then, 2 if-then-else) and
// compare function, 64-bit compare address, mask and reference, then base, size
// and cache policy of the buffer run when the compare passes and when it fails.
static uint32_t RunCondIndirectBuffer(CommandProcessor* cp, uint32_t cmd_id, const uint32_t* buffer, uint32_t dw)
{
	RequirePacketBody(cmd_id, dw, 13u);

	const uint32_t mode     = buffer[0] & 0x3u;
	const uint32_t function = (buffer[0] >> 8u) & 0x7u;
	const auto*    compare  = reinterpret_cast<const uint64_t*>((buffer[1] & ~0x7u) | (static_cast<uint64_t>(buffer[2]) << 32u));
	if ((mode != 1u && mode != 2u) || function > 6u || (compare == nullptr && function != 0u))
	{
		EXIT("unsupported COND_INDIRECT_BUFFER: mode=%" PRIu32 " function=%" PRIu32 " compare=%p\n", mode, function,
		     static_cast<const void*>(compare));
	}
	const uint64_t mask      = buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u);
	const uint64_t reference = buffer[5] | (static_cast<uint64_t>(buffer[6]) << 32u);
	const bool     passed    = cp->BranchCompare(function, compare, reference, mask);
	if (!passed && mode == 1u)
	{
		return 13u;
	}

	const uint32_t* target = buffer + (passed ? 7u : 10u);
	auto*           ib     = reinterpret_cast<uint32_t*>((target[0] & ~0x3u) | (static_cast<uint64_t>(target[1]) << 32u));
	const uint32_t  ib_dw  = target[2] & 0xfffffu;
	if (ib == nullptr || ib_dw == 0u)
	{
		return 13u;
	}

	GraphicsDbgDumpDcb("ci", ib_dw, ib);
	cp->Run(ib, ib_dw, ib);
	return 13u;
}

KYTY_CP_OP_PARSER(cp_op_indirect_buffer)
{
	KYTY_PROFILER_FUNCTION();

	if (KYTY_PM4_LEN(cmd_id) == 14u)
	{
		return RunCondIndirectBuffer(cp, cmd_id, buffer, dw);
	}
	if (KYTY_PM4_LEN(cmd_id) != 4u)
	{
		EXIT("invalid INDIRECT_BUFFER header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 3u);
	if ((buffer[2] & (1u << 23u)) == 0u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: INDIRECT_BUFFER without VALID (continuing)\n"); }

	auto*    indirect_buffer = reinterpret_cast<uint32_t*>(buffer[0] | (static_cast<uint64_t>(buffer[1] & 0xffffu) << 32u));
	uint32_t indirect_num_dw = buffer[2] & 0xfffffu;

	if (indirect_buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: indirect_buffer == nullptr condition ignored (continuing)\n"); }
	if (indirect_num_dw == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: indirect_num_dw == 0 condition ignored (continuing)\n"); }

	GraphicsDbgDumpDcb("ci", indirect_num_dw, indirect_buffer);

	cp->Run(indirect_buffer, indirect_num_dw, indirect_buffer);

	// A chained buffer replaces the rest of the current one.
	const bool chain = (buffer[2] & (1u << 20u)) != 0u;
	return chain ? available_dw : 3u;
}

KYTY_CP_OP_PARSER(cp_op_indirect_buffer_end)
{
	KYTY_PROFILER_FUNCTION();

	if (((cmd_id >> 8u) & 0xffu) != Pm4::IT_INDIRECT_BUFFER_END) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ((cmd_id >> 8u) & 0xffu) != Pm4::IT_INDIRECT_BUFFER_END condition ignored (continuing)\n"); }
	RequirePacketBody(cmd_id, dw, 1u);

	// PAL names opcode 0x17 as IT_INDIRECT_BUFFER_END. It terminates the
	// current IB; any following dwords belong to the parent stream or padding,
	// not to this nested Run invocation.
	return available_dw;
}

KYTY_CP_OP_PARSER(cp_op_indirect_cx_regs)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xc0021048)
	{
		EXIT("unsupported indirect context-register header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 3u);

	auto*    indirect_buffer   = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint32_t indirect_num_regs = buffer[0];

	if (indirect_buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: indirect_buffer == nullptr condition ignored (continuing)\n"); }
	if (indirect_num_regs == 0)
	{
		// Empty indirect register block: valid no-op marker emitted by some
		// titles between passes. Nothing to apply.
		return 3;
	}

	static const bool dump_ps_input_writes = std::getenv("KYTY_DUMP_PS_INPUT_WRITES") != nullptr;
	for (uint32_t i = 0; i < indirect_num_regs; i++, indirect_buffer += 2)
	{
		const auto raw_cmd_offset = indirect_buffer[0];
		auto       cmd_offset     = GraphicsDecodeIndirectCxRegisterOffset(raw_cmd_offset);
		auto       value          = indirect_buffer[1];

		if (raw_cmd_offset == 0xffffffffu)
		{
			continue;
		}
		if (cmd_offset >= Pm4::CX_NUM)
		{
			static std::atomic_uint32_t dropped_out_of_range {0};
			ReportDroppedIndirectRegister("cx", i, raw_cmd_offset, indirect_buffer[1], &dropped_out_of_range);
			continue;
		}
		if (dump_ps_input_writes && cmd_offset >= Pm4::SPI_PS_INPUT_CNTL_0 && cmd_offset <= Pm4::SPI_PS_INPUT_CNTL_31)
		{
			KYTY_LOG_DEBUG( "KYTY_PS_INPUT_WRITE indirect pair=%u slot=%u value=0x%08" PRIx32 " count=%u base=%p\n", i,
			             cmd_offset - Pm4::SPI_PS_INPUT_CNTL_0, value, indirect_num_regs, static_cast<void*>(indirect_buffer - i * 2u));
		}

		auto pfunc = g_hw_ctx_indirect_func[cmd_offset];

		if (pfunc == nullptr)
		{
			char identity[64] {};
			std::snprintf(identity, sizeof(identity), "unknown-cx-reg:0x%05" PRIx32, cmd_offset);
			const auto decision = Core::BringUp::Report(Core::BringUp::Feature::GraphicsPermissive, Core::BringUp::Subsystem::Graphics,
			                                            identity, __FILE__, __LINE__);
			if (decision == Core::BringUp::Decision::Continue)
			{
				continue;
			}
			EXIT("unsupported/unknown indirect cx register: 0x%" PRIx32 "\n", cmd_offset);
		}

		pfunc(cp, cmd_offset, value);
	}

	return 3;
}

KYTY_CP_OP_PARSER(cp_op_indirect_sh_regs)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xc0021044)
	{
		EXIT("unsupported indirect shader-register header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 3u);

	auto*    indirect_buffer   = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint32_t indirect_num_regs = buffer[0];

	if (indirect_buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: indirect_buffer == nullptr condition ignored (continuing)\n"); }
	if (indirect_num_regs == 0)
	{
		// Empty indirect register block: valid no-op marker emitted between shader passes.
		return 3;
	}

	for (uint32_t i = 0; i < indirect_num_regs; i++, indirect_buffer += 2)
	{
		auto cmd_offset = indirect_buffer[0];
		auto value      = indirect_buffer[1];
		if (GraphicsIsDefaultIndirectRegisterPair(cmd_offset, value))
		{
			continue;
		}
		if (cmd_offset >= Pm4::SH_NUM)
		{
			static std::atomic_uint32_t dropped_out_of_range {0};
			ReportDroppedIndirectRegister("sh", i, cmd_offset, value, &dropped_out_of_range);
			continue;
		}

		auto pfunc = g_hw_sh_indirect_func[cmd_offset];

		if (pfunc == nullptr)
		{
			char identity[64] {};
			std::snprintf(identity, sizeof(identity), "unknown-sh-reg:0x%05" PRIx32, cmd_offset);
			const auto decision = Core::BringUp::Report(Core::BringUp::Feature::GraphicsPermissive, Core::BringUp::Subsystem::Graphics,
			                                            identity, __FILE__, __LINE__);
			if (decision == Core::BringUp::Decision::Continue)
			{
				continue;
			}
			EXIT("unsupported/unknown indirect sh register: 0x%" PRIx32 "\n", cmd_offset);
		}

		pfunc(cp, cmd_offset, value);
	}

	return 3;
}

KYTY_CP_OP_PARSER(cp_op_indirect_uc_regs)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xc002104c)
	{
		EXIT("unsupported indirect UCONFIG-register header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 3u);

	auto*    indirect_buffer   = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint32_t indirect_num_regs = buffer[0];

	if (indirect_buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: indirect_buffer == nullptr condition ignored (continuing)\n"); }
	if (indirect_num_regs == 0)
	{
		// Empty indirect SH register block: valid no-op marker.
		return 3;
	}

	for (uint32_t i = 0; i < indirect_num_regs; i++, indirect_buffer += 2)
	{
		auto cmd_offset = indirect_buffer[0];
		auto value      = indirect_buffer[1];
		if (GraphicsIsDefaultIndirectRegisterPair(cmd_offset, value))
		{
			continue;
		}
		if (cmd_offset >= Pm4::UC_NUM)
		{
			static std::atomic_uint32_t dropped_out_of_range {0};
			ReportDroppedIndirectRegister("uc", i, cmd_offset, value, &dropped_out_of_range);
			continue;
		}

		auto pfunc = g_hw_uc_indirect_func[cmd_offset];
		if (pfunc != nullptr)
		{
			pfunc(cp, cmd_offset, value);
			continue;
		}

		char identity[64] {};
		std::snprintf(identity, sizeof(identity), "unknown-uc-reg:0x%05" PRIx32, cmd_offset);
		const auto decision = Core::BringUp::Report(Core::BringUp::Feature::GraphicsPermissive, Core::BringUp::Subsystem::Graphics,
		                                            identity, __FILE__, __LINE__);
		if (decision == Core::BringUp::Decision::Continue)
		{
			continue;
		}
		EXIT("unsupported/unknown indirect uc register: 0x%" PRIx32 "\n", cmd_offset);
	}

	return 3;
}

KYTY_CP_OP_PARSER(cp_op_marker)
{
	KYTY_PROFILER_FUNCTION();

	RequirePacketBody(cmd_id, dw, 1u);
	// EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0001000);

	uint32_t id     = buffer[0] & 0xfffu;
	uint32_t align  = (buffer[0] >> 12u) & 0xfu;
	uint32_t len_dw = ((cmd_id >> 16u) & 0x3fffu);

	switch (id)
	{
		case 0x0:
			RequirePacketBody(cmd_id, dw, len_dw + 1u);
			cp->SetEmbeddedDataMarker(buffer + 1, len_dw, align);
			break;
		case 0x4: cp->SetUserDataMarker(HW::UserSgprType::Vsharp); break;
		case 0xd: cp->SetUserDataMarker(HW::UserSgprType::Region); break;
		case 0x777:
		{
			cp->Flip();
			break;
		}
		case 0x778:
		{
			RequirePacketBody(cmd_id, dw, 4u);
			auto*    addr  = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
			uint32_t value = buffer[3];
			cp->Flip(addr, value);
			break;
		}
		case 0x781:
		{
			RequirePacketBody(cmd_id, dw, 6u);
			auto*    addr           = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
			uint32_t value          = buffer[3];
			uint32_t eop_event_type = buffer[4];
			uint32_t cache_action   = buffer[5];
			cp->FlipWithInterrupt(eop_event_type, cache_action, addr, value);
			break;
		}
		default: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown marker (continuing)\n"); break;
	}

	return len_dw + 1;
}

KYTY_CP_OP_PARSER(cp_op_nop)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	auto r = KYTY_PM4_R(cmd_id);

	if (r == Pm4::R_ZERO)
	{
		if ((buffer[0] & 0xffff0000u) == 0x68750000u)
		{
			return cp_op_marker(cp, cmd_id, buffer, dw, available_dw, num_dw);
		}

		// GraphicsCbNop emits a complete Type3 NOP whose body has no side
		// effects. Consume the encoded body so the next packet stays aligned.
		return Pm4::Pm4Type3NopBodyDwords(cmd_id);
	}

	auto hw_ctx = g_hw_sh_custom_func[r];
	auto cp_op  = g_cp_op_custom_func[r];

	if (hw_ctx != nullptr && cp_op == nullptr)
	{
		return hw_ctx(cp, cmd_id, 0, buffer, dw, available_dw);
	}

	if (hw_ctx == nullptr && cp_op != nullptr)
	{
		return cp_op(cp, cmd_id, buffer, dw, available_dw, num_dw);
	}

	EXIT("unsupported custom Type-3 NOP code: cmd_id=0x%08" PRIx32 "\n", cmd_id);
	return 0;
}

KYTY_CP_OP_PARSER(cp_op_num_instances)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xc0002f00)
	{
		EXIT("unsupported NUM_INSTANCES header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 1u);

	cp->SetNumInstances(buffer[0]);

	return 1;
}

// SET_PREDICATION. Clearing predication reads no result. With the wait hint set
// (bit 12), packets run unpredicated while the result is not ready; the command
// processor runs ahead of the GPU work that writes the result, so a predicate
// set that way never skips here. Predicates that must wait for their result
// are not implemented yet.
KYTY_CP_OP_PARSER(cp_op_set_predication)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	const uint32_t operation  = (buffer[0] >> 16u) & 0x7u;
	const bool     draw_early = ((buffer[0] >> 12u) & 0x1u) != 0u;
	if (operation != 0u && !draw_early)
	{
		RequirePacketBody(cmd_id, dw, 3u);
		EXIT("SET_PREDICATION operation %u is not implemented: condition=%u wait=%u address=0x%08x%08x\n", operation,
		     (buffer[0] >> 8u) & 0x1u, (buffer[0] >> 12u) & 0x1u, buffer[2], buffer[1]);
	}
	return KYTY_PM4_LEN(cmd_id) - 1u;
}

KYTY_CP_OP_PARSER(cp_op_pop_marker)
{
	KYTY_PROFILER_FUNCTION();

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;

	KYTY_LOG_DEBUG("Pop marker\n");

	cp->PopMarker();

	return dw_num + 1;
}

KYTY_CP_OP_PARSER(cp_op_push_marker)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	const size_t body_bytes = static_cast<size_t>(dw) * sizeof(uint32_t);
	const char*  str        = reinterpret_cast<const char*>(buffer);
	const auto*  terminator = static_cast<const char*>(memchr(str, '\0', body_bytes));
	if (terminator == nullptr)
	{
		EXIT("unterminated push marker text: cmd_id=0x%08" PRIx32 " body_dw=%" PRIu32 "\n", cmd_id, dw);
	}
	KYTY_LOG_DEBUG("Push marker: %.*s\n", static_cast<int>(terminator - str), str);

	cp->PushMarker(str);

	return dw;
}

KYTY_CP_OP_PARSER(cp_op_release_mem)
{
	KYTY_PROFILER_FUNCTION();

	// 0xC0051060 = 7-DW custom envelope; 0xC0061060 = 8-DW with interrupt_ctx_id.
	if (cmd_id != 0xC0054902 && cmd_id != 0xc0051060 && cmd_id != 0xc0061060)
	{
		EXIT("unsupported RELEASE_MEM header: 0x%08" PRIx32 "\n", cmd_id);
	}

	bool custom = (cmd_id == 0xc0051060 || cmd_id == 0xc0061060);
	RequirePacketBody(cmd_id, dw, cmd_id == 0xc0061060 ? 7u : 6u);

	if (custom && buffer[0] == KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE))
	{
		// Observed Gen5 flip stream can contain a lone R_RELEASE_MEM marker
		// before the real WaitFlipDone packet. The marker has no payload; do
		// not consume the following Type3 header as ReleaseMem data.
		if (!GraphicsRunType3PacketAt(buffer, available_dw, 0u, KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE)))
		{
			EXIT("truncated ReleaseMem/WaitFlipDone split packet: available_dw=%" PRIu32 "\n", available_dw);
		}
		return 0;
	}

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t cache_action       = (buffer[0] >> 12u) & 0x3fu;
	uint32_t eop_event_type     = (buffer[0]) & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_write_dest   = ((buffer[0] >> 23u) & 0x10u) | ((buffer[1] >> 16u) & 0x01u);
	uint32_t event_write_source = (buffer[1] >> 29u) & 0x7u;
	uint32_t interrupt_selector = (buffer[1] >> 24u) & 0x7u;
	auto*    dst_gpu_addr       = reinterpret_cast<void*>(buffer[2] | (static_cast<uint64_t>(buffer[3]) << 32u));
	uint64_t value              = (buffer[4] | (static_cast<uint64_t>(buffer[5]) << 32u));

	if (custom)
	{
		const auto control  = GraphicsDecodeAgcReleaseMemControl(buffer[1]);
		uint32_t   gcr_cntl = control.gcr_cntl;
		uint32_t   data_sel = control.data_sel;
		interrupt_selector  = control.interrupt;

		// data_sel matches GraphicsCbReleaseMem: 0 = no destination write
		// (flush/barrier only), 1 = 32-bit immediate, 2 = 64-bit immediate,
		// 3 = GPU clock counter. gcr_cntl selects cache ops on real HW; the
		// software CP only needs a host barrier when any GCR bit is set. Do not
		// EXIT on non-zero gcr — Gen5 titles combine flush bits with label
		// writes (observed after PlayGo: data_sel=1 with gcr!=0).
		if (data_sel == 0)
		{
			if (gcr_cntl != 0u)
			{
				cp->MemoryBarrier();
			}
			return (cmd_id == 0xc0061060) ? 7 : 6;
		}
		if (data_sel == 1)
		{
			if (dst_gpu_addr == nullptr)
			{
				// Null destination: flush/ordering only. Action 0x14 was the
				// first observed form; other CACHE_* events are accepted the
				// same way when there is no label store.
				if (gcr_cntl != 0u || (buffer[0] & 0xffu) != 0u)
				{
					cp->MemoryBarrier();
				}
				if ((buffer[4] >> 30u) == 3u)
				{
					// Null-destination ReleaseMem has no data write. Some
					// streams place the following packet where data_lo would
					// be in the full 7-dword form.
					return 4;
				}
				return (cmd_id == 0xc0061060) ? 7 : 6;
			}

			if (gcr_cntl != 0u)
			{
				cp->MemoryBarrier();
			}
			// Normalize to the known 32-bit immediate EOP branch when the
			// guest action is outside the previously observed set.
			if (eop_event_type != 0x14u && eop_event_type != 0x30u && eop_event_type != 0x2fu)
			{
				eop_event_type = 0x14u;
			}
			cache_action       = 0x00;
			cache_policy       = 0;
			event_index        = 0;
			event_write_dest   = 0;
			event_write_source = 1; // 32-bit immediate
		} else if (data_sel == 2)
		{
			// GCR cache behavior and queued-interrupt selection are independent.
			// GL2 writeback maps to the existing 0x38 execution path while the
			// interrupt selector is preserved for completion signaling.
			if (gcr_cntl != 0u)
			{
				cp->MemoryBarrier();
			}
			cache_action = GraphicsAgcReleaseMemCacheAction(static_cast<uint16_t>(gcr_cntl));
			if (gcr_cntl == 0u)
			{
				eop_event_type = 0x04;
				event_index    = 0x05;
			} else
			{
				event_index = 0;
			}

			cache_policy       = 0;
			event_write_dest   = 0;
			event_write_source = 2;
		} else if (data_sel == 3)
		{
			if (gcr_cntl != 0u)
			{
				cp->MemoryBarrier();
			}
			cache_action = 0x00;

			cache_policy       = 0;
			event_index        = 0;
			event_write_dest   = 0;
			event_write_source = 4;
		} else
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unsupported ReleaseMem data_sel (continuing)\n");
		}
	}

	const uint64_t destination_size = event_write_source == 1u ? 4u : ((event_write_source == 2u || event_write_source == 4u) ? 8u : 0u);
	if (destination_size != 0u &&
	    GpuMemoryValidateAllocatedRange(reinterpret_cast<uint64_t>(dst_gpu_addr), destination_size) !=
	        GpuMemoryRangeValidationStatus::Valid)
	{
		const bool no_destination_sentinel = reinterpret_cast<uint64_t>(dst_gpu_addr) == 1u;
		if (Log::ShouldLog(Log::Level::Warn))
		{
			static std::atomic<uint32_t> invalid_destination_logs {0};
			if (invalid_destination_logs.fetch_add(1, std::memory_order_relaxed) < 32u)
			{
				const uint32_t packet_dw = custom ? (cmd_id == 0xc0061060 ? 8u : 7u) : KYTY_PM4_LEN(cmd_id);
				KYTY_LOG_WARN("KYTY_RELEASE_MEM_INVALID cmd=0x%08" PRIx32 " addr=0x%016" PRIx64 " bytes=%" PRIu64
				              " source=%" PRIu32 " value=0x%016" PRIx64 " packet=0x%08" PRIx32,
				              cmd_id, reinterpret_cast<uint64_t>(dst_gpu_addr), destination_size, event_write_source, value, cmd_id);
				for (uint32_t i = 0; i + 1u < packet_dw; i++)
				{
					KYTY_LOG_WARN(",0x%08" PRIx32, buffer[i]);
				}
				KYTY_LOG_WARN("\n");
			}
		}
		if (!no_destination_sentinel)
		{
			// The destination comes directly from guest PM4. Never enqueue an EOP
			// label that can later dereference an unmapped host address.
			return (cmd_id == 0xc0061060) ? 7 : 6;
		}

		// Address 1 is the guest's no-destination marker for cache/writeback
		// ReleaseMem packets. Keep the submission barrier and interrupt callback,
		// but represent the absent guest store with a null transient label target.
		dst_gpu_addr = nullptr;
	}

	cp->WriteAtEndOfPipe64(cache_policy, event_write_dest, eop_event_type, cache_action, event_index, event_write_source, dst_gpu_addr,
	                       value, interrupt_selector, GraphicsAgcReleaseMemInterruptContextId(cmd_id, buffer));

	// Body dwords after the Type-3 header: 6 for the 7-DW form, 7 when the
	// packet carries interrupt_ctx_id as an eighth dword.
	return (cmd_id == 0xc0061060) ? 7 : 6;
}

KYTY_CP_OP_PARSER(cp_op_one_reg_write)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	if (((cmd_id >> 8u) & 0xffu) != Pm4::IT_ONE_REG_WRITE) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ((cmd_id >> 8u) & 0xffu) != Pm4::IT_ONE_REG_WRITE condition ignored (continuing)\n"); }
	return 1;
}

KYTY_CP_OP_PARSER(cp_op_get_lod_stats)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 1u);

	if (((cmd_id >> 8u) & 0xffu) != Pm4::IT_GET_LOD_STATS)
	{
		EXIT("unsupported GET_LOD_STATS header: 0x%08" PRIx32 "\n", cmd_id);
	}
	// This query packet has no guest-visible producer in the current renderer.
	// Consume its payload so following PM4 packets remain aligned.
	KYTY_LOG_LIMIT(Log::Level::Warn, 1, "WARNING: ignoring unsupported IT_GET_LOD_STATS\n");
	return dw;
}

KYTY_CP_OP_PARSER(cp_op_set_context_reg)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 2u);

	auto cmd_offset = buffer[0];

	if (cmd_offset >= Pm4::CX_NUM)
	{
		EXIT("context-register offset out of range: 0x%08" PRIx32 "\n", cmd_offset);
	}

	auto pfunc = g_hw_ctx_func[cmd_offset];

	if (pfunc == nullptr)
	{
		EXIT("unsupported context-register write: cmd_id=0x%08" PRIx32 " offset=0x%08" PRIx32 "\n", cmd_id, cmd_offset);
	}

	const uint32_t register_dw           = dw - 1u;
	const uint32_t available_register_dw = available_dw - 1u;
	auto           s                     = pfunc(cp, cmd_id, cmd_offset, buffer + 1, register_dw, available_register_dw);
	if (s == 0u || s > available_register_dw)
	{
		EXIT("context-register parser returned invalid count: cmd_id=0x%08" PRIx32 " offset=0x%08" PRIx32
		     " consumed_dw=%" PRIu32 " available_dw=%" PRIu32 "\n",
		     cmd_id, cmd_offset, s, available_register_dw);
	}

	return s + 1;
}

KYTY_CP_OP_PARSER(cp_op_set_shader_reg)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 2u);

	auto cmd_offset = buffer[0];

	if (cmd_offset >= Pm4::SH_NUM)
	{
		EXIT("shader-register offset out of range: 0x%08" PRIx32 "\n", cmd_offset);
	}

	auto pfunc = g_hw_sh_func[cmd_offset];

	if (pfunc == nullptr)
	{
		EXIT("unsupported shader-register write: cmd_id=0x%08" PRIx32 " offset=0x%08" PRIx32 "\n", cmd_id, cmd_offset);
	}

	const uint32_t register_dw           = dw - 1u;
	const uint32_t available_register_dw = available_dw - 1u;
	auto           s                     = pfunc(cp, cmd_id, cmd_offset, buffer + 1, register_dw, available_register_dw);
	if (s == 0u || s > available_register_dw)
	{
		EXIT("shader-register parser returned invalid count: cmd_id=0x%08" PRIx32 " offset=0x%08" PRIx32
		     " consumed_dw=%" PRIu32 " available_dw=%" PRIu32 "\n",
		     cmd_id, cmd_offset, s, available_register_dw);
	}
	return s + 1;
}

KYTY_CP_OP_PARSER(cp_op_set_uconfig_reg)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 2u);

	auto cmd_offset = buffer[0] & 0xEFFFFFFFu; // ignore neo bit

	if (cmd_offset >= Pm4::UC_NUM)
	{
		EXIT("uconfig-register offset out of range: 0x%08" PRIx32 "\n", cmd_offset);
	}

	auto pfunc = g_hw_uc_func[cmd_offset];

	if (pfunc == nullptr)
	{
		EXIT("unsupported uconfig-register write: cmd_id=0x%08" PRIx32 " offset=0x%08" PRIx32 "\n", cmd_id, cmd_offset);
	}

	const uint32_t register_dw           = dw - 1u;
	const uint32_t available_register_dw = available_dw - 1u;
	auto           s                     = pfunc(cp, cmd_id, cmd_offset, buffer + 1, register_dw, available_register_dw);
	if (s == 0u || s > available_register_dw)
	{
		EXIT("uconfig-register parser returned invalid count: cmd_id=0x%08" PRIx32 " offset=0x%08" PRIx32
		     " consumed_dw=%" PRIu32 " available_dw=%" PRIu32 "\n",
		     cmd_id, cmd_offset, s, available_register_dw);
	}
	return s + 1;
}

KYTY_CP_OP_PARSER(cp_op_set_uconfig_reg_index)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 2u);

	uint32_t index_type = 0;
	if (!Gen5::GraphicsDecodeIndexedUconfigVgtIndexType(cmd_id, buffer, dw, &index_type))
	{
		EXIT("invalid indexed UCONFIG packet: cmd_id=0x%08" PRIx32 " body_dw=%" PRIu32 "\n", cmd_id, dw);
	}

	cp->SetIndexType(index_type);
	return 2;
}

KYTY_CP_OP_PARSER(cp_op_wait_flip_done)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xc0051018)
	{
		EXIT("unsupported WAIT_FLIP_DONE header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 6u);

	auto video_out_handle     = buffer[0];
	auto display_buffer_index = buffer[1];

	cp->WaitFlipDone(video_out_handle, display_buffer_index);

	return 6;
}

KYTY_CP_OP_PARSER(cp_op_wait_reg_mem_32)
{
	KYTY_PROFILER_FUNCTION();
	uint32_t expected_body_dw = 0;
	if (cmd_id == 0xc0051028u)
	{
		expected_body_dw = 6u;
	} else if (cmd_id == 0xc00c1028u)
	{
		expected_body_dw = 13u;
	} else
	{
		EXIT("unsupported WaitRegMem32 header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, expected_body_dw);
	if (dw != expected_body_dw)
	{
		EXIT("invalid WaitRegMem32 body size: cmd_id=0x%08" PRIx32 " body_dw=%" PRIu32 " expected_dw=%" PRIu32 "\n", cmd_id, dw,
		     expected_body_dw);
	}

	auto*    addr = reinterpret_cast<uint32_t*>(buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u));
	uint32_t mask = buffer[2];
	uint32_t func = 0;
	uint32_t ref  = 0;
	uint32_t poll = 0;
	uint32_t used = 0;

	if (cmd_id == 0xc0051028u)
	{
		ref  = buffer[3];
		func = buffer[4] & 0x7u;
		poll = buffer[5];
		used = 6;
	} else
	{
		func = buffer[3];
		ref  = buffer[4];
		poll = 10;
		used = 13;
	}

	cp->WaitRegMem32(func, addr, ref, mask, poll);

	return used;
}

KYTY_CP_OP_PARSER(cp_op_wait_reg_mem_64)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xc0071058)
	{
		EXIT("unsupported WAIT_MEM_64 header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 8u);

	auto* addr = reinterpret_cast<uint64_t*>(buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u));
	auto  mask = buffer[2] | (static_cast<uint64_t>(buffer[3]) << 32u);
	auto  ref  = buffer[4] | (static_cast<uint64_t>(buffer[5]) << 32u);
	auto  func = buffer[6] & 0x7u;
	auto  poll = buffer[7];

	// Post-Play: WaitMem often keeps address=0 while the preceding contiguous
	// ReleaseMem is EopPatched to the real Label*. Inherit that address.
	if (addr == nullptr)
	{
		addr = Gen5::GraphicsResolveWaitMemAddressFromPrecedingRelease(buffer, cp->GetActiveRunBegin(), cp->GetActiveRunEnd());
	}
	if (addr != nullptr &&
	    GpuMemoryValidateAllocatedRange(reinterpret_cast<uint64_t>(addr), sizeof(uint64_t)) != GpuMemoryRangeValidationStatus::Valid)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: WaitRegMem64 resolved an unallocated address (continuing)\n");
		addr = nullptr;
	}

	if (addr == nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: WaitRegMem64 null addr (continuing)\n");
	}

	cp->WaitRegMem64(func, addr, ref, mask, poll);

	return 8;
}

KYTY_CP_OP_PARSER(cp_op_wait_on_ce_counter)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xC0008600)
	{
		EXIT("unsupported WAIT_ON_CE_COUNTER header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 1u);
	if (buffer[0] != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer[0] != 1 condition ignored (continuing)\n"); }

	cp->WaitCe();

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_wait_on_de_counter_diff)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xC0008800)
	{
		EXIT("unsupported WAIT_ON_DE_COUNTER_DIFF header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 1u);

	cp->WaitDeDiff(buffer[0]);

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_wait_reg_mem)
{
	KYTY_PROFILER_FUNCTION();

	if (cmd_id != 0xC0053C00)
	{
		EXIT("unsupported WAIT_REG_MEM header: 0x%08" PRIx32 "\n", cmd_id);
	}
	RequirePacketBody(cmd_id, dw, 6u);

	auto  func = buffer[0] & 0x7u;
	bool  me   = (buffer[0] & 0x100u) == 0;
	bool  mem  = (buffer[0] & 0x10u) != 0;
	auto* addr = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	auto  ref  = buffer[3];
	auto  mask = buffer[4];
	auto  poll = buffer[5];

	if (!me) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !me condition ignored (continuing)\n"); }
	if (!mem) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !mem condition ignored (continuing)\n"); }

	cp->WaitRegMem32(func, addr, ref, mask, poll);

	return 6;
}

KYTY_CP_OP_PARSER(cp_op_write_const_ram)
{
	KYTY_PROFILER_FUNCTION();

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;
	RequirePacketBody(cmd_id, dw, 1u + dw_num);
	auto offset = buffer[0];

	if (dw_num >= 0x3000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dw_num >= 0x3000 condition ignored (continuing)\n"); }
	if (offset > 0xbffc) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: offset > 0xbffc condition ignored (continuing)\n"); }
	if ((offset & 0x3u) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (offset & 0x3u) != 0 condition ignored (continuing)\n"); }

	cp->WriteConstRam(offset, buffer + 1, dw_num);

	return 1 + dw_num;
}

KYTY_CP_OP_PARSER(cp_op_write_data)
{
	KYTY_PROFILER_FUNCTION();
	RequirePacketBody(cmd_id, dw, 3u);

	auto op = (cmd_id >> 8u) & 0xffu;

	if (op != Pm4::IT_WRITE_DATA && op != Pm4::IT_NOP)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: op != Pm4::IT_WRITE_DATA && op != Pm4::IT_NOP condition ignored (continuing)\n");
	}

	bool custom = (op == Pm4::IT_NOP);

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;

	auto  write_control = buffer[0];
	auto* dst           = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));

	const uint32_t write_body_dwords = 1u + dw_num;
	RequirePacketBody(cmd_id, dw, write_body_dwords);
	if (available_dw < write_body_dwords)
	{
		EXIT("truncated WRITE_DATA packet body: cmd_id=0x%08" PRIx32 " required_dw=%" PRIu32 " available_dw=%" PRIu32 "\n", cmd_id,
		     write_body_dwords, available_dw);
	}
	const uint32_t  next_packet_dwords = available_dw - write_body_dwords;
	const uint32_t* next_packet        = buffer + write_body_dwords;
	const bool      matching_wait_mem64 =
	    custom && GraphicsWriteDataPrecedesMatchingWaitMem64(buffer, write_body_dwords, next_packet, next_packet_dwords);

	cp->WriteData(dst, buffer + 3, dw_num - 2, write_control, custom, matching_wait_mem64);

	return 1 + dw_num;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
