#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_WAITCNT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_WAITCNT_H_

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Every memory operation the paired lowering admits completes at its own PC, so
// no counter threshold needs tracking: a wait only has to be the exact plain SOPP
// tuple. The lgkmcnt(0)-only form 0xc07f additionally marks a descriptor as drained
// for the specialized scalar and vector buffer routes.
// True only for the exact plain SOPP lgkmcnt(0)-only tuple admitted above.
[[nodiscard]] bool ShaderComputeWaveIsLgkmZeroOnlyWait(const ShaderInstruction& instruction);
// Exact SOPP s_waitcnt encoding with any counter immediate.
[[nodiscard]] bool ShaderComputeWaveIsExactWait(const ShaderInstruction& instruction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_WAITCNT_H_
