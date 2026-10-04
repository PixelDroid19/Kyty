#ifndef EMULATOR_SRC_GRAPHICS_SHADER_NATIVE_WAVE_INTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_NATIVE_WAVE_INTERNAL_H_

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Predicates shared by the native-wave admission and the fragment mask-flow
// analysis, so that both classify EXEC/VCC words and packed results identically.
[[nodiscard]] bool ShaderNativeMaskOperand(const ShaderOperand& operand);
[[nodiscard]] bool ShaderNativePackedResult(const ShaderInstruction& inst);
[[nodiscard]] bool ShaderInstructionTypeChangesExec(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionWritesExec(const ShaderInstruction& inst);
// Branches, program transfers and S_ENDPGM.
[[nodiscard]] bool ShaderInstructionIsControlFlowBoundary(const ShaderInstruction& inst);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_SRC_GRAPHICS_SHADER_NATIVE_WAVE_INTERNAL_H_
