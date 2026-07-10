#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATA_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATA_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::SaveData {

int KYTY_SYSV_ABI SaveDataCreateTransactionResource(int32_t user_id);

} // namespace Kyty::Libs::SaveData

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATA_H_
