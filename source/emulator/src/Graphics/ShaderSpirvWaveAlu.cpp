#include "Emulator/Graphics/ShaderComputeWaveAlu.h"

#include "Emulator/Graphics/ShaderComputeWaveSdwa.h"

#include "ShaderSpirvInternal.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr int kMaxSgpr = 105; // RDNA2 SGPR0..SGPR105
constexpr int kMaxVgpr = 255;

bool ComputeWaveAluOperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool ComputeWaveAluRegisterRangeIsValid(int register_id, int size, int maximum_register)
{
	if (register_id < 0 || size <= 0 || register_id > maximum_register)
	{
		return false;
	}

	return size <= maximum_register - register_id + 1;
}

bool ComputeWaveAluOrdinaryVgpr(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Vgpr && operand.size == 1 && ComputeWaveAluOperandIsPlain(operand) &&
	       ComputeWaveAluRegisterRangeIsValid(operand.register_id, operand.size, kMaxVgpr);
}

bool ComputeWaveAluOrdinarySgpr(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Sgpr && operand.size == 1 && ComputeWaveAluOperandIsPlain(operand) &&
	       ComputeWaveAluRegisterRangeIsValid(operand.register_id, operand.size, kMaxSgpr);
}

bool ComputeWaveAluIntegerOrLiteralConstant(const ShaderOperand& operand)
{
	return ComputeWaveAluOperandIsPlain(operand) && operand.size == 0 &&
	       (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant);
}

bool ComputeWaveAluSourceSupported(const ShaderOperand& operand)
{
	return ComputeWaveAluOrdinaryVgpr(operand) || ComputeWaveAluOrdinarySgpr(operand) || ComputeWaveAluIntegerOrLiteralConstant(operand);
}

bool ComputeWaveAluUnusedDestination(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Unknown && operand.size == 0 && ComputeWaveAluOperandIsPlain(operand);
}

// One 32-bit VCC word read as wave-uniform scalar data.
bool ComputeWaveAluVccWord(const ShaderOperand& operand)
{
	return ComputeWaveAluOperandIsPlain(operand) && operand.size == 1 && operand.register_id == 0 &&
	       (operand.type == ShaderOperandType::VccLo || operand.type == ShaderOperandType::VccHi);
}

// RDNA2 section 6.2: at most two scalar values (SGPRs, VCC, EXEC data and one
// literal) per instruction; a repeated value counts once and inline constants
// are free.
bool ComputeWaveAluScalarValuesWithinLimit(const ShaderInstruction& instruction)
{
	int  scalar_values = 0;
	bool literal       = false;
	for (int source = 0; source < instruction.src_num; ++source)
	{
		const auto& operand = instruction.src[source];
		if (operand.type == ShaderOperandType::LiteralConstant)
		{
			scalar_values += literal ? 0 : 1;
			literal = true;
			continue;
		}
		if (operand.type != ShaderOperandType::Sgpr && !ComputeWaveAluVccWord(operand))
		{
			continue;
		}
		bool repeated = false;
		for (int earlier = 0; earlier < source; ++earlier)
		{
			repeated = repeated || (instruction.src[earlier].type == operand.type && instruction.src[earlier].register_id == operand.register_id);
		}
		scalar_values += repeated ? 0 : 1;
	}
	return scalar_values <= 2;
}

bool ComputeWaveAluTernarySourceSupported(const ShaderOperand& operand)
{
	return ComputeWaveAluSourceSupported(operand) || ComputeWaveAluVccWord(operand);
}

// V_CNDMASK_B32 data sources may carry float abs/neg input modifiers and may
// be float inline constants; the selection itself moves raw bits.
ShaderOperand CndmaskStripModifiers(const ShaderOperand& operand)
{
	auto stripped     = operand;
	stripped.absolute = false;
	stripped.negate   = false;
	return stripped;
}

bool ComputeWaveAluCndmaskDataSupported(const ShaderOperand& operand)
{
	const auto stripped = CndmaskStripModifiers(operand);
	return ComputeWaveAluTernarySourceSupported(stripped) ||
	       (stripped.type == ShaderOperandType::FloatInlineConstant && stripped.size == 0 && ComputeWaveAluOperandIsPlain(stripped));
}

// src2 of V_CNDMASK_B32 is a packed mask pair read per lane, never data.
bool ComputeWaveAluMaskPairSourceSupported(const ShaderOperand& operand)
{
	if (!ComputeWaveAluOperandIsPlain(operand) || operand.size != 2)
	{
		return false;
	}
	switch (operand.type)
	{
		case ShaderOperandType::Sgpr: return ComputeWaveAluRegisterRangeIsValid(operand.register_id, operand.size, kMaxSgpr);
		case ShaderOperandType::VccLo:
		case ShaderOperandType::ExecLo: return operand.register_id == 0;
		default: return false;
	}
}

