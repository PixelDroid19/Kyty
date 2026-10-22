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
	if (instruction.type == ShaderInstructionType::VMadU64U32)
	{
		return EmitComputeWaveMadU64Instruction(instruction, index, output);
	}
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

// v_mad_u64_u32 over both banks: {carry, D.u64} = S0.u32 * S1.u32 + S2.u64.
// A literal S2 is zero-extended and an inline integer sign-extended to 64
// bits (ISA "Literal Expansion to 64 bits"). Carry bits of inactive lanes are
// zero; the mask pair is written after both banks' inputs are captured.
bool Spirv::EmitComputeWaveMadU64Instruction(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks() || instruction.src_num != 3 || instruction.dst.type != ShaderOperandType::Vgpr ||
	    instruction.dst.size != 2 || instruction.src[2].type == ShaderOperandType::FloatInlineConstant)
	{
		return false;
	}
	// One 32-bit word of an operand; a constant has no register words.
	const auto word = [](ShaderOperand operand, int w)
	{
		operand.size = operand_is_constant(operand) ? 0 : 1;
		operand.register_id += operand_is_constant(operand) ? 0 : w;
		return operand;
	};
	const auto& addend      = instruction.src[2];
	const bool  addend_wide = !operand_is_constant(addend);
	const auto  addend_high =
	    GetConstantUint(addend.type == ShaderOperandType::IntegerInlineConstant && addend.constant.i < 0 ? 0xffffffffu : 0u);
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;

	String8 source;
	String8 carry[2];
	int     slot = 0;
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto p = String8::FromPrintf("wave_mad_%u_%s", index, bank == ShaderWaveBank::Low ? "low" : "high");
		if (!EmitComputeWaveOperandUint(word(instruction.src[0], 0), bank, p + "_s0", &source) ||
		    !EmitComputeWaveOperandUint(word(instruction.src[1], 0), bank, p + "_s1", &source) ||
		    !EmitComputeWaveOperandUint(word(addend, 0), bank, p + "_a0", &source) ||
		    (addend_wide && !EmitComputeWaveOperandUint(word(addend, 1), bank, p + "_a1", &source)) ||
		    !EmitComputeWaveOperandUint(word(instruction.dst, 0), bank, p + "_old0", &source) ||
		    !EmitComputeWaveOperandUint(word(instruction.dst, 1), bank, p + "_old1", &source) ||
		    !EmitComputeWaveMaskBit(exec, bank, p + "_exec", &source))
		{
			return false;
		}
		const auto target_low  = GetComputeWaveRegister(word(instruction.dst, 0), bank, 0);
		const auto target_high = GetComputeWaveRegister(word(instruction.dst, 1), bank, 0);
		if (target_low.type != SpirvType::Float || target_high.type != SpirvType::Float)
		{
			return false;
		}
		if (!addend_wide)
		{
			source += String8::FromPrintf("%%%s_a1 = OpCopyObject %%uint %%%s\n", p.c_str(), addend_high.c_str());
		}
		source += String8(R"(
%<p>_mul = OpUMulExtended %ResTypeU %<p>_s0 %<p>_s1
%<p>_mul_lo = OpCompositeExtract %uint %<p>_mul 0
%<p>_mul_hi = OpCompositeExtract %uint %<p>_mul 1
%<p>_lo_pair = OpIAddCarry %ResTypeU %<p>_mul_lo %<p>_a0
%<p>_lo = OpCompositeExtract %uint %<p>_lo_pair 0
%<p>_c0 = OpCompositeExtract %uint %<p>_lo_pair 1
%<p>_hi_pair = OpIAddCarry %ResTypeU %<p>_mul_hi %<p>_a1
%<p>_hi_base = OpCompositeExtract %uint %<p>_hi_pair 0
%<p>_c1 = OpCompositeExtract %uint %<p>_hi_pair 1
%<p>_hi_full = OpIAddCarry %ResTypeU %<p>_hi_base %<p>_c0
%<p>_hi = OpCompositeExtract %uint %<p>_hi_full 0
%<p>_c2 = OpCompositeExtract %uint %<p>_hi_full 1
%<p>_carry_u = OpBitwiseOr %uint %<p>_c1 %<p>_c2
%<p>_carry_raw = OpINotEqual %bool %<p>_carry_u %<zero>
%<p>_carry = OpLogicalAnd %bool %<p>_carry_raw %<p>_exec
%<p>_result_lo = OpSelect %uint %<p>_exec %<p>_lo %<p>_old0
%<p>_result_hi = OpSelect %uint %<p>_exec %<p>_hi %<p>_old1
%<p>_result_lo_f = OpBitcast %float %<p>_result_lo
%<p>_result_hi_f = OpBitcast %float %<p>_result_hi
               OpStore %<dst_lo> %<p>_result_lo_f
               OpStore %<dst_hi> %<p>_result_hi_f
)")
		              .ReplaceStr("<p>", p)
		              .ReplaceStr("<dst_lo>", target_low.value)
		              .ReplaceStr("<dst_hi>", target_high.value)
		              .ReplaceStr("<zero>", GetConstantUint(0u));
		carry[slot++] = p + "_carry";
	}
	const auto mask_low  = String8::FromPrintf("wave_mad_mask_low_%u", index);
	const auto mask_high = String8::FromPrintf("wave_mad_mask_high_%u", index);
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
