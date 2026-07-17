#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_MISSINGIMPORTSTUBS_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_MISSINGIMPORTSTUBS_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader::MissingImport {

// Fixed capacity for statically compiled stubs (Rosetta-safe). Exhaustion aborts;
// there is never an anonymous shared fallback.
constexpr int kMaxSlots = 2048;

struct SlotInfo
{
	const char* name                  = nullptr;
	const char* library               = nullptr;
	int         library_version       = 0;
	const char* module                = nullptr;
	int         module_version_major  = 0;
	int         module_version_minor  = 0;
	SymbolType  type                  = SymbolType::Func;
	uint64_t    call_count            = 0;
	uint64_t    last_args[6]          = {};
	uint64_t    vaddr                 = 0;
};

// Resolve a missing Func import to a dedicated stub address. Same SymbolResolve
// identity always returns the same address. Non-Func types must not call this.
// On capacity exhaustion, aborts the process (never returns a shared stub).
// Requires BringUp::AllowMissingFunctionImport(); caller must gate.
uint64_t Assign(const SymbolResolve& sr);

// Diagnostics / tests.
[[nodiscard]] int      UsedSlots();
[[nodiscard]] bool     FindByIdentity(const SymbolResolve& sr, SlotInfo* out);
[[nodiscard]] uint64_t TotalCalls();

// Test helper: clear slots (integration binary only).
void ResetForTests();

} // namespace Kyty::Loader::MissingImport

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_MISSINGIMPORTSTUBS_H_ */
