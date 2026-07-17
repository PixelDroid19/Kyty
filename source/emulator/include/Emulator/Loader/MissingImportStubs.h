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

// Minimal return taxonomy for discovery stubs. Does not invent NIDs — only
// classifies how a missing Func answers the ABI when the real export is absent.
enum class ReturnClass : uint8_t
{
	// Return 0 (common success / null for many libc-style APIs).
	IntegerZero = 0,
	// Return -1 (errno-style / SCE error placeholder).
	IntegerError = 1,
	// Return nullptr (0) for pointer-producing APIs without fabricating objects.
	NullPointer = 2,
	// Return a real, static callable for function-pointer consumers (vtable /
	// callback slots). Only this class may expose a trampoline address.
	Callable = 3,
};

struct SlotInfo
{
	const char* name                 = nullptr;
	const char* library              = nullptr;
	int         library_version      = 0;
	const char* module               = nullptr;
	int         module_version_major = 0;
	int         module_version_minor = 0;
	SymbolType  type                 = SymbolType::Func;
	ReturnClass ret_class            = ReturnClass::IntegerZero;
	uint64_t    call_count           = 0;
	uint64_t    last_args[6]         = {};
	uint64_t    vaddr                = 0;
};

// Pick a conservative return class from the resolve identity (library/name).
[[nodiscard]] ReturnClass ClassifyReturn(const SymbolResolve& sr);

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
