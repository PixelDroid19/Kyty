#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <cinttypes>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("GameLiveStreaming", 1, "GameLiveStreaming", 1, 1);

namespace GameLiveStreaming {

constexpr int GAME_LIVE_STREAMING_ERROR_INVALID_ARGUMENT = static_cast<int>(0x80A00002u);
constexpr int USER_SERVICE_USER_ID_INVALID               = -1;

struct GameLiveStreamingStatus2
{
	int32_t  user_id;
	bool     is_on_air;
	uint32_t spectator_count;
	uint32_t text_message_count;
	uint32_t command_message_count;
	uint8_t  reserved[52];
};
static_assert(sizeof(GameLiveStreamingStatus2) == 72);

// The emulator has no broadcast service: the library starts and stops, and no user is ever streaming.
static int KYTY_SYSV_ABI GameLiveStreamingInitialize(size_t heap_size)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t heap_size = 0x%016" PRIx64 "\n", static_cast<uint64_t>(heap_size));
	return OK;
}

static int KYTY_SYSV_ABI GameLiveStreamingTerminate()
{
	PRINT_NAME();
	return OK;
}

static int KYTY_SYSV_ABI GameLiveStreamingGetCurrentStatus2(GameLiveStreamingStatus2* status)
{
	PRINT_NAME();
	if (status == nullptr)
	{
		return GAME_LIVE_STREAMING_ERROR_INVALID_ARGUMENT;
	}
	*status         = {};
	status->user_id = USER_SERVICE_USER_ID_INVALID;
	return OK;
}

} // namespace GameLiveStreaming

LIB_DEFINE(InitGameLiveStreaming_1)
{
	LIB_FUNC("kvYEw2lBndk", GameLiveStreaming::GameLiveStreamingInitialize);
	LIB_FUNC("9yK6Fk8mKOQ", GameLiveStreaming::GameLiveStreamingTerminate);
	LIB_FUNC("lK8dLBNp9OE", GameLiveStreaming::GameLiveStreamingGetCurrentStatus2);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
