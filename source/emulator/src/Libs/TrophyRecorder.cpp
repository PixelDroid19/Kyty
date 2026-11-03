#include "Emulator/Libs/TrophyRecorder.h"

#include "Kyty/Core/String.h"

#include "Emulator/AtomicFile.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/SaveData.h"
#include "Emulator/Libs/SaveDataPaths.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Trophies {

namespace {

// The user the user service reports as the initial, logged-in user.
constexpr int32_t TROPHY_USER_ID = 1;

struct Recorder
{
	std::mutex                 mutex;
	bool                       loaded    = false;
	bool                       available = false;
	// Progress changed in memory that is not on disk yet; a failed write is tried again on the next event.
	bool                       unsaved = false;
	TrophySet                  set;
	Progress                   progress;
	std::filesystem::path      path;
	TrophyCatalog              catalog;
};

Recorder g_recorder;

bool ReadHostFile(const std::filesystem::path& path, std::string* bytes)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		return false;
	}
	bytes->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return !in.bad();
}

bool ReadContainerFile(const char* guest_path, std::map<std::string, std::string>* entries)
{
	const auto  host = Kernel::FileSystem::GetExistingFilename(String::FromUtf8(guest_path));
	std::string bytes;
	if (host.IsEmpty())
	{
		return false;
	}
	const auto      path = std::filesystem::u8path(host.utf8_str().GetData());
	std::error_code size_error;
	const auto      size = std::filesystem::file_size(path, size_error);
	if (!size_error && size > CONTAINER_MAX_SIZE)
	{
		KYTY_LOG_ERROR("Trophies: %s is larger than a trophy container can be\n", guest_path);
		return false;
	}
	if (size_error || !ReadHostFile(path, &bytes))
	{
		return false;
	}
	if (!ReadContainer(std::vector<uint8_t>(bytes.begin(), bytes.end()), entries))
	{
		KYTY_LOG_ERROR("Trophies: %s is not a valid container\n", guest_path);
		return false;
	}
	return true;
}

bool Load(Recorder* recorder)
{
	std::map<std::string, std::string> trophy_files;
	std::map<std::string, std::string> uds_files;
	if (!ReadContainerFile("/app0/sce_sys/trophy2/trophy00.ucp", &trophy_files) ||
	    !ReadContainerFile("/app0/sce_sys/uds/uds00.ucp", &uds_files))
	{
		KYTY_LOG_INFO("Trophies: the title has no readable trophy set\n");
		return false;
	}
	std::string error;
	if (!ReadCatalog(trophy_files, &recorder->catalog) ||
	    !recorder->set.Load(trophy_files["tropconf.json"], uds_files["stats_definition.json"], uds_files["stats_extraction.json"], &error))
	{
		KYTY_LOG_ERROR("Trophies: cannot load the trophy set: %s\n", error.empty() ? "tropconf.json is missing or malformed" : error.c_str());
		return false;
	}
	recorder->path = SaveData::SaveDataBuildTrophyPath(SaveData::SaveDataCurrentTitleRoot(), TROPHY_USER_ID);
	if (recorder->path.empty())
	{
		KYTY_LOG_ERROR("Trophies: the save root cannot be resolved, so progress cannot be kept\n");
		return false;
	}
	std::string     saved;
	std::error_code exists_error;
	if (std::filesystem::exists(recorder->path, exists_error) &&
	    (!ReadHostFile(recorder->path, &saved) || !ProgressFromJson(saved, &recorder->progress)))
	{
		// Never overwrite progress that could not be read.
		KYTY_LOG_ERROR("Trophies: %s cannot be read; trophies stay off for this run\n", recorder->path.u8string().c_str());
		return false;
	}
	if (recorder->set.Unsupported() > 0)
	{
		KYTY_LOG_WARN("Trophies: %d trophy rules use forms Kyty does not evaluate; their trophies cannot unlock\n",
		              recorder->set.Unsupported());
	}
	return true;
}

bool EnsureLoadedLocked()
{
	// Without a mounted file system there is no title to read a trophy set from yet.
	if (!g_recorder.loaded && Kernel::FileSystem::IsMounted())
	{
		g_recorder.loaded    = true;
		g_recorder.available = Load(&g_recorder);
	}
	return g_recorder.available;
}

} // namespace

void TrophyRecordEvent(const std::string& event, const EventValues& values)
{
	std::lock_guard lock(g_recorder.mutex);
	if (!EnsureLoadedLocked())
	{
		return;
	}
	const auto stats_before = g_recorder.progress.stats;
	const auto unlocked     = g_recorder.set.Post(event, values, static_cast<int64_t>(std::time(nullptr)), &g_recorder.progress);
	g_recorder.unsaved = g_recorder.unsaved || !unlocked.empty() || g_recorder.progress.stats != stats_before;
	if (!g_recorder.unsaved)
	{
		return;
	}
	for (const int id: unlocked)
	{
		const auto& trophies = g_recorder.catalog.trophies;
		const auto  found    = std::find_if(trophies.begin(), trophies.end(), [id](const TrophyEntry& entry) { return entry.id == id; });
		KYTY_LOG_INFO("Trophy unlocked: %s\n", found != trophies.end() ? found->name.c_str() : std::to_string(id).c_str());
	}
	const auto json = ProgressToJson(g_recorder.progress);
	if (!AtomicFileWrite(g_recorder.path, json.data(), json.size()))
	{
		KYTY_LOG_ERROR("Trophies: cannot save progress to %s\n", g_recorder.path.u8string().c_str());
		return;
	}
	g_recorder.unsaved = false;
}

bool TrophyQuery(TrophyCatalog* catalog, Progress* progress)
{
	std::lock_guard lock(g_recorder.mutex);
	if (!EnsureLoadedLocked())
	{
		return false;
	}
	*catalog  = g_recorder.catalog;
	*progress = g_recorder.progress;
	return true;
}

} // namespace Kyty::Libs::Trophies

#endif // KYTY_EMU_ENABLED
