#include "ShaderSpirvInternal.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

ShaderOperand PlainSource(ShaderOperand operand)
{
	operand.dpp = false;
	operand.dpp_ctrl = 0;
	operand.dpp_row_mask = 0;
	operand.dpp_bank_mask = 0;
	operand.dpp_fetch_inactive = false;
	operand.dpp_bound_ctrl = false;
	return operand;
}

const char* BinaryOpcode(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::VAndB32: return "OpBitwiseAnd";
		case ShaderInstructionType::VOrB32: return "OpBitwiseOr";
		case ShaderInstructionType::VXorB32: return "OpBitwiseXor";
		default: return nullptr;
	}
}

String8 DppTarget(const Spirv& spirv, uint32_t control, const String8& tag)
{
	String8 source;
	if (control <= 0xffu)
	{
		source = "%<t>_quad = OpBitwiseAnd %uint %wave_lane_id %uint_3\n"
		         "%<t>_shift = OpShiftLeftLogical %uint %<t>_quad %uint_1\n"
		         "%<t>_table = OpShiftRightLogical %uint %<control> %<t>_shift\n"
		         "%<t>_select = OpBitwiseAnd %uint %<t>_table %uint_3\n"
		         "%<t>_quad_base = OpBitwiseAnd %uint %wave_lane_id %<quad_mask>\n"
		         "%<t>_target = OpBitwiseOr %uint %<t>_quad_base %<t>_select\n"
		         "%<t>_bounds = OpCopyObject %bool %true\n";
	} else if (control == 0x140u || control == 0x141u)
	{
		source = "%<t>_target = OpBitwiseXor %uint %wave_lane_id %<mirror>\n"
		         "%<t>_bounds = OpCopyObject %bool %true\n";
	} else
	{
		const bool left = control <= 0x10fu;
		const bool rotate = control >= 0x121u;
		source = "%<t>_local = OpBitwiseAnd %uint %wave_lane_id %uint_15\n"
		         "%<t>_base = OpBitwiseAnd %uint %wave_lane_id %<row_mask>\n"
		         "%<t>_offset = <operation> %uint %<t>_local %<step>\n"
		         "%<t>_wrapped = OpBitwiseAnd %uint %<t>_offset %uint_15\n"
		         "%<t>_target = OpBitwiseOr %uint %<t>_base %<t>_wrapped\n";
		source += rotate ? "%<t>_bounds = OpCopyObject %bool %true\n"
		                 : left ? "%<t>_bounds = OpULessThan %bool %<t>_offset %uint_16\n"
		                        : "%<t>_bounds = OpUGreaterThanEqual %bool %<t>_local %<step>\n";
		source = source.ReplaceStr("<operation>", left ? "OpIAdd" : "OpISub");
	}
	return source.ReplaceStr("<t>", tag)
	    .ReplaceStr("<control>", spirv.GetConstantUint(control))
	    .ReplaceStr("<quad_mask>", spirv.GetConstantUint(0xfffffffcu))
	    .ReplaceStr("<row_mask>", spirv.GetConstantUint(0xfffffff0u))
	    .ReplaceStr("<mirror>", spirv.GetConstantUint(control == 0x140u ? 15u : 7u))
	    .ReplaceStr("<step>", spirv.GetConstantUint(control & 15u));
}

bool TargetAlwaysInBounds(uint32_t control)
{
	return control <= 0xffu || control >= 0x121u;
}

String8 Fetch(const String8& tag, ShaderWaveBank bank, bool fetch_inactive, bool in_bounds)
{
	// Every admitted target remains inside its 32-lane bank. Architectural
	// EXEC is uniform, so source activity can be tested without a second shuffle.
	String8 source = "%<t>_fetched = OpGroupNonUniformShuffle %uint %uint_3 %<t>_src %<t>_target\n";
	if (!fetch_inactive)
	{
		source += "%<t>_src_exec_word = OpLoad %uint %<exec>\n"
		          "%<t>_src_bit = OpShiftLeftLogical %uint %uint_1 %<t>_target\n"
		          "%<t>_src_mask = OpBitwiseAnd %uint %<t>_src_exec_word %<t>_src_bit\n"
		          "%<t>_src_active = OpINotEqual %bool %<t>_src_mask %uint_0\n";
		source += in_bounds ? "%<t>_available = OpCopyObject %bool %<t>_src_active\n"
		                    : "%<t>_available = OpLogicalAnd %bool %<t>_src_active %<t>_bounds\n";
	} else
	{
		source += in_bounds ? "%<t>_available = OpCopyObject %bool %true\n"
		                    : "%<t>_available = OpCopyObject %bool %<t>_bounds\n";
	}
	source += "%<t>_value = OpSelect %uint %<t>_available %<t>_fetched %uint_0\n";
	return source.ReplaceStr("<t>", tag).ReplaceStr("<exec>", bank == ShaderWaveBank::Low ? "exec_lo" : "exec_hi");
}

