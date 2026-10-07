#include "Emulator/Graphics/ShaderComputeWaveLds.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

bool IsKnownUintConstant(const String8& value)
{
	return !value.IsEmpty() && value != "unknown_uint_constant";
}

enum class LdsOperation
{
	Write,
	Read,
	AddReturn,
};

struct LdsBankSnapshot
{
	String8 stem;
	String8 address;
	String8 data;
	String8 old_destination;
	String8 exec;
};

struct LdsSnapshots
{
	LdsBankSnapshot low;
	LdsBankSnapshot high;
	SpirvValue      destination_low;
	SpirvValue      destination_high;
};

LdsBankSnapshot MakeLdsBankSnapshot(const char* bank, uint32_t index)
{
	LdsBankSnapshot snapshot {};
	snapshot.stem            = String8::FromPrintf("wave_lds_%s_%u", bank, index);
	snapshot.address         = snapshot.stem + "_address";
	snapshot.data            = snapshot.stem + "_data";
	snapshot.old_destination = snapshot.stem + "_old_destination";
	snapshot.exec            = snapshot.stem + "_exec";
	return snapshot;
}

bool EmitLdsSnapshots(const Spirv& spirv, const ShaderInstruction& instruction, uint32_t index, LdsOperation operation,
                      LdsSnapshots* snapshots, String8* output)
{
	if (snapshots == nullptr || output == nullptr)
	{
		return false;
	}

	snapshots->low               = MakeLdsBankSnapshot("low", index);
	snapshots->high              = MakeLdsBankSnapshot("high", index);
	const bool needs_data        = operation != LdsOperation::Read;
	const bool needs_destination = operation != LdsOperation::Write;
	if (needs_destination)
	{
		snapshots->destination_low  = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::Low, 0);
		snapshots->destination_high = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::High, 0);
		if (snapshots->destination_low.type != SpirvType::Float || snapshots->destination_high.type != SpirvType::Float ||
		    snapshots->destination_low.value.IsEmpty() || snapshots->destination_high.value.IsEmpty())
		{
			return false;
		}
	}

	// Capture every banked input before either bank can modify Workgroup memory
	// or a destination that aliases an address/data operand.
	if (!spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::Low, snapshots->low.address, output) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::High, snapshots->high.address, output) ||
	    (needs_data && !spirv.EmitComputeWaveOperandUint(instruction.src[1], ShaderWaveBank::Low, snapshots->low.data, output)) ||
	    (needs_data && !spirv.EmitComputeWaveOperandUint(instruction.src[1], ShaderWaveBank::High, snapshots->high.data, output)) ||
	    (needs_destination &&
	     !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::Low, snapshots->low.old_destination, output)) ||
	    (needs_destination &&
	     !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::High, snapshots->high.old_destination, output)))
	{
		return false;
	}

	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	return spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::Low, snapshots->low.exec, output) &&
	       spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::High, snapshots->high.exec, output);
}

void AppendLdsPointer(String8* output, const LdsBankSnapshot& snapshot, const String8& offset, const String8& shift)
{
	const auto byte_address = snapshot.stem + "_byte_address";
	const auto word_index   = snapshot.stem + "_word_index";
	const auto pointer      = snapshot.stem + "_pointer";
	*output += String8(R"(
%<byte_address> = OpIAdd %uint %<address> %<offset>
%<word_index> = OpShiftRightLogical %uint %<byte_address> %<shift>
%<pointer> = OpAccessChain %_ptr_Workgroup_uint %lds %<word_index>
)")
	               .ReplaceStr("<byte_address>", byte_address)
	               .ReplaceStr("<word_index>", word_index)
	               .ReplaceStr("<pointer>", pointer)
	               .ReplaceStr("<address>", snapshot.address)
	               .ReplaceStr("<offset>", offset)
	               .ReplaceStr("<shift>", shift);
}

void AppendGuardedLdsWrite(String8* output, const LdsBankSnapshot& snapshot, const String8& offset, const String8& shift)
{
	const auto then_label  = snapshot.stem + "_then";
	const auto merge_label = snapshot.stem + "_merge";
	const auto pointer     = snapshot.stem + "_pointer";
	*output += String8(R"(
               OpSelectionMerge %<merge> None
               OpBranchConditional %<exec> %<then> %<merge>
%<then> = OpLabel
)")
	               .ReplaceStr("<merge>", merge_label)
	               .ReplaceStr("<exec>", snapshot.exec)
	               .ReplaceStr("<then>", then_label);
	AppendLdsPointer(output, snapshot, offset, shift);
	*output += String8(R"(
               OpStore %<pointer> %<data>
               OpBranch %<merge>
%<merge> = OpLabel
)")
	               .ReplaceStr("<pointer>", pointer)
	               .ReplaceStr("<data>", snapshot.data)
	               .ReplaceStr("<merge>", merge_label);
}

