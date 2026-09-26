#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

const char* BankName(ShaderWaveBank bank)
{
	return bank == ShaderWaveBank::Low ? "low" : "high";
}

// Loads a 32-bit mbcnt mask source. A one-word EXEC operand is its low word.
bool EmitMaskWord(const Spirv& spirv, const ShaderOperand& operand, ShaderWaveBank bank, const String8& id, String8* output)
{
	if ((operand.type == ShaderOperandType::ExecLo || operand.type == ShaderOperandType::ExecHi) && operand.size <= 1)
	{
		*output += String8::FromPrintf("%%%s = OpLoad %%uint %%%s\n", id.c_str(),
		                               operand.type == ShaderOperandType::ExecLo ? "exec_lo" : "exec_hi");
		return true;
	}
	return spirv.EmitComputeWaveOperandUint(operand, bank, id, output);
}

// Stores value to the bank's destination VGPR for lanes active in EXEC.
bool EmitGuardedStore(const Spirv& spirv, const ShaderOperand& destination, ShaderWaveBank bank, const String8& value,
                      const String8& prefix, String8* output)
{
	const auto target = spirv.GetComputeWaveRegister(destination, bank, 0);
	if (target.type != SpirvType::Float || target.value.IsEmpty())
	{
		return false;
	}
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	if (!spirv.EmitComputeWaveMaskBit(exec, bank, prefix + "_exec", output) ||
	    !spirv.EmitComputeWaveOperandUint(destination, bank, prefix + "_old", output))
	{
		return false;
	}
	*output += String8::FromPrintf("%%%s_result = OpSelect %%uint %%%s_exec %%%s %%%s_old\n"
	                               "%%%s_float = OpBitcast %%float %%%s_result\n"
	                               "               OpStore %%%s %%%s_float\n",
	                               prefix.c_str(), prefix.c_str(), value.c_str(), prefix.c_str(), prefix.c_str(), prefix.c_str(),
	                               target.value.c_str(), prefix.c_str());
	return true;
}

} // namespace

// v_mbcnt_{lo,hi}_u32_b32 over wave position p = lane + 32 * bank:
// D = S1 + popcount(S0 & ThreadMask), ThreadMask = (1 << p) - 1 restricted to
// the instruction's 32-bit half of the 64-bit wave.
bool Spirv::EmitComputeWaveMbcnt(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks() || instruction.src_num != 2 || instruction.dst.type != ShaderOperandType::Vgpr)
	{
		return false;
	}
	const bool low_half = instruction.type == ShaderInstructionType::VMbcntLoU32B32;
	const auto one      = GetConstantUint(1u);
	const auto zero     = GetConstantUint(0u);
	const auto all      = GetConstantUint(0xffffffffu);

	String8 source;
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto prefix = String8::FromPrintf("wave_mbcnt_%u_%s", index, BankName(bank));
		if (!EmitMaskWord(*this, instruction.src[0], bank, prefix + "_s0", &source) ||
		    !EmitComputeWaveOperandUint(instruction.src[1], bank, prefix + "_s1", &source))
		{
			return false;
		}
		// The lane's own half uses the partial mask; the other half is all
		// lanes (below this one) or none (above this one).
		const bool own_half = (bank == ShaderWaveBank::Low) == low_half;
		source += String8::FromPrintf("%%%s_bit = OpShiftLeftLogical %%uint %%%s %%wave_lane_id\n"
		                              "%%%s_partial = OpISub %%uint %%%s_bit %%%s\n",
		                              prefix.c_str(), one.c_str(), prefix.c_str(), prefix.c_str(), one.c_str());
		const auto mask = own_half ? prefix + "_partial" : (low_half ? all : zero);
		source += String8::FromPrintf("%%%s_masked = OpBitwiseAnd %%uint %%%s_s0 %%%s\n"
		                              "%%%s_count = OpBitCount %%uint %%%s_masked\n"
		                              "%%%s_value = OpIAdd %%uint %%%s_s1 %%%s_count\n",
		                              prefix.c_str(), prefix.c_str(), mask.c_str(), prefix.c_str(), prefix.c_str(), prefix.c_str(),
		                              prefix.c_str(), prefix.c_str());
		if (!EmitGuardedStore(*this, instruction.dst, bank, prefix + "_value", prefix, &source))
		{
			return false;
		}
	}
	*output += source;
	return true;
}

// ds_append/ds_consume on GDS: the wave adds (subtracts) the number of active
// lanes once and every active lane receives the counter's previous value.
bool Spirv::EmitComputeWaveAppend(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	const auto* bind = GetBindInfo();
	if (output == nullptr || !UsesComputeWaveBanks() || bind == nullptr || bind->gds_pointers.pointers_num <= 0 ||
	    instruction.dst.type != ShaderOperandType::Vgpr)
	{
		return false;
	}
	const char* atomic = instruction.type == ShaderInstructionType::DsAppend ? "OpAtomicIAdd" : "OpAtomicISub";
	const auto  zero   = GetConstantUint(0u);
	const auto  one    = GetConstantUint(1u);
	const auto  scope  = GetConstantUint(3u);
	const auto  shift  = GetConstantUint(16u);
	const auto  p      = String8::FromPrintf("wave_append_%u", index);

	// Only the elected invocation contributes the count, so the other
	// invocations' atomics add zero and cannot change the counter.
	String8 source = String8::FromPrintf(
	    "%%%s_lo = OpLoad %%uint %%exec_lo\n"
	    "%%%s_hi = OpLoad %%uint %%exec_hi\n"
	    "%%%s_clo = OpBitCount %%uint %%%s_lo\n"
	    "%%%s_chi = OpBitCount %%uint %%%s_hi\n"
	    "%%%s_count = OpIAdd %%uint %%%s_clo %%%s_chi\n"
	    "%%%s_elect = OpGroupNonUniformElect %%bool %%%s\n"
	    "%%%s_value = OpSelect %%uint %%%s_elect %%%s_count %%%s\n"
	    "%%%s_m0 = OpLoad %%uint %%m0\n"
	    "%%%s_index = OpShiftRightLogical %%uint %%%s_m0 %%%s\n"
	    "%%%s_ptr = OpAccessChain %%_ptr_StorageBuffer_uint %%gds %%int_0 %%%s_index\n"
	    "%%%s_old = %s %%uint %%%s_ptr %%%s %%%s %%%s_value\n"
	    "%%%s_first = OpGroupNonUniformBroadcastFirst %%uint %%%s %%%s_old\n"
	    "               OpMemoryBarrier %%%s %%uint_72\n",
	    p.c_str(), p.c_str(), p.c_str(), p.c_str(), p.c_str(), p.c_str(), p.c_str(), p.c_str(), p.c_str(), p.c_str(), scope.c_str(),
	    p.c_str(), p.c_str(), p.c_str(), zero.c_str(), p.c_str(), p.c_str(), p.c_str(), shift.c_str(), p.c_str(), p.c_str(), p.c_str(),
	    atomic, p.c_str(), one.c_str(), zero.c_str(), p.c_str(), p.c_str(), scope.c_str(), p.c_str(), one.c_str());
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		if (!EmitGuardedStore(*this, instruction.dst, bank, p + "_first", p + "_" + BankName(bank), &source))
		{
			return false;
		}
	}
	*output += source;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
