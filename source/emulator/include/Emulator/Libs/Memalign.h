#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_MEMALIGN_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_MEMALIGN_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

#include <cstddef>

namespace Kyty::Libs::LibC {

// FreeBSD / Prospero memalign accepts any non-zero power-of-two alignment,
// including values smaller than alignof(void*) (e.g. 4 for uint32 tables).
// Rejecting align < alignof(void*) returned null and left Gen5 titles writing
// into a null buffer after a successful-looking call site.
[[nodiscard]] inline bool MemalignAlignmentOk(size_t alignment) noexcept
{
	return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

} // namespace Kyty::Libs::LibC

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_MEMALIGN_H_ */
