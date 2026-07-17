#include "Kyty/Core/BringUp.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Subsystems.h"

#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

namespace Kyty::Core::BringUp {
namespace {

constexpr uint32_t kMaxSites = 512;

struct SiteSlot
{
	std::atomic<uint64_t> key {0};
	const char*           file  = nullptr;
	int                   line  = 0;
	const char*           expr  = nullptr;
	Subsystem             sub   = Subsystem::Other;
	std::atomic<uint64_t> hits {0};
	std::atomic<uint64_t> continues {0};
	// Burst window: start of current window (ms) and hits within it.
	std::atomic<uint64_t> window_start_ms {0};
	std::atomic<uint64_t> window_hits {0};
};

struct State
{
	std::atomic<bool> initialized {false};
	Config            config {};
	SiteSlot          sites[kMaxSites];
	std::atomic<uint64_t> unique_sites {0};
	std::atomic<uint64_t> total_continuations {0};
	std::atomic<uint64_t> missing_import_assigns {0};
	std::atomic<uint64_t> missing_import_calls {0};
	std::atomic<uint32_t> missing_import_slots {0};

	// Last circuit-break (written under spin, read for snapshot).
	std::atomic_flag    cb_lock = ATOMIC_FLAG_INIT;
	CircuitBreakSnapshot last_cb {};

	// Spinlock for site insertion (string pointer publish).
	std::atomic_flag insert_lock = ATOMIC_FLAG_INIT;
};

State g_state {};

void SpinLock(std::atomic_flag& f)
{
	while (f.test_and_set(std::memory_order_acquire))
	{
		// busy
	}
}

void SpinUnlock(std::atomic_flag& f)
{
	f.clear(std::memory_order_release);
}

[[noreturn]] void FailConfig(const char* msg)
{
	std::fprintf(stderr, "KYTY_BRINGUP: invalid configuration: %s\n", msg);
	std::fflush(stderr);
	std::_Exit(2);
}

uint64_t NowMs()
{
#if defined(CLOCK_MONOTONIC)
	struct timespec ts {};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
	{
		return static_cast<uint64_t>(ts.tv_sec) * 1000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
	}
#endif
	// Fallback: coarse wall clock.
	return static_cast<uint64_t>(std::time(nullptr)) * 1000ull;
}

// FNV-1a 64-bit over file pointer identity, line, and expression text.
uint64_t HashSite(const char* expr, const char* file, int line)
{
	uint64_t h = 14695981039346656037ull;
	auto mix = [&](uint64_t v) {
		h ^= v;
		h *= 1099511628211ull;
	};
	mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(file)));
	mix(static_cast<uint64_t>(static_cast<uint32_t>(line)));
	if (expr != nullptr)
	{
		for (const char* p = expr; *p != '\0'; ++p)
		{
			h ^= static_cast<uint64_t>(static_cast<unsigned char>(*p));
			h *= 1099511628211ull;
		}
	}
	if (h == 0)
	{
		h = 1; // zero is empty-slot sentinel
	}
	return h;
}

bool EqualsInsensitive(const char* a, const char* b)
{
	if (a == nullptr || b == nullptr)
	{
		return a == b;
	}
	while (*a != '\0' && *b != '\0')
	{
		const auto ca = static_cast<unsigned char>(*a);
		const auto cb = static_cast<unsigned char>(*b);
		if (std::tolower(ca) != std::tolower(cb))
		{
			return false;
		}
		++a;
		++b;
	}
	return *a == *b;
}

const char* Basename(const char* path)
{
	if (path == nullptr)
	{
		return "";
	}
	const char* base = path;
	for (const char* p = path; *p != '\0'; ++p)
	{
		if (*p == '/' || *p == '\\')
		{
			base = p + 1;
		}
	}
	return base;
}

bool PathContains(const char* path, const char* needle)
{
	if (path == nullptr || needle == nullptr || *needle == '\0')
	{
		return false;
	}
	// Case-sensitive substring (source tree is ASCII).
	return std::strstr(path, needle) != nullptr;
}

