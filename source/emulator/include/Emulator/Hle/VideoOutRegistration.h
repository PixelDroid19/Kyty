#ifndef EMULATOR_INCLUDE_EMULATOR_HLE_VIDEOOUTREGISTRATION_H_
#define EMULATOR_INCLUDE_EMULATOR_HLE_VIDEOOUTREGISTRATION_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {
class SymbolDatabase;
} // namespace Kyty::Loader

namespace Kyty::Hle::VideoOutRegistration {

void InitVideoOut_1(::Kyty::Loader::SymbolDatabase* symbols);

} // namespace Kyty::Hle::VideoOutRegistration

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_HLE_VIDEOOUTREGISTRATION_H_ */