const char* ComputeWaveAluOpcode(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::VAndB32: return "OpBitwiseAnd";
		case ShaderInstructionType::VOrB32: return "OpBitwiseOr";
		case ShaderInstructionType::VXorB32: return "OpBitwiseXor";
		case ShaderInstructionType::VAddI32: return "OpIAdd";
		case ShaderInstructionType::VSubI32: return "OpISub";
		default: return nullptr;
	}
}

bool EmitComputeWaveAluBinaryU32(const Spirv& spirv, const ShaderInstruction& instruction, uint32_t index, const char* opcode,
                                 String8* output)
{
	if (output == nullptr || opcode == nullptr)
	{
		return false;
	}

	const auto destination_low  = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::Low, 0);
	const auto destination_high = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::High, 0);
	if (destination_low.type != SpirvType::Float || destination_high.type != SpirvType::Float || destination_low.value.IsEmpty() ||
	    destination_high.value.IsEmpty())
	{
		return false;
	}

	const auto index_string = String8::FromPrintf("%u", index);
	const auto src0_low     = String8("wave_alu_src0_low_") + index_string;
	const auto src0_high    = String8("wave_alu_src0_high_") + index_string;
	const auto src1_low     = String8("wave_alu_src1_low_") + index_string;
	const auto src1_high    = String8("wave_alu_src1_high_") + index_string;
	const auto old_low      = String8("wave_alu_old_low_") + index_string;
	const auto old_high     = String8("wave_alu_old_high_") + index_string;
	const auto exec_low     = String8("wave_alu_exec_low_") + index_string;
	const auto exec_high    = String8("wave_alu_exec_high_") + index_string;
	const auto value_low    = String8("wave_alu_value_low_") + index_string;
	const auto value_high   = String8("wave_alu_value_high_") + index_string;
	const auto result_low   = String8("wave_alu_result_low_") + index_string;
	const auto result_high  = String8("wave_alu_result_high_") + index_string;

	String8 source;
	// Every source and both old destination banks are captured before either
	// result is stored. This keeps dst==src0 and dst==src1 bank-independent.
	if (!spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::Low, src0_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::High, src0_high, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.src[1], ShaderWaveBank::Low, src1_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.src[1], ShaderWaveBank::High, src1_high, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::Low, old_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::High, old_high, &source))
	{
		return false;
	}

	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	if (!spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::Low, exec_low, &source) ||
	    !spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::High, exec_high, &source))
	{
		return false;
	}

	source += String8(R"(
%<value_low> = <opcode> %uint %<src0_low> %<src1_low>
%<value_high> = <opcode> %uint %<src0_high> %<src1_high>
%<result_low> = OpSelect %uint %<exec_low> %<value_low> %<old_low>
%<result_high> = OpSelect %uint %<exec_high> %<value_high> %<old_high>
%<result_low>_float = OpBitcast %float %<result_low>
%<result_high>_float = OpBitcast %float %<result_high>
               OpStore %<destination_low> %<result_low>_float
               OpStore %<destination_high> %<result_high>_float
)")
	              .ReplaceStr("<opcode>", opcode)
	              .ReplaceStr("<src0_low>", src0_low)
	              .ReplaceStr("<src0_high>", src0_high)
	              .ReplaceStr("<src1_low>", src1_low)
	              .ReplaceStr("<src1_high>", src1_high)
	              .ReplaceStr("<old_low>", old_low)
	              .ReplaceStr("<old_high>", old_high)
	              .ReplaceStr("<exec_low>", exec_low)
	              .ReplaceStr("<exec_high>", exec_high)
	              .ReplaceStr("<value_low>", value_low)
	              .ReplaceStr("<value_high>", value_high)
	              .ReplaceStr("<result_low>", result_low)
	              .ReplaceStr("<result_high>", result_high)
	              .ReplaceStr("<destination_low>", destination_low.value)
	              .ReplaceStr("<destination_high>", destination_high.value);
	*output += source;
	return true;
}

