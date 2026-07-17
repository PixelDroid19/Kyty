#ifndef EMULATOR_INCLUDE_EMULATOR_VALIDATION_DOMAINVALIDATORS_H_
#define EMULATOR_INCLUDE_EMULATOR_VALIDATION_DOMAINVALIDATORS_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DomainResult.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Validation {

// Pure validators: inspect input, return typed results. No log / globals / EXIT.

// --- Guest executable path (RuntimeLinker::LoadProgram) ---

struct GuestExecutableRequest
{
	const char* root_path          = nullptr;
	bool        require_eboot_name = false;
};

[[nodiscard]] Core::Domain::ValidationResult ValidateGuestExecutable(const GuestExecutableRequest& req) noexcept;

// --- Module metadata (LoadProgram for .prx/.sprx — name from basename only) ---
//
// Production LoadProgram passes FilenameWithoutDirectory().FilenameWithoutExtension().
// Rules match only fields that path can supply (no fabricated version numbers):
//   empty name            → malformed
//   control characters    → malformed
//   name longer than 64   → unsupported

struct ModuleMetadataRequest
{
	const char* name = nullptr;
};

[[nodiscard]] Core::Domain::ValidationResult ValidateModuleMetadata(const ModuleMetadataRequest& req) noexcept;

// --- Import resolution (RuntimeLinker::Resolve) ---

struct ImportResolveRequest
{
	Loader::SymbolType type                            = Loader::SymbolType::Unknown;
	bool               has_export                      = false;
	bool               missing_function_import_enabled = false;
	const char*        identity                        = nullptr;
};

enum class ImportResolutionOutcome : uint8_t
{
	Resolved                 = 0,
	DiagnosticFunctionStub   = 1,
	StrictUnresolvedFunction = 2,
	UnresolvedNonFunction    = 3,
	Malformed                = 4,
};

struct ImportResolutionDecision
{
	ImportResolutionOutcome        outcome = ImportResolutionOutcome::Malformed;
	Core::Domain::ValidationResult validation {};
};

[[nodiscard]] ImportResolutionDecision ClassifyImportResolution(const ImportResolveRequest& req) noexcept;

// --- Agent request (ParseAndValidateRequestLine) ---

struct AgentRequestDraft
{
	uint64_t    id                = 0;
	const char* tool              = nullptr;
	bool        known_tool        = false;
	bool        requires_button   = false;
	bool        has_string_button = false;
};

[[nodiscard]] Core::Domain::ValidationResult ValidateAgentRequest(const AgentRequestDraft& req) noexcept;

// --- Game-run launch (kyty_mount_func) ---

struct GameRunRequest
{
	const char* guest_root                     = nullptr;
	bool        bringup_env_present            = false;
	bool        allow_diagnostic_override      = false;
	bool        removed_permissive_env_present = false;
};

[[nodiscard]] Core::Domain::ValidationResult ValidateGameRunRequest(const GameRunRequest& req) noexcept;

void FormatErrorLine(const Core::Domain::Error& err, char* buf, std::size_t cap) noexcept;

} // namespace Kyty::Emulator::Validation

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_VALIDATION_DOMAINVALIDATORS_H_ */
