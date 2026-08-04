#ifndef EMULATOR_SRC_GRAPHICS_SHADERPARSEOPERANDS_H_
#define EMULATOR_SRC_GRAPHICS_SHADERPARSEOPERANDS_H_

#include "Emulator/Graphics/Shader.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

ShaderOperand operand_parse(uint32_t code);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERPARSEOPERANDS_H_ */
