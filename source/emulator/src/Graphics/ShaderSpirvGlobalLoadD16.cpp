// GLOBAL_LOAD_{UBYTE,SBYTE}_D16_HI (RDNA2 Shader ISA, section 12.18.3, rows 33 and 35).
// D0[31:16] receives the zero- or sign-extended byte and D0[15:0] keeps its value.
// A byte is aligned at every address under every alignment mode (section 8.1.7), so
// no alignment rule applies to it. The byte lies in the dword at the address rounded
// down to a multiple of 4; that dword is loaded as one dword, which never crosses a
// page. Only lanes with EXEC set access memory and write the register; inactive lanes
// keep the whole register. The GLOBAL_LOAD_SHORT_D16_HI form is refused by the parser
// (ShaderParseFLAT.cpp), so this file emits bytes only.
// The address and the guest load are shared with the dword path
// (EmitGlobalLoadAddress in ShaderSpirvBuffer.cpp and Spirv::EmitGuestLoad).
#include "ShaderSpirvEmitters.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

// Extracts the byte at bit offset `shift` of the aligned dword into a 32-bit value,
// zero-extended for UBYTE and sign-extended for SBYTE. The merge reads only its low
// 16 bits, which are the bits that D0[31:16] receives.
String8 emit_byte_value(ShaderInstructionType type, Spirv* spirv, const String8& tag, String8* source)
{
	const auto  eight = spirv->GetConstantUint(8u);
	const char* op    = type == ShaderInstructionType::GlobalLoadSbyteD16Hi ? "OpBitFieldSExtract" : "OpBitFieldUExtract";
	*source += String8::FromPrintf("%%%s_v = %s %%uint %%%s_g_d0 %%%s_shift %%%s\n", tag.c_str(), op, tag.c_str(), tag.c_str(),
	                               eight.c_str());
	return String8::FromPrintf("%%%s_v", tag.c_str());
}

bool emit_global_load_d16_hi(Spirv* spirv, const ShaderInstruction& inst, uint32_t instruction_index, String8* dst_source)
{
	const auto dst = operand_variable_to_str(inst.dst);
	if (dst.type != SpirvType::Float)
	{
		return false;
	}
	const auto tag = String8::FromPrintf("glbh_%u", instruction_index);
	String8    source;
	if (!EmitGlobalLoadAddress(spirv, inst, tag, &source))
	{
		return false;
	}
	// The byte index is the offset of the address from its aligned dword. The
	// EXEC predicate is the one the buffer store lowering uses before its access.
	source += String8(R"(%<t>_al = OpBitwiseAnd %uint %<t>_lo %<mask>
%<t>_byte = OpISub %uint %<t>_lo %<t>_al
%<t>_shift = OpIMul %uint %<t>_byte %<eight>
%<t>_exec = OpLoad %uint %exec_lo
%<t>_active = OpINotEqual %bool %<t>_exec %uint_0
OpSelectionMerge %<t>_done None
OpBranchConditional %<t>_active %<t>_load %<t>_done
%<t>_load = OpLabel
)")
	              .ReplaceStr("<mask>", spirv->GetConstantUint(0xfffffffcu))
	              .ReplaceStr("<eight>", spirv->GetConstantUint(8u))
	              .ReplaceStr("<t>", tag);
	if (!spirv->EmitGuestLoad(tag + "_al", tag + "_hi", 1, tag + "_g", &source))
	{
		return false;
	}
	const auto value = emit_byte_value(inst.type, spirv, tag, &source);
	source += String8(R"(%<t>_old_float = OpLoad %float %<dst>
%<t>_old = OpBitcast %uint %<t>_old_float
%<t>_merged = OpBitFieldInsert %uint %<t>_old <value> %<sixteen> %<sixteen>
%<t>_merged_float = OpBitcast %float %<t>_merged
OpStore %<dst> %<t>_merged_float
OpBranch %<t>_done
%<t>_done = OpLabel
)")
	              .ReplaceStr("<value>", value)
	              .ReplaceStr("<sixteen>", spirv->GetConstantUint(16u))
	              .ReplaceStr("<dst>", dst.value)
	              .ReplaceStr("<t>", tag);
	*dst_source += source;
	return true;
}

} // namespace

KYTY_RECOMPILER_FUNC(Recompile_GlobalLoadD16Hi)
{
	return emit_global_load_d16_hi(spirv, code.GetInstructions().At(index), index, dst_source);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
