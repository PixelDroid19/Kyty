#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_APPLICATIONHEAP_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_APPLICATIONHEAP_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

#include <cstddef>
#include <cstdint>

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

// libc startup consumes the declared process-parameter chain, never a scan of
// load segments. A missing replacement is valid. Malformed metadata returns
// false without publishing an allocator; the initializer result is ignored.
[[nodiscard]] bool InitializeProcessHeap(uint64_t process_parameters);
// Why the last InitializeProcessHeap call on this thread returned false: the
// failed record or callback and its decoded fields. Empty after success.
[[nodiscard]] const char* ProcessHeapFailureReason();

[[nodiscard]] bool IsInitialized();
[[nodiscard]] bool HasAllocator();
[[nodiscard]] bool IsAllocatorCallbackActive();
[[nodiscard]] bool HasMallocStatsFast();
[[nodiscard]] void* Malloc(size_t size);
int                 MallocStatsFast(void* stats);
bool                Free(void* ptr);

void Reset();

} // namespace Kyty::Libs::LibKernel::ApplicationHeap

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_APPLICATIONHEAP_H_ */
