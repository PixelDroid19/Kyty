#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_VECTOR_BUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_VECTOR_BUFFER_H_

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Paired compute-wave admission of the MUBUF raw dword load at `index`.
//
// RDNA2 BUFFER_LOAD_DWORD{,X2,X3,X4} computes, per lane,
//   VDATA[..] = MEM[V#.base + index*stride + V_OFFSET + IMM + S_OFFSET]
// with IDXEN supplying the lane index. The paired lowering performs one host
// load per guest lane-half through the shared raw-address helper, so the
// contract must pin down every input that helper consumes: the exact tuple
// (IDXEN only, no OFFEN/GLC/LDS/SLC/TFE or undefined control bits, a zero
// inline S_OFFSET, an aligned SGPR-quad V#); the V# provenance (a drained
// mapped EUD S_LOAD on the unique fallthrough path, or a never-written user
// SGPR quad bound directly); and a descriptor the raw equation can address
// (non-empty, unswizzled, no ADD_TID, a stride the dword math supports and a
// byte range that can contain the immediate span). Anything else fails
// closed: ADD_TID in particular cannot be lowered by the shared helper
// because it adds the *native* lane id rather than the guest lane.
[[nodiscard]] bool ShaderComputeWaveVectorBufferLoadSupported(const ShaderInstruction& instruction);

[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveVectorBufferLoad(const ShaderCode& code, uint32_t index,
                                                                                       const ShaderBindResources& bind);

// BUFFER_ATOMIC_UMAX without a returned value: one guarded unsigned atomic
// operation per active guest lane, using a proven, writable raw V# binding.
[[nodiscard]] bool ShaderComputeWaveVectorBufferAtomicUmaxSupported(const ShaderInstruction& instruction);
[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveVectorBufferAtomicUmax(const ShaderCode& code, uint32_t index,
                                                                                             const ShaderBindResources& bind);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_VECTOR_BUFFER_H_
