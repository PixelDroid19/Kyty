#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_NP_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_NP_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::NpUniversalDataSystem {

int KYTY_SYSV_ABI Initialize(const void* parameters);
int KYTY_SYSV_ABI CreateContext(int32_t* context);
int KYTY_SYSV_ABI CreateHandle(int32_t* handle, int32_t* alternate_handle);
int KYTY_SYSV_ABI RegisterContext();

} // namespace Kyty::Libs::NpUniversalDataSystem

namespace Kyty::Libs::NpTrophy2 {

int KYTY_SYSV_ABI CreateContext(int32_t* context, int32_t user_id, uint32_t service_label, uint64_t options);
int KYTY_SYSV_ABI CreateHandle(int32_t* handle);

} // namespace Kyty::Libs::NpTrophy2

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_NP_H_
