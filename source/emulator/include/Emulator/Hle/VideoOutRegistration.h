#ifndef EMULATOR_INCLUDE_EMULATOR_HLE_VIDEOOUTREGISTRATION_H_
#define EMULATOR_INCLUDE_EMULATOR_HLE_VIDEOOUTREGISTRATION_H_

#include "Emulator/Common.h"
#include "Emulator/Hle/SymbolRegistry.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Hle::VideoOutRegistration {

void InitVideoOut_1(::Kyty::Hle::HleSymbolRegistry* symbols);

} // namespace Kyty::Hle::VideoOutRegistration

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_HLE_VIDEOOUTREGISTRATION_H_ */
