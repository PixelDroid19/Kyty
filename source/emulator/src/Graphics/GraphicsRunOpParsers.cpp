#include "GraphicsRunInternal.h"

#include "Kyty/Core/BringUp.h"

#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Log.h"

#include <atomic>
#include <cinttypes>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_CP_OP_PARSER(cp_op_acquire_mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0055800 && cmd_id != 0xc0061050);

	bool custom = (cmd_id == 0xc0061050);

	uint32_t                  stall_mode   = buffer[0] >> 31u;
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
	EXIT_NOT_IMPLEMENTED(size_hi != 0);
	EXIT_NOT_IMPLEMENTED(base_hi != 0);
	EXIT_NOT_IMPLEMENTED(poll != 10);

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
			EXIT_NOT_IMPLEMENTED(size_lo == 0);
			EXIT_NOT_IMPLEMENTED(base_lo == 0);

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
				EXIT_NOT_IMPLEMENTED(base_lo == 0);
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
			EXIT_NOT_IMPLEMENTED(size_lo != 1);
			EXIT_NOT_IMPLEMENTED(base_lo != 0);

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
			EXIT_NOT_IMPLEMENTED(size_lo != 1);
			EXIT_NOT_IMPLEMENTED(base_lo != 0);

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
				EXIT_NOT_IMPLEMENTED(base_lo == 0);
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

	if (stall_mode == 0)
	{
		cp->BufferFlush();
		cp->BufferWait();
	}

	return (custom ? 7 : 6);
}

KYTY_CP_OP_PARSER(cp_op_dispatch_direct)
{
	KYTY_PROFILER_FUNCTION();

	const bool standard = (cmd_id == KYTY_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0));
	const bool custom   = (cmd_id == KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_DISPATCH_DIRECT));

	EXIT_NOT_IMPLEMENTED(!standard && !custom);
	EXIT_NOT_IMPLEMENTED(standard && dw < 5);
	EXIT_NOT_IMPLEMENTED(custom && dw < 6);

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

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0001024);

	cp->Reset();

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_dma_data)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0055000);

	uint32_t type  = buffer[0];
	uint32_t type2 = (buffer[5] >> 21u);
	uint64_t src   = (buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint64_t dst   = (buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u));
	uint32_t size  = buffer[5] & 0x1fffffu;

	if (type == 0x60000000 && dst == 0x0003022C && type2 == 0x141u)
	{
		auto* addr = reinterpret_cast<void*>(src);

		cp->PrefetchL2(addr, size);
	} else if (type == 0xc4104000 && type2 == 0x0u)
	{
		EXIT_NOT_IMPLEMENTED((dst & 0x3u) != 0);
		EXIT_NOT_IMPLEMENTED((size & 0x3u) != 0);

		cp->ClearGds(dst / 4, size / 4, static_cast<uint32_t>(src));
	} else if (type == 0xa4004000 && type2 == 0x0u)
	{
		EXIT_NOT_IMPLEMENTED((src & 0x3u) != 0);
		EXIT_NOT_IMPLEMENTED((size & 0x3u) != 0);

		auto* addr = reinterpret_cast<uint32_t*>(dst);

		cp->ReadGds(addr, src / 4, size / 4);
	} else
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown DMA (continuing)\n");
	}

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
	EXIT_NOT_IMPLEMENTED(cmd_id != KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_DMA_DATA));

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
		return 7;
	}

	if (src == 0 || GpuMemoryValidateAllocatedRange(src, byte_count) != GpuMemoryRangeValidationStatus::Valid)
	{
		return 7;
	}

	GpuMemoryNotifyHostWrite(dst, byte_count);
	memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), byte_count);
	GraphicsRenderMemoryFlush(dst, byte_count);
	return 7;
}

