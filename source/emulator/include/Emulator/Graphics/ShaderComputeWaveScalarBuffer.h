#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_SCALAR_BUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_SCALAR_BUFFER_H_

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Paired compute-wave admission of the S_BUFFER_LOAD_DWORD at `index`.
//
// RDNA2 reads SGPR[SDATA] = MEM[V#.base + offset] using only the V# base,
// stride and num_records. The paired lowering reuses the scalar SSBO read,
// which is wave-uniform and synchronous. Admission requires: the plain tuple
// (GLC/DLC and undefined bits clear, null SOFFSET, 4-aligned immediate, VCC_LO
// or ordinary SGPR destination); a V# written only by a mapped EUD S_LOAD
// whose collector mapping covers this consumer, drained by lgkmcnt(0) on the
// unique fallthrough path; and an unswizzled descriptor for which the dword is
// in range. The out-of-range zero policy is not admitted here.
[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveScalarBufferLoad(const ShaderCode& code, uint32_t index,
                                                                                       const ShaderBindResources& bind);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_SCALAR_BUFFER_H_
