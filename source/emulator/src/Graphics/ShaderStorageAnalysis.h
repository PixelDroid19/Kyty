#ifndef EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_

#include "Emulator/Graphics/Shader.h"

namespace Kyty::Libs::Graphics {

[[nodiscard]] bool ShaderOperandOverlapsSgprRange(const ShaderOperand& operand, int start_register, int registers_num);
[[nodiscard]] bool ShaderInstructionHasStaticBranchTarget(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionReadsImageResource(ShaderInstructionType type);
[[nodiscard]] bool ShaderInstructionWritesImageResource(ShaderInstructionType type);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_STORAGE_ANALYSIS_H_ */