String8 DppDestination(const Spirv& spirv, const ShaderOperand& operand, const String8& tag, ShaderWaveBank bank)
{
	String8 source;
	if (operand.dpp_row_mask == 15u && operand.dpp_bank_mask == 15u)
	{
		// Full masks admit every row and quarter-bank without dynamic index work.
		source = "%<t>_enabled = OpCopyObject %bool %<t>_exec\n";
	} else
	{
		source = "%<t>_row0 = OpShiftRightLogical %uint %wave_lane_id %uint_4\n"
	                 "%<t>_row = OpIAdd %uint %<t>_row0 %<bank_base>\n"
	                 "%<t>_row_bit = OpShiftLeftLogical %uint %uint_1 %<t>_row\n"
	                 "%<t>_rows = OpBitwiseAnd %uint %<t>_row_bit %<rows>\n"
	                 "%<t>_row_enabled = OpINotEqual %bool %<t>_rows %uint_0\n"
	                 "%<t>_bank0 = OpShiftRightLogical %uint %wave_lane_id %uint_2\n"
	                 "%<t>_bank = OpBitwiseAnd %uint %<t>_bank0 %uint_3\n"
	                 "%<t>_bank_bit = OpShiftLeftLogical %uint %uint_1 %<t>_bank\n"
	                 "%<t>_banks = OpBitwiseAnd %uint %<t>_bank_bit %<banks>\n"
	                 "%<t>_bank_enabled = OpINotEqual %bool %<t>_banks %uint_0\n"
	                 "%<t>_masked = OpLogicalAnd %bool %<t>_row_enabled %<t>_bank_enabled\n"
	                 "%<t>_enabled = OpLogicalAnd %bool %<t>_masked %<t>_exec\n";
	}
	source += operand.dpp_bound_ctrl || TargetAlwaysInBounds(operand.dpp_ctrl)
	              ? "%<t>_write = OpCopyObject %bool %<t>_enabled\n"
	              : "%<t>_write = OpLogicalAnd %bool %<t>_enabled %<t>_bounds\n";
	return source.ReplaceStr("<t>", tag)
	    .ReplaceStr("<bank_base>", spirv.GetConstantUint(bank == ShaderWaveBank::Low ? 0u : 2u))
	    .ReplaceStr("<rows>", spirv.GetConstantUint(operand.dpp_row_mask))
	    .ReplaceStr("<banks>", spirv.GetConstantUint(operand.dpp_bank_mask));
}

bool LoadBank(const Spirv& spirv, const ShaderInstruction& instruction, ShaderWaveBank bank, const String8& tag, String8* output)
{
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	return spirv.EmitComputeWaveOperandUint(PlainSource(instruction.src[0]), bank, tag + "_src", output) &&
	       spirv.EmitComputeWaveOperandUint(instruction.dst, bank, tag + "_old", output) &&
	       spirv.EmitComputeWaveMaskBit(exec, bank, tag + "_exec", output);
}

String8 Store(const Spirv& spirv, const ShaderInstruction& instruction, ShaderWaveBank bank, const String8& tag)
{
	const auto destination = spirv.GetComputeWaveRegister(instruction.dst, bank, 0);
	return String8("%<t>_result = OpSelect %uint %<t>_write %<t>_new %<t>_old\n"
	               "%<t>_float = OpBitcast %float %<t>_result\nOpStore %<dst> %<t>_float\n")
	    .ReplaceStr("<t>", tag).ReplaceStr("<dst>", destination.value);
}

} // namespace

