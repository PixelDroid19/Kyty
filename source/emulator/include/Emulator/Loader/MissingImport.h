#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_MISSINGIMPORT_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_MISSINGIMPORT_H_

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

// Diagnostics published by ImportDiagnostics / MissingImportRegistry.
// - resolution_attempts: every registry Assign path (including duplicate hits)
// - unique_symbols: distinct canonical identities registered
// - used: allocated stub slots (never grows on duplicate resolve)
// - table_overflows: capacity-exhaust events observed before fatal halt
struct MissingImportDiagnostics
{
	uint32_t resolution_attempts = 0;
	uint32_t unique_symbols      = 0;
	uint32_t used                = 0;
	uint32_t table_overflows     = 0;
	uint32_t table_capacity      = 0;
};

namespace MissingImport {

// Hard capacity for statically compiled Func stubs (Rosetta-safe). Exhaustion is
// a typed fatal unsupported outcome; there is never a silent shared fallback.
constexpr uint32_t kCapacity = 2048;

// --- SymbolIdentity -----------------------------------------------------------
// Canonical owned identity for a missing import. All strings are copied at
// construction; callers may drop temporary String/C_Str storage after Resolve.
struct SymbolIdentity
{
	enum class StubAbiCategory
	{
		// The diagnostic stub defines only the integer/pointer return register.
		// SymbolResolve has no signature metadata, so this is not evidence of
		// safety for FP, vector, aggregate, or hidden-sret return classes.
		IntegerOrPointerRegisterZero,
	};

	String     canonical;
	String     name;
	String     library;
	String     module;
	int        library_version      = 0;
	int        module_version_major = 0;
	int        module_version_minor = 0;
	SymbolType type                 = SymbolType::Unknown;
	StubAbiCategory stub_abi        = StubAbiCategory::IntegerOrPointerRegisterZero;

	// Build from SymbolResolve + already-generated canonical name (GenerateName).
	// Copies every field; does not retain pointers into temporary storage.
	static SymbolIdentity From(const SymbolResolve& sr, const String& canonical_name);

	[[nodiscard]] bool MatchesCanonical(const String& other_canonical) const;
};

// --- ImportPolicy -------------------------------------------------------------
// BringUp gate for missing Func stubs. RuntimeLinker remains the coordinator;
// policy only answers whether the feature is allowed and records Report sites.
struct PolicyDecision
{
	bool                    feature_enabled = false;
	Core::BringUp::Decision report          = Core::BringUp::Decision::Halt;
};

// Query IsEnabled + Report for MissingFunctionImport / Loader.
// identity_cstr must remain valid for the duration of the call (owned buffer OK).
[[nodiscard]] PolicyDecision RequestBringUp(const char* identity_cstr);

// --- StubAllocator / Registry / Diagnostics -----------------------------------
// Assign a stable non-zero stub for a missing Func identity, or abort on
// capacity exhaustion / NEW-slot policy Halt. Duplicate identities reuse the
// same address without re-entering ImportPolicy/Report (burst must not unseat
// an already-registered stub). Non-Func must never call this.
//
// Precondition: caller already validated Func + feature enabled via ValidateImport.
// The current symbol metadata cannot identify FP/vector/aggregate/sret returns;
// those ABI classes remain unsupported and are not claimed safe by this API.
// Postcondition: return value != 0 (or process has halted).
[[nodiscard]] uint64_t AssignFuncStubOrAbort(const SymbolIdentity& identity);

[[nodiscard]] MissingImportDiagnostics Snapshot();

} // namespace MissingImport
} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_MISSINGIMPORT_H_ */
