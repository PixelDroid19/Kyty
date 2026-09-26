#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_CONTROL_FLOW_ANALYSIS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_CONTROL_FLOW_ANALYSIS_H_

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveControlFlow(const ShaderCode& code);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_CONTROL_FLOW_ANALYSIS_H_
