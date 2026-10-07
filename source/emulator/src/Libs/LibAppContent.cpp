#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/AddcontInventory.h"
#include "Emulator/Loader/SystemContent.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("AppContent", 1, "AppContentUtil", 1, 1);

namespace AppContent {

struct AppContentInitParam
{
	char reserved[32];
};

struct AppContentBootParam
{
	char     reserved1[4];
	uint32_t attr;
	char     reserved2[32];
};

struct NpUnifiedEntitlementLabel
{
	char data[17];
	char padding[3];
};

struct AppContentAddcontInfo
{
	NpUnifiedEntitlementLabel entitlement_label;
	uint32_t                  status;
};

struct AppContentMountPoint
{
	char data[16];
};

static constexpr int      APP_CONTENT_ERROR_PARAMETER          = static_cast<int>(0x80d90002u);
static constexpr int      APP_CONTENT_ERROR_BUSY               = static_cast<int>(0x80d90003u);
static constexpr int      APP_CONTENT_ERROR_NOT_FOUND          = static_cast<int>(0x80d90005u);
static constexpr int      APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT = static_cast<int>(0x80d90007u);
static constexpr uint64_t TEMPORARY_DATA_QUOTA_KB              = 1024ULL * 1024ULL;

static void WriteAddcontInfo(const Loader::AddcontEntry& entry, AppContentAddcontInfo* info)
{
	std::memset(info, 0, sizeof(*info));
	std::memcpy(info->entitlement_label.data, entry.entitlement_label.data(), entry.entitlement_label.size());
	info->status = entry.download_status;
}

int KYTY_SYSV_ABI AppContentInitialize(const AppContentInitParam* init_param, AppContentBootParam* boot_param)
{
	PRINT_NAME();

	if (init_param == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (boot_param == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	boot_param->attr = 0;

	return OK;
}

// hit_num receives every installed entry of the service label; the list gets
// as many as fit.
int KYTY_SYSV_ABI AppContentGetAddcontInfoList(uint32_t service_label, AppContentAddcontInfo* list, uint32_t list_num,
                                               uint32_t* hit_num)
{
	PRINT_NAME();
	if (Loader::AddcontInventoryGetState() == Loader::AddcontInventoryState::Invalid)
	{
		return APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
	}
	const bool wants_list = (list != nullptr && list_num != 0);
	if (!wants_list && hit_num == nullptr)
	{
		return APP_CONTENT_ERROR_PARAMETER;
	}
	const auto     entries = Loader::AddcontInventoryList(service_label);
	const uint32_t written = wants_list ? std::min<uint32_t>(list_num, static_cast<uint32_t>(entries.size())) : 0u;
	for (uint32_t i = 0; i < written; i++)
	{
		WriteAddcontInfo(entries[i], &list[i]);
	}
	if (hit_num != nullptr)
	{
		*hit_num = static_cast<uint32_t>(entries.size());
	}
	return OK;
}

int KYTY_SYSV_ABI AppContentGetAddcontInfo(uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label,
                                           AppContentAddcontInfo* info)
{
	PRINT_NAME();
	std::string label;
	if (entitlement_label == nullptr || info == nullptr || !Loader::AddcontReadGuestLabel(entitlement_label->data, &label))
	{
		return APP_CONTENT_ERROR_PARAMETER;
	}
	Loader::AddcontEntry entry;
	if (!Loader::AddcontInventoryFind(service_label, label, &entry))
	{
		return APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
	}
	WriteAddcontInfo(entry, info);
	return OK;
}

int KYTY_SYSV_ABI AppContentGetEntitlementKey(uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label, uint8_t* key)
{
	PRINT_NAME();
	std::string label;
	if (entitlement_label == nullptr || key == nullptr || !Loader::AddcontReadGuestLabel(entitlement_label->data, &label))
	{
		return APP_CONTENT_ERROR_PARAMETER;
	}
	Loader::AddcontEntry entry;
	if (!Loader::AddcontInventoryFind(service_label, label, &entry))
	{
		return APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
	}
	std::memcpy(key, entry.entitlement_key.data(), entry.entitlement_key.size());
	return OK;
}

// Only installed content with data (PSAC) is mountable; the mount point stays
// claimed until AddcontUnmount releases it.
int KYTY_SYSV_ABI AppContentAddcontMount(uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label,
                                         AppContentMountPoint* mount_point)
{
	PRINT_NAME();
	std::string label;
	if (entitlement_label == nullptr || mount_point == nullptr || !Loader::AddcontReadGuestLabel(entitlement_label->data, &label))
	{
		return APP_CONTENT_ERROR_PARAMETER;
	}
	std::array<char, Loader::ADDCONT_MOUNT_POINT_SIZE> point {};
	switch (Loader::AddcontInventoryMount(service_label, label, &point))
	{
		case Loader::AddcontMountResult::Mounted: std::memcpy(mount_point->data, point.data(), point.size()); return OK;
		case Loader::AddcontMountResult::Busy: return APP_CONTENT_ERROR_BUSY;
		case Loader::AddcontMountResult::NotFound: return APP_CONTENT_ERROR_NOT_FOUND;
	}
	return APP_CONTENT_ERROR_NOT_FOUND;
}

int KYTY_SYSV_ABI AppContentAddcontUnmount(const AppContentMountPoint* mount_point)
{
	PRINT_NAME();
	if (mount_point == nullptr)
	{
		return APP_CONTENT_ERROR_PARAMETER;
	}
	const std::string point(mount_point->data, strnlen(mount_point->data, sizeof(mount_point->data)));
	return Loader::AddcontInventoryUnmount(point) ? OK : APP_CONTENT_ERROR_NOT_FOUND;
}

int KYTY_SYSV_ABI AppContentAppParamGetInt(uint32_t param_id, int32_t* value)
{
	PRINT_NAME();

	if (value == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	*value     = 0;
	bool found = false;

	KYTY_LOG_DEBUG("\t param_id = %u\n", param_id);

	switch (param_id)
	{
		case 0:
			*value = 3;
			found  = true;
			break;
		case 1: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_1", value); break;
		case 2: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_2", value); break;
		case 3: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_3", value); break;
		case 4: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_4", value); break;
		default:
			KYTY_LOG_WARN("AppContentAppParamGetInt: unknown param_id %u; returning parameter error\n", param_id);
			return APP_CONTENT_ERROR_PARAMETER;
	}

	KYTY_LOG_DEBUG("\t value    = %d [%s]\n", *value, found ? "found" : "not found");

	return OK;
}

int KYTY_SYSV_ABI AppContentTemporaryDataMount2(const void* /*param*/, AppContentMountPoint* mount_point)
{
	PRINT_NAME();
	if (mount_point == nullptr)
	{
		return APP_CONTENT_ERROR_PARAMETER;
	}
	static constexpr char mount_name[] = "/temp0";
	std::memcpy(mount_point->data, mount_name, sizeof(mount_name));
	return OK;
}

int KYTY_SYSV_ABI AppContentTemporaryDataGetAvailableSpaceKb(const AppContentMountPoint* mount_point, uint64_t* available_kb)
{
	PRINT_NAME();
	static constexpr char mount_name[] = "/temp0";
	if (mount_point == nullptr || available_kb == nullptr ||
	    std::memcmp(mount_point->data, mount_name, sizeof(mount_name)) != 0)
	{
		return APP_CONTENT_ERROR_PARAMETER;
	}
	*available_kb = TEMPORARY_DATA_QUOTA_KB;
	return OK;
}

int KYTY_SYSV_ABI AppContentDownloadDataGetAvailableSpaceKb(const void* /*param*/, uint64_t* available_kb)
{
	PRINT_NAME();
	if (available_kb == nullptr)
	{
		return APP_CONTENT_ERROR_PARAMETER;
	}
	*available_kb = TEMPORARY_DATA_QUOTA_KB;
	return OK;
}

} // namespace AppContent

LIB_DEFINE(InitAppContent_1)
{
	LIB_FUNC("R9lA82OraNs", AppContent::AppContentInitialize);
	LIB_FUNC("xnd8BJzAxmk", AppContent::AppContentGetAddcontInfoList);
	LIB_FUNC("m47juOmH0VE", AppContent::AppContentGetAddcontInfo);
	LIB_FUNC("XTWR0UXvcgs", AppContent::AppContentGetEntitlementKey);
	LIB_FUNC("VANhIWcqYak", AppContent::AppContentAddcontMount);
	LIB_FUNC("3rHWaV-1KC4", AppContent::AppContentAddcontUnmount);
	LIB_FUNC("99b82IKXpH4", AppContent::AppContentAppParamGetInt);
	LIB_FUNC("buYbeLOGWmA", AppContent::AppContentTemporaryDataMount2);
	LIB_FUNC("SaKib2Ug0yI", AppContent::AppContentTemporaryDataGetAvailableSpaceKb);
	LIB_FUNC("Gl6w5i0JokY", AppContent::AppContentDownloadDataGetAvailableSpaceKb);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
