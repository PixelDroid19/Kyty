#ifndef INCLUDE_KYTY_CORE_BRINGUP_H_
#define INCLUDE_KYTY_CORE_BRINGUP_H_

#include "Kyty/Core/Common.h"

#include <cstdint>
#include <cstdio>

namespace Kyty::Core::BringUp {

// Agent / diagnostics protocol version for bring-up snapshot fields.
// Bump when the JSON schema changes.
constexpr int kDiagnosticsProtocolVersion = 2;

enum class Mode : uint8_t
{
	Strict = 0,
	Unsafe = 1,
};

enum class Feature : uint32_t
{
	None                   = 0,
	NotImplemented         = 1u << 0,
	MissingFunctionImport  = 1u << 1,
	GfxPermissive          = 1u << 2,
	// Discover and soft-load neighboring PRX next to the main ELF (unsafe only).
	PrxPreload             = 1u << 3,
	All                    = NotImplemented | MissingFunctionImport | GfxPermissive | PrxPreload,
};

inline Feature operator|(Feature a, Feature b)
{
	return static_cast<Feature>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline Feature operator&(Feature a, Feature b)
{
	return static_cast<Feature>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool Any(Feature f)
{
	return static_cast<uint32_t>(f) != 0;
}

enum class Subsystem : uint32_t
{
	None     = 0,
	Core     = 1u << 0,
	Loader   = 1u << 1,
	Kernel   = 1u << 2,
	Graphics = 1u << 3,
	Audio    = 1u << 4,
	Network  = 1u << 5,
	Hle      = 1u << 6,
	Other    = 1u << 7,
	All      = Core | Loader | Kernel | Graphics | Audio | Network | Hle | Other,
};

inline Subsystem operator|(Subsystem a, Subsystem b)
{
	return static_cast<Subsystem>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline Subsystem operator&(Subsystem a, Subsystem b)
{
	return static_cast<Subsystem>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool Any(Subsystem s)
{
	return static_cast<uint32_t>(s) != 0;
}

enum class Decision : uint8_t
{
	// Strict path: stack, message, ShutdownAll, abort.
	Abort = 0,
	// Unsafe: first or subsequent allowed hit; continue without shutdown.
	Continue = 1,
	// Burst limit exceeded for one site; summary then strict abort.
	CircuitBreak = 2,
};

struct Config
{
	Mode       mode            = Mode::Strict;
	Feature    features        = Feature::None;
	Subsystem  subsystems      = Subsystem::All;
	uint32_t   burst_limit     = 10000;
	uint32_t   burst_window_ms = 1000;
};

struct SiteSnapshot
{
	const char* file           = nullptr;
	int         line           = 0;
	const char* expr           = nullptr;
	Subsystem   subsystem      = Subsystem::Other;
	uint64_t    hit_count      = 0;
	uint64_t    continue_count = 0;
};

struct CircuitBreakSnapshot
{
	bool        active    = false;
	const char* file      = nullptr;
	int         line      = 0;
	const char* expr      = nullptr;
	uint64_t    hits      = 0;
	uint32_t    window_ms = 0;
	uint32_t    limit     = 0;
};

// Read-only diagnostic snapshot. Pointers to site slots are valid for process life.
struct Snapshot
{
	Config              config {};
	uint64_t            unique_sites           = 0;
	uint64_t            total_continuations    = 0;
	uint64_t            missing_import_assigns = 0;
	uint64_t            missing_import_calls   = 0;
	uint32_t            missing_import_slots   = 0;
	uint32_t            prx_preload_discovered = 0;
	uint32_t            prx_preload_loaded     = 0;
	CircuitBreakSnapshot last_circuit_break {};
	// Fixed table of unique sites (may contain empty slots when count < capacity).
	const SiteSnapshot* sites          = nullptr;
	uint32_t            sites_capacity = 0;
};

// Load immutable config from environment exactly once. Unknown/empty/zero/contradictory
// values abort the process immediately (fail closed). Safe to call multiple times;
// subsequent calls are no-ops once loaded.
//
// Environment contract:
//   KYTY_BRINGUP_MODE=unsafe              (absent => Strict)
//   KYTY_BRINGUP_FEATURES=...             (absent in unsafe => all features)
//   KYTY_BRINGUP_SUBSYSTEMS=...           (absent => all)
//   KYTY_BRINGUP_BURST_LIMIT=10000
//   KYTY_BRINGUP_BURST_WINDOW_MS=1000
void InitFromEnvironment();

// Test-only: force a specific config (must be called before first use, or after ResetForTests).
void InitForTests(const Config& config);

// Test-only: clear site registry and re-allow Init*. Not for production.
void ResetForTests();

[[nodiscard]] bool   IsInitialized();
[[nodiscard]] Mode   GetMode();
[[nodiscard]] Config GetConfig();

[[nodiscard]] bool FeatureEnabled(Feature f);
[[nodiscard]] bool SubsystemAllowed(Subsystem s);

// Classify a source path into a subsystem bucket (file may be absolute or relative).
[[nodiscard]] Subsystem ClassifyFile(const char* file);

// Core policy entry for EXIT_NOT_IMPLEMENTED.
// Abort/CircuitBreak => caller must halt; Continue => return without shutdown.
Decision HandleNotImplemented(const char* expr, const char* file, int line);

// Graphics unknown-register skip (replaces KYTY_GFX_PERMISSIVE).
[[nodiscard]] bool AllowGfxPermissive();

// Missing Func import stubs (replaces KYTY_STUB_MISSING).
[[nodiscard]] bool AllowMissingFunctionImport();

// Neighbor / system PRX discovery next to the main program (unsafe only).
[[nodiscard]] bool AllowPrxPreload();

// Linker reports assignment/call metrics (no ownership of strings).
void NoteMissingImportAssigned();
void NoteMissingImportCalled();
void NoteMissingImportSlots(uint32_t used_slots);
void NotePrxPreloadCandidates(uint32_t discovered, uint32_t loaded);

// Fill a read-only snapshot (sites point into internal fixed storage).
void GetSnapshot(Snapshot* out);

// Write agent diagnostics JSON line (protocol version included). Returns bytes written or -1.
int WriteDiagnosticsJson(std::FILE* out);

// Reject leftover legacy env vars with a clear fatal message (called from Init).
void RejectLegacyEnvironment();

} // namespace Kyty::Core::BringUp

#endif /* INCLUDE_KYTY_CORE_BRINGUP_H_ */
