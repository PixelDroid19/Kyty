#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_VERTEX_EXEC_FLOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_VERTEX_EXEC_FLOW_H_

#include "Emulator/Graphics/Shader.h"

#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Per instruction of a fused vertex program, plus one entry past the last: before it, EXEC is known to hold every
// live host invocation. The launch mask is full, and the front's S_LSHR EXEC, -1, -count rebuilds it from the lane
// counts in s3, which reach the highest live lane. Copies saved in scalars or spilled to VGPR lanes stay full and
// restore it. Empty when the control flow cannot be followed.
[[nodiscard]] std::vector<bool> ShaderAnalyzeVertexExecFull(const ShaderCode& code, bool wave32);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_VERTEX_EXEC_FLOW_H_
