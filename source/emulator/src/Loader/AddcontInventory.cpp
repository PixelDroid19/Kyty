#include "Emulator/Loader/AddcontInventory.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/JsonReader.h"
#include "Kyty/Core/String.h"

#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string_view>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

namespace {

constexpr size_t           kMaxRecords      = 256;
constexpr uintmax_t        kMaxManifestSize = 64u * 1024u;
constexpr std::string_view kApp0Prefix      = "/app0/";

struct Record
{
	AddcontEntry entry;
	std::string  guest_mount_point; // a declared /app0/ directory, or /addcontN for a package
	std::string  host_folder;       // package folder mounted at guest_mount_point; empty for /app0 data
	bool         mounted = false;
};

struct Inventory
{
	AddcontInventoryState state = AddcontInventoryState::None;
	std::vector<Record>   records;
};

struct ManifestRecord
{
	std::string           section;
	std::string           content_id;
	std::string           download_status;
	std::string           mount_point;
	std::string           entitlement_key;
	std::string           np_service_label;
	std::set<std::string> keys;
};

std::mutex g_mutex;
Inventory  g_inventory;

bool IsDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool IsUpper(char c)
{
	return c >= 'A' && c <= 'Z';
}

bool IsLabelChar(char c)
{
	return IsDigit(c) || IsUpper(c) || (c >= 'a' && c <= 'z') || c == '-' || c == '_';
}

std::string Trim(const std::string& text)
{
	const auto first = text.find_first_not_of(" \t\r");
	if (first == std::string::npos)
	{
		return {};
	}
	return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// XX0000-PPSA00000_00-LABEL: region, publisher id, title id, then the
// 16-character entitlement label.
bool ParseContentId(const std::string& content_id, const std::string& title_id, std::string* label)
{
	if (content_id.size() != 20 + ADDCONT_LABEL_SIZE || !IsUpper(content_id[0]) || !IsUpper(content_id[1]) || content_id[6] != '-')
	{
		return false;
	}
	if (!std::all_of(content_id.begin() + 2, content_id.begin() + 6, IsDigit) || content_id.compare(16, 4, "_00-") != 0 ||
	    content_id.compare(7, 9, title_id) != 0)
	{
		return false;
	}
	*label = content_id.substr(20);
	return std::all_of(label->begin(), label->end(), IsLabelChar);
}

// The dump tool writes records without a status for content that is installed.
bool ParseDownloadStatus(const std::string& text, uint32_t* status)
{
	if (text.empty() || text == "INSTALLED")
	{
		*status = ADDCONT_DOWNLOAD_STATUS_INSTALLED;
		return true;
	}
	if (text == "NO_EXTRA_DATA")
	{
		*status = ADDCONT_DOWNLOAD_STATUS_NO_EXTRA_DATA;
		return true;
	}
	return false;
}

// Absent or -1 is the wildcard; otherwise a 32-bit decimal service label.
bool ParseServiceLabel(const std::string& text, int64_t* service_label)
{
	if (text.empty() || text == "-1")
	{
		*service_label = -1;
		return true;
	}
	if (text.size() > 10 || !std::all_of(text.begin(), text.end(), IsDigit))
	{
		return false;
	}
	const uint64_t value = std::stoull(text);
	*service_label       = static_cast<int64_t>(value);
	return value <= UINT32_MAX;
}

bool ParseKey(const std::string& text, std::array<uint8_t, 16>* key)
{
	if (text.size() != key->size() * 2 || !std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(c) != 0; }))
	{
		return false;
	}
	for (size_t i = 0; i < key->size(); i++)
	{
		(*key)[i] = static_cast<uint8_t>(std::stoul(text.substr(i * 2, 2), nullptr, 16));
	}
	return true;
}

bool IsApp0Directory(const std::filesystem::path& app0_root, const std::string& mount_point)
{
	if (mount_point.size() >= ADDCONT_MOUNT_POINT_SIZE || mount_point.compare(0, kApp0Prefix.size(), kApp0Prefix) != 0 ||
	    mount_point.back() == '/')
	{
		return false;
	}
	const std::filesystem::path relative(mount_point.substr(kApp0Prefix.size()));
	for (const auto& part: relative)
	{
		if (part.empty() || part == "." || part == ".." || part.string().find('\\') != std::string::npos)
		{
			return false;
		}
	}
	std::error_code ec;
	return std::filesystem::is_directory(app0_root / relative, ec);
}

std::string* ManifestField(ManifestRecord* record, const std::string& key)
{
	if (key == "content_id") { return &record->content_id; }
	if (key == "download_status") { return &record->download_status; }
	if (key == "mount_point") { return &record->mount_point; }
	if (key == "entitlement_key") { return &record->entitlement_key; }
	if (key == "np_service_label") { return &record->np_service_label; }
	return nullptr;
}

std::string ParseManifestProperty(const std::string& line, ManifestRecord* record)
{
	const size_t equal = line.find('=');
	if (equal == std::string::npos || equal == 0 || equal + 1 == line.size())
	{
		return "malformed property '" + line + "'";
	}
	const std::string key   = Trim(line.substr(0, equal));
	std::string*      field = ManifestField(record, key);
	if (field == nullptr)
	{
		return "unsupported property '" + key + "'";
	}
	if (!record->keys.insert(key).second)
	{
		return "duplicate property '" + key + "'";
	}
	*field = Trim(line.substr(equal + 1));
	return {};
}