KYTY_CP_OP_PARSER(cp_op_draw_index)
{
	KYTY_PROFILER_FUNCTION();

	const uint32_t packet_len = KYTY_PM4_LEN(cmd_id);

	if (KYTY_PM4_R(cmd_id) == Pm4::R_DRAW_INDEX)
	{
		EXIT_NOT_IMPLEMENTED(packet_len != 7);
		EXIT_NOT_IMPLEMENTED(dw < 6);

		uint32_t index_count = buffer[0];
		auto*    index_addr  = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
		uint64_t modifier    = buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u);
		EXIT_NOT_IMPLEMENTED(buffer[5] != 0);

		cp->DrawIndex(index_count, index_addr, modifier, 1);

		return 6;
	}

	if (cmd_id == KYTY_PM4(6, Pm4::IT_DRAW_INDEX_2, 0u))
	{
		uint32_t index_count = buffer[0];
		auto*    index_addr  = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));

		EXIT_NOT_IMPLEMENTED(buffer[3] != index_count);
		EXIT_NOT_IMPLEMENTED(buffer[4] != 0);

		cp->DrawIndex(index_count, index_addr, 0, 1);

		EXIT_NOT_IMPLEMENTED(!(dw >= 7));

		if (buffer[5] == 0xc0001000)
		{
			EXIT_NOT_IMPLEMENTED(buffer[6] != 0);

			return 7;
		}

		if (buffer[5] == 0xc0021000)
		{
			EXIT_NOT_IMPLEMENTED(buffer[6] != 0);

			return 9;
		}

		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: invalid draw_index (continuing)\n");
	}

	EXIT_NOT_IMPLEMENTED(true);
	return 0;
}

// IT_DRAW_INDEX_OFFSET_2 (0x35): draw from IT_INDEX_BASE + index_offset.
// Body: index_count, index_offset, index_count, flags.
KYTY_CP_OP_PARSER(cp_op_draw_index_offset)
{
	KYTY_PROFILER_FUNCTION();
	EXIT_NOT_IMPLEMENTED((cmd_id & ~1u) != 0xc0033500);
	EXIT_NOT_IMPLEMENTED(buffer[0] != buffer[2]);
	EXIT_NOT_IMPLEMENTED((buffer[3] & ~0xE0000001u) != 0);
	cp->DrawIndexOffset(buffer[1], buffer[0], buffer[3] & 0xE0000001u);
	return 4;
}

KYTY_CP_OP_PARSER(cp_op_draw_index_auto)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0051010 && cmd_id != 0xc0012d00);

	if (cmd_id == 0xC0051010)
	{
		uint32_t index_count    = buffer[0];
		uint64_t draw_modifier = static_cast<uint64_t>(buffer[1]) | (static_cast<uint64_t>(buffer[2]) << 32u);

		cp->DrawIndexAuto(index_count, draw_modifier);

		return 6;
	}

	if (cmd_id == 0xc0012d00)
	{
		uint32_t index_count = buffer[0];
		uint32_t flags       = buffer[1];

		EXIT_NOT_IMPLEMENTED(!GraphicsDrawIndexAutoFlagsSupported(flags));

		cp->DrawIndexAuto(index_count, 0);

		// The Type3 packet has exactly two body dwords. CommandProcessor::Run
		// dispatches the following packet on the next iteration.
		return 2;
	}

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_draw_reset)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0001014);

	cp->Reset();

	return 1;
}

// Gen5 IT_SET_BASE from GraphicsDcbSetBaseIndirectArgs: header + 3 body dwords.
// Indirect-arg base is recorded for later draw/dispatch; no guest-visible
// side effect beyond stream progress is required yet.
KYTY_CP_OP_PARSER(cp_op_set_base)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(dw < 3);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 1u);

	const uint32_t base_index = (cmd_id >> 1u) & 0x1u;
	const uint64_t address    = (buffer[1] & ~7ull) | (static_cast<uint64_t>(buffer[2]) << 32u);
	cp->SetIndirectArgsBaseAddress(base_index, address);

	return 3;
}

// Gen5 IT_DISPATCH_INDIRECT: header + data_offset + modifier.
// Full GPU dispatch from the SetBaseIndirect arg buffer is future work;
// consuming the packet keeps the command stream aligned.
KYTY_CP_OP_PARSER(cp_op_dispatch_indirect)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(dw < 2);
	cp->DispatchIndirect(buffer[0], buffer[1]);

	return 2;
}

// Gen5 IT_DRAW_INDEX_INDIRECT: header + 4 body dwords (offset, patch lo/hi, initiator).
KYTY_CP_OP_PARSER(cp_op_draw_index_indirect)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(dw < 4);
	cp->DrawIndexIndirect(buffer[0], buffer[3]);

	return 4;
}

// Gen5 IT_CLEAR_STATE from GraphicsDcbResetQueue: header + 4-bit state body.
KYTY_CP_OP_PARSER(cp_op_clear_state)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0001200);
	EXIT_NOT_IMPLEMENTED((buffer[0] & ~0xfu) != 0);

	// This packet executes inside CommandProcessor::Run. Calling Reset here
	// would wait on the command buffer currently being decoded and recursively
	// re-enter the same packet. CLEAR_STATE restores context-register defaults;
	// shader and UCONFIG state belong to separate register files.
	cp->GetCtx()->Reset();

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_dump_const_ram)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0038300);

	auto  offset = buffer[0];
	auto  dw_num = buffer[1];
	auto* dst    = reinterpret_cast<uint32_t*>(buffer[2] | (static_cast<uint64_t>(buffer[3]) << 32u));

	EXIT_NOT_IMPLEMENTED(dw_num >= 0x3000);
	EXIT_NOT_IMPLEMENTED(offset > 0xbffc);
	EXIT_NOT_IMPLEMENTED((offset & 0x3u) != 0);

	cp->DumpConstRam(dst, offset, dw_num);

	return 4;
}

KYTY_CP_OP_PARSER(cp_op_event_write)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0004600);

	uint32_t event_index = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_type  = (buffer[0]) & 0x3fu;

	cp->TriggerEvent(event_type, event_index);

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_event_write_eop)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0044700);

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t event_write_dest   = ((buffer[0] >> 23u) & 0x10u) | ((buffer[2] >> 16u) & 0x01u);
	uint32_t eop_event_type     = (buffer[0]) & 0x3fu;
	uint32_t cache_action       = (buffer[0] >> 12u) & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_write_source = ((buffer[2] >> 29u) & 0x7u);
	uint32_t interrupt_selector = (buffer[2] >> 24u) & 0x7u;
	auto*    dst_gpu_addr       = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2] & 0xffffu) << 32u));
	uint64_t value              = (buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u));

	cp->WriteAtEndOfPipe64(cache_policy, event_write_dest, eop_event_type, cache_action, event_index, event_write_source, dst_gpu_addr,
	                       value, interrupt_selector);

	return 5;
}

KYTY_CP_OP_PARSER(cp_op_event_write_eos)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0034802);

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

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc004105c);

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

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008400);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 1);

	cp->IncremenetCe();

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_increment_de_counter)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008500);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 0);

	cp->IncremenetDe();

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_index_type)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0002A00);

	cp->SetIndexType(buffer[0]);

	return 1;
}

// IT_INDEX_BASE (0x26): two dwords absolute GPU VA of the index buffer.
// Emitted by sceAgcDcbSetIndexBuffer / GraphicsDcbSetIndexBuffer; required
// before DRAW_INDEX_OFFSET / DRAW_INDEX_INDIRECT that resolve against base.
KYTY_CP_OP_PARSER(cp_op_index_base)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0012600);

	const auto index_base_addr = buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u);
	cp->SetIndexBaseAddress(index_base_addr);

	return 2;
}

// IT_INDEX_BUFFER_SIZE (0x13): index count cap for indirect/offset draws.
KYTY_CP_OP_PARSER(cp_op_index_buffer_size)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0001300);

	cp->SetIndexBufferSize(buffer[0]);

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_indirect_buffer)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0023f02);

	EXIT_NOT_IMPLEMENTED((buffer[2] & 0xff00000u) != 0x1800000u);

	auto*    indirect_buffer = reinterpret_cast<uint32_t*>(buffer[0] | (static_cast<uint64_t>(buffer[1] & 0xffffu) << 32u));
	uint32_t indirect_num_dw = buffer[2] & 0xfffffu;

	EXIT_NOT_IMPLEMENTED(indirect_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(indirect_num_dw == 0);

	GraphicsDbgDumpDcb("ci", indirect_num_dw, indirect_buffer);

	cp->Run(indirect_buffer, indirect_num_dw, indirect_buffer);

	return 3;
}

KYTY_CP_OP_PARSER(cp_op_indirect_buffer_end)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_INDIRECT_BUFFER_END);
	EXIT_NOT_IMPLEMENTED(dw < 1);

	// PAL names opcode 0x17 as IT_INDIRECT_BUFFER_END. It terminates the
	// current IB; any following dwords belong to the parent stream or padding,
	// not to this nested Run invocation.
	return dw - 1u;
}

KYTY_CP_OP_PARSER(cp_op_indirect_cx_regs)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0021048);

	auto*    indirect_buffer   = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint32_t indirect_num_regs = buffer[0];

	EXIT_NOT_IMPLEMENTED(indirect_buffer == nullptr);
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
			if (dropped_out_of_range.fetch_add(1, std::memory_order_relaxed) < 4u)
			{
				KYTY_LOG_DEBUG(
				             "WARNING: dropping out-of-range indirect cx register pair=%" PRIu32 " offset=0x%08" PRIx32
				             " value=0x%08" PRIx32 "\n",
				             i, raw_cmd_offset, indirect_buffer[1]);
			}
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

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0021044);

	auto*    indirect_buffer   = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint32_t indirect_num_regs = buffer[0];

	EXIT_NOT_IMPLEMENTED(indirect_buffer == nullptr);
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
			if (dropped_out_of_range.fetch_add(1, std::memory_order_relaxed) < 4u)
			{
				KYTY_LOG_DEBUG(
				             "WARNING: dropping out-of-range indirect sh register pair=%" PRIu32 " offset=0x%08" PRIx32
				             " value=0x%08" PRIx32 "\n",
				             i, cmd_offset, value);
			}
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

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc002104c);

	auto*    indirect_buffer   = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint32_t indirect_num_regs = buffer[0];

	EXIT_NOT_IMPLEMENTED(indirect_buffer == nullptr);
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
			if (dropped_out_of_range.fetch_add(1, std::memory_order_relaxed) < 4u)
			{
				KYTY_LOG_DEBUG(
				             "WARNING: dropping out-of-range indirect uc register pair=%" PRIu32 " offset=0x%08" PRIx32
				             " value=0x%08" PRIx32 "\n",
				             i, cmd_offset, value);
			}
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

	// EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0001000);

	uint32_t id     = buffer[0] & 0xfffu;
	uint32_t align  = (buffer[0] >> 12u) & 0xfu;
	uint32_t len_dw = ((cmd_id >> 16u) & 0x3fffu);

	switch (id)
	{
		case 0x0: cp->SetEmbeddedDataMarker(buffer + 1, len_dw, align); break;
		case 0x4: cp->SetUserDataMarker(HW::UserSgprType::Vsharp); break;
		case 0xd: cp->SetUserDataMarker(HW::UserSgprType::Region); break;
		case 0x777:
		{
			cp->Flip();
			break;
		}
		case 0x778:
		{
			auto*    addr  = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
			uint32_t value = buffer[3];
			cp->Flip(addr, value);
			break;
		}
		case 0x781:
		{
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

	auto r = KYTY_PM4_R(cmd_id);

	if (r == Pm4::R_ZERO)
	{
		if ((buffer[0] & 0xffff0000u) == 0x68750000u)
		{
			return cp_op_marker(cp, cmd_id, buffer, dw, num_dw);
		}

		// GraphicsCbNop emits a complete Type3 NOP whose body has no side
		// effects. Consume the encoded body so the next packet stays aligned.
		return Pm4::Pm4Type3NopBodyDwords(cmd_id);
	}

	auto hw_ctx = g_hw_sh_custom_func[r];
	auto cp_op  = g_cp_op_custom_func[r];

	if (hw_ctx != nullptr && cp_op == nullptr)
	{
		return hw_ctx(cp, cmd_id, 0, buffer, dw);
	}

	if (hw_ctx == nullptr && cp_op != nullptr)
	{
		return cp_op(cp, cmd_id, buffer, dw, num_dw);
	}

	KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown custom code (continuing)\n");

	return 0;
}

KYTY_CP_OP_PARSER(cp_op_num_instances)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0002f00);

	cp->SetNumInstances(buffer[0]);

	return 1;
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

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;

	const char* str = reinterpret_cast<const char*>(buffer);

	KYTY_LOG_DEBUG("Push marker: %s\n", str);

	cp->PushMarker(str);

	return dw_num + 1;
}

