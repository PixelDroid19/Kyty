#ifndef INCLUDE_KYTY_SYS_LINUX_SYSLINUXVIRTUAL_H_
#define INCLUDE_KYTY_SYS_LINUX_SYSLINUXVIRTUAL_H_

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
//#error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/VirtualMemory.h"

namespace Kyty::Core {

void sys_get_system_info(SystemInfo* info);

void sys_virtual_init();

uint64_t sys_virtual_get_page_size();

uint64_t sys_virtual_alloc(uint64_t address, uint64_t size, VirtualMemory::Mode mode);
uint64_t sys_virtual_alloc_aligned(uint64_t address, uint64_t size, VirtualMemory::Mode mode, uint64_t alignment);
bool     sys_virtual_alloc_fixed(uint64_t address, uint64_t size, VirtualMemory::Mode mode);
void*    sys_virtual_create_shared_backing(uint64_t size);
void     sys_virtual_destroy_shared_backing(void* backing);
// Drop host pages for a released direct-memory range without shrinking the
// sparse backing file. Safe only when no live maps still cover the range.
bool     sys_virtual_discard_shared_backing_range(void* backing, uint64_t backing_offset, uint64_t size);
uint64_t sys_virtual_map_shared_aligned(void* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
                                        VirtualMemory::Mode mode, uint64_t alignment);
bool     sys_virtual_map_shared_fixed(void* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
                                      VirtualMemory::Mode mode);
uint64_t sys_virtual_map_shared_fixed_or_relocated(void* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
                                                   VirtualMemory::Mode mode, uint64_t alignment);
bool     sys_virtual_free(uint64_t address);
bool     sys_virtual_protect(uint64_t address, uint64_t size, VirtualMemory::Mode mode, VirtualMemory::Mode* old_mode = nullptr);
VirtualMemory::ProtectionChangeResult sys_virtual_remove_write_and_capture(uint64_t address, uint64_t size,
	                                                                      VirtualMemory::CapturedProtectionVisitor visitor,
	                                                                      void* context) noexcept;
bool     sys_virtual_remove_write_from_protection(uint64_t address, uint64_t size, uint32_t restore_token) noexcept;
bool     sys_virtual_restore_protection(uint64_t address, uint64_t size, uint32_t restore_token) noexcept;
bool     sys_virtual_restore_protection_signal_safe(uint64_t address, uint64_t size, uint32_t restore_token) noexcept;
bool     sys_virtual_protect_write_signal_safe(uint64_t address, uint64_t size);
bool     sys_virtual_flush_instruction_cache(uint64_t address, uint64_t size);
bool     sys_virtual_patch_replace(uint64_t vaddr, uint64_t value);

} // namespace Kyty::Core

#endif

#endif /* INCLUDE_KYTY_SYS_LINUX_SYSLINUXVIRTUAL_H_ */
