#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERPARSE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERPARSE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class ShaderCode;

void ShaderParse(const uint32_t* src, ShaderCode* dst);
void ShaderParse(const uint32_t* src, uint32_t code_size_bytes, ShaderCode* dst);
// A registered fused front transfers through terminal s_setpc_b64 to its
// separately mapped continuation. This entry point is only valid after that
// relationship has been established.
void ShaderParseFusedFront(const uint32_t* src, uint32_t code_size_bytes, ShaderCode* dst);
// Boundary-only diagnostic form used by integration tests. Instruction
// semantic errors remain strict; false means the byte range ended before a
// complete, reachable program terminator.
[[nodiscard]] bool ShaderTryParseBounded(const uint32_t* src, uint32_t code_size_bytes, ShaderCode* dst);

enum class ShaderParseBoundary
{
	CompleteProgram,
	RegisteredFront,
};
// Copied source has no guest address to query. RegisteredFront is valid only
// after the caller binds a continuation under the shader-generation lease.
// Instruction decoding and all ordinary terminator checks are shared.
[[nodiscard]] bool ShaderTryParseBounded(const uint32_t* src, uint32_t code_size_bytes, ShaderCode* dst,
                                         ShaderParseBoundary boundary);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERPARSE_H_ */
