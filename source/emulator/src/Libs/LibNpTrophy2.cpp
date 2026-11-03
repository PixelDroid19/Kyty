#include "Emulator/Libs/Np.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/TrophyRecorder.h"

#include "Kyty/Core/VirtualMemory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string_view>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::NpTrophy2 {

LIB_VERSION("NpTrophy2", 1, "NpTrophy2", 1, 1);

static constexpr int error_invalid_argument = static_cast<int32_t>(0x80551604u);
static std::atomic<int32_t> g_next_context {0};
static std::atomic<int32_t> g_next_handle {0};

// sceNpTrophy2CreateContext — NID Bagshr7OQ6Q
int KYTY_SYSV_ABI CreateContext(int32_t* context, int32_t /*user_id*/, uint32_t /*service_label*/, uint64_t options)
{
	if (context == nullptr || options != 0)
	{
		return error_invalid_argument;
	}

	*context = g_next_context.fetch_add(1, std::memory_order_relaxed) + 1;
	return OK;
}

// sceNpTrophy2CreateHandle — NID Gz1rmUZpROM
int KYTY_SYSV_ABI CreateHandle(int32_t* handle)
{
	if (handle == nullptr)
	{
		return error_invalid_argument;
	}

	*handle = g_next_handle.fetch_add(1, std::memory_order_relaxed) + 1;
	return OK;
}

// sceNpTrophy2RegisterContext — NID bIDov3wBu5Q
int KYTY_SYSV_ABI RegisterContext(int32_t context, int32_t handle, uint64_t options)
{
	return (context > 0 && handle > 0 && options == 0 ? OK : error_invalid_argument);
}

// sceNpTrophy2RegisterUnlockCallback — NID sUXGfNMalIo
int KYTY_SYSV_ABI RegisterUnlockCallback(void* /*callback*/, void* /*userdata*/)
{
	return OK;
}

int KYTY_SYSV_ABI DestroyContext(int32_t context)
{
	return (context > 0 ? OK : error_invalid_argument);
}

int KYTY_SYSV_ABI DestroyHandle(int32_t handle)
{
	return (handle > 0 ? OK : error_invalid_argument);
}

int KYTY_SYSV_ABI AbortHandle(int32_t handle)
{
	return (handle > 0 ? OK : error_invalid_argument);
}

int KYTY_SYSV_ABI UnregisterUnlockCallback(void* /*callback*/)
{
	return OK;
}

namespace {

// Layouts as titles read them; two independent implementations agree on every offset.
struct TrophyProgress
{
	int32_t  type;
	uint8_t  reserved[4];
	uint64_t value;
};

struct GameDetails
{
	uint32_t groups;
	uint32_t trophies;
	uint32_t platinum;
	uint32_t gold;
	uint32_t silver;
	uint32_t bronze;
	char     title[128];
};

struct GameData
{
	uint32_t unlocked;
	uint32_t unlocked_platinum;
	uint32_t unlocked_gold;
	uint32_t unlocked_silver;
	uint32_t unlocked_bronze;
	uint32_t progress_percent;
};

struct TrophyDetails
{
	int32_t        id;
	int32_t        grade; // 1 platinum, 2 gold, 3 silver, 4 bronze
	int32_t        group;
	uint8_t        hidden;
	uint8_t        has_reward;
	uint8_t        reserved[2];
	TrophyProgress target;
	char           name[128];
	char           description[1024];
	char           reward[128];
};

struct TrophyData
{
	int32_t        id;
	uint8_t        unlocked;
	uint8_t        reserved[3];
	TrophyProgress progress;
	uint64_t       unlock_tick; // microseconds since 0001-01-01 UTC
};

static_assert(sizeof(GameDetails) == 152 && sizeof(GameData) == 24);
static_assert(sizeof(TrophyDetails) == 1312 && sizeof(TrophyData) == 32);

constexpr uint64_t kUnixEpochTick = 62135596800ull * 1000000ull;

bool IsMappedOutput(void* output, uint64_t size)
{
	return output != nullptr && Core::VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(output), size);
}

void CopyText(char* field, size_t size, const std::string& text)
{
	const size_t length = std::min(text.size(), size - 1);
	std::memcpy(field, text.data(), length);
	field[length] = '\0';
}

int32_t GradeNumber(char grade)
{
	return static_cast<int32_t>(std::string_view("PGSB").find(grade)) + 1;
}

// A title without a readable trophy set has no trophies.
void QueryTrophies(Trophies::TrophyCatalog* catalog, Trophies::Progress* progress)
{
	if (!Trophies::TrophyQuery(catalog, progress))
	{
		*catalog  = {};
		*progress = {};
	}
}

void WriteTrophy(const Trophies::TrophyEntry& entry, const Trophies::Progress& progress, TrophyDetails* details, TrophyData* data)
{
	if (details != nullptr)
	{
		*details            = {};
		details->id         = entry.id;
		details->grade      = GradeNumber(entry.grade);
		details->group      = entry.group;
		details->hidden     = entry.hidden ? 1 : 0;
		details->has_reward = entry.has_reward ? 1 : 0;
		CopyText(details->name, sizeof(details->name), entry.name);
		CopyText(details->description, sizeof(details->description), entry.detail);
	}
	if (data != nullptr)
	{
		*data            = {};
		data->id         = entry.id;
		const auto found = progress.unlocked.find(entry.id);
		if (found != progress.unlocked.end())
		{
			data->unlocked    = 1;
			data->unlock_tick = kUnixEpochTick + static_cast<uint64_t>(std::max<int64_t>(found->second, 0)) * 1000000ull;
		}
	}
}

} // namespace

