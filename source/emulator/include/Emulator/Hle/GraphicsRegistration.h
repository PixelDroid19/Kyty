#ifndef EMULATOR_INCLUDE_EMULATOR_HLE_GRAPHICSREGISTRATION_H_
#define EMULATOR_INCLUDE_EMULATOR_HLE_GRAPHICSREGISTRATION_H_

#include "Emulator/Common.h"
#include "Emulator/Libs/HleSymbolRegistry.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Hle::GraphicsRegistration {

void InitGraphicsDriver_1(::Kyty::Libs::HleSymbolRegistry* symbols);

} // namespace Kyty::Hle::GraphicsRegistration

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_HLE_GRAPHICSREGISTRATION_H_ */
