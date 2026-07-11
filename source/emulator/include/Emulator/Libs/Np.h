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

// SceNpServiceLabel is a 32-bit service id (0 is the default/base title service).
using ServiceLabel = uint32_t;

// SceNpUnifiedEntitlementLabel — 16-character alphanumeric label + NUL in a
// 17-byte data field, with 3 bytes of reserved padding (total 20).
struct UnifiedEntitlementLabel
{
	char data[17];
	char padding[3];
};

// SceNpEntitlementAccessAddcontEntitlementInfo (28 bytes).
// Provenance: size and field order match independent PS5 addcont entitlement
// query documentation and observed guest buffers (label + packageType +
// downloadStatus).
struct AddcontEntitlementInfo
{
	UnifiedEntitlementLabel entitlement_label;
	uint32_t                package_type;
	uint32_t                download_status;
};

// Error codes (psdevwiki / SCE CE-44185..): facility 0x817D.
constexpr int ERROR_NOT_INITIALIZED = static_cast<int32_t>(0x817D0001u);
constexpr int ERROR_PARAMETER       = static_cast<int32_t>(0x817D0002u);
constexpr int ERROR_BUSY            = static_cast<int32_t>(0x817D0003u);
constexpr int ERROR_NOT_FOUND       = static_cast<int32_t>(0x817D0005u);
constexpr int ERROR_NO_ENTITLEMENT  = static_cast<int32_t>(0x817D0007u);
constexpr int ERROR_NOT_SUPPORTED   = static_cast<int32_t>(0x817D0009u);
constexpr int ERROR_INTERNAL        = static_cast<int32_t>(0x817D000Au);

// Package / download status enumerations used when an entitlement is present.
// Values follow the documented sequential SCE_NP_ENTITLEMENT_ACCESS_* enums.
constexpr uint32_t PACKAGE_TYPE_NONE   = 0;
constexpr uint32_t PACKAGE_TYPE_PSGD   = 1;
constexpr uint32_t PACKAGE_TYPE_PSAC   = 2;
constexpr uint32_t PACKAGE_TYPE_PSAL   = 3;
constexpr uint32_t PACKAGE_TYPE_PSCONS = 4;
constexpr uint32_t PACKAGE_TYPE_PSVC   = 5;
constexpr uint32_t PACKAGE_TYPE_PSSUBS = 6;

constexpr uint32_t DOWNLOAD_STATUS_NO_EXTRA_DATA      = 0;
constexpr uint32_t DOWNLOAD_STATUS_NO_IN_QUEUE        = 1;
constexpr uint32_t DOWNLOAD_STATUS_DOWNLOADING        = 2;
constexpr uint32_t DOWNLOAD_STATUS_DOWNLOAD_SUSPENDED = 3;
constexpr uint32_t DOWNLOAD_STATUS_INSTALLED          = 4;

int KYTY_SYSV_ABI Initialize(const void* init_parameters, void* boot_parameters);

// sceNpEntitlementAccessGetAddcontEntitlementInfo (NID xddD23+8TfQ).
// SysV: rdi=serviceLabel, rsi=label*, rdx=info*.
int KYTY_SYSV_ABI GetAddcontEntitlementInfo(ServiceLabel service_label, const UnifiedEntitlementLabel* entitlement_label,
                                            AddcontEntitlementInfo* info);

} // namespace Kyty::Libs::NpEntitlementAccess

namespace Kyty::Libs::NpTrophy2 {

int KYTY_SYSV_ABI CreateContext(int32_t* context, int32_t user_id, uint32_t service_label, uint64_t options);
int KYTY_SYSV_ABI CreateHandle(int32_t* handle);
int KYTY_SYSV_ABI RegisterContext(int32_t context, int32_t handle, uint64_t options);

} // namespace Kyty::Libs::NpTrophy2

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_NP_H_
