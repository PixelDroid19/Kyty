#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_WAITCNT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_WAITCNT_H_

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Paired compute-wave admission of the SOPP S_WAITCNT at `index`.
//
// RDNA2 S_WAITCNT waits until vmcnt <= {SIMM16[15:14], SIMM16[3:0]},
// expcnt <= SIMM16[6:4] and lgkmcnt <= SIMM16[13:8]. Two exact immediate
// families are admitted. The lgkmcnt(0)-only form 0xc07f has vmcnt/expcnt at
// the counter maxima; the pure vmcnt(N) form keeps expcnt and lgkmcnt at
// their maxima (mask 0x3ff0 == 0x3f70) and is satisfied because every VMEM
// operation the subset admits is a banked buffer load lowered synchronously.
// The LGKM operations an lgkmcnt(0) wait can cover are those issued on
// the unique fallthrough path since program entry or the previous lgkmcnt(0)
// wait. Each must be a mapped EUD S_LOAD or an admitted S_BUFFER_LOAD_DWORD,
// which the paired lowering completes synchronously, and no instruction may
// access such a destination before this wait. LDS/GDS, unmapped SMEM,
// messages, other instructions and branch targets inside that window fail
// with the PC that needs a contract.
[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveWaitcnt(const ShaderCode& code, uint32_t index,
                                                                              const ShaderBindResources& bind);

// True only for the exact plain SOPP lgkmcnt(0)-only tuple admitted above.
[[nodiscard]] bool ShaderComputeWaveIsLgkmZeroOnlyWait(const ShaderInstruction& instruction);
// Exact SOPP s_waitcnt encoding with any counter immediate.
[[nodiscard]] bool ShaderComputeWaveIsExactWait(const ShaderInstruction& instruction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_WAITCNT_H_