bool ParseCsvToken(const char*& cursor, char* out, size_t out_cap)
{
	while (*cursor == ' ' || *cursor == '\t')
	{
		++cursor;
	}
	if (*cursor == '\0')
	{
		return false;
	}
	size_t n = 0;
	while (*cursor != '\0' && *cursor != ',')
	{
		if (n + 1 < out_cap)
		{
			out[n++] = *cursor;
		}
		++cursor;
	}
	out[n] = '\0';
	// trim trailing space
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
	{
		out[--n] = '\0';
	}
	if (*cursor == ',')
	{
		++cursor;
	}
	return n > 0;
}

Feature ParseFeatures(const char* text, bool* ok)
{
	*ok = true;
	if (text == nullptr || text[0] == '\0')
	{
		*ok = false;
		return Feature::None;
	}
	Feature f = Feature::None;
	const char* c = text;
	char tok[64];
	bool any = false;
	while (ParseCsvToken(c, tok, sizeof(tok)))
	{
		any = true;
		if (EqualsInsensitive(tok, "not_implemented"))
		{
			f = f | Feature::NotImplemented;
		} else if (EqualsInsensitive(tok, "missing_function_import"))
		{
			f = f | Feature::MissingFunctionImport;
		} else if (EqualsInsensitive(tok, "gfx_permissive"))
		{
			f = f | Feature::GfxPermissive;
		} else
		{
			*ok = false;
			return Feature::None;
		}
	}
	if (!any)
	{
		*ok = false;
		return Feature::None;
	}
	return f;
}

Subsystem ParseSubsystems(const char* text, bool* ok)
{
	*ok = true;
	if (text == nullptr || text[0] == '\0')
	{
		*ok = false;
		return Subsystem::None;
	}
	Subsystem s = Subsystem::None;
	const char* c = text;
	char tok[64];
	bool any = false;
	while (ParseCsvToken(c, tok, sizeof(tok)))
	{
		any = true;
		if (EqualsInsensitive(tok, "core"))
		{
			s = s | Subsystem::Core;
		} else if (EqualsInsensitive(tok, "loader"))
		{
			s = s | Subsystem::Loader;
		} else if (EqualsInsensitive(tok, "kernel"))
		{
			s = s | Subsystem::Kernel;
		} else if (EqualsInsensitive(tok, "graphics"))
		{
			s = s | Subsystem::Graphics;
		} else if (EqualsInsensitive(tok, "audio"))
		{
			s = s | Subsystem::Audio;
		} else if (EqualsInsensitive(tok, "network"))
		{
			s = s | Subsystem::Network;
		} else if (EqualsInsensitive(tok, "hle"))
		{
			s = s | Subsystem::Hle;
		} else if (EqualsInsensitive(tok, "other"))
		{
			s = s | Subsystem::Other;
		} else
		{
			*ok = false;
			return Subsystem::None;
		}
	}
	if (!any)
	{
		*ok = false;
		return Subsystem::None;
	}
	return s;
}

uint32_t ParsePositiveU32(const char* text, bool* ok)
{
	*ok = false;
	if (text == nullptr || text[0] == '\0')
	{
		return 0;
	}
	char* end = nullptr;
	const unsigned long v = std::strtoul(text, &end, 10);
	if (end == text || (end != nullptr && *end != '\0') || v == 0 || v > 0xfffffffful)
	{
		return 0;
	}
	*ok = true;
	return static_cast<uint32_t>(v);
}

void EnsureInitLazy()
{
	if (!g_state.initialized.load(std::memory_order_acquire))
	{
		InitFromEnvironment();
	}
}

