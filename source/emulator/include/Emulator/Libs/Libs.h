#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_

#include "Emulator/Hle/Registration.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

bool Init(const String& id, HleSymbolRegistry* s);
void InitAll(HleSymbolRegistry* s);

} // namespace Libs

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_ */
