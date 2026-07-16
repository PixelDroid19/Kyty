#include "Emulator/Libs/Np.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::NpUniversalDataSystem {

LIB_VERSION("NpUniversalDataSystem", 1, "NpUniversalDataSystem", 1, 1);

static constexpr int error_invalid_argument = static_cast<int32_t>(0x80553102u);
static std::atomic<int32_t> g_next_handle {1};

static constexpr uint64_t event_magic           = 0x4b59545955445345ull;
static constexpr uint64_t event_properties_magic = 0x4b59545955445350ull;

struct EventPropertyObject
{
	uint64_t magic = event_properties_magic;
};

struct Event
{
	uint64_t            magic = event_magic;
	EventPropertyObject properties;
};

int KYTY_SYSV_ABI Initialize(const void* parameters)
{
	return (parameters != nullptr ? OK : error_invalid_argument);
}

int KYTY_SYSV_ABI CreateContext(int32_t* context)
{
	if (context != nullptr)
	{
		*context = 1;
	}
	return OK;
}

int KYTY_SYSV_ABI CreateHandle(int32_t* handle, int32_t* alternate_handle)
{
	auto* output = (handle != nullptr ? handle : alternate_handle);
	if (output == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}

	*output = g_next_handle.fetch_add(1, std::memory_order_relaxed) + 1;
	return OK;
}

int KYTY_SYSV_ABI RegisterContext()
{
	return OK;
}

int KYTY_SYSV_ABI CreateEvent(const char* name, uint64_t /*options*/, Event** event, EventPropertyObject** properties)
{
	if (name == nullptr || name[0] == '\0' || event == nullptr || properties == nullptr)
	{
		return error_invalid_argument;
	}

	auto* local_event = new Event;
	*event            = local_event;
	*properties       = &local_event->properties;
	return OK;
}

int KYTY_SYSV_ABI EventPropertyObjectSetInt32(EventPropertyObject* properties, const char* name, int32_t /*value*/)
{
	return (properties != nullptr && properties->magic == event_properties_magic && name != nullptr && name[0] != '\0'
	            ? OK
	            : error_invalid_argument);
}

int KYTY_SYSV_ABI EventPropertyObjectSetString(EventPropertyObject* properties, const char* name, const char* value)
{
	return (properties != nullptr && properties->magic == event_properties_magic && name != nullptr && name[0] != '\0' && value != nullptr
	            ? OK
	            : error_invalid_argument);
}

int KYTY_SYSV_ABI PostEvent(int32_t /*context*/, int32_t /*handle*/, Event* event, uint32_t /*options*/)
{
	return (event != nullptr && event->magic == event_magic ? OK : error_invalid_argument);
}

int KYTY_SYSV_ABI DestroyEvent(Event* event)
{
	if (event == nullptr || event->magic != event_magic)
	{
		return error_invalid_argument;
	}

	event->magic            = 0;
	event->properties.magic = 0;
	delete event;
	return OK;
}

LIB_DEFINE(InitNpUniversalDataSystem_1)
{
	LIB_FUNC("sjaobBgqeB4", Initialize);
	// Captured Gen5 TrophyManager thread: dual NID for Initialize.
	LIB_FUNC("AUIHb7jUX3I", Initialize);
	LIB_FUNC("5zBnau1uIEo", CreateContext);
	LIB_FUNC("hT0IAEvN+M0", CreateHandle);
	LIB_FUNC("tpFJ8LIKvPw", RegisterContext);
	LIB_FUNC("p+GcLqwpL9M", CreateEvent);
	LIB_FUNC("YE4dbtbz6OE", EventPropertyObjectSetInt32);
	LIB_FUNC("MfDb+4Nln64", EventPropertyObjectSetString);
	LIB_FUNC("CzkKf7ahIyU", PostEvent);
	LIB_FUNC("wG+84pnNIuo", DestroyEvent);
}

} // namespace Kyty::Libs::NpUniversalDataSystem

namespace Kyty::Libs::NpGameIntent {

LIB_VERSION("NpGameIntent", 1, "NpGameIntent", 1, 1);

static std::atomic<bool> g_initialized {false};

int KYTY_SYSV_ABI Initialize()
{
	g_initialized.store(true, std::memory_order_release);
	return OK;
}

LIB_DEFINE(InitNpGameIntent_1)
{
	LIB_FUNC("m87BHxt-H60", Initialize);
}

} // namespace Kyty::Libs::NpGameIntent