SiteSlot* FindOrInsertSite(const char* expr, const char* file, int line, Subsystem sub)
{
	const uint64_t key = HashSite(expr, file, line);
	const uint32_t start = static_cast<uint32_t>(key % kMaxSites);

	// Fast path: find existing without lock.
	for (uint32_t n = 0; n < kMaxSites; ++n)
	{
		SiteSlot& s = g_state.sites[(start + n) % kMaxSites];
		const uint64_t k = s.key.load(std::memory_order_acquire);
		if (k == 0)
		{
			break;
		}
		if (k == key && s.line == line && s.file == file &&
		    (s.expr == expr || (s.expr != nullptr && expr != nullptr && std::strcmp(s.expr, expr) == 0)))
		{
			return &s;
		}
	}

	// Insert path.
	SpinLock(g_state.insert_lock);
	SiteSlot* found = nullptr;
	for (uint32_t n = 0; n < kMaxSites; ++n)
	{
		SiteSlot& s = g_state.sites[(start + n) % kMaxSites];
		const uint64_t k = s.key.load(std::memory_order_relaxed);
		if (k == key && s.line == line && s.file == file &&
		    (s.expr == expr || (s.expr != nullptr && expr != nullptr && std::strcmp(s.expr, expr) == 0)))
		{
			found = &s;
			break;
		}
		if (k == 0)
		{
			s.file = file;
			s.line = line;
			s.expr = expr;
			s.sub  = sub;
			s.key.store(key, std::memory_order_release);
			g_state.unique_sites.fetch_add(1, std::memory_order_relaxed);
			found = &s;
			break;
		}
	}
	SpinUnlock(g_state.insert_lock);

	// Table full: treat as abort by returning null; caller aborts.
	return found;
}

void RecordCircuitBreak(SiteSlot* site, uint64_t hits)
{
	SpinLock(g_state.cb_lock);
	g_state.last_cb.active    = true;
	g_state.last_cb.file      = site->file;
	g_state.last_cb.line      = site->line;
	g_state.last_cb.expr      = site->expr;
	g_state.last_cb.hits      = hits;
	g_state.last_cb.window_ms = g_state.config.burst_window_ms;
	g_state.last_cb.limit     = g_state.config.burst_limit;
	SpinUnlock(g_state.cb_lock);

	std::fprintf(stderr,
	             "KYTY_BRINGUP: circuit-break site=%s:%d expr=(%s) hits=%" PRIu64 " limit=%" PRIu32 " window_ms=%" PRIu32 "\n",
	             site->file != nullptr ? site->file : "?", site->line, site->expr != nullptr ? site->expr : "?", hits,
	             g_state.config.burst_limit, g_state.config.burst_window_ms);

	// Accumulated summary of unique sites.
	const uint64_t n = g_state.unique_sites.load(std::memory_order_relaxed);
	std::fprintf(stderr, "KYTY_BRINGUP: summary unique_sites=%" PRIu64 " total_continuations=%" PRIu64 "\n", n,
	             g_state.total_continuations.load(std::memory_order_relaxed));
	std::fflush(stderr);
}

const char* ModeName(Mode m)
{
	return m == Mode::Unsafe ? "unsafe" : "strict";
}

void FeatureNames(Feature f, char* buf, size_t cap)
{
	buf[0] = '\0';
	auto append = [&](const char* name) {
		const size_t len = std::strlen(buf);
		if (len + std::strlen(name) + 2 >= cap)
		{
			return;
		}
		if (len > 0)
		{
			std::strcat(buf, ",");
		}
		std::strcat(buf, name);
	};
	if (Any(f & Feature::NotImplemented))
	{
		append("not_implemented");
	}
	if (Any(f & Feature::MissingFunctionImport))
	{
		append("missing_function_import");
	}
	if (Any(f & Feature::GfxPermissive))
	{
		append("gfx_permissive");
	}
}

void SubsystemNames(Subsystem s, char* buf, size_t cap)
{
	buf[0] = '\0';
	auto append = [&](const char* name) {
		const size_t len = std::strlen(buf);
		if (len + std::strlen(name) + 2 >= cap)
		{
			return;
		}
		if (len > 0)
		{
			std::strcat(buf, ",");
		}
		std::strcat(buf, name);
	};
	if (Any(s & Subsystem::Core))
	{
		append("core");
	}
	if (Any(s & Subsystem::Loader))
	{
		append("loader");
	}
	if (Any(s & Subsystem::Kernel))
	{
		append("kernel");
	}
	if (Any(s & Subsystem::Graphics))
	{
		append("graphics");
	}
	if (Any(s & Subsystem::Audio))
	{
		append("audio");
	}
	if (Any(s & Subsystem::Network))
	{
		append("network");
	}
	if (Any(s & Subsystem::Hle))
	{
		append("hle");
	}
	if (Any(s & Subsystem::Other))
	{
		append("other");
	}
}

} // namespace

