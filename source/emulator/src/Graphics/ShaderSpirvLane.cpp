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

bool HasFutureScalarSpillRead(const ShaderCode& code, uint32_t instruction_index, int register_id, int lane)
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

bool UsesNativeLaneExchange(const ShaderCode& code)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		switch (inst.type)
		{
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
