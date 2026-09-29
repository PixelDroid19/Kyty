#ifndef EMULATOR_SRC_GRAPHICS_SHADER_MASK_ANALYSIS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_MASK_ANALYSIS_H_

#include "Emulator/Graphics/Shader.h"

#include <cstdint>

namespace Kyty::Libs::Graphics {

// Proves that a VOP3B reverse-borrow SGPR source pair was written by one of
// the currently scalarized full-pair compare emitters in the same straight-
// line block. Native wave32 also permits an independently produced VCC_HI
// word. VCC and EXEC low pairs are explicit backend mask contracts.
[[nodiscard]] bool ShaderReverseBorrowMaskHasProvenance(const ShaderCode& code, uint32_t instruction_index,
                                                       bool native_wave32 = false);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_MASK_ANALYSIS_H_ */