void RejectLegacyEnvironment()
{
	if (std::getenv("KYTY_STUB_MISSING") != nullptr)
	{
		FailConfig("KYTY_STUB_MISSING is removed; use KYTY_BRINGUP_MODE=unsafe "
		           "and feature missing_function_import");
	}
	if (std::getenv("KYTY_GFX_PERMISSIVE") != nullptr)
	{
		FailConfig("KYTY_GFX_PERMISSIVE is removed; use KYTY_BRINGUP_MODE=unsafe "
		           "and feature gfx_permissive");
	}
}

void InitFromEnvironment()
{
	// Single-flight init.
	static std::atomic_flag once = ATOMIC_FLAG_INIT;
	if (g_state.initialized.load(std::memory_order_acquire))
	{
		return;
	}
	// Spin until we own init or someone else finished.
	while (once.test_and_set(std::memory_order_acq_rel))
	{
		if (g_state.initialized.load(std::memory_order_acquire))
		{
			return;
		}
	}
	if (g_state.initialized.load(std::memory_order_acquire))
	{
		once.clear(std::memory_order_release);
		return;
	}

	RejectLegacyEnvironment();

	Config cfg {};
	cfg.mode            = Mode::Strict;
	cfg.features        = Feature::None;
	cfg.subsystems      = Subsystem::All;
	cfg.burst_limit     = 10000;
	cfg.burst_window_ms = 1000;

	const char* mode = std::getenv("KYTY_BRINGUP_MODE");
	if (mode != nullptr)
	{
		if (EqualsInsensitive(mode, "unsafe"))
		{
			cfg.mode = Mode::Unsafe;
		} else if (EqualsInsensitive(mode, "strict"))
		{
			cfg.mode = Mode::Strict;
		} else if (mode[0] == '\0')
		{
			FailConfig("KYTY_BRINGUP_MODE is empty");
		} else
		{
			FailConfig("KYTY_BRINGUP_MODE must be 'strict' or 'unsafe'");
		}
	}

	const char* features = std::getenv("KYTY_BRINGUP_FEATURES");
	if (features != nullptr)
	{
		bool ok = false;
		cfg.features = ParseFeatures(features, &ok);
		if (!ok)
		{
			FailConfig("KYTY_BRINGUP_FEATURES invalid or empty");
		}
	} else if (cfg.mode == Mode::Unsafe)
	{
		// Absent features in unsafe mode enables all three.
		cfg.features = Feature::All;
	}

	const char* subsystems = std::getenv("KYTY_BRINGUP_SUBSYSTEMS");
	if (subsystems != nullptr)
	{
		bool ok = false;
		cfg.subsystems = ParseSubsystems(subsystems, &ok);
		if (!ok)
		{
			FailConfig("KYTY_BRINGUP_SUBSYSTEMS invalid or empty");
		}
	}

	const char* burst = std::getenv("KYTY_BRINGUP_BURST_LIMIT");
	if (burst != nullptr)
	{
		bool ok = false;
		cfg.burst_limit = ParsePositiveU32(burst, &ok);
		if (!ok)
		{
			FailConfig("KYTY_BRINGUP_BURST_LIMIT must be a positive integer");
		}
	}

	const char* window = std::getenv("KYTY_BRINGUP_BURST_WINDOW_MS");
	if (window != nullptr)
	{
		bool ok = false;
		cfg.burst_window_ms = ParsePositiveU32(window, &ok);
		if (!ok)
		{
			FailConfig("KYTY_BRINGUP_BURST_WINDOW_MS must be a positive integer");
		}
	}

	// Contradictions: unsafe with zero features is useless and rejected.
	if (cfg.mode == Mode::Unsafe && !Any(cfg.features))
	{
		FailConfig("unsafe mode requires at least one feature");
	}
	// Strict mode must not carry diagnostic features (contradictory).
	if (cfg.mode == Mode::Strict && Any(cfg.features))
	{
		// Only if features were explicitly set; absent features stay None in strict.
		if (features != nullptr)
		{
			FailConfig("strict mode cannot enable bring-up features");
		}
	}

	g_state.config = cfg;
	g_state.initialized.store(true, std::memory_order_release);
	once.clear(std::memory_order_release);
}