std::string ParseManifestLine(const std::string& line, std::vector<ManifestRecord>* records)
{
	if (line.empty() || line[0] == '#' || line[0] == ';')
	{
		return {};
	}
	if (line[0] == '[')
	{
		if (line != "[PSAC]" && line != "[PSAL]")
		{
			return "unknown section '" + line + "'";
		}
		if (records->size() == kMaxRecords)
		{
			return "too many records";
		}
		records->push_back({});
		records->back().section = line.substr(1, 4);
		return {};
	}
	if (records->empty())
	{
		return "property before any section";
	}
	return ParseManifestProperty(line, &records->back());
}

std::string BuildManifestRecord(const ManifestRecord& in, const std::filesystem::path& app0_root, const std::string& title_id,
                                Record* out)
{
	if (!ParseContentId(in.content_id, title_id, &out->entry.entitlement_label))
	{
		return "invalid or foreign content_id '" + in.content_id + "'";
	}
	out->entry.package_type = (in.section == "PSAC" ? AddcontPackageType::Psac : AddcontPackageType::Psal);
	if (!ParseDownloadStatus(in.download_status, &out->entry.download_status))
	{
		return "unsupported download_status '" + in.download_status + "'";
	}
	if (!ParseServiceLabel(in.np_service_label, &out->entry.service_label))
	{
		return "invalid np_service_label '" + in.np_service_label + "'";
	}
	if (!in.entitlement_key.empty() && !ParseKey(in.entitlement_key, &out->entry.entitlement_key))
	{
		return "malformed entitlement_key";
	}
	if (!in.mount_point.empty() && !IsApp0Directory(app0_root, in.mount_point))
	{
		return "mount_point '" + in.mount_point + "' is not a directory under /app0";
	}
	out->guest_mount_point = in.mount_point;
	return {};
}

std::string LoadManifest(const std::filesystem::path& app0_root, const std::string& title_id, std::vector<Record>* records)
{
	const auto      path = app0_root / "dlc_emu.ini";
	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec))
	{
		return {};
	}
	if (std::filesystem::file_size(path, ec) > kMaxManifestSize || ec)
	{
		return "dlc_emu.ini is unreadable or too large";
	}
	std::ifstream               file(path);
	std::vector<ManifestRecord> manifest;
	std::string                 line;
	for (size_t number = 1; std::getline(file, line); number++)
	{
		if (auto error = ParseManifestLine(Trim(line), &manifest); !error.empty())
		{
			return "dlc_emu.ini line " + std::to_string(number) + ": " + error;
		}
	}
	for (const auto& in: manifest)
	{
		Record record;
		if (auto error = BuildManifestRecord(in, app0_root, title_id, &record); !error.empty())
		{
			return "dlc_emu.ini: " + error;
		}
		records->push_back(std::move(record));
	}
	return {};
}

std::string ReadPackageContentId(const std::filesystem::path& folder)
{
	const auto      path = folder / "sce_sys" / "param.json";
	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec))
	{
		return {};
	}
	const auto  contents = Core::File::Read(String::FromUtf8(path.string().c_str()), Core::File::Encoding::Utf8);
	const auto* json     = Core::Json::Create(contents);
	std::string content_id;
	if (json != nullptr && json->IsObject())
	{
		content_id = json->GetString("contentId").utf8_str().GetData();
	}
	delete json;
	return content_id;
}

std::string BuildPackageRecord(const std::filesystem::path& folder, const std::string& title_id, size_t index, Record* out)
{
	const std::string content_id = ReadPackageContentId(folder);
	if (!ParseContentId(content_id, title_id, &out->entry.entitlement_label))
	{
		return "package '" + folder.filename().string() + "' has no valid contentId for this title in sce_sys/param.json";
	}
	out->entry.package_type    = AddcontPackageType::Psac;
	out->entry.download_status = ADDCONT_DOWNLOAD_STATUS_INSTALLED;
	out->guest_mount_point     = "/addcont" + std::to_string(index);
	out->host_folder           = folder.string();
	return {};
}

std::string LoadPackages(const std::filesystem::path& title_root, const std::string& title_id, std::vector<Record>* records)
{
	std::error_code ec;
	if (!std::filesystem::is_directory(title_root, ec))
	{
		return {};
	}
	std::vector<std::filesystem::path> folders;
	for (std::filesystem::directory_iterator it(title_root, ec), end; !ec && it != end; it.increment(ec))
	{
		if (it->is_directory(ec))
		{
			folders.push_back(it->path());
		}
	}
	if (ec)
	{
		return "add-on packages folder of the title is unreadable";
	}
	// Sorted, so a package keeps its /addcontN mount point from run to run.
	std::sort(folders.begin(), folders.end());
	for (size_t index = 0; index < folders.size(); index++)
	{
		Record record;
		if (auto error = BuildPackageRecord(folders[index], title_id, index, &record); !error.empty())
		{
			return error;
		}
		records->push_back(std::move(record));
	}
	return {};
}

