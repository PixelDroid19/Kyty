#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool IsStaticScalarSpillWrite(const ShaderInstruction& inst, int* register_id, int* lane)
{
	if (inst.type != ShaderInstructionType::VWritelaneB32 || inst.dst.type != ShaderOperandType::Vgpr || inst.src_num < 2 ||
	    inst.src[1].type != ShaderOperandType::IntegerInlineConstant || inst.src[1].constant.i < 0 || inst.src[1].constant.i > 63)
	{
		return false;
	}

	if (register_id != nullptr)
	{
		*register_id = inst.dst.register_id;
	}
	if (lane != nullptr)
	{
		*lane = inst.src[1].constant.i;
	}
	return true;
}

bool IsStaticScalarSpillRead(const ShaderInstruction& inst, int* register_id, int* lane)
{
	if (inst.type != ShaderInstructionType::VReadlaneB32 || inst.src_num < 2 || inst.src[0].type != ShaderOperandType::Vgpr ||
	    inst.src[1].type != ShaderOperandType::IntegerInlineConstant || inst.src[1].constant.i < 0 || inst.src[1].constant.i > 63)
	{
		return false;
	}

	if (register_id != nullptr)
	{
		*register_id = inst.src[0].register_id;
	}
	if (lane != nullptr)
	{
		*lane = inst.src[1].constant.i;
	}
	return true;
}

String8 ScalarSpillSlotName(int register_id, int lane)
{
	return String8::FromPrintf("spill_v%d_lane%d", register_id, lane);
}

bool HasLiveScalarSpill(const ShaderCode& code, uint32_t instruction_index, int register_id, int lane)
{
	bool live = false;
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = 0; index < instruction_index && index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		int         written_register = 0;
		int         written_lane     = 0;
		if (IsStaticScalarSpillWrite(inst, &written_register, &written_lane))
		{
			if (written_register == register_id && written_lane == lane)
			{
				live = true;
			}
			continue;
		}

		if (instruction_writes_vgpr(inst, register_id))
		{
			live = false;
		}
	}
	return live;
}

bool HasInvalidatedScalarSpill(const ShaderCode& code, uint32_t instruction_index, int register_id, int lane)
{
	bool live = false;
	bool invalidated = false;
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = 0; index < instruction_index && index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		int         written_register = 0;
		int         written_lane     = 0;
		if (IsStaticScalarSpillWrite(inst, &written_register, &written_lane))
		{
			if (written_register == register_id && written_lane == lane)
			{
				live        = true;
				invalidated = false;
			}
			continue;
		}

		if (instruction_writes_vgpr(inst, register_id))
		{
			if (live)
			{
				invalidated = true;
			}
			live = false;
		}
	}
	return invalidated && !live;
}

static bool HasForwardScalarSpillRead(const ShaderCode& code, uint32_t instruction_index, int register_id, int lane)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = instruction_index + 1; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		int         read_register = 0;
		int         read_lane     = 0;
		if (IsStaticScalarSpillRead(inst, &read_register, &read_lane))
		{
			if (read_register == register_id && read_lane == lane)
			{
				return true;
			}
			continue;
		}

		int written_register = 0;
		int written_lane     = 0;
		if (IsStaticScalarSpillWrite(inst, &written_register, &written_lane))
		{
			continue;
		}

		if (instruction_writes_vgpr(inst, register_id))
		{
			return false;
		}
	}
	return false;
}

static bool IsBranch(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SBranch:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchExecnz: return true;
		default: return false;
	}
}

// No instruction in [first, last] writes the register other than through a static spill slot.
static bool LoopKeepsSpillRegister(const ShaderCode& code, uint32_t first, uint32_t last, int register_id)
{
	for (uint32_t index = first; index <= last; ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		if (!IsStaticScalarSpillWrite(inst, nullptr, nullptr) && instruction_writes_vgpr(inst, register_id)) { return false; }
	}
	return true;
}

// A read before the write inside a loop around it, reached again through the back edge.
static bool HasLoopCarriedScalarSpillRead(const ShaderCode& code, uint32_t write_index, int register_id, int lane)
{
	const auto& instructions = code.GetInstructions();
	const uint32_t write_pc = instructions.At(write_index).pc;
	for (uint32_t back = write_index + 1; back < instructions.Size(); ++back)
	{
		const auto& branch = instructions.At(back);
		if (!IsBranch(branch.type) || branch.src_num < 1) { continue; }
		const int64_t target = static_cast<int64_t>(branch.pc) + 4 + branch.src[0].constant.i;
		if (target < 0 || target > write_pc) { continue; }
		uint32_t head = 0;
		while (head < write_index && instructions.At(head).pc != static_cast<uint32_t>(target)) { ++head; }
		if (head == write_index || !LoopKeepsSpillRegister(code, head, back, register_id)) { continue; }
		for (uint32_t index = head; index < write_index; ++index)
		{
			int read_register = 0;
			int read_lane     = 0;
			if (IsStaticScalarSpillRead(instructions.At(index), &read_register, &read_lane) && read_register == register_id &&
			    read_lane == lane)
			{
				return true;
			}
		}
	}
	return false;
}

bool HasFutureScalarSpillRead(const ShaderCode& code, uint32_t instruction_index, int register_id, int lane)
{
	return HasForwardScalarSpillRead(code, instruction_index, register_id, lane) ||
	       HasLoopCarriedScalarSpillRead(code, instruction_index, register_id, lane);
}

bool UsesNativeLaneExchange(const ShaderCode& code)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		switch (inst.type)
		{
			case ShaderInstructionType::VPermlane16B32:
			case ShaderInstructionType::VPermlanex16B32: return true;
			case ShaderInstructionType::VReadfirstlaneB32:
				if (!ShaderReadfirstlaneCanUseUniformCopy(code, index))
				{
					return true;
				}
				break;
			case ShaderInstructionType::VReadlaneB32:
			{
				int register_id = 0;
				int lane         = 0;
				if (!IsStaticScalarSpillRead(inst, &register_id, &lane) ||
				    !HasLiveScalarSpill(code, index, register_id, lane))
				{
					return true;
				}
				break;
			}
			case ShaderInstructionType::VWritelaneB32:
			{
				int register_id = 0;
				int lane         = 0;
				if (!IsStaticScalarSpillWrite(inst, &register_id, &lane) ||
				    !HasFutureScalarSpillRead(code, index, register_id, lane))
				{
					return true;
				}
				break;
			}
			default: break;
		}
	}
	return false;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