void InitForTests(const Config& config)
{
	ResetForTests();
	if (config.mode == Mode::Unsafe && !Any(config.features))
	{
		FailConfig("unsafe mode requires at least one feature");
	}
	if (config.burst_limit == 0 || config.burst_window_ms == 0)
	{
		FailConfig("burst_limit and burst_window_ms must be positive");
	}
	g_state.config = config;
	g_state.initialized.store(true, std::memory_order_release);
}

void ResetForTests()
{
	// Wipe registry; leave flags re-initable.
	for (uint32_t i = 0; i < kMaxSites; ++i)
	{
		g_state.sites[i].key.store(0, std::memory_order_relaxed);
		g_state.sites[i].file  = nullptr;
		g_state.sites[i].line  = 0;
		g_state.sites[i].expr  = nullptr;
		g_state.sites[i].sub   = Subsystem::Other;
		g_state.sites[i].hits.store(0, std::memory_order_relaxed);
		g_state.sites[i].continues.store(0, std::memory_order_relaxed);
		g_state.sites[i].window_start_ms.store(0, std::memory_order_relaxed);
		g_state.sites[i].window_hits.store(0, std::memory_order_relaxed);
	}
	g_state.unique_sites.store(0, std::memory_order_relaxed);
	g_state.total_continuations.store(0, std::memory_order_relaxed);
	g_state.missing_import_assigns.store(0, std::memory_order_relaxed);
	g_state.missing_import_calls.store(0, std::memory_order_relaxed);
	g_state.missing_import_slots.store(0, std::memory_order_relaxed);
	g_state.last_cb = {};
	g_state.config  = {};
	g_state.initialized.store(false, std::memory_order_release);
}

bool IsInitialized()
{
	return g_state.initialized.load(std::memory_order_acquire);
}

Mode GetMode()
{
	EnsureInitLazy();
	return g_state.config.mode;
}

Config GetConfig()
{
	EnsureInitLazy();
	return g_state.config;
}

bool FeatureEnabled(Feature f)
{
	EnsureInitLazy();
	return g_state.config.mode == Mode::Unsafe && Any(g_state.config.features & f);
}

bool SubsystemAllowed(Subsystem s)
{
	EnsureInitLazy();
	return Any(g_state.config.subsystems & s);
}

Subsystem ClassifyFile(const char* file)
{
	if (file == nullptr)
	{
		return Subsystem::Other;
	}
	// Prefer path segments over basename so lib/Core and emulator Graphics both match.
	if (PathContains(file, "/Graphics/") || PathContains(file, "\\Graphics\\") || PathContains(file, "/Graphics\\") ||
	    PathContains(file, "Graphics/Graphics") || PathContains(file, "GraphicsRun") || PathContains(file, "GraphicsRender") ||
	    PathContains(file, "VideoOut") || PathContains(file, "Tile.cpp") || PathContains(file, "Shader"))
	{
		return Subsystem::Graphics;
	}
	if (PathContains(file, "/Loader/") || PathContains(file, "\\Loader\\") || PathContains(file, "RuntimeLinker") ||
	    PathContains(file, "SymbolDatabase") || PathContains(file, "/Elf"))
	{
		return Subsystem::Loader;
	}
	if (PathContains(file, "/Kernel/") || PathContains(file, "\\Kernel\\") || PathContains(file, "Pthread") ||
	    PathContains(file, "Memory.cpp") || PathContains(file, "Fiber.cpp") || PathContains(file, "Semaphore"))
	{
		return Subsystem::Kernel;
	}
	if (PathContains(file, "Audio") || PathContains(file, "/Audio."))
	{
		return Subsystem::Audio;
	}
	if (PathContains(file, "Network") || PathContains(file, "Http"))
	{
		return Subsystem::Network;
	}
	if (PathContains(file, "/Libs/") || PathContains(file, "\\Libs\\") || PathContains(file, "LibC.cpp") ||
	    PathContains(file, "LibKernel") || PathContains(file, "LibGraphics") || PathContains(file, "LibAmpr") ||
	    PathContains(file, "HLE") || PathContains(file, "Hle"))
	{
		return Subsystem::Hle;
	}
	if (PathContains(file, "/Core/") || PathContains(file, "\\Core\\") || PathContains(file, "DbgAssert") ||
	    PathContains(file, "BringUp") || PathContains(file, "Subsystems"))
	{
		return Subsystem::Core;
	}
	// Basename fallbacks for short test paths.
	const char* base = Basename(file);
	if (EqualsInsensitive(base, "BringUp.cpp") || EqualsInsensitive(base, "DbgAssert.cpp"))
	{
		return Subsystem::Core;
	}
	return Subsystem::Other;
}

