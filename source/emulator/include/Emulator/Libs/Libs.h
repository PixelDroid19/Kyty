#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_

#include "Emulator/Hle/Registration.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Hle {

class HleSymbolRegistry;

} // namespace Kyty::Hle

namespace Kyty::Libs {

bool Init(const String& id, ::Kyty::Hle::HleSymbolRegistry* s);
void InitAll(::Kyty::Hle::HleSymbolRegistry* s);

} // namespace Libs

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_ */
