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

int KYTY_SYSV_ABI SaveDataCreateTransactionResource(int32_t user_id);
int KYTY_SYSV_ABI SaveDataGetMountInfo(const SaveDataMountPoint* mount_point, SaveDataMountInfo* info);
int KYTY_SYSV_ABI SaveDataGetEventResult(const void* event_param, void* event);
int KYTY_SYSV_ABI SaveDataDeleteTransactionResource(int32_t resource, const SaveDataMountPoint* mount_point);

} // namespace Kyty::Libs::SaveData

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATA_H_