Decision HandleNotImplemented(const char* expr, const char* file, int line)
{
	EnsureInitLazy();

	const Subsystem sub = ClassifyFile(file);
	const bool allow =
	    g_state.config.mode == Mode::Unsafe && FeatureEnabled(Feature::NotImplemented) && SubsystemAllowed(sub);

	if (!allow)
	{
		return Decision::Abort;
	}

	SiteSlot* site = FindOrInsertSite(expr, file, line, sub);
	if (site == nullptr)
	{
		// Registry full — fail closed.
		std::fprintf(stderr, "KYTY_BRINGUP: site registry full; aborting\n");
		std::fflush(stderr);
		return Decision::Abort;
	}

	site->hits.fetch_add(1, std::memory_order_relaxed);

	// Burst window accounting.
	const uint64_t now = NowMs();
	uint64_t start     = site->window_start_ms.load(std::memory_order_relaxed);
	if (start == 0)
	{
		uint64_t expected = 0;
		if (site->window_start_ms.compare_exchange_strong(expected, now, std::memory_order_relaxed))
		{
			start = now;
			site->window_hits.store(0, std::memory_order_relaxed);
		} else
		{
			start = expected;
		}
	}

	if (now - start > g_state.config.burst_window_ms)
	{
		// Reset window (best-effort; races only affect burst precision).
		site->window_start_ms.store(now, std::memory_order_relaxed);
		site->window_hits.store(1, std::memory_order_relaxed);
	} else
	{
		const uint64_t wh = site->window_hits.fetch_add(1, std::memory_order_relaxed) + 1;
		if (wh > g_state.config.burst_limit)
		{
			RecordCircuitBreak(site, wh);
			return Decision::CircuitBreak;
		}
	}

	const uint64_t prev = site->continues.fetch_add(1, std::memory_order_relaxed);
	g_state.total_continuations.fetch_add(1, std::memory_order_relaxed);

	if (prev == 0)
	{
		// First encounter: log once.
		std::fprintf(stderr, "KYTY_BRINGUP: continue NotImplemented (%s) in %s:%d\n", expr != nullptr ? expr : "?",
		             file != nullptr ? file : "?", line);
		std::fflush(stderr);
	}

	return Decision::Continue;
}

bool AllowGfxPermissive()
{
	EnsureInitLazy();
	return FeatureEnabled(Feature::GfxPermissive);
}

bool AllowMissingFunctionImport()
{
	EnsureInitLazy();
	return FeatureEnabled(Feature::MissingFunctionImport);
}

void NoteMissingImportAssigned()
{
	EnsureInitLazy();
	g_state.missing_import_assigns.fetch_add(1, std::memory_order_relaxed);
}

void NoteMissingImportCalled()
{
	EnsureInitLazy();
	g_state.missing_import_calls.fetch_add(1, std::memory_order_relaxed);
}

