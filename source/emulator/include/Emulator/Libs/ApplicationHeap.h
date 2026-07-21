#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_APPLICATIONHEAP_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_APPLICATIONHEAP_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

#include <cstddef>

namespace Kyty::Libs::LibKernel::ApplicationHeap {

constexpr size_t kApiSlotCount         = 10;
constexpr size_t kMallocSlot           = 0;
constexpr size_t kFreeSlot             = 1;
constexpr size_t kPosixMemalignSlot    = 6;
// The replacement table follows the Gen5 order used by libkernel's
// MallocReplace record: malloc_stats is slot 7 and malloc_stats_fast is slot 8.
constexpr size_t kMallocStatsFastSlot = 8;

struct Api
{
	void* slots[kApiSlotCount];
};

[[nodiscard]] bool IsValidApi(const Api* api);

// The runtime linker supplies a direct function table. Registration does not
// execute any slot; libc owns construction and publishes the table when ready.
void RegisterApi(void* const api[kApiSlotCount]);

[[nodiscard]] bool IsInitialized();
[[nodiscard]] bool HasAllocator();
[[nodiscard]] bool HasMallocStatsFast();
[[nodiscard]] void* Malloc(size_t size);
int                 MallocStatsFast(void* stats);
bool                Free(void* ptr);

void Reset();

} // namespace Kyty::Libs::LibKernel::ApplicationHeap

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_APPLICATIONHEAP_H_ */
