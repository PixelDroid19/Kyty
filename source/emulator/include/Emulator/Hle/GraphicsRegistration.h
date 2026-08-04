#ifndef EMULATOR_INCLUDE_EMULATOR_HLE_GRAPHICSREGISTRATION_H_
#define EMULATOR_INCLUDE_EMULATOR_HLE_GRAPHICSREGISTRATION_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {
class SymbolDatabase;
} // namespace Kyty::Loader

namespace Kyty::Hle::GraphicsRegistration {

void InitGraphicsDriver_1(::Kyty::Loader::SymbolDatabase* symbols);

} // namespace Kyty::Hle::GraphicsRegistration

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_HLE_GRAPHICSREGISTRATION_H_ */