void NoteMissingImportSlots(uint32_t used_slots)
{
	EnsureInitLazy();
	g_state.missing_import_slots.store(used_slots, std::memory_order_relaxed);
}

void GetSnapshot(Snapshot* out)
{
	if (out == nullptr)
	{
		return;
	}
	EnsureInitLazy();
	out->config                   = g_state.config;
	out->unique_sites             = g_state.unique_sites.load(std::memory_order_relaxed);
	out->total_continuations      = g_state.total_continuations.load(std::memory_order_relaxed);
	out->missing_import_assigns   = g_state.missing_import_assigns.load(std::memory_order_relaxed);
	out->missing_import_calls     = g_state.missing_import_calls.load(std::memory_order_relaxed);
	out->missing_import_slots     = g_state.missing_import_slots.load(std::memory_order_relaxed);
	SpinLock(g_state.cb_lock);
	out->last_circuit_break = g_state.last_cb;
	SpinUnlock(g_state.cb_lock);

	// Sites are exposed via a static snapshot table filled on demand for diagnostics.
	static SiteSnapshot snap_sites[kMaxSites];
	uint32_t            filled = 0;
	for (uint32_t i = 0; i < kMaxSites && filled < kMaxSites; ++i)
	{
		if (g_state.sites[i].key.load(std::memory_order_relaxed) == 0)
		{
			continue;
		}
		snap_sites[filled].file           = g_state.sites[i].file;
		snap_sites[filled].line           = g_state.sites[i].line;
		snap_sites[filled].expr           = g_state.sites[i].expr;
		snap_sites[filled].subsystem      = g_state.sites[i].sub;
		snap_sites[filled].hit_count      = g_state.sites[i].hits.load(std::memory_order_relaxed);
		snap_sites[filled].continue_count = g_state.sites[i].continues.load(std::memory_order_relaxed);
		++filled;
	}
	out->sites          = snap_sites;
	out->sites_capacity = filled;
}

int WriteDiagnosticsJson(std::FILE* out)
{
	if (out == nullptr)
	{
		return -1;
	}
	Snapshot snap {};
	GetSnapshot(&snap);

	char feat[128];
	char subs[128];
	FeatureNames(snap.config.features, feat, sizeof(feat));
	SubsystemNames(snap.config.subsystems, subs, sizeof(subs));

	const int n = std::fprintf(
	    out,
	    "{\"protocolVersion\":%d,\"bringup\":{\"mode\":\"%s\",\"features\":\"%s\",\"subsystems\":\"%s\","
	    "\"burst_limit\":%" PRIu32 ",\"burst_window_ms\":%" PRIu32 ",\"unique_sites\":%" PRIu64
	    ",\"continuations\":%" PRIu64 ",\"missing_imports_assigned\":%" PRIu64 ",\"missing_import_calls\":%" PRIu64
	    ",\"missing_import_slots\":%" PRIu32 ",\"last_circuit_break\":{\"active\":%s,\"file\":%s%s%s,\"line\":%d,"
	    "\"expr\":%s%s%s,\"hits\":%" PRIu64 "}}}\n",
	    kDiagnosticsProtocolVersion, ModeName(snap.config.mode), feat, subs, snap.config.burst_limit,
	    snap.config.burst_window_ms, snap.unique_sites, snap.total_continuations, snap.missing_import_assigns,
	    snap.missing_import_calls, snap.missing_import_slots, snap.last_circuit_break.active ? "true" : "false",
	    snap.last_circuit_break.file != nullptr ? "\"" : "null",
	    snap.last_circuit_break.file != nullptr ? snap.last_circuit_break.file : "",
	    snap.last_circuit_break.file != nullptr ? "\"" : "", snap.last_circuit_break.line,
	    snap.last_circuit_break.expr != nullptr ? "\"" : "null",
	    snap.last_circuit_break.expr != nullptr ? snap.last_circuit_break.expr : "",
	    snap.last_circuit_break.expr != nullptr ? "\"" : "", snap.last_circuit_break.hits);
	std::fflush(out);
	return n;
}

} // namespace Kyty::Core::BringUp
