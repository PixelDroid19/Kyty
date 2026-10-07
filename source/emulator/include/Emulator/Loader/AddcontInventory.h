#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_ADDCONTINVENTORY_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_ADDCONTINVENTORY_H_

#include "Emulator/Common.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#ifdef KYTY_EMU_ENABLED

// Locally installed add-on content (DLC) of the running title. AppContent and
// NpEntitlementAccess both answer from this one inventory, so they can never
// disagree. Two sources are read:
//  - <app0>/dlc_emu.ini, the inventory a PS5 dump tool writes next to the
//    application: [PSAC] (content with data) and [PSAL] (license only)
//    records with content_id, download_status, mount_point (a directory under
//    /app0/), entitlement_key and np_service_label.
//  - <packages>/<TITLE_ID>/<package>/, one directory per installed add-on
//    package whose sce_sys/param.json declares its contentId; the package is
//    mounted at /addcontN while the title holds it mounted.
// The entitlement label is the last 16 characters of the content id.
namespace Kyty::Loader {

enum class AddcontPackageType : uint32_t
{
	Psac = 2, // add-on content with data
	Psal = 3, // license only
};

constexpr uint32_t ADDCONT_DOWNLOAD_STATUS_NO_EXTRA_DATA = 0;
constexpr uint32_t ADDCONT_DOWNLOAD_STATUS_INSTALLED     = 4;
constexpr size_t   ADDCONT_LABEL_SIZE                    = 16;
constexpr size_t   ADDCONT_MOUNT_POINT_SIZE              = 16;

struct AddcontEntry
{
	int64_t                 service_label = -1; // -1 matches every service label
	std::string             entitlement_label;
	AddcontPackageType      package_type    = AddcontPackageType::Psal;
	uint32_t                download_status = ADDCONT_DOWNLOAD_STATUS_INSTALLED;
	std::array<uint8_t, 16> entitlement_key {};
};

enum class AddcontInventoryState
{
	None,    // no add-on content is installed
	Ready,
	Invalid, // a source exists but failed validation; never reported as "no add-on content"
};

enum class AddcontMountResult
{
	Mounted,
	NotFound,
	Busy,
};

// Parses both sources once. packages_root may be empty.
void                      AddcontInventoryLoad(const std::string& app0_root, const std::string& packages_root, const std::string& title_id);
AddcontInventoryState     AddcontInventoryGetState();
std::vector<AddcontEntry> AddcontInventoryList(uint32_t service_label);
bool                      AddcontInventoryFind(uint32_t service_label, const std::string& entitlement_label, AddcontEntry* entry);
AddcontMountResult        AddcontInventoryMount(uint32_t service_label, const std::string& entitlement_label,
                                                std::array<char, ADDCONT_MOUNT_POINT_SIZE>* mount_point);
bool                      AddcontInventoryUnmount(const std::string& mount_point);

// Reads a guest SceNpUnifiedEntitlementLabel (17 bytes, NUL terminated).
bool AddcontReadGuestLabel(const char* data, std::string* label);

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_ADDCONTINVENTORY_H_ */
