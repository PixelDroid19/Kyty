#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_IMAGE_GRADIENT_PROOF_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_IMAGE_GRADIENT_PROOF_H_

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Proves that all four explicit gradients consumed by this image_sample_cd
// have one value per quad on every static path to the instruction. Call once
// for each reachable image_sample_cd in the program.
[[nodiscard]] bool ShaderImageSampleCdHasQuadUniformGradients(const ShaderCode& code, uint32_t instruction_index);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_IMAGE_GRADIENT_PROOF_H_
