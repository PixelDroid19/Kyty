#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_MODULELOAD_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_MODULELOAD_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/GuestPlatform.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

class RuntimeLinker;
struct Program;

// ---------------------------------------------------------------------------
// ModuleLoadPlan — immutable load description produced before linker mutation.
// ---------------------------------------------------------------------------

constexpr uint32_t kModuleLoadPlanMaxEntries    = 32;
constexpr uint32_t kModuleLoadPlanMaxRejections = 16;
constexpr uint32_t kModuleLoadPlanMaxConflicts  = 16;

enum class ModulePlanRole : uint8_t
{
	Primary        = 0,
	AdjacentShared = 1,
};

struct ModulePlanEntry
{
	// Sanitized package-relative key used for deterministic sort (never host abs).
	char relative_key[192] = {};
	// Host path used only for open/load; not published in agent diagnostics.
	char host_path[768] = {};
	// Basename without extension (module identity for duplicate detection).
	char           identity[96] = {};
	ModulePlanRole role         = ModulePlanRole::Primary;
	GuestPlatform  platform     = GuestPlatform::Unknown;
	uint8_t        elf_abi      = 0xff;
};

struct ModuleLoadPlanDiagnostics
{
	bool     discovery_enabled   = false;
	bool     discovery_attempted = false;
	uint32_t entry_count         = 0;
	uint32_t adjacent_count      = 0;
	uint32_t rejection_count     = 0;
	uint32_t applied_count       = 0;
	// Conflicting export symbols (same GenerateName from two sources).
	uint32_t export_conflict_count = 0;
	GuestPlatform detected_platform = GuestPlatform::Unknown;
	GuestPlatform metadata_platform = GuestPlatform::Unknown;
	uint8_t       elf_abi           = 0xff;
	bool          platform_conflict = false;
	// Sanitized relative keys only (no host paths / title IDs).
	char entries[kModuleLoadPlanMaxEntries][192]       = {};
	char rejections[kModuleLoadPlanMaxRejections][160] = {};
	// Short sanitized previews remain wire-compatible with Agent diagnostics.
	char export_conflicts[kModuleLoadPlanMaxConflicts][192] = {};
	// Full sanitized identities own the exact diagnostic and prevent long-name
	// collisions during deduplication; never serialize host paths here.
	Core::String export_conflict_identities[kModuleLoadPlanMaxConflicts] {};
	char         primary_identity[96] = {};
};

struct ModuleLoadPlan
{
	ModulePlanEntry           entries[kModuleLoadPlanMaxEntries] {};
	uint32_t                  count = 0;
	ModuleLoadPlanDiagnostics diag {};
	bool                      valid      = false;
	char                      error[192] = {};
};

// --- GuestExecutableLocator -------------------------------------------------
namespace GuestExecutableLocator {

[[nodiscard]] bool         IsPrimaryExecutableName(const Core::String& file_name);
[[nodiscard]] Core::String PackageRootFromPrimary(const Core::String& primary_host_path);
[[nodiscard]] bool         ValidatePrimaryPath(const Core::String& primary_host_path);

} // namespace GuestExecutableLocator

// --- ModuleDiscovery --------------------------------------------------------
namespace ModuleDiscovery {

[[nodiscard]] bool IsSupportedPackageSubdir(const char* relative_subdir);

struct DiscoveryResult
{
	uint32_t count     = 0;
	bool     truncated = false;
};

[[nodiscard]] DiscoveryResult DiscoverAdjacentCandidates(const Core::String& package_root_host, Core::String* out_host_paths,
                                                         Core::String* out_relative_keys, uint32_t capacity);

} // namespace ModuleDiscovery

// --- ModuleLoadPlan builder -------------------------------------------------
namespace ModuleLoadPlanning {

[[nodiscard]] ModuleLoadPlan BuildPlan(const Core::String& primary_host_path, bool discovery_enabled);

} // namespace ModuleLoadPlanning

// --- ProgramLoader / RelocationEngine / ImportResolver ----------------------
namespace ProgramLoader {
Program* Load(RuntimeLinker* rt, const Core::String& host_path);
void     Unload(RuntimeLinker* rt, Program* program);
} // namespace ProgramLoader

namespace RelocationEngine {
void RelocateAll(RuntimeLinker* rt);
}

namespace ImportResolver {
// Real Resolve path (does not invent addresses). Writes SymbolRecord via RuntimeLinker::Resolve.
void Resolve(RuntimeLinker* rt, const Core::String& name, SymbolType type, Program* program, SymbolRecord* out_info);

// True when the HLE SymbolDatabase owns a non-zero vaddr for the same identity
// that Resolve would use for a missing-or-module export of `sr`.
[[nodiscard]] bool HleOwns(RuntimeLinker* rt, const SymbolResolve& sr);

// After Resolve, true iff out vaddr matches the HLE record.
[[nodiscard]] bool ResolvedMatchesHle(RuntimeLinker* rt, const SymbolResolve& sr, uint64_t resolved_vaddr);
} // namespace ImportResolver

// --- ModuleLifecycleCoordinator ---------------------------------------------
namespace ModuleLifecycleCoordinator {

void                                    PublishDiagnostics(const ModuleLoadPlanDiagnostics& diag);
[[nodiscard]] ModuleLoadPlanDiagnostics GetDiagnostics();

// Scan HLE + all loaded program export tables for duplicate GenerateName keys.
// Appends sanitized notes to *diag (export_conflicts). Returns conflict count found
// in this scan. Does not invent symbols — only compares records already present.
uint32_t DetectExportConflicts(RuntimeLinker* rt, ModuleLoadPlanDiagnostics* diag);

// Batch helper used by deferred application: scan the last entry in `batch` for export
// conflicts against HLE and other loaded programs. On inter-module conflict, unload
// the entire batch (reverse order) and return 0. On success return batch_count.
// HLE conflicts are recorded but do not unload. Does not invent symbols.
// `batch` must contain only programs loaded for this adjacent apply (not primary).
int CommitAdjacentBatchOrRollback(RuntimeLinker* rt, Program** batch, uint32_t batch_count, ModuleLoadPlanDiagnostics* diag);

// Stage during primary loading; never mutates the linker with adjacent modules.
void AfterPrimaryLoaded(RuntimeLinker* rt, const Core::String& primary_host_path);
// Consume the staged plan only after the complete HLE symbol database exists.
void               AfterHleSymbolsRegistered(RuntimeLinker* rt);
[[nodiscard]] bool HasPendingAdjacentPlan(RuntimeLinker* rt);

// Resolves a strict lazy PLT miss from one validated package provider. The
// candidate must declare the requested module identity; a module is never
// selected only because it happens to be adjacent on disk.
[[nodiscard]] bool TryLoadProviderForLazyImport(RuntimeLinker* rt, const Core::String& import_name, SymbolType type);
void               ClearPending(RuntimeLinker* rt);

} // namespace ModuleLifecycleCoordinator

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_MODULELOAD_H_ */
