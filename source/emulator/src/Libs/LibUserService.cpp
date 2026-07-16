#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("UserService", 1, "UserService", 1, 1);

namespace UserService {

struct UserServiceLoginUserIdList
{
	int user_id[4];
};

enum UserServiceEventType
{
	UserServiceEventTypeLogin,
	UserServiceEventTypeLogout
};

struct SceUserServiceEvent
{
	UserServiceEventType event_type;
	int                  user_id;
};

static KYTY_SYSV_ABI int UserServiceInitialize(const void* /*params*/)
{
	PRINT_NAME();

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetInitialUser(int* user_id)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(user_id == nullptr);

	*user_id = 1;

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetEvent(SceUserServiceEvent* event)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(event == nullptr);

	static bool logged_in = false;

	if (!logged_in)
	{
		logged_in         = true;
		event->event_type = UserServiceEventTypeLogin;
		event->user_id    = 1;
		return OK;
	}

	return USER_SERVICE_ERROR_NO_EVENT;
}

static KYTY_SYSV_ABI int UserServiceGetLoginUserIdList(UserServiceLoginUserIdList* user_id_list)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(user_id_list == nullptr);

	user_id_list->user_id[0] = 1;
	user_id_list->user_id[1] = -1;
	user_id_list->user_id[2] = -1;
	user_id_list->user_id[3] = -1;

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetUserName(int user_id, char* name, size_t size)
{
	EXIT_NOT_IMPLEMENTED(user_id != 1);
	EXIT_NOT_IMPLEMENTED(size < 5);

	int s = snprintf(name, size, "%s", "Kyty");

	EXIT_NOT_IMPLEMENTED(static_cast<size_t>(s) >= size);

	return OK;
}

// sceUserServicePlatformPrivacyWs1* — NID D-CzAxQL0XI (UserServicePlatformPrivacyWs1_v1).
// Observed Astro after font glyph blit; accept any args and return OK so boot continues.
static KYTY_SYSV_ABI int UserServicePlatformPrivacyWs1Stub(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                                           uint64_t a4, uint64_t a5)
{
	PRINT_NAME();
	printf("\t a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 "\n", a0, a1, a2);
	printf("\t a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a3, a4, a5);
	return OK;
}

// Guest game presets blob (40 bytes). Defaults to zeroed options.
struct UserServiceGamePresets
{
	size_t   this_size;
	uint32_t difficulty;
	uint32_t priority;
	uint32_t invert_vertical_view_for_1st_person_view;
	uint32_t invert_horizontal_view_for_1st_person_view;
	uint32_t invert_vertical_view_for_3rd_person_view;
	uint32_t invert_horizontal_view_for_3rd_person_view;
	uint32_t display_sub_titles;
	uint32_t audio_language;
};

// sceUserServiceGetGamePresets — NID -sD02mFDBh4. Observed Astro after PlayGo finished.
static KYTY_SYSV_ABI int UserServiceGetGamePresets(int user_id, UserServiceGamePresets* presets)
{
	PRINT_NAME();
	printf("\t user_id = %d\n", user_id);
	if (presets == nullptr)
	{
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	// Kyty primary user id is 1 (see GetInitialUser / GetLoginUserIdList).
	if (user_id != 1)
	{
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	size_t this_size = presets->this_size;
	if (this_size == 0 || this_size > sizeof(UserServiceGamePresets))
	{
		this_size = sizeof(UserServiceGamePresets);
	}
	std::memset(presets, 0, this_size);
	presets->this_size = sizeof(UserServiceGamePresets);
	return OK;
}

// Accessibility / age probes after save-memory setup. Primary user is 1.
static KYTY_SYSV_ABI int UserServiceGetAccessibilityVibration(int user_id, int32_t* vibration)
{
	PRINT_NAME();
	if (vibration == nullptr)
	{
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (user_id != 1)
	{
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	*vibration = 1;
	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityTriggerEffect(int user_id, int32_t* trigger_effect)
{
	PRINT_NAME();
	if (trigger_effect == nullptr)
	{
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (user_id != 1)
	{
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	*trigger_effect = 1;
	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAgeLevel(int user_id, uint32_t* age_level)
{
	PRINT_NAME();
	if (age_level == nullptr)
	{
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (user_id != 1)
	{
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	*age_level = 9;
	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityChatTranscription(int user_id, int32_t* value)
{
	PRINT_NAME();
	if (value == nullptr)
	{
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (user_id != 1)
	{
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	*value = 0;
	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityPressAndHoldDelay(int user_id, int32_t* value)
{
	PRINT_NAME();
	if (value == nullptr)
	{
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (user_id != 1)
	{
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	*value = 0;
	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityZoomEnabled(int user_id, int32_t* value)
{
	PRINT_NAME();
	if (value == nullptr)
	{
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (user_id != 1)
	{
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	*value = 0;
	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityZoomFollowFocus(int user_id, int32_t* value)
{
	PRINT_NAME();
	if (value == nullptr)
	{
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (user_id != 1)
	{
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}
	*value = 0;
	return OK;
}

} // namespace UserService

LIB_DEFINE(InitUserService_1)
{
	LIB_FUNC("j3YMu1MVNNo", UserService::UserServiceInitialize);
	LIB_FUNC("CdWp0oHWGr0", UserService::UserServiceGetInitialUser);
	LIB_FUNC("yH17Q6NWtVg", UserService::UserServiceGetEvent);
	LIB_FUNC("fPhymKNvK-A", UserService::UserServiceGetLoginUserIdList);
	LIB_FUNC("1xxcMiGu2fo", UserService::UserServiceGetUserName);
	// Gen5 privacy Ws1 entry used on Astro after font setup.
	LIB_FUNC("D-CzAxQL0XI", UserService::UserServicePlatformPrivacyWs1Stub);
	LIB_FUNC("-sD02mFDBh4", UserService::UserServiceGetGamePresets);
	LIB_FUNC("qWYHOFwqCxY", UserService::UserServiceGetAccessibilityVibration);
	LIB_FUNC("-3Y5GO+-i78", UserService::UserServiceGetAccessibilityTriggerEffect);
	LIB_FUNC("woNpu+45RLk", UserService::UserServiceGetAgeLevel);
	LIB_FUNC("rnEhHqG-4xo", UserService::UserServiceGetAccessibilityChatTranscription);
	LIB_FUNC("ZKJtxdgvzwg", UserService::UserServiceGetAccessibilityPressAndHoldDelay);
	LIB_FUNC("hD-H81EN9Vg", UserService::UserServiceGetAccessibilityZoomEnabled);
	LIB_FUNC("O6IW1-Dwm-w", UserService::UserServiceGetAccessibilityZoomFollowFocus);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