// V_ADD3_U32: D.u = S0.u + S1.u + S2.u modulo 2^32, per guest lane and bank.
bool EmitComputeWaveAluAdd3U32(const Spirv& spirv, const ShaderInstruction& instruction, uint32_t index, String8* output)
{
	const auto destination_low  = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::Low, 0);
	const auto destination_high = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::High, 0);
	if (output == nullptr || destination_low.type != SpirvType::Float || destination_high.type != SpirvType::Float ||
	    destination_low.value.IsEmpty() || destination_high.value.IsEmpty())
	{
		return false;
	}

	const auto id = [index](const char* name) { return String8::FromPrintf("wave_add3_%s_%u", name, index); };
	String8    source;
	// All sources and both old destination banks are captured before either
	// result is stored, so destination aliases stay bank-independent.
	for (int operand = 0; operand < 3; ++operand)
	{
		const auto low  = id(String8::FromPrintf("src%d_low", operand).c_str());
		const auto high = id(String8::FromPrintf("src%d_high", operand).c_str());
		if (!spirv.EmitComputeWaveOperandUint(instruction.src[operand], ShaderWaveBank::Low, low, &source) ||
		    !spirv.EmitComputeWaveOperandUint(instruction.src[operand], ShaderWaveBank::High, high, &source))
		{
			return false;
		}
	}
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	if (!spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::Low, id("old_low"), &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::High, id("old_high"), &source) ||
	    !spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::Low, id("exec_low"), &source) ||
	    !spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::High, id("exec_high"), &source))
	{
		return false;
	}

	for (const char* bank: {"low", "high"})
	{
		const auto destination = std::strcmp(bank, "low") == 0 ? destination_low.value : destination_high.value;
		source += String8(R"(
%<partial> = OpIAdd %uint %<src0> %<src1>
%<value> = OpIAdd %uint %<partial> %<src2>
%<result> = OpSelect %uint %<exec> %<value> %<old>
%<result>_float = OpBitcast %float %<result>
               OpStore %<destination> %<result>_float
)")
		              .ReplaceStr("<partial>", id(String8::FromPrintf("partial_%s", bank).c_str()))
		              .ReplaceStr("<value>", id(String8::FromPrintf("value_%s", bank).c_str()))
		              .ReplaceStr("<result>", id(String8::FromPrintf("result_%s", bank).c_str()))
		              .ReplaceStr("<src0>", id(String8::FromPrintf("src0_%s", bank).c_str()))
		              .ReplaceStr("<src1>", id(String8::FromPrintf("src1_%s", bank).c_str()))
		              .ReplaceStr("<src2>", id(String8::FromPrintf("src2_%s", bank).c_str()))
		              .ReplaceStr("<exec>", id(String8::FromPrintf("exec_%s", bank).c_str()))
		              .ReplaceStr("<old>", id(String8::FromPrintf("old_%s", bank).c_str()))
		              .ReplaceStr("<destination>", destination);
	}
	*output += source;
	return true;
}

// Loads both banks of a cndmask data source as low_id/high_id, applying the
// float input modifiers in ISA order: neg(abs(x)).
bool EmitCndmaskData(const Spirv& spirv, const ShaderOperand& operand, const String8& low_id, const String8& high_id, String8* output)
{
	const auto stripped = CndmaskStripModifiers(operand);
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto name = bank == ShaderWaveBank::Low ? low_id : high_id;
		const auto raw  = operand.absolute || operand.negate ? name + "_raw" : name;
		if (!spirv.EmitComputeWaveOperandUint(stripped, bank, raw, output))
		{
			return false;
		}
		auto value = raw;
		if (operand.absolute)
		{
			const auto next = name + (operand.negate ? "_abs" : "");
			*output += String8::FromPrintf("%%%s = OpBitwiseAnd %%uint %%%s %%%s\n", next.c_str(), value.c_str(),
			                               spirv.GetConstantUint(0x7fffffffu).c_str());
			value = next;
		}
		if (operand.negate)
		{
			*output += String8::FromPrintf("%%%s = OpBitwiseXor %%uint %%%s %%%s\n", name.c_str(), value.c_str(),
			                               spirv.GetConstantUint(0x80000000u).c_str());
		}
	}
	return true;
}

