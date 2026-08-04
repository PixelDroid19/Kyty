#ifndef EMULATOR_SRC_GRAPHICS_GRAPHICSRUNTRACE_H_
#define EMULATOR_SRC_GRAPHICS_GRAPHICSRUNTRACE_H_

#include <cstdint>

namespace Kyty::Libs::Graphics {

void GraphicsRunTraceAaRegisterWrite(const char* path, const char* name, uint32_t value);
void GraphicsRunTraceWait(const char* stage, int queue, uint64_t address, uint64_t value, uint64_t reference, uint64_t mask,
                          uint64_t sequence, uint64_t elapsed_ns = 0);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_GRAPHICSRUNTRACE_H_ */
