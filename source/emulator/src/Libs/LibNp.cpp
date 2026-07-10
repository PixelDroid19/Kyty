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

int KYTY_SYSV_ABI Initialize(const void* /*init_parameters*/, void* boot_parameters)
{
	if (boot_parameters != nullptr)
	{
		std::memset(boot_parameters, 0, 0x20);
	}
	return OK;
}

LIB_DEFINE(InitNpEntitlementAccess_1)
{
	LIB_FUNC("jO8DM8oyego", Initialize);
}

} // namespace Kyty::Libs::NpEntitlementAccess

#endif // KYTY_EMU_ENABLED
