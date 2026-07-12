#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATA_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATA_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::SaveData {

struct SaveDataMountPoint
{
	char data[16];
};

struct SaveDataMountInfo
{
	uint64_t blocks;
	uint64_t free_blocks;
	uint8_t  reserved[32];
};

// sceSaveDataDirNameSearch condition / result (public PS4/PS5 ABI layout).
// NID dyIhnXq-0SM on SaveData / SaveData_native.
struct SaveDataDirName
{
	char data[32];
};

struct SaveDataTitleId
{
	char data[10];
	char pad[6];
};

struct SaveDataDirNameSearchCond
{
	int32_t                  user_id;
	int32_t                  pad;
	const SaveDataTitleId*   title_id;
	const SaveDataDirName*   dir_name;
	uint32_t                 key;
	uint32_t                 order;
	uint8_t                  reserved[32];
};

struct SaveDataSearchInfo
{
	uint64_t blocks;
	uint64_t free_blocks;
	uint8_t  reserved[32];
};

struct SaveDataDirNameSearchResult
{
	uint32_t            hit_num;
	int32_t             pad;
	SaveDataDirName*    dir_names;
	uint32_t            dir_names_num;
	uint32_t            set_num;
	void*               params;
	SaveDataSearchInfo* infos;
	uint8_t             reserved[12];
	int32_t             pad2;
};

// Gen5 call site: mov edi, user_id; call; test eax,eax; js error; store eax as resource id.
// Positive return is the resource handle (not 0/OK).
int KYTY_SYSV_ABI SaveDataCreateTransactionResource(int32_t user_id);
int KYTY_SYSV_ABI SaveDataGetMountInfo(const SaveDataMountPoint* mount_point, SaveDataMountInfo* info);
int KYTY_SYSV_ABI SaveDataGetEventResult(const void* event_param, void* event);
int KYTY_SYSV_ABI SaveDataDeleteTransactionResource(int32_t resource, const SaveDataMountPoint* mount_point);
int KYTY_SYSV_ABI SaveDataDirNameSearch(const SaveDataDirNameSearchCond* cond, SaveDataDirNameSearchResult* result);

} // namespace Kyty::Libs::SaveData

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATA_H_
