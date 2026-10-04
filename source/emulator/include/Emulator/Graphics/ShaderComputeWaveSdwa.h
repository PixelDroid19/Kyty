#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_SDWA_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_SDWA_H_

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Admit only v_mov_b32 in its SDWA form as a zero-extend BYTE/WORD/DWORD
// extract: VGPR destination and source, dst_sel=DWORD, dst_u=PAD, no clamp,
// output modifier, sign extension, negate or absolute, S0=0, and every
// reserved control bit clear. Other SDWA forms and opcodes stay fail-closed.
[[nodiscard]] bool ShaderComputeWaveSdwaExtractSupported(const ShaderInstruction& instruction);

// Exact VOP1 signed integer-to-float conversion with a selected VGPR byte,
// word or dword, source sign extension, and an unmodified DWORD destination.
[[nodiscard]] bool ShaderComputeWaveSdwaSignedConvertSupported(const ShaderInstruction& instruction);

// Admit only the VOPC SDWAB form of the packed-mask U32 compares whose
// control word keeps DWORD selects on both sources and no sext/neg/abs or
// reserved bits. The mask destination fields (SDST, SD) and the source
// register-file flags (S0, S1) stay free because the operand tuple and the
// shared compare emitter already bound them.
[[nodiscard]] bool ShaderComputeWaveSdwaCompareTupleSupported(const ShaderInstruction& instruction);
// Element-type agnostic operand tuple of an SDWA compare with whole-dword selects and no modifiers; the caller checks the opcode family.
[[nodiscard]] bool ShaderComputeWaveSdwaCompareIdentityTuple(const ShaderInstruction& instruction);

// Admit a VOP2 SDWA control word only when it is the identity form: DWORD
// destination and source selects, dst_u=PAD, no clamp, output modifier,
// sext/neg/abs or reserved bits. Such an instruction computes exactly its
// plain VOP2 result; SRC0 and the S0/S1 register-file flags stay free
// because the operand tuple already carries the decoded sources.
[[nodiscard]] bool ShaderComputeWaveSdwaVop2IdentitySupported(const ShaderInstruction& instruction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_SDWA_H_