std::string CheckUnique(const std::vector<Record>& records)
{
	std::set<std::string> labels;
	std::set<std::string> mount_points;
	for (const auto& record: records)
	{
		if (!labels.insert(record.entry.entitlement_label).second)
		{
			return "entitlement label '" + record.entry.entitlement_label + "' is declared twice";
		}
		if (!record.guest_mount_point.empty() && !mount_points.insert(record.guest_mount_point).second)
		{
			return "mount point '" + record.guest_mount_point + "' is declared twice";
		}
	}
	return {};
}

bool MatchesService(const AddcontEntry& entry, uint32_t service_label)
{
	return entry.service_label == -1 || static_cast<uint32_t>(entry.service_label) == service_label;
}

Record* FindRecord(uint32_t service_label, const std::string& entitlement_label)
{
	for (auto& record: g_inventory.records)
	{
		if (MatchesService(record.entry, service_label) && record.entry.entitlement_label == entitlement_label)
		{
			return &record;
		}
	}
	return nullptr;
}

bool IsMountable(const Record& record)
{
	return record.entry.package_type == AddcontPackageType::Psac && !record.guest_mount_point.empty() &&
	       record.entry.download_status == ADDCONT_DOWNLOAD_STATUS_INSTALLED;
}

} // namespace

void AddcontInventoryLoad(const std::string& app0_root, const std::string& packages_root, const std::string& title_id)
{
	Inventory   inventory;
	std::string error = LoadManifest(app0_root, title_id, &inventory.records);
	if (error.empty() && !packages_root.empty())
	{
		error = LoadPackages(std::filesystem::path(packages_root) / title_id, title_id, &inventory.records);
	}
	if (error.empty())
	{
		error = CheckUnique(inventory.records);
	}
	if (!error.empty())
	{
		KYTY_LOG_ERROR("add-on content inventory is invalid: %s\n", error.c_str());
		inventory.records.clear();
		inventory.state = AddcontInventoryState::Invalid;
	} else
	{
		inventory.state = inventory.records.empty() ? AddcontInventoryState::None : AddcontInventoryState::Ready;
		KYTY_LOG_INFO("add-on content inventory: %zu installed\n", inventory.records.size());
	}
	std::lock_guard lock(g_mutex);
	g_inventory = std::move(inventory);
}

AddcontInventoryState AddcontInventoryGetState()
{
	std::lock_guard lock(g_mutex);
	return g_inventory.state;
}

std::vector<AddcontEntry> AddcontInventoryList(uint32_t service_label)
{
	std::lock_guard           lock(g_mutex);
	std::vector<AddcontEntry> entries;
	for (const auto& record: g_inventory.records)
	{
		if (MatchesService(record.entry, service_label))
		{
			entries.push_back(record.entry);
		}
	}
	return entries;
}

bool AddcontInventoryFind(uint32_t service_label, const std::string& entitlement_label, AddcontEntry* entry)
{
	std::lock_guard lock(g_mutex);
	const Record*   record = FindRecord(service_label, entitlement_label);
	if (record == nullptr)
	{
		return false;
	}
	*entry = record->entry;
	return true;
}

AddcontMountResult AddcontInventoryMount(uint32_t service_label, const std::string& entitlement_label,
                                         std::array<char, ADDCONT_MOUNT_POINT_SIZE>* mount_point)
{
	std::lock_guard lock(g_mutex);
	Record*         record = FindRecord(service_label, entitlement_label);
	if (record == nullptr || !IsMountable(*record))
	{
		return AddcontMountResult::NotFound;
	}
	if (record->mounted)
	{
		return AddcontMountResult::Busy;
	}
	if (!record->host_folder.empty())
	{
		Kernel::FileSystem::Mount(String::FromUtf8(record->host_folder.c_str()), String::FromUtf8(record->guest_mount_point.c_str()));
	}
	record->mounted = true;
	mount_point->fill('\0');
	std::memcpy(mount_point->data(), record->guest_mount_point.data(), record->guest_mount_point.size());
	KYTY_LOG_INFO("add-on content '%s' mounted at %s\n", entitlement_label.c_str(), record->guest_mount_point.c_str());
	return AddcontMountResult::Mounted;
}

bool AddcontInventoryUnmount(const std::string& mount_point)
{
	std::lock_guard lock(g_mutex);
	for (auto& record: g_inventory.records)
	{
		if (!record.mounted || record.guest_mount_point != mount_point)
		{
			continue;
		}
		if (!record.host_folder.empty())
		{
			Kernel::FileSystem::Umount(String::FromUtf8(record.guest_mount_point.c_str()));
		}
		record.mounted = false;
		return true;
	}
	return false;
}

bool AddcontReadGuestLabel(const char* data, std::string* label)
{
	if (data == nullptr)
	{
		return false;
	}
	const size_t size = strnlen(data, ADDCONT_LABEL_SIZE + 1);
	if (size == 0 || size > ADDCONT_LABEL_SIZE)
	{
		return false;
	}
	label->assign(data, size);
	return true;
}

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED
