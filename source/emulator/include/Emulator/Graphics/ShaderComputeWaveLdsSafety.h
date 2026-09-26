#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_LDS_SAFETY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_LDS_SAFETY_H_

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveLdsSafety(const ShaderCode&             code,
                                                                                const ShaderComputeInputInfo& input);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_LDS_SAFETY_H_
