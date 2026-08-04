#ifndef EMULATOR_SRC_GRAPHICS_SHADERSPIRVTOOLCHAIN_H_
#define EMULATOR_SRC_GRAPHICS_SHADERSPIRVTOOLCHAIN_H_

#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include <cstddef>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::ShaderToolchain {

bool Disassemble(const uint32_t* src_binary, size_t src_binary_size, String8* dst_disassembly);
bool ToGlsl(const uint32_t* src_binary, size_t src_binary_size, String8* dst_code);
bool Run(const String8& src, Vector<uint32_t>* dst, String8* err_msg);

} // namespace Kyty::Libs::Graphics::ShaderToolchain

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERSPIRVTOOLCHAIN_H_ */
