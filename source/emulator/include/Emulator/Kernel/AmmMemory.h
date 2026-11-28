#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_AMM_MEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_AMM_MEMORY_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::Amm {

// The AMM virtual window: one kernel reservation that AMM commands map pages
// into, followed by its multimap range.
struct VirtualRanges
{
	uint64_t start          = 0;
	uint64_t end            = 0;
	uint64_t multimap_start = 0;
	uint64_t multimap_end   = 0;
};

// Reserves the window on first use.
int GetVirtualRanges(VirtualRanges* out);

// Allocates direct memory. Usage 1 hands it to the AMM page pool, usage 0 only
// allocates it for the caller.
int GiveDirectMemory(int64_t search_start, int64_t search_end, uint64_t size, uint64_t alignment, int usage, int64_t* offset);

// PRT page states, run when a command buffer is submitted. Unbacked PRT pages
// stay reserved; backed pages are pool pages mapped in place.
int MapAsPrt(uint64_t address, uint64_t size);
int AllocatePaForPrt(uint64_t address, uint64_t size, int mtype, int protection);
int UnmapToPrt(uint64_t address, uint64_t size);

} // namespace Kyty::Kernel::Amm

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_AMM_MEMORY_H_ */