// V_CNDMASK_B32: D.u = S2.u[lane] ? S1.u : S0.u, per guest lane and bank.
// src[2] is a packed mask pair; EXEC still gates the destination write.
bool EmitComputeWaveAluCndmaskU32(const Spirv& spirv, const ShaderInstruction& instruction, uint32_t index, String8* output)
{
	const auto destination_low  = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::Low, 0);
	const auto destination_high = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::High, 0);
	if (output == nullptr || destination_low.type != SpirvType::Float || destination_high.type != SpirvType::Float ||
	    destination_low.value.IsEmpty() || destination_high.value.IsEmpty())
	{
		return false;
	}

	const auto id = [index](const char* name) { return String8::FromPrintf("wave_cndmask_%s_%u", name, index); };
	String8    source;
	// Both data sources, both select-mask bits, EXEC bits, and the old
	// destination banks are captured before either store so aliases stay
	// bank-independent.
	if (!EmitCndmaskData(spirv, instruction.src[0], id("src0_low"), id("src0_high"), &source) ||
	    !EmitCndmaskData(spirv, instruction.src[1], id("src1_low"), id("src1_high"), &source) ||
	    !spirv.EmitComputeWaveMaskBit(instruction.src[2], ShaderWaveBank::Low, id("select_low"), &source) ||
	    !spirv.EmitComputeWaveMaskBit(instruction.src[2], ShaderWaveBank::High, id("select_high"), &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::Low, id("old_low"), &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::High, id("old_high"), &source))
	{
		return false;
	}
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	if (!spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::Low, id("exec_low"), &source) ||
	    !spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::High, id("exec_high"), &source))
	{
		return false;
	}

	for (const char* bank: {"low", "high"})
	{
		const auto destination = std::strcmp(bank, "low") == 0 ? destination_low.value : destination_high.value;
		source += String8(R"(
%<selected> = OpSelect %uint %<select> %<src1> %<src0>
%<result> = OpSelect %uint %<exec> %<selected> %<old>
%<result>_float = OpBitcast %float %<result>
               OpStore %<destination> %<result>_float
)")
		              .ReplaceStr("<selected>", id(String8::FromPrintf("selected_%s", bank).c_str()))
		              .ReplaceStr("<result>", id(String8::FromPrintf("result_%s", bank).c_str()))
		              .ReplaceStr("<select>", id(String8::FromPrintf("select_%s", bank).c_str()))
		              .ReplaceStr("<src0>", id(String8::FromPrintf("src0_%s", bank).c_str()))
		              .ReplaceStr("<src1>", id(String8::FromPrintf("src1_%s", bank).c_str()))
		              .ReplaceStr("<exec>", id(String8::FromPrintf("exec_%s", bank).c_str()))
		              .ReplaceStr("<old>", id(String8::FromPrintf("old_%s", bank).c_str()))
		              .ReplaceStr("<destination>", destination);
	}
	*output += source;
	return true;
}

} // namespace

bool ShaderComputeWaveAluInstructionSupported(const ShaderInstruction& instruction)
{
	const bool plain = instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u && !instruction.vop_sdwa &&
	                   ComputeWaveAluUnusedDestination(instruction.dst2) && ComputeWaveAluOrdinaryVgpr(instruction.dst);
	if (instruction.type == ShaderInstructionType::VAdd3U32)
	{
		return plain && instruction.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2 && instruction.src_num == 3 &&
		       ComputeWaveAluTernarySourceSupported(instruction.src[0]) && ComputeWaveAluTernarySourceSupported(instruction.src[1]) &&
		       ComputeWaveAluTernarySourceSupported(instruction.src[2]) && ComputeWaveAluScalarValuesWithinLimit(instruction);
	}
	if (instruction.type == ShaderInstructionType::VCndmaskB32)
	{
		// The identity SDWA form selects the same dwords as the plain VOP2.
		const bool identity_sdwa = instruction.vop_sdwa && ShaderComputeWaveSdwaVop2IdentitySupported(instruction) &&
		                           ComputeWaveAluUnusedDestination(instruction.dst2) && ComputeWaveAluOrdinaryVgpr(instruction.dst);
		return (plain || identity_sdwa) && instruction.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2 && instruction.src_num == 3 &&
		       ComputeWaveAluCndmaskDataSupported(instruction.src[0]) && ComputeWaveAluCndmaskDataSupported(instruction.src[1]) &&
		       ComputeWaveAluMaskPairSourceSupported(instruction.src[2]) && ComputeWaveAluUnusedDestination(instruction.src[3]) &&
		       ComputeWaveAluScalarValuesWithinLimit(instruction);
	}
	return plain && ComputeWaveAluOpcode(instruction.type) != nullptr && instruction.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1 &&
	       instruction.src_num == 2 && ComputeWaveAluSourceSupported(instruction.src[0]) && ComputeWaveAluSourceSupported(instruction.src[1]);
}

bool Spirv::EmitComputeWaveAluInstruction(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks() || !ShaderComputeWaveAluInstructionSupported(instruction))
	{
		return false;
	}
	if (instruction.type == ShaderInstructionType::VAdd3U32)
	{
		return EmitComputeWaveAluAdd3U32(*this, instruction, index, output);
	}
	if (instruction.type == ShaderInstructionType::VCndmaskB32)
	{
		return EmitComputeWaveAluCndmaskU32(*this, instruction, index, output);
	}
	return EmitComputeWaveAluBinaryU32(*this, instruction, index, ComputeWaveAluOpcode(instruction.type), output);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