void AppendGuardedLdsRead(String8* output, const LdsBankSnapshot& snapshot, const SpirvValue& destination, const String8& offset,
                          const String8& shift)
{
	const auto then_label  = snapshot.stem + "_then";
	const auto else_label  = snapshot.stem + "_else";
	const auto merge_label = snapshot.stem + "_merge";
	const auto pointer     = snapshot.stem + "_pointer";
	const auto loaded      = snapshot.stem + "_loaded";
	const auto result      = snapshot.stem + "_result";
	*output += String8(R"(
               OpSelectionMerge %<merge> None
               OpBranchConditional %<exec> %<then> %<else>
%<then> = OpLabel
)")
	               .ReplaceStr("<merge>", merge_label)
	               .ReplaceStr("<exec>", snapshot.exec)
	               .ReplaceStr("<then>", then_label)
	               .ReplaceStr("<else>", else_label);
	AppendLdsPointer(output, snapshot, offset, shift);
	*output += String8(R"(
%<loaded> = OpLoad %uint %<pointer>
               OpBranch %<merge>
%<else> = OpLabel
               OpBranch %<merge>
%<merge> = OpLabel
%<result> = OpPhi %uint %<loaded> %<then> %<old_destination> %<else>
%<result>_float = OpBitcast %float %<result>
               OpStore %<destination> %<result>_float
)")
	               .ReplaceStr("<loaded>", loaded)
	               .ReplaceStr("<pointer>", pointer)
	               .ReplaceStr("<merge>", merge_label)
	               .ReplaceStr("<else>", else_label)
	               .ReplaceStr("<then>", then_label)
	               .ReplaceStr("<result>", result)
	               .ReplaceStr("<old_destination>", snapshot.old_destination)
	               .ReplaceStr("<destination>", destination.value);
}

void AppendGuardedLdsAddReturn(String8* output, const LdsBankSnapshot& snapshot, const SpirvValue& destination, const String8& offset,
                               const String8& shift, const String8& scope, const String8& semantics)
{
	const auto then_label  = snapshot.stem + "_then";
	const auto else_label  = snapshot.stem + "_else";
	const auto merge_label = snapshot.stem + "_merge";
	const auto pointer     = snapshot.stem + "_pointer";
	const auto prior       = snapshot.stem + "_prior";
	const auto result      = snapshot.stem + "_result";
	*output += String8(R"(
               OpSelectionMerge %<merge> None
               OpBranchConditional %<exec> %<then> %<else>
%<then> = OpLabel
)")
	               .ReplaceStr("<merge>", merge_label)
	               .ReplaceStr("<exec>", snapshot.exec)
	               .ReplaceStr("<then>", then_label)
	               .ReplaceStr("<else>", else_label);
	AppendLdsPointer(output, snapshot, offset, shift);
	*output += String8(R"(
%<prior> = OpAtomicIAdd %uint %<pointer> %<scope> %<semantics> %<data>
               OpBranch %<merge>
%<else> = OpLabel
               OpBranch %<merge>
%<merge> = OpLabel
%<result> = OpPhi %uint %<prior> %<then> %<old_destination> %<else>
%<result>_float = OpBitcast %float %<result>
               OpStore %<destination> %<result>_float
)")
	               .ReplaceStr("<prior>", prior)
	               .ReplaceStr("<pointer>", pointer)
	               .ReplaceStr("<scope>", scope)
	               .ReplaceStr("<semantics>", semantics)
	               .ReplaceStr("<data>", snapshot.data)
	               .ReplaceStr("<merge>", merge_label)
	               .ReplaceStr("<else>", else_label)
	               .ReplaceStr("<then>", then_label)
	               .ReplaceStr("<result>", result)
	               .ReplaceStr("<old_destination>", snapshot.old_destination)
	               .ReplaceStr("<destination>", destination.value);
}

} // namespace

bool Spirv::EmitComputeWaveLdsInstruction(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks() || !ShaderComputeWaveLdsInstructionSupported(instruction))
	{
		return false;
	}
	const auto* input = GetCsInputInfo();
	if (input == nullptr || input->lds_dwords == 0u)
	{
		return false;
	}

	LdsOperation operation = LdsOperation::Write;
	switch (instruction.type)
	{
		case ShaderInstructionType::DsWriteB32: operation = LdsOperation::Write; break;
		case ShaderInstructionType::DsReadB32: operation = LdsOperation::Read; break;
		case ShaderInstructionType::DsAddRtnU32: operation = LdsOperation::AddReturn; break;
		default: return false;
	}

	const auto offset    = GetConstantUint(instruction.ds_offset);
	const auto shift     = GetConstantUint(2u);
	const auto scope     = operation == LdsOperation::AddReturn ? GetConstantUint(2u) : String8 {};
	const auto semantics = operation == LdsOperation::AddReturn ? GetConstantUint(SPIRV_WORKGROUP_MEMORY_ACQ_REL) : String8 {};
	if (!IsKnownUintConstant(offset) || !IsKnownUintConstant(shift) ||
	    (operation == LdsOperation::AddReturn && (!IsKnownUintConstant(scope) || !IsKnownUintConstant(semantics))))
	{
		return false;
	}

	LdsSnapshots snapshots {};
	String8      source;
	if (!EmitLdsSnapshots(*this, instruction, index, operation, &snapshots, &source))
	{
		return false;
	}

	switch (operation)
	{
		case LdsOperation::Write:
			AppendGuardedLdsWrite(&source, snapshots.low, offset, shift);
			AppendGuardedLdsWrite(&source, snapshots.high, offset, shift);
			break;
		case LdsOperation::Read:
			AppendGuardedLdsRead(&source, snapshots.low, snapshots.destination_low, offset, shift);
			AppendGuardedLdsRead(&source, snapshots.high, snapshots.destination_high, offset, shift);
			break;
		case LdsOperation::AddReturn:
			AppendGuardedLdsAddReturn(&source, snapshots.low, snapshots.destination_low, offset, shift, scope, semantics);
			AppendGuardedLdsAddReturn(&source, snapshots.high, snapshots.destination_high, offset, shift, scope, semantics);
			break;
	}

	*output += source;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
