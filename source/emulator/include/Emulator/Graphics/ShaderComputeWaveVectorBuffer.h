#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_VECTOR_BUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_VECTOR_BUFFER_H_

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// RDNA2 BUFFER_LOAD_DWORD{,X2,X3,X4} computes, per lane,
//   VDATA[..] = MEM[V#.base + index*stride + V_OFFSET + IMM + S_OFFSET]
// with IDXEN supplying the lane index. Admits only the exact tuple: IDXEN only,
// no OFFEN/GLC/LDS/SLC/TFE or undefined control bits, a zero inline S_OFFSET
// and an aligned SGPR-quad V#.
[[nodiscard]] bool ShaderComputeWaveVectorBufferLoadSupported(const ShaderInstruction& instruction);

// BUFFER_ATOMIC_UMAX without a returned value: one guarded unsigned atomic
// operation per active guest lane, using a proven, writable raw V# binding.
[[nodiscard]] bool ShaderComputeWaveVectorBufferAtomicUmaxSupported(const ShaderInstruction& instruction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_VECTOR_BUFFER_H_