KYTY_CP_OP_PARSER(cp_op_release_mem)
{
	KYTY_PROFILER_FUNCTION();

	// 0xC0051060 = 7-DW custom envelope; 0xC0061060 = 8-DW with interrupt_ctx_id.
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0054902 && cmd_id != 0xc0051060 && cmd_id != 0xc0061060);

	bool custom = (cmd_id == 0xc0051060 || cmd_id == 0xc0061060);

	if (custom && buffer[0] == KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE))
	{
		// Observed Gen5 flip stream can contain a lone R_RELEASE_MEM marker
		// before the real WaitFlipDone packet. The marker has no payload; do
		// not consume the following Type3 header as ReleaseMem data.
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
	                       value, interrupt_selector);

	// Body dwords after the Type-3 header: 6 for the 7-DW form, 7 when the
	// packet carries interrupt_ctx_id as an eighth dword.
	return (cmd_id == 0xc0061060) ? 7 : 6;
}

KYTY_CP_OP_PARSER(cp_op_one_reg_write)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_ONE_REG_WRITE);
	EXIT_NOT_IMPLEMENTED(dw < 2);

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_get_lod_stats)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(((cmd_id >> 8u) & 0xffu) != Pm4::IT_GET_LOD_STATS);
	EXIT_NOT_IMPLEMENTED(dw < 2);

	// This query packet has no guest-visible producer in the current renderer.
	// Consume its payload so following PM4 packets remain aligned.
	KYTY_LOG_LIMIT(Log::Level::Warn, 1, "WARNING: ignoring unsupported IT_GET_LOD_STATS\n");
	return 1;
}

KYTY_CP_OP_PARSER(cp_op_set_context_reg)
{
	KYTY_PROFILER_FUNCTION();

	auto cmd_offset = buffer[0];

	EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::CX_NUM);

	auto pfunc = g_hw_ctx_func[cmd_offset];

	if (pfunc == nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown register write (continuing)\n");
	}

	auto s = pfunc(cp, cmd_id, cmd_offset, buffer + 1, dw);

	EXIT_IF(s == 0);

	return s + 1;
}

KYTY_CP_OP_PARSER(cp_op_set_shader_reg)
{
	KYTY_PROFILER_FUNCTION();

	auto cmd_offset = buffer[0];

	EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::SH_NUM);

	auto pfunc = g_hw_sh_func[cmd_offset];

	if (pfunc == nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown register write (continuing)\n");
	}

	auto s = pfunc(cp, cmd_id, cmd_offset, buffer + 1, dw);
	EXIT_IF(s == 0);
	return s + 1;
}

