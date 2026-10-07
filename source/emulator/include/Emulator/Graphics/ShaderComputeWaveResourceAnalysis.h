#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_RESOURCE_ANALYSIS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_RESOURCE_ANALYSIS_H_

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

[[nodiscard]] bool ShaderPairedEudStorageLoadSupported(const ShaderInstruction& instruction,
	                                                    const ShaderBindResources& bind) noexcept;

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_RESOURCE_ANALYSIS_H_
