#ifndef INCLUDE_KYTY_CORE_BRINGUP_H_
#define INCLUDE_KYTY_CORE_BRINGUP_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DomainResult.h"

#include <cstdint>

namespace Kyty::Core::BringUp {

enum class Mode : uint8_t
{
	Strict = 0,
	Unsafe = 1,
};

enum class Feature : uint8_t
{
	NotImplemented           = 0,
	MissingFunctionImport    = 1,
	GraphicsPermissive       = 2,
	// Explicit adjacent PRX/shared-module discovery (plan-before-mutate).
	// Off in strict and not part of default unsafe until proven safe.
	AdjacentModuleDiscovery  = 3,
};

enum class Subsystem : uint8_t
{
	Core     = 0,
	Loader   = 1,
	Kernel   = 2,
	Graphics = 3,
	Audio    = 4,
	Network  = 5,
	Hle      = 6,
	Other    = 7,
};

enum class Decision : uint8_t
{
	Halt     = 0,
	Continue = 1,
};

// Fixed table sizes for diagnostics snapshots (match enum ranges).
constexpr uint32_t kFeatureCount    = 4;
constexpr uint32_t kSubsystemCount  = 8;

struct Config
{
	Mode     mode                  = Mode::Strict;
	uint32_t feature_mask          = 0;
	uint32_t subsystem_mask        = 0;
	uint32_t burst_limit           = 10'000;
	uint32_t burst_window_ms       = 1'000;
	bool     explicitly_configured = false;
};

// Immutable diagnostics snapshot (copy of counters + last breaker + effective config).
struct Diagnostics
{
	Config   config {};
	uint64_t unique_sites          = 0;
	uint64_t total_continuations   = 0;
	uint64_t total_halts           = 0;
	uint64_t breaker_trips         = 0;
	uint64_t table_overflows       = 0;
	uint64_t config_rejections     = 0;
	uint64_t last_breaker_key      = 0;
	Feature  last_breaker_feature  = Feature::NotImplemented;
	Subsystem last_breaker_subsystem = Subsystem::Other;
	// Per-feature / per-subsystem decision counters (Continue vs Halt).
	uint64_t continues_by_feature[kFeatureCount]       = {};
	uint64_t halts_by_feature[kFeatureCount]           = {};
	uint64_t continues_by_subsystem[kSubsystemCount]   = {};
	uint64_t halts_by_subsystem[kSubsystemCount]       = {};
};

struct ConfigError
{
	char message[256] = {};
};

// Pure env snapshot for validation (no getenv inside the pure validator).
struct EnvView
{
	const char* mode            = nullptr;
	const char* features        = nullptr;
	const char* subsystems      = nullptr;
	const char* burst_limit     = nullptr;
	const char* burst_window_ms = nullptr;
	const char* stub_missing    = nullptr;
	const char* gfx_permissive  = nullptr;
};

struct ConfigValidation
{
	Domain::ValidationResult result {};
	Config                   config {};
};

// Pure validate: no log, mutate, or terminate.
[[nodiscard]] ConfigValidation ValidateConfig(const EnvView& env) noexcept;

// Apply validated config once. Fail-closed; never invents silent strict after bad parse.
[[nodiscard]] bool InitializeFromEnvironment(ConfigError* error) noexcept;

// Immutable effective config (fail-closed if init failed).
[[nodiscard]] Config GetConfig() noexcept;

// Bounded counters + copy of effective config.
[[nodiscard]] Diagnostics GetDiagnostics() noexcept;

[[nodiscard]] Subsystem ClassifySourceFile(const char* file) noexcept;
[[nodiscard]] bool IsEnabled(Feature feature, Subsystem subsystem) noexcept;

// Sole authority for unsupported-runtime continuation decisions.
[[nodiscard]] Decision Report(Feature feature, Subsystem subsystem, const char* identity, const char* file,
                              int line) noexcept;
[[nodiscard]] Decision ReportNotImplemented(const char* expression, const char* file, int line) noexcept;

// Optional observer hook (agent diagnostics). Must not wake guest sync or
// change the Report decision. Called after counters are updated.
// breaker=true when Halt was caused by burst circuit-break.
using DecisionHook = void (*)(Feature feature, Subsystem subsystem, Decision decision, const char* identity,
                              bool breaker) noexcept;
void SetDecisionHook(DecisionHook hook) noexcept;

} // namespace Kyty::Core::BringUp

#endif /* INCLUDE_KYTY_CORE_BRINGUP_H_ */