bool Spirv::EmitComputeWaveDppInstruction(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks() || !ShaderComputeWaveDppInstructionSupported(instruction)) { return false; }
	if (instruction.src[0].dpp_row_mask == 0u || instruction.src[0].dpp_bank_mask == 0u)
	{
		// These admitted instructions have no effects besides their masked VGPR write.
		return true;
	}
	String8 source;
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto tag = String8::FromPrintf("wave_dpp_%u_%s", index, bank == ShaderWaveBank::Low ? "low" : "high");
		if (!LoadBank(*this, instruction, bank, tag, &source)) { return false; }
		source += DppTarget(*this, instruction.src[0].dpp_ctrl, tag);
		source += Fetch(tag, bank, instruction.src[0].dpp_fetch_inactive, TargetAlwaysInBounds(instruction.src[0].dpp_ctrl));
		source += DppDestination(*this, instruction.src[0], tag, bank);
		if (instruction.type == ShaderInstructionType::VMovB32)
		{
			source += String8::FromPrintf("%%%s_new = OpCopyObject %%uint %%%s_value\n", tag.c_str(), tag.c_str());
		} else
		{
			if (!EmitComputeWaveOperandUint(instruction.src[1], bank, tag + "_other", &source)) { return false; }
			source += String8::FromPrintf("%%%s_new = %s %%uint %%%s_value %%%s_other\n", tag.c_str(),
			                              BinaryOpcode(instruction.type), tag.c_str(), tag.c_str());
		}
	}
	// Capture both source banks before either store, including dst==src.
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto tag = String8::FromPrintf("wave_dpp_%u_%s", index, bank == ShaderWaveBank::Low ? "low" : "high");
		source += Store(*this, instruction, bank, tag);
	}
	*output += source;
	return true;
}

bool Spirv::EmitComputeWavePermutation(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks() || !ShaderComputeWavePermutationSupported(instruction)) { return false; }
	String8 source;
	const auto prefix = String8::FromPrintf("wave_permute_%u", index);
	if (!EmitComputeWaveOperandUint(instruction.src[1], ShaderWaveBank::Low, prefix + "_table_low", &source) ||
	    !EmitComputeWaveOperandUint(instruction.src[2], ShaderWaveBank::Low, prefix + "_table_high", &source)) { return false; }
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto tag = prefix + (bank == ShaderWaveBank::Low ? "_low" : "_high");
		if (!LoadBank(*this, instruction, bank, tag, &source)) { return false; }
		source += String8(
		    "%<t>_local = OpBitwiseAnd %uint %wave_lane_id %uint_15\n"
		    "%<t>_second = OpUGreaterThanEqual %bool %<t>_local %uint_8\n"
		    "%<t>_table = OpSelect %uint %<t>_second %<p>_table_high %<p>_table_low\n"
		    "%<t>_nibble = OpBitwiseAnd %uint %<t>_local %uint_7\n"
		    "%<t>_shift = OpShiftLeftLogical %uint %<t>_nibble %uint_2\n"
		    "%<t>_shifted = OpShiftRightLogical %uint %<t>_table %<t>_shift\n"
		    "%<t>_selected = OpBitwiseAnd %uint %<t>_shifted %uint_15\n"
		    "%<t>_own_row = OpBitwiseAnd %uint %wave_lane_id %<row_mask>\n"
		    "%<t>_row = OpBitwiseXor %uint %<t>_own_row %<row_xor>\n"
		    "%<t>_target = OpBitwiseOr %uint %<t>_row %<t>_selected\n"
		    "%<t>_bounds = OpCopyObject %bool %true\n")
		    .ReplaceStr("<t>", tag).ReplaceStr("<p>", prefix)
		    .ReplaceStr("<row_mask>", GetConstantUint(0xfffffff0u))
		    .ReplaceStr("<row_xor>", GetConstantUint(instruction.type == ShaderInstructionType::VPermlanex16B32 ? 16u : 0u));
		source += Fetch(tag, bank, (instruction.vop3_op_sel & 1u) != 0u, true);
		source += String8::FromPrintf("%%%s_new = OpCopyObject %%uint %%%s_value\n"
		                              "%%%s_write = OpCopyObject %%bool %%%s_exec\n",
		                              tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str());
	}
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		source += Store(*this, instruction, bank, prefix + (bank == ShaderWaveBank::Low ? "_low" : "_high"));
	}
	*output += source;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
