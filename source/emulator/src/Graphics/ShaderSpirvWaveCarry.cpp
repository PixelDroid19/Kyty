#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// v_add_co_ci_u32 / v_subrev_co_ci_u32 over both banks:
//   add:    D = S0 + S1 + CI,  carry  = 33-bit sum overflows
//   subrev: D = S1 - S0 - CI,  borrow = S0 + CI > S1 (33-bit)
// CI is the lane's bit of the src2 mask pair. The ISA text states the subrev
// borrow as "S1 + VCC > S0", copied from v_sub; the arithmetic definition of
// D fixes it as above. Carry bits of inactive lanes are zero; the mask pair is
// written after both banks' inputs are captured, so a VCC alias is safe.
bool Spirv::EmitComputeWaveCarryInstruction(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	const bool add = instruction.type == ShaderInstructionType::VAddCoCiU32;
	if (output == nullptr || !UsesComputeWaveBanks() || instruction.src_num != 3 || instruction.dst.type != ShaderOperandType::Vgpr ||
	    (!add && instruction.type != ShaderInstructionType::VSubrevCoCiU32))
	{
		return false;
	}
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;

	String8 source;
	String8 carry[2];
	int     slot = 0;
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto p = String8::FromPrintf("wave_carry_%u_%s", index, bank == ShaderWaveBank::Low ? "low" : "high");
		if (!EmitComputeWaveOperandUint(instruction.src[0], bank, p + "_s0", &source) ||
		    !EmitComputeWaveOperandUint(instruction.src[1], bank, p + "_s1", &source) ||
		    !EmitComputeWaveMaskBit(instruction.src[2], bank, p + "_ci_b", &source) || !EmitComputeWaveMaskBit(exec, bank, p + "_exec", &source))
		{
			return false;
		}
		source += String8::FromPrintf("%%%s_ci = OpSelect %%uint %%%s_ci_b %%uint_1 %%uint_0\n", p.c_str(), p.c_str());
		if (add)
		{
			source += String8(R"(
%<p>_t = OpIAdd %uint %<p>_s0 %<p>_s1
%<p>_c1 = OpULessThan %bool %<p>_t %<p>_s0
%<p>_value = OpIAdd %uint %<p>_t %<p>_ci
%<p>_c2 = OpULessThan %bool %<p>_value %<p>_t
%<p>_carry_raw = OpLogicalOr %bool %<p>_c1 %<p>_c2
)").ReplaceStr("<p>", p);
		} else
		{
			source += String8(R"(
%<p>_t = OpISub %uint %<p>_s1 %<p>_s0
%<p>_b1 = OpULessThan %bool %<p>_s1 %<p>_s0
%<p>_value = OpISub %uint %<p>_t %<p>_ci
%<p>_b2 = OpULessThan %bool %<p>_t %<p>_ci
%<p>_carry_raw = OpLogicalOr %bool %<p>_b1 %<p>_b2
)").ReplaceStr("<p>", p);
		}
		const auto target = GetComputeWaveRegister(instruction.dst, bank, 0);
		if (target.type != SpirvType::Float || !EmitComputeWaveOperandUint(instruction.dst, bank, p + "_old", &source))
		{
			return false;
		}
		source += String8(R"(
%<p>_carry = OpLogicalAnd %bool %<p>_carry_raw %<p>_exec
%<p>_result = OpSelect %uint %<p>_exec %<p>_value %<p>_old
%<p>_result_f = OpBitcast %float %<p>_result
               OpStore %<dst> %<p>_result_f
)").ReplaceStr("<p>", p).ReplaceStr("<dst>", target.value);
		carry[slot++] = p + "_carry";
	}
	const auto mask_low  = String8::FromPrintf("wave_carry_mask_low_%u", index);
	const auto mask_high = String8::FromPrintf("wave_carry_mask_high_%u", index);
	const auto low       = GetComputeWaveRegister(instruction.dst2, ShaderWaveBank::Low, 0);
	const auto high      = GetComputeWaveRegister(instruction.dst2, ShaderWaveBank::High, 1);
	if (low.type != SpirvType::Uint || high.type != SpirvType::Uint || !EmitComputeWaveBallot(carry[0], carry[1], mask_low, mask_high, &source))
	{
		return false;
	}
	source += String8::FromPrintf("               OpStore %%%s %%%s\n               OpStore %%%s %%%s\n", low.value.c_str(), mask_low.c_str(),
	                              high.value.c_str(), mask_high.c_str());
	*output += source;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
