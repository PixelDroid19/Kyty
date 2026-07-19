#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBAMPR_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBAMPR_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Ampr {

int SubmitCommandBuffer(void* cmd_obj, uintptr_t submit_ident);

} // namespace Kyty::Libs::Ampr

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBAMPR_H_ */
