#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_APPLICATIONHEAP_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_APPLICATIONHEAP_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

#include <cstdint>

namespace Kyty::Loader {
class Program;
} // namespace Kyty::Loader

namespace Kyty::Libs::LibKernel::ApplicationHeap {

constexpr uint64_t kApiV2Size    = 0x78;
constexpr uint64_t kApiV2Version = 2;

struct ApiV2
{
	uint64_t size;
	uint64_t version;
	void(KYTY_SYSV_ABI* create)();
	void(KYTY_SYSV_ABI* destroy)();
	void*(KYTY_SYSV_ABI* malloc)(size_t size);
	void(KYTY_SYSV_ABI* free)(void* ptr);
};

[[nodiscard]] bool IsApiV2Header(uint64_t size, uint64_t version);

[[nodiscard]] bool IsGuestCodePointer(uint64_t addr, uint64_t text_begin, uint64_t text_end);

// All v2 allocator slots must point into the main image text before create runs.
[[nodiscard]] bool IsValidApiV2Table(const ApiV2* table, uint64_t text_begin, uint64_t text_end);

// Persist the table registered by KernelRtldSetApplicationHeapAPI and invoke the
// evidenced v2 create slot when present.
void RegisterApi(void* api);

// Invoke create on the registered v2 table, or (fallback) locate a fully
// validated table in main-image readable PT_LOAD and invoke its create slot.
// Required when the guest never calls KernelRtldSetApplicationHeapAPI before
// the first application-heap malloc (captured Gen5 startup path).
void EnsureInitialized(Loader::Program* program);

[[nodiscard]] bool IsInitialized();

[[nodiscard]] bool HasAllocator();
[[nodiscard]] void* Malloc(size_t size);
bool                Free(void* ptr);

void Reset();

} // namespace Kyty::Libs::LibKernel::ApplicationHeap

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_APPLICATIONHEAP_H_ */
