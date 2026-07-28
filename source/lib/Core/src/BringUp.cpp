#include "Kyty/Core/BringUp.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

namespace Kyty::Core::BringUp {

namespace {

struct NamedFeature
{
	const char* name;
	Feature     value;
};

constexpr NamedFeature kFeatures[] = {
    {"not_implemented", Feature::NotImplemented},
    {"missing_function_import", Feature::MissingFunctionImport},
    {"gfx_permissive", Feature::GraphicsPermissive},
    {"adjacent_module_discovery", Feature::AdjacentModuleDiscovery},
};

struct NamedSubsystem
{
	const char* name;
	Subsystem   value;
};

constexpr NamedSubsystem kSubsystems[] = {
    {"core", Subsystem::Core},
    {"loader", Subsystem::Loader},
    {"kernel", Subsystem::Kernel},
    {"graphics", Subsystem::Graphics},
    {"audio", Subsystem::Audio},
    {"network", Subsystem::Network},
    {"hle", Subsystem::Hle},
    {"other", Subsystem::Other},
};

constexpr uint32_t kSiteCapacity = 4096;

constexpr uint32_t kFeatureBitNotImplemented          = 1u << static_cast<uint32_t>(Feature::NotImplemented);
constexpr uint32_t kFeatureBitMissingFunctionImport   = 1u << static_cast<uint32_t>(Feature::MissingFunctionImport);
constexpr uint32_t kFeatureBitGraphicsPermissive      = 1u << static_cast<uint32_t>(Feature::GraphicsPermissive);
constexpr uint32_t kFeatureBitAdjacentModuleDiscovery = 1u << static_cast<uint32_t>(Feature::AdjacentModuleDiscovery);
// Suppress unused warning for the explicit bit constant (mask built via ParseTokenList / tests).
static_assert(kFeatureBitAdjacentModuleDiscovery != 0);

constexpr uint32_t kSubsystemBitCore     = 1u << static_cast<uint32_t>(Subsystem::Core);
constexpr uint32_t kSubsystemBitLoader   = 1u << static_cast<uint32_t>(Subsystem::Loader);
constexpr uint32_t kSubsystemBitKernel   = 1u << static_cast<uint32_t>(Subsystem::Kernel);
constexpr uint32_t kSubsystemBitGraphics = 1u << static_cast<uint32_t>(Subsystem::Graphics);
constexpr uint32_t kSubsystemBitAudio    = 1u << static_cast<uint32_t>(Subsystem::Audio);
constexpr uint32_t kSubsystemBitNetwork  = 1u << static_cast<uint32_t>(Subsystem::Network);
constexpr uint32_t kSubsystemBitHle      = 1u << static_cast<uint32_t>(Subsystem::Hle);
constexpr uint32_t kSubsystemBitOther    = 1u << static_cast<uint32_t>(Subsystem::Other);

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime      = 1099511628211ULL;

constexpr Config kDefaultUnsafeConfig = {
	Mode::Unsafe,
	kFeatureBitNotImplemented | kFeatureBitMissingFunctionImport | kFeatureBitGraphicsPermissive,
	kSubsystemBitCore | kSubsystemBitLoader | kSubsystemBitKernel | kSubsystemBitGraphics | kSubsystemBitAudio |
	    kSubsystemBitNetwork | kSubsystemBitHle | kSubsystemBitOther,
	10000,
	1000,
	true,
};

constexpr Config kDefaultStrictConfig {
	Mode::Strict,
	0,
	0,
	10000,
	1000,
	false,
};

struct SiteEntry
{
	uint64_t key             = 0;
	uint64_t total_hits      = 0;
	uint64_t window_start_ms = 0;
	uint32_t window_hits     = 0;
	bool     first_logged    = false;
};

std::once_flag         g_init_once;
bool                   g_init_ok = false;
Config                 g_config {};
ConfigError            g_init_error {};
std::mutex             g_sites_mutex;

std::atomic<uint64_t> g_unique_sites {0};
std::atomic<uint64_t> g_total_continuations {0};
std::atomic<uint64_t> g_total_halts {0};
std::atomic<uint64_t> g_breaker_trips {0};
std::atomic<uint64_t> g_table_overflows {0};
std::atomic<uint64_t> g_config_rejections {0};
std::atomic<uint64_t> g_last_breaker_key {0};
std::atomic<uint32_t> g_last_breaker_feature {static_cast<uint32_t>(Feature::NotImplemented)};
std::atomic<uint32_t> g_last_breaker_subsystem {static_cast<uint32_t>(Subsystem::Other)};
std::atomic_bool      g_breaker_detail_logged {false};

std::atomic<uint64_t> g_continues_by_feature[kFeatureCount] {};
std::atomic<uint64_t> g_halts_by_feature[kFeatureCount] {};
std::atomic<uint64_t> g_continues_by_subsystem[kSubsystemCount] {};
std::atomic<uint64_t> g_halts_by_subsystem[kSubsystemCount] {};

// Optional agent observer; never changes the decision.
std::atomic<DecisionHook> g_decision_hook {nullptr};

SiteEntry g_sites[kSiteCapacity];

void InvokeDecisionHook(Feature feature, Subsystem subsystem, Decision decision, const char* identity,
                        bool breaker) noexcept
{
	const DecisionHook hook = g_decision_hook.load(std::memory_order_acquire);
	if (hook != nullptr)
	{
		hook(feature, subsystem, decision, identity, breaker);
	}
}

uint64_t SteadyMs()
{
	using clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

// Preserve empty strings: a set-but-empty variable is invalid config, not "unset".
// (Coercing "" → nullptr would silently rewrite invalid config into strict defaults.)
const char* EnvRaw(const char* name)
{
	return std::getenv(name);
}

bool EnvIsEmpty(const char* value) noexcept
{
	return value != nullptr && value[0] == '\0';
}

void CopyText(char* out, std::size_t out_size, const char* message)
{
	if (out == nullptr || out_size == 0)
	{
		return;
	}
	if (message == nullptr)
	{
		out[0] = '\0';
		return;
	}
	std::size_t idx = 0;
	for (; idx + 1 < out_size && message[idx] != '\0'; ++idx)
	{
		out[idx] = message[idx];
	}
	out[idx] = '\0';
}

void SetInitError(const char* message)
{
	CopyText(g_init_error.message, sizeof(g_init_error.message), message);
}

uint64_t HashAppend(uint64_t h, uint32_t value)
{
	h ^= static_cast<uint64_t>(value);
	h *= kFnvPrime;
	return h;
}

uint64_t HashAppend(uint64_t h, const char* value)
{
	if (value == nullptr)
	{
		return h;
	}
	for (const char* p = value; *p != '\0'; ++p)
	{
		h ^= static_cast<uint8_t>(*p);
		h *= kFnvPrime;
	}
	return h;
}

uint64_t ComputeKey(Feature feature, Subsystem subsystem, const char* identity, const char* file, int line)
{
	uint64_t h = kFnvOffsetBasis;
	h           = HashAppend(h, static_cast<uint32_t>(feature));
	h           = HashAppend(h, static_cast<uint32_t>(subsystem));
	h           = HashAppend(h, identity);
	h           = HashAppend(h, file);
	h           = HashAppend(h, static_cast<uint32_t>(line));
	return h;
}

bool HasWhitespace(const char* start, const char* end)
{
	for (const char* p = start; p < end; ++p)
	{
		if (std::isspace(static_cast<unsigned char>(*p)) != 0)
		{
			return true;
		}
	}
	return false;
}

// Pure token-list rule: no logging, no globals.
Domain::ValidationResult ParseTokenList(const char* value, uint32_t& out_mask, bool is_feature) noexcept
{
	const char* op = is_feature ? "parse_features" : "parse_subsystems";
	if (value == nullptr || value[0] == '\0')
	{
		return Domain::Fail("malformed", "core", op, "list is empty");
	}

	uint32_t    mask = 0;
	const char* p    = value;
	const char* end  = p + std::strlen(value);
	while (p < end)
	{
		const char* token_start = p;
		while (p < end && *p != ',')
		{
			++p;
		}
		if (token_start == p)
		{
			return Domain::Fail("malformed", "core", op, "list contains empty element");
		}
		if (HasWhitespace(token_start, p))
		{
			return Domain::Fail("malformed", "core", op, "list contains whitespace");
		}

		bool     found = false;
		uint32_t bit   = 0;
		if (is_feature)
		{
			for (const auto& f: kFeatures)
			{
				const auto len = std::char_traits<char>::length(f.name);
				if (static_cast<std::size_t>(p - token_start) == len && std::strncmp(f.name, token_start, len) == 0)
				{
					bit   = 1u << static_cast<uint32_t>(f.value);
					found = true;
					break;
				}
			}
		} else
		{
			for (const auto& s: kSubsystems)
			{
				const auto len = std::char_traits<char>::length(s.name);
				if (static_cast<std::size_t>(p - token_start) == len && std::strncmp(s.name, token_start, len) == 0)
				{
					bit   = 1u << static_cast<uint32_t>(s.value);
					found = true;
					break;
				}
			}
		}
		if (!found)
		{
			return Domain::Fail("unsupported", "core", op, "list contains unknown token");
		}
		if ((mask & bit) != 0)
		{
			return Domain::Fail("malformed", "core", op, "list contains duplicate entry");
		}
		mask |= bit;

		if (p < end && *p == ',')
		{
			++p;
			if (p == end)
			{
				return Domain::Fail("malformed", "core", op, "list contains empty element");
			}
		}
	}

	if (mask == 0)
	{
		return Domain::Fail("malformed", "core", op, "list is empty");
	}
	out_mask = mask;
	return Domain::Ok();
}

bool ParsePositiveDecimal(const char* value, uint32_t* out) noexcept
{
	if (value == nullptr || out == nullptr || *value == '\0')
	{
		return false;
	}
	uint64_t accumulator = 0;
	for (const char* p = value; *p != '\0'; ++p)
	{
		if (*p < '0' || *p > '9')
		{
			return false;
		}
		accumulator = accumulator * 10ull + static_cast<uint64_t>(*p - '0');
		if (accumulator == 0 || accumulator > UINT32_MAX)
		{
			return false;
		}
	}
	*out = static_cast<uint32_t>(accumulator);
	return true;
}

Domain::ValidationResult RuleRejectLegacy(const EnvView& env) noexcept
{
	// Present at all (including empty) is a configuration error — never a live control.
	if (env.stub_missing != nullptr || env.gfx_permissive != nullptr)
	{
		return Domain::Fail("unsupported", "core", "validate_config",
		                    "legacy KYTY_STUB_MISSING/KYTY_GFX_PERMISSIVE removed", "use KYTY_BRINGUP_MODE");
	}
	return Domain::Ok();
}

Domain::ValidationResult RuleRejectEmptyVars(const EnvView& env) noexcept
{
	// Set-but-empty is malformed (fail closed). Do not treat as absent.
	struct Field
	{
		const char* value;
		const char* name;
	};
	const Field fields[] = {
	    {env.mode, "KYTY_BRINGUP_MODE"},
	    {env.features, "KYTY_BRINGUP_FEATURES"},
	    {env.subsystems, "KYTY_BRINGUP_SUBSYSTEMS"},
	    {env.burst_limit, "KYTY_BRINGUP_BURST_LIMIT"},
	    {env.burst_window_ms, "KYTY_BRINGUP_BURST_WINDOW_MS"},
	};
	for (const auto& f: fields)
	{
		if (EnvIsEmpty(f.value))
		{
			return Domain::Fail("malformed", "core", "validate_config", "bring-up variable must not be empty",
			                    f.name);
		}
	}
	return Domain::Ok();
}

Domain::ValidationResult RuleModeAndPolicy(const EnvView& env, Config& config) noexcept
{
	const char* mode = env.mode;
	// Unset (nullptr) only — empty already rejected by RuleRejectEmptyVars.
	if (mode == nullptr || std::strcmp(mode, "strict") == 0)
	{
		config = kDefaultStrictConfig;
		if (env.features != nullptr || env.subsystems != nullptr || env.burst_limit != nullptr ||
		    env.burst_window_ms != nullptr)
		{
			return Domain::Fail("policy_denied", "core", "validate_config",
			                    "KYTY_BRINGUP_* variables are only valid in unsafe mode");
		}
		return Domain::Ok();
	}
	if (std::strcmp(mode, "unsafe") != 0)
	{
		return Domain::Fail("malformed", "core", "validate_config", "KYTY_BRINGUP_MODE must be strict or unsafe",
		                    mode);
	}
	config                       = kDefaultUnsafeConfig;
	config.mode                  = Mode::Unsafe;
	config.explicitly_configured = false;
	return Domain::Ok();
}

Domain::ValidationResult RuleLimitsAndLists(const EnvView& env, Config& config) noexcept
{
	if (config.mode != Mode::Unsafe)
	{
		return Domain::Ok();
	}
	if (env.burst_window_ms != nullptr)
	{
		uint32_t value = 0;
		if (!ParsePositiveDecimal(env.burst_window_ms, &value))
		{
			return Domain::Fail("malformed", "core", "validate_config",
			                    "KYTY_BRINGUP_BURST_WINDOW_MS must be a positive decimal integer");
		}
		config.burst_window_ms = value;
	}
	if (env.burst_limit != nullptr)
	{
		uint32_t value = 0;
		if (!ParsePositiveDecimal(env.burst_limit, &value))
		{
			return Domain::Fail("malformed", "core", "validate_config",
			                    "KYTY_BRINGUP_BURST_LIMIT must be a positive decimal integer");
		}
		config.burst_limit = value;
	}
	if (env.features != nullptr)
	{
		uint32_t mask = 0;
		const auto r  = ParseTokenList(env.features, mask, true);
		if (!r.Ok())
		{
			return r;
		}
		config.feature_mask = mask;
	}
	if (env.subsystems != nullptr)
	{
		uint32_t mask = 0;
		const auto r  = ParseTokenList(env.subsystems, mask, false);
		if (!r.Ok())
		{
			return r;
		}
		config.subsystem_mask = mask;
	}
	config.explicitly_configured =
	    (env.features != nullptr || env.subsystems != nullptr || env.burst_limit != nullptr ||
	     env.burst_window_ms != nullptr);
	return Domain::Ok();
}

void InitializeState()
{
	g_unique_sites.store(0, std::memory_order_relaxed);
	g_total_continuations.store(0, std::memory_order_relaxed);
	g_total_halts.store(0, std::memory_order_relaxed);
	g_breaker_trips.store(0, std::memory_order_relaxed);
	g_table_overflows.store(0, std::memory_order_relaxed);
	// config_rejections is process-lifetime (not cleared on successful init).
	g_last_breaker_key.store(0, std::memory_order_relaxed);
	g_last_breaker_feature.store(static_cast<uint32_t>(Feature::NotImplemented), std::memory_order_relaxed);
	g_last_breaker_subsystem.store(static_cast<uint32_t>(Subsystem::Other), std::memory_order_relaxed);
	g_breaker_detail_logged.store(false, std::memory_order_relaxed);
	for (uint32_t i = 0; i < kFeatureCount; ++i)
	{
		g_continues_by_feature[i].store(0, std::memory_order_relaxed);
		g_halts_by_feature[i].store(0, std::memory_order_relaxed);
	}
	for (uint32_t i = 0; i < kSubsystemCount; ++i)
	{
		g_continues_by_subsystem[i].store(0, std::memory_order_relaxed);
		g_halts_by_subsystem[i].store(0, std::memory_order_relaxed);
	}
	for (uint32_t i = 0; i < kSiteCapacity; ++i)
	{
		g_sites[i] = {};
	}
}

void NoteContinue(Feature feature, Subsystem subsystem) noexcept
{
	const auto fi = static_cast<uint32_t>(feature);
	const auto si = static_cast<uint32_t>(subsystem);
	g_total_continuations.fetch_add(1, std::memory_order_relaxed);
	if (fi < kFeatureCount)
	{
		g_continues_by_feature[fi].fetch_add(1, std::memory_order_relaxed);
	}
	if (si < kSubsystemCount)
	{
		g_continues_by_subsystem[si].fetch_add(1, std::memory_order_relaxed);
	}
}

void NoteHalt(Feature feature, Subsystem subsystem) noexcept
{
	const auto fi = static_cast<uint32_t>(feature);
	const auto si = static_cast<uint32_t>(subsystem);
	g_total_halts.fetch_add(1, std::memory_order_relaxed);
	if (fi < kFeatureCount)
	{
		g_halts_by_feature[fi].fetch_add(1, std::memory_order_relaxed);
	}
	if (si < kSubsystemCount)
	{
		g_halts_by_subsystem[si].fetch_add(1, std::memory_order_relaxed);
	}
}

EnvView EnvViewFromProcess() noexcept
{
	// Raw getenv: empty string stays empty (invalid), nullptr means unset.
	EnvView v {};
	v.mode            = EnvRaw("KYTY_BRINGUP_MODE");
	v.features        = EnvRaw("KYTY_BRINGUP_FEATURES");
	v.subsystems      = EnvRaw("KYTY_BRINGUP_SUBSYSTEMS");
	v.burst_limit     = EnvRaw("KYTY_BRINGUP_BURST_LIMIT");
	v.burst_window_ms = EnvRaw("KYTY_BRINGUP_BURST_WINDOW_MS");
	v.stub_missing    = EnvRaw("KYTY_STUB_MISSING");
	v.gfx_permissive  = EnvRaw("KYTY_GFX_PERMISSIVE");
	return v;
}

} // namespace

ConfigValidation ValidateConfig(const EnvView& env) noexcept
{
	ConfigValidation out {};
	out.config = kDefaultStrictConfig;
	// Short sequence of named pure rules — no nested trees, no side effects.
	out.result = Domain::RunRules([&] { return RuleRejectLegacy(env); },
	                              [&] { return RuleRejectEmptyVars(env); },
	                              [&] { return RuleModeAndPolicy(env, out.config); },
	                              [&] { return RuleLimitsAndLists(env, out.config); });
	return out;
}

namespace {

bool IsFeatureEnabled(const Config& config, Feature feature)
{
	const uint32_t bit = 1u << static_cast<uint32_t>(feature);
	return (config.feature_mask & bit) != 0;
}

bool IsSubsystemEnabled(const Config& config, Subsystem subsystem)
{
	const uint32_t bit = 1u << static_cast<uint32_t>(subsystem);
	return (config.subsystem_mask & bit) != 0;
}

} // namespace

bool InitializeFromEnvironment(ConfigError* error) noexcept
{
	// Mutation only after pure ValidateConfig succeeds.
	std::call_once(g_init_once, [] {
		const ConfigValidation validated = ValidateConfig(EnvViewFromProcess());
		if (!validated.result.Ok())
		{
			g_init_ok = false;
			g_config_rejections.fetch_add(1, std::memory_order_relaxed);
			// Prefer stable reason for ConfigError message bag.
			SetInitError(validated.result.error.reason[0] != '\0' ? validated.result.error.reason
			                                                      : validated.result.error.code);
			return;
		}
		g_config  = validated.config;
		g_init_ok = true;
		InitializeState();
	});
	if (error != nullptr)
	{
		CopyText(error->message, sizeof(error->message), g_init_error.message);
	}
	return g_init_ok;
}

Config GetConfig() noexcept
{
	ConfigError error {};
	if (!InitializeFromEnvironment(&error))
	{
		// Never convert a failed parse into silent strict.
		std::fprintf(stderr, "KYTY_BRINGUP: invalid configuration: %s\n", error.message);
		std::fflush(stderr);
		std::_Exit(125);
	}
	return g_config;
}

Diagnostics GetDiagnostics() noexcept
{
	Diagnostics out {};
	ConfigError error {};
	if (InitializeFromEnvironment(&error))
	{
		out.config = g_config;
	}
	out.unique_sites           = g_unique_sites.load(std::memory_order_relaxed);
	out.total_continuations    = g_total_continuations.load(std::memory_order_relaxed);
	out.total_halts            = g_total_halts.load(std::memory_order_relaxed);
	out.breaker_trips          = g_breaker_trips.load(std::memory_order_relaxed);
	out.table_overflows        = g_table_overflows.load(std::memory_order_relaxed);
	out.config_rejections      = g_config_rejections.load(std::memory_order_relaxed);
	out.last_breaker_key       = g_last_breaker_key.load(std::memory_order_relaxed);
	out.last_breaker_feature   = static_cast<Feature>(g_last_breaker_feature.load(std::memory_order_relaxed));
	out.last_breaker_subsystem = static_cast<Subsystem>(g_last_breaker_subsystem.load(std::memory_order_relaxed));
	for (uint32_t i = 0; i < kFeatureCount; ++i)
	{
		out.continues_by_feature[i] = g_continues_by_feature[i].load(std::memory_order_relaxed);
		out.halts_by_feature[i]     = g_halts_by_feature[i].load(std::memory_order_relaxed);
	}
	for (uint32_t i = 0; i < kSubsystemCount; ++i)
	{
		out.continues_by_subsystem[i] = g_continues_by_subsystem[i].load(std::memory_order_relaxed);
		out.halts_by_subsystem[i]     = g_halts_by_subsystem[i].load(std::memory_order_relaxed);
	}
	return out;
}

Subsystem ClassifySourceFile(const char* file) noexcept
{
	if (file == nullptr || file[0] == '\0')
	{
		return Subsystem::Other;
	}

	char normalized[2048] {};
	const std::size_t len = std::min<std::size_t>(sizeof(normalized) - 1, std::strlen(file));
	for (std::size_t i = 0; i < len; ++i)
	{
		normalized[i] = (file[i] == '\\' ? '/' : file[i]);
	}
	normalized[len] = '\0';
	const std::string_view path {normalized, len};

	constexpr std::string_view kLoader {"/source/emulator/src/Loader/"};
	constexpr std::string_view kKernel {"/source/emulator/src/Kernel/"};
	constexpr std::string_view kGraphics {"/source/emulator/src/Graphics/"};
	constexpr std::string_view kAudio {"/source/emulator/src/Audio.cpp"};
	constexpr std::string_view kNetwork1 {"/source/emulator/src/Network.cpp"};
	constexpr std::string_view kNetwork2 {"/source/emulator/src/Libs/Network"};
	constexpr std::string_view kLibs {"/source/emulator/src/Libs/"};
	constexpr std::string_view kCore1 {"/source/lib/Core/"};
	constexpr std::string_view kCore2 {"/source/include/Kyty/Core/"};

	if (path.find(kLoader) != std::string_view::npos)
	{
		return Subsystem::Loader;
	}
	if (path.find(kKernel) != std::string_view::npos)
	{
		return Subsystem::Kernel;
	}
	if (path.find(kGraphics) != std::string_view::npos)
	{
		return Subsystem::Graphics;
	}
	if (path.find(kAudio) != std::string_view::npos)
	{
		return Subsystem::Audio;
	}
	if (path.find(kNetwork1) != std::string_view::npos || path.find(kNetwork2) != std::string_view::npos)
	{
		return Subsystem::Network;
	}
	if (path.find(kLibs) != std::string_view::npos)
	{
		return Subsystem::Hle;
	}
	if (path.find(kCore1) != std::string_view::npos || path.find(kCore2) != std::string_view::npos)
	{
		return Subsystem::Core;
	}
	return Subsystem::Other;
}

bool IsEnabled(Feature feature, Subsystem subsystem) noexcept
{
	const Config config = GetConfig();
	if (config.mode != Mode::Unsafe)
	{
		return false;
	}
	return IsFeatureEnabled(config, feature) && IsSubsystemEnabled(config, subsystem);
}

struct SiteHit
{
	bool     found       = false;
	bool     first_log   = false;
	uint32_t window_hits = 0;
	uint64_t total_hits  = 0;
};

SiteHit RecordSiteHit(uint64_t key, uint64_t now, uint32_t burst_window_ms)
{
	std::lock_guard<std::mutex> lock(g_sites_mutex);
	const uint32_t              start_slot = static_cast<uint32_t>(key & (kSiteCapacity - 1));
	SiteEntry*                  slot       = nullptr;

	for (uint32_t probe = 0; probe < kSiteCapacity; ++probe)
	{
		SiteEntry& candidate = g_sites[(start_slot + probe) & (kSiteCapacity - 1)];
		if (candidate.key == key)
		{
			slot = &candidate;
			break;
		}
		if (candidate.key != 0)
		{
			continue;
		}
		candidate     = {};
		candidate.key = key;
		g_unique_sites.fetch_add(1, std::memory_order_relaxed);
		slot = &candidate;
		break;
	}
	if (slot == nullptr)
	{
		return {};
	}

	const bool new_window = slot->window_start_ms == 0 || now < slot->window_start_ms ||
	                        (now - slot->window_start_ms) > burst_window_ms;
	if (new_window)
	{
		slot->window_start_ms = now;
		slot->window_hits     = 0;
	}
	++slot->window_hits;
	++slot->total_hits;

	SiteHit hit {};
	hit.found         = true;
	hit.first_log     = !slot->first_logged;
	hit.window_hits   = slot->window_hits;
	hit.total_hits    = slot->total_hits;
	slot->first_logged = true;
	return hit;
}

Decision Report(Feature feature, Subsystem subsystem, const char* identity, const char* file, int line) noexcept
{
	const Config config = GetConfig();
	if (config.mode != Mode::Unsafe || !IsFeatureEnabled(config, feature) || !IsSubsystemEnabled(config, subsystem) ||
	    identity == nullptr || file == nullptr || line <= 0)
	{
		NoteHalt(feature, subsystem);
		InvokeDecisionHook(feature, subsystem, Decision::Halt, identity, false);
		return Decision::Halt;
	}

	const uint64_t key = ComputeKey(feature, subsystem, identity, file, line);
	if (key == 0)
	{
		NoteHalt(feature, subsystem);
		InvokeDecisionHook(feature, subsystem, Decision::Halt, identity, false);
		return Decision::Halt;
	}

	const SiteHit hit = RecordSiteHit(key, SteadyMs(), config.burst_window_ms);
	if (!hit.found)
	{
		g_table_overflows.fetch_add(1, std::memory_order_relaxed);
		NoteHalt(feature, subsystem);
		InvokeDecisionHook(feature, subsystem, Decision::Halt, identity, false);
		return Decision::Halt;
	}

	if (hit.window_hits > config.burst_limit)
	{
		g_breaker_trips.fetch_add(1, std::memory_order_relaxed);
		g_last_breaker_key.store(key, std::memory_order_relaxed);
		g_last_breaker_feature.store(static_cast<uint32_t>(feature), std::memory_order_relaxed);
		g_last_breaker_subsystem.store(static_cast<uint32_t>(subsystem), std::memory_order_relaxed);
		// One detailed breaker log for the process (bounded).
		bool expected_log = false;
		if (g_breaker_detail_logged.compare_exchange_strong(expected_log, true, std::memory_order_relaxed))
		{
			std::fprintf(stderr,
			             "KYTY_BRINGUP_BREAKER feature=%u subsystem=%u identity=%s file=%s:%d hits=%u limit=%u\n",
			             static_cast<unsigned>(feature), static_cast<unsigned>(subsystem), identity, file, line,
			             hit.window_hits, config.burst_limit);
		}
		NoteHalt(feature, subsystem);
		InvokeDecisionHook(feature, subsystem, Decision::Halt, identity, true);
		return Decision::Halt;
	}

	// Detail once per site; later hits are counters only (no stack spam).
	if (hit.first_log)
	{
		std::fprintf(stderr, "KYTY_BRINGUP_CONTINUE feature=%u subsystem=%u identity=%s file=%s:%d\n",
		             static_cast<unsigned>(feature), static_cast<unsigned>(subsystem), identity, file, line);
	} else
	{
		// Bounded summary every 1024 continues on the same site.
		if ((hit.total_hits & 1023ull) == 0)
		{
			std::fprintf(stderr, "KYTY_BRINGUP_SUMMARY feature=%u subsystem=%u identity=%s hits=%" PRIu64 "\n",
			             static_cast<unsigned>(feature), static_cast<unsigned>(subsystem), identity, hit.total_hits);
		}
	}
	NoteContinue(feature, subsystem);
	InvokeDecisionHook(feature, subsystem, Decision::Continue, identity, false);
	return Decision::Continue;
}

Decision ReportNotImplemented(const char* expression, const char* file, int line) noexcept
{
	return Report(Feature::NotImplemented, ClassifySourceFile(file), expression, file, line);
}

void SetDecisionHook(DecisionHook hook) noexcept
{
	g_decision_hook.store(hook, std::memory_order_release);
}

} // namespace Kyty::Core::BringUp
