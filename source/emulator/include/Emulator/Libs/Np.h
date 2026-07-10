#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_NP_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_NP_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::NpUniversalDataSystem {

struct Event;
struct EventPropertyObject;

int KYTY_SYSV_ABI Initialize(const void* parameters);
int KYTY_SYSV_ABI CreateContext(int32_t* context);
int KYTY_SYSV_ABI CreateHandle(int32_t* handle, int32_t* alternate_handle);
int KYTY_SYSV_ABI RegisterContext();
int KYTY_SYSV_ABI CreateEvent(const char* name, uint64_t options, Event** event, EventPropertyObject** properties);
int KYTY_SYSV_ABI EventPropertyObjectSetInt32(EventPropertyObject* properties, const char* name, int32_t value);
int KYTY_SYSV_ABI EventPropertyObjectSetString(EventPropertyObject* properties, const char* name, const char* value);
int KYTY_SYSV_ABI PostEvent(int32_t context, int32_t handle, Event* event, uint32_t options);
int KYTY_SYSV_ABI DestroyEvent(Event* event);

} // namespace Kyty::Libs::NpUniversalDataSystem

namespace Kyty::Libs::NpGameIntent {

int KYTY_SYSV_ABI Initialize();

} // namespace Kyty::Libs::NpGameIntent

namespace Kyty::Libs::NpEntitlementAccess {

int KYTY_SYSV_ABI Initialize(const void* init_parameters, void* boot_parameters);

} // namespace Kyty::Libs::NpEntitlementAccess

namespace Kyty::Libs::NpTrophy2 {

int KYTY_SYSV_ABI CreateContext(int32_t* context, int32_t user_id, uint32_t service_label, uint64_t options);
int KYTY_SYSV_ABI CreateHandle(int32_t* handle);
int KYTY_SYSV_ABI RegisterContext(int32_t context, int32_t handle, uint64_t options);

} // namespace Kyty::Libs::NpTrophy2

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_NP_H_