int KYTY_SYSV_ABI GetGameInfo(int32_t context, int32_t handle, void* details, void* data)
{
	if (context <= 0 || handle <= 0 || !IsMappedOutput(details, sizeof(GameDetails)) || !IsMappedOutput(data, sizeof(GameData)))
	{
		return error_invalid_argument;
	}
	Trophies::TrophyCatalog catalog;
	Trophies::Progress      progress;
	QueryTrophies(&catalog, &progress);

	GameDetails game {};
	GameData    earned {};
	game.groups = static_cast<uint32_t>(catalog.groups);
	CopyText(game.title, sizeof(game.title), catalog.title);
	uint32_t* const total[] = {&game.platinum, &game.gold, &game.silver, &game.bronze};
	uint32_t* const owned[] = {&earned.unlocked_platinum, &earned.unlocked_gold, &earned.unlocked_silver, &earned.unlocked_bronze};
	for (const auto& entry: catalog.trophies)
	{
		const uint32_t unlocked = progress.unlocked.count(entry.id);
		const auto     grade    = GradeNumber(entry.grade) - 1;
		game.trophies++;
		(*total[grade])++;
		earned.unlocked += unlocked;
		*owned[grade] += unlocked;
	}
	earned.progress_percent = (game.trophies != 0 ? earned.unlocked * 100 / game.trophies : 0);
	std::memcpy(details, &game, sizeof(game));
	std::memcpy(data, &earned, sizeof(earned));
	return OK;
}

int KYTY_SYSV_ABI GetTrophyInfo(int32_t context, int32_t handle, int32_t trophy_id, void* details, void* data)
{
	if (context <= 0 || handle <= 0 || !IsMappedOutput(details, sizeof(TrophyDetails)) || !IsMappedOutput(data, sizeof(TrophyData)))
	{
		return error_invalid_argument;
	}
	Trophies::TrophyCatalog catalog;
	Trophies::Progress      progress;
	QueryTrophies(&catalog, &progress);
	const auto found = std::find_if(catalog.trophies.begin(), catalog.trophies.end(),
	                                [trophy_id](const Trophies::TrophyEntry& entry) { return entry.id == trophy_id; });
	if (found == catalog.trophies.end())
	{
		return error_invalid_argument;
	}
	WriteTrophy(*found, progress, static_cast<TrophyDetails*>(details), static_cast<TrophyData*>(data));
	return OK;
}

// sceNpTrophy2GetTrophyInfoArray(context, handle, offset, limit, details[], data[], written*): records offset ..
// offset + limit of the set in id order; either array may be null.
int KYTY_SYSV_ABI GetTrophyInfoArray(int32_t context, int32_t handle, uint32_t offset, uint32_t limit, void* details, void* data,
                                     uint32_t* written)
{
	if (context <= 0 || handle <= 0 || written == nullptr || (details == nullptr && data == nullptr))
	{
		return error_invalid_argument;
	}
	Trophies::TrophyCatalog catalog;
	Trophies::Progress      progress;
	QueryTrophies(&catalog, &progress);
	const uint32_t available = static_cast<uint32_t>(catalog.trophies.size());
	const uint32_t first     = std::min(offset, available);
	const uint32_t count     = std::min(limit, available - first);
	if ((details != nullptr && !IsMappedOutput(details, uint64_t {count} * sizeof(TrophyDetails))) ||
	    (data != nullptr && !IsMappedOutput(data, uint64_t {count} * sizeof(TrophyData))))
	{
		return error_invalid_argument;
	}
	for (uint32_t i = 0; i < count; i++)
	{
		WriteTrophy(catalog.trophies[first + i], progress, details != nullptr ? static_cast<TrophyDetails*>(details) + i : nullptr,
		            data != nullptr ? static_cast<TrophyData*>(data) + i : nullptr);
	}
	*written = count;
	return OK;
}

int KYTY_SYSV_ABI ShowTrophyList(int32_t context, int32_t handle, uint32_t /*flags*/)
{
	return (context > 0 && handle > 0 ? OK : error_invalid_argument);
}

LIB_DEFINE(InitNpTrophy2_1)
{
	LIB_FUNC("Bagshr7OQ6Q", CreateContext);
	LIB_FUNC("Gz1rmUZpROM", CreateHandle);
	LIB_FUNC("bIDov3wBu5Q", RegisterContext);
	LIB_FUNC("sUXGfNMalIo", RegisterUnlockCallback);
	LIB_FUNC("sysY2FHYff4", DestroyContext);
	LIB_FUNC("d8P11CI40KE", DestroyHandle);
	LIB_FUNC("fYapWA9xVmA", AbortHandle);
	LIB_FUNC("wVqxM58sIKs", UnregisterUnlockCallback);
	LIB_FUNC("4IzqhhUQ3nk", GetGameInfo);
	LIB_FUNC("EwNylPdWUTM", GetTrophyInfo);
	LIB_FUNC("y3zHpdZO6ME", GetTrophyInfoArray);
	LIB_FUNC("EHQEDVXZ0TI", ShowTrophyList);
}

} // namespace Kyty::Libs::NpTrophy2

#endif // KYTY_EMU_ENABLED
