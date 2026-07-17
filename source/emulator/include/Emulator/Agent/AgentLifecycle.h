#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTLIFECYCLE_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTLIFECYCLE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/BringUp.h"

#include "Emulator/Agent/EventRing.h"
#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent::Lifecycle {

// Stable machine-readable codes for lifecycle seams (EventRecord.code).
// Keep ≤31 chars. Messages must already be sanitized (no private host paths).
inline constexpr const char* kCodeStartupConfig         = "startup_config";
inline constexpr const char* kCodeExecutableDiscovered  = "executable_discovered";
inline constexpr const char* kCodeModuleDiscovery       = "module_discovery";
inline constexpr const char* kCodeModuleLoaded          = "module_loaded";
inline constexpr const char* kCodeSymbolResolved        = "symbol_resolved";
inline constexpr const char* kCodeMissingImport         = "missing_import";
inline constexpr const char* kCodeRelocationFailure     = "relocation_failure";
inline constexpr const char* kCodeBringUpContinue       = "bringup_continue";
inline constexpr const char* kCodeBringUpHalt           = "bringup_halt";
inline constexpr const char* kCodeBringUpBreaker        = "bringup_breaker";
inline constexpr const char* kCodeGraphicsInit          = "graphics_init";
inline constexpr const char* kCodeGraphicsStencilFrontier = "gfx_stencil_frontier";
inline constexpr const char* kCodeGraphicsStorageFrontier = "gfx_storage_frontier";
inline constexpr const char* kCodeFirstFrame            = "first_frame";
inline constexpr const char* kCodeFirstPresent          = "first_present";
inline constexpr const char* kCodeInputReady            = "input_ready";
inline constexpr const char* kCodeGuestExit             = "guest_exit";
inline constexpr const char* kCodeHostCrash             = "host_crash";

struct StencilFrontierContext
{
	bool stencil_enable     = false;
	bool clear_enable       = false;
	bool htile              = false;
	bool depth_decompress   = false;
	bool stencil_decompress = false;
	bool resummarize        = false;
	bool copy_centroid      = false;
	uint8_t copy_sample     = 0;
	bool read_only          = false;
	bool read_base_present  = false;
	bool write_base_present = false;
};

enum class StorageAccessClass: uint8_t
{
	Unknown,
	Raw,
	Typed,
	Mixed,
};

enum class StorageBindingSource: uint8_t
{
	Direct,
	Metadata,
};

enum class StorageUnknownReason: uint8_t
{
	None,
	CodeUnavailable,
	NoMatchingInstruction,
	RegisterBaseMismatch,
	MetadataOnlyBinding,
};

struct StorageFrontierContext
{
	StorageAccessClass   access         = StorageAccessClass::Unknown;
	StorageBindingSource source         = StorageBindingSource::Direct;
	StorageUnknownReason unknown_reason = StorageUnknownReason::None;
	bool                 code_available = false;
	bool                 exact_match    = false;
	bool                 unbased_match  = false;
	bool                 decoded_unknown = false;
	bool                 indirect_use   = false;
	int                  resource_index = 0;
	int                  sgpr           = 0;
	int                  slot           = 0;
	uint32_t             usage          = 0;
	uint32_t             stride         = 0;
	uint32_t             format         = 0;
	uint32_t             dst_sel        = 0;
	bool                 add_tid        = false;
	bool                 swizzle        = false;
};

// Read-only publish edge: never wakes guest sync or mutates execution.
// Sanitizes message (strips absolute host path prefixes) before ring push.
void Emit(EventKind kind, const char* code, const char* message);

// Convenience wrappers for common lifecycle seams.
void EmitStartupConfig(const char* mode_name, bool explicitly_configured);
void EmitExecutableDiscovered(const char* sanitized_relative_or_basename);
void EmitModuleDiscovery(uint32_t entry_count, uint32_t adjacent_count, uint32_t rejections);
void EmitModuleLoaded(const char* sanitized_relative_key);
void EmitMissingImport(const char* sanitized_identity);
// Successful HLE or loaded-module export resolve (not missing-import stubs).
// source is "hle" or "export". Identity must already be sanitized.
void EmitSymbolResolved(const char* sanitized_identity, const char* source);
void EmitRelocationFailure(const char* sanitized_symbol);
void EmitGraphicsInit();
void EmitStencilFrontier(const StencilFrontierContext& context);
void EmitStorageFrontier(const StorageFrontierContext& context);
void EmitStorageFrontierFatal(const StorageFrontierContext& context);
void EmitFirstFrame(int frame);
void EmitFirstPresent(uint64_t present);
void EmitInputReady();
void EmitGuestExit(int status);
void EmitHostCrash(const char* sanitized_hint);

// Maps synchronous host-fault hook codes to lifecycle event codes.
// Intentional unsupported halts are not host crashes.
const char* ClassifyHostFaultCode(const char* host_fault_code);

// Register BringUp decision hook + other one-time install. Safe to call once.
void InstallHooks();

// Classify a coarse frontier label for status (current state, not history).
// Values: "none" | "unsupported" | "stall" | "launch" | "graphics" | "interactive" | "error"
const char* ClassifyFrontier(const char* phase_name, bool has_last_error, const char* last_error_code,
                             bool graphic_ready, uint64_t present);

} // namespace Kyty::Emulator::Agent::Lifecycle

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTLIFECYCLE_H_ */