KYTY_CP_OP_PARSER(cp_op_set_uconfig_reg)
{
	KYTY_PROFILER_FUNCTION();

	auto cmd_offset = buffer[0] & 0xEFFFFFFFu; // ignore neo bit

	EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::UC_NUM);

	auto pfunc = g_hw_uc_func[cmd_offset];

	if (pfunc == nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown register write (continuing)\n");
	}

	auto s = pfunc(cp, cmd_id, cmd_offset, buffer + 1, dw);
	EXIT_IF(s == 0);
	return s + 1;
}

KYTY_CP_OP_PARSER(cp_op_set_uconfig_reg_index)
{
	KYTY_PROFILER_FUNCTION();

	uint32_t index_type = 0;
	EXIT_NOT_IMPLEMENTED(!Gen5::GraphicsDecodeIndexedUconfigVgtIndexType(cmd_id, buffer, dw, &index_type));

	cp->SetIndexType(index_type);
	return 2;
}

KYTY_CP_OP_PARSER(cp_op_wait_flip_done)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0051018);

	auto video_out_handle     = buffer[0];
	auto display_buffer_index = buffer[1];

	cp->WaitFlipDone(video_out_handle, display_buffer_index);

	return 6;
}

KYTY_CP_OP_PARSER(cp_op_wait_reg_mem_32)
{
	KYTY_PROFILER_FUNCTION();

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
	} else if (cmd_id == 0xc00c1028u)
	{
		func = buffer[3];
		ref  = buffer[4];
		poll = 10;
		used = 13;
	} else
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown WaitRegMem32 packet (continuing)\n");
	}

	cp->WaitRegMem32(func, addr, ref, mask, poll);

	return used;
}

KYTY_CP_OP_PARSER(cp_op_wait_reg_mem_64)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0071058);

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

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008600);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 1);

	cp->WaitCe();

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_wait_on_de_counter_diff)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008800);

	cp->WaitDeDiff(buffer[0]);

	return 1;
}

KYTY_CP_OP_PARSER(cp_op_wait_reg_mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0053C00);

	auto  func = buffer[0] & 0x7u;
	bool  me   = (buffer[0] & 0x100u) == 0;
	bool  mem  = (buffer[0] & 0x10u) != 0;
	auto* addr = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	auto  ref  = buffer[3];
	auto  mask = buffer[4];
	auto  poll = buffer[5];

	EXIT_NOT_IMPLEMENTED(!me);
	EXIT_NOT_IMPLEMENTED(!mem);

	cp->WaitRegMem32(func, addr, ref, mask, poll);

	return 6;
}

KYTY_CP_OP_PARSER(cp_op_write_const_ram)
{
	KYTY_PROFILER_FUNCTION();

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;
	auto offset = buffer[0];

	EXIT_NOT_IMPLEMENTED(dw_num >= 0x3000);
	EXIT_NOT_IMPLEMENTED(offset > 0xbffc);
	EXIT_NOT_IMPLEMENTED((offset & 0x3u) != 0);

	cp->WriteConstRam(offset, buffer + 1, dw_num);

	return 1 + dw_num;
}

KYTY_CP_OP_PARSER(cp_op_write_data)
{
	KYTY_PROFILER_FUNCTION();

	auto op = (cmd_id >> 8u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(op != Pm4::IT_WRITE_DATA && op != Pm4::IT_NOP);

	bool custom = (op == Pm4::IT_NOP);

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;

	auto  write_control = buffer[0];
	auto* dst           = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));

	const uint32_t  write_body_dwords  = 1u + dw_num;
	const uint32_t  next_packet_dwords = (dw > 1u + write_body_dwords) ? (dw - 1u - write_body_dwords) : 0u;
	const uint32_t* next_packet        = buffer + write_body_dwords;
	const bool      matching_wait_mem64 =
	    custom && GraphicsWriteDataPrecedesMatchingWaitMem64(buffer, write_body_dwords, next_packet, next_packet_dwords);

	cp->WriteData(dst, buffer + 3, dw_num - 2, write_control, custom, matching_wait_mem64);

	return 1 + dw_num;
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
