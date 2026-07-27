#pragma once

#include "Emulator/Common.h"

#include <cstdint>

namespace Kyty::Libs::LibKernel {

#pragma pack(push, 1)

struct ModuleInfoForUnwind
{
	uint64_t st_size;
	char     name[256];
	uint64_t eh_frame_hdr_addr;
	uint64_t eh_frame_addr;
	uint64_t eh_frame_size;
	uint64_t seg0_addr;
	uint64_t seg0_size;
};

#pragma pack(pop)

static_assert(sizeof(ModuleInfoForUnwind) == 0x130);

int KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info);

} // namespace Kyty::Libs::LibKernel
