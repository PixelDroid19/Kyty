#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_NATIVE_EQUIVALENCE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_NATIVE_EQUIVALENCE_H_

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Proves that a wave64 compute program computes the same results when each
// host invocation runs one guest lane with its full guest workgroup preserved,
// regardless of how the host groups invocations into subgroups. The proof is
// fail-closed and requires all of:
//
// - no instruction that reads or writes another lane's data or a wave-wide
//   count (lane reads/writes, MBCNT, WQM, DS append/consume, DPP, VCCZ/EXECZ
//   operands, program-counter writes, messages);
// - every EXEC/VCC value used as a mask is consumed only per lane, and every
//   scalar register that may hold a mask at a program point is never read there
//   as uniform data (and vice versa); mask constants are only 0 or all ones;
// - SCC is never read after a mask operation could have produced it;
// - for every EXEC/VCC-conditional forward branch, no scalar register or SCC
//   written inside the skippable region is live at the branch target, and the
//   region writes no M0 and performs no scalar memory side effect.
//
// The last rule is what makes a host subgroup that skips a region (because
// all of its own lanes are inactive) indistinguishable from the guest wave
// that executed it with some lanes of the other half active.
[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveNativeEquivalence(const ShaderCode& code);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_NATIVE_EQUIVALENCE_H_
