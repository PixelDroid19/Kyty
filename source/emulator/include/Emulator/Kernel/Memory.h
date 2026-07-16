#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::Memory {

KYTY_SUBSYSTEM_DEFINE(Memory);

using callback_func_t = void (*)(uintptr_t addr, size_t size);

void RegisterCallbacks(callback_func_t alloc_func, callback_func_t free_func);

// Decode Orbis/PS5 mprotect/mmap protection bits into host VirtualMemory mode
// and GpuMemoryMode. Returns false for unsupported prot values.
bool KernelDecodeMprotectProt(int prot, Core::VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode);

int KYTY_SYSV_ABI    KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags, const char* name);
int KYTY_SYSV_ABI    KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags);
int KYTY_SYSV_ABI    KernelMunmap(uint64_t vaddr, size_t len);
size_t KYTY_SYSV_ABI KernelGetDirectMemorySize();
int KYTY_SYSV_ABI    KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type,
                                                int64_t* phys_addr_out);
int KYTY_SYSV_ABI    KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type, int64_t* phys_addr_out);
int KYTY_SYSV_ABI    KernelReleaseDirectMemory(int64_t start, size_t len);
int KYTY_SYSV_ABI    KernelCheckedReleaseDirectMemory(int64_t start, size_t len);
int KYTY_SYSV_ABI    KernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment);
// Gen5: type arg recorded by titles; mapping uses the same path as MapDirectMemory.
int KYTY_SYSV_ABI    KernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags, int64_t direct_memory_start,
                                            size_t alignment);
int KYTY_SYSV_ABI    KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags, off_t direct_memory_start, size_t alignment,
                                                const char* name);
// Diagnostic name tag only; does not change mapping rights.
int KYTY_SYSV_ABI    KernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name);
int KYTY_SYSV_ABI    KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);
int KYTY_SYSV_ABI    KernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size);
int KYTY_SYSV_ABI    KernelAvailableFlexibleMemorySize(size_t* size);
// Configured flexible size for Gen5 queries (same budget as available for now).
int KYTY_SYSV_ABI    KernelConfiguredFlexibleMemorySize(uint64_t* size);
int KYTY_SYSV_ABI    KernelMprotect(const void* addr, size_t len, int prot);

} // namespace Kyty::Libs::LibKernel::Memory

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_ */
