#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_LDS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_LDS_H_

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// The exact ds_write_b32 / ds_read_b32 / ds_add_rtn_u32 tuples lowered by the
// banked LDS emitter. Other admitted LDS accesses take the ordered generic path.
[[nodiscard]] bool ShaderComputeWaveLdsInstructionSupported(const ShaderInstruction& instruction);

// Exact memory tuple shared by native DS emission and the ordered generic
// paired path. GDS, legacy opcode aliases and lost atomic operands are refused.
[[nodiscard]] bool ShaderLdsMemoryInstructionSupported(const ShaderInstruction& instruction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_LDS_H_
