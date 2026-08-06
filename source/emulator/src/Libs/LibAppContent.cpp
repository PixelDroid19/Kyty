#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SystemContent.h"

#include <cstring>

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

static constexpr int      APP_CONTENT_ERROR_PARAMETER = static_cast<int>(0x80d90002u);
static constexpr uint64_t TEMPORARY_DATA_QUOTA_KB     = 1024ULL * 1024ULL;

int KYTY_SYSV_ABI AppContentInitialize(const AppContentInitParam* init_param, AppContentBootParam* boot_param)
{
	PRINT_NAME();

	if (init_param == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (boot_param == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	boot_param->attr = 0;

	return OK;
}

int KYTY_SYSV_ABI AppContentGetAddcontInfoList(uint32_t service_label, AppContentAddcontInfo* /*list*/, uint32_t list_num,
                                               uint32_t* hit_num)
{
	PRINT_NAME();

	if (hit_num == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t service_label = %u\n", service_label);
	KYTY_LOG_DEBUG("\t list_num      = %u\n", list_num);

	*hit_num = 0;

	return OK;
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
	LIB_FUNC("99b82IKXpH4", AppContent::AppContentAppParamGetInt);
	LIB_FUNC("buYbeLOGWmA", AppContent::AppContentTemporaryDataMount2);
	LIB_FUNC("SaKib2Ug0yI", AppContent::AppContentTemporaryDataGetAvailableSpaceKb);
	LIB_FUNC("Gl6w5i0JokY", AppContent::AppContentDownloadDataGetAvailableSpaceKb);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
