#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_ALU_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_ALU_H_

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Admit only the independently exercised plain binary ALU subset and plain
// V_ADD3_U32 (VCC words allowed as scalar data within the two-scalar limit).
// All other forms stay fail-closed so paired-wave lowering never inherits
// native32 state.
[[nodiscard]] bool ShaderComputeWaveAluInstructionSupported(const ShaderInstruction& instruction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_ALU_H_