namespace Kyty::Libs::NpEntitlementAccess {

LIB_VERSION("NpEntitlementAccess", 1, "NpEntitlementAccess", 1, 1);

// Boot parameter block cleared on Initialize. Size matches observed guest
// SceNpEntitlementAccessBootParam buffer (32 bytes reserved).
static constexpr size_t kBootParametersSize = 0x20;

static std::atomic<bool> g_entitlement_initialized {false};

static bool LabelHasNulTerminator(const char* data, size_t size)
{
	for (size_t i = 0; i < size; i++)
	{
		if (data[i] == '\0')
		{
			return true;
		}
	}
	return false;
}

static bool LabelPaddingIsZero(const char padding[3])
{
	return padding[0] == 0 && padding[1] == 0 && padding[2] == 0;
}

static bool IsValidUnifiedLabel(const UnifiedEntitlementLabel* label)
{
	if (label == nullptr)
	{
		return false;
	}
	if (label->data[0] == '\0')
	{
		return false;
	}
	if (!LabelHasNulTerminator(label->data, sizeof(label->data)))
	{
		return false;
	}
	if (!LabelPaddingIsZero(label->padding))
	{
		return false;
	}
	return true;
}

int KYTY_SYSV_ABI Initialize(const void* init_parameters, void* boot_parameters)
{
	// Both arguments are required. Boot storage is zeroed for the guest.
	if (init_parameters == nullptr || boot_parameters == nullptr)
	{
		return ERROR_PARAMETER;
	}

	std::memset(boot_parameters, 0, kBootParametersSize);
	g_entitlement_initialized.store(true, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI GetAddcontEntitlementInfo(ServiceLabel /*service_label*/, const UnifiedEntitlementLabel* entitlement_label,
                                            AddcontEntitlementInfo* info)
{
	if (!g_entitlement_initialized.load(std::memory_order_acquire))
	{
		return ERROR_NOT_INITIALIZED;
	}

	// Null label/info or an ill-formed unified label are guest PARAMETER errors.
	if (!IsValidUnifiedLabel(entitlement_label) || info == nullptr)
	{
		return ERROR_PARAMETER;
	}

	// No local entitlement catalog is registered. Base titles without addcont
	// packages correctly receive NO_ENTITLEMENT; the output buffer is left
	// untouched so residual guest data is not misinterpreted as a result.
	return ERROR_NO_ENTITLEMENT;
}

int KYTY_SYSV_ABI GetAddcontEntitlementInfoList(ServiceLabel /*service_label*/, AddcontEntitlementInfo* list, uint32_t list_num,
                                                uint32_t* hit_num)
{
	if (!g_entitlement_initialized.load(std::memory_order_acquire))
	{
		return ERROR_NOT_INITIALIZED;
	}
	if (hit_num == nullptr || (list == nullptr && list_num != 0))
	{
		return ERROR_PARAMETER;
	}

	// This runtime has no registered add-on entitlement catalog. Report an
	// empty enumeration explicitly; do not fabricate list records.
	*hit_num = 0;
	return OK;
}

LIB_DEFINE(InitNpEntitlementAccess_1)
{
	LIB_FUNC("jO8DM8oyego", Initialize);
	// sceNpEntitlementAccessGetAddcontEntitlementInfo
	LIB_FUNC("xddD23+8TfQ", GetAddcontEntitlementInfo);
	LIB_FUNC("TFyU+KFBv54", GetAddcontEntitlementInfoList);
}

} // namespace Kyty::Libs::NpEntitlementAccess

namespace Kyty::Libs::NpManager {

// Gen5 NpManager_v1 — offline-safe account queries for titles that probe NP
// country before online services. Does not contact PSN.
LIB_VERSION("NpManager", 1, "NpManager", 1, 1);

// sceNpGetAccountCountryA — NID JT+t00a3TxA. Observed SysV (user_id=1, out*).
// Country code is a 2-letter lowercase ISO string plus NUL.
static KYTY_SYSV_ABI int GetAccountCountryA(int32_t /*user_id*/, char* country)
{
	PRINT_NAME();
	if (country == nullptr)
	{
		return static_cast<int>(0x80550003u); // SCE_NP_ERROR_INVALID_ARGUMENT-style
	}
	country[0] = 'u';
	country[1] = 's';
	country[2] = '\0';
	return OK;
}

LIB_DEFINE(InitNpManager_1)
{
	LIB_FUNC("JT+t00a3TxA", GetAccountCountryA);
}

} // namespace Kyty::Libs::NpManager

#endif // KYTY_EMU_ENABLED
