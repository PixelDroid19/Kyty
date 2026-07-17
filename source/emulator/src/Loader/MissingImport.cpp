#include "Emulator/Loader/MissingImport.h"

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Agent/AgentLifecycle.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader::MissingImport {
namespace {

// ---------------------------------------------------------------------------
// Helpers: own every C string; never store String::C_Str() past the copy.
// ---------------------------------------------------------------------------

void CopyCStr(char* dst, size_t cap, const char* src)
{
	if (dst == nullptr || cap == 0)
	{
		return;
	}
	if (src == nullptr)
	{
		dst[0] = '\0';
		return;
	}
	std::snprintf(dst, cap, "%s", src);
}

// ---------------------------------------------------------------------------
// StubAllocator: statically compiled SYSV callables (Rosetta-safe).
// One address per slot index. No runtime-generated pages, no scratch fallback.
//
// ABI contract is intentionally conservative: unknown signatures stay fatal at
// the Resolve gate (non-Func / Unknown never reach here). Func stubs return 0
// and do not invent per-NID return classes.
// ---------------------------------------------------------------------------

using StubFn = KYTY_SYSV_ABI int64_t (*)();

struct SlotMeta
{
	char               name[96]    = {};
	char               library[64] = {};
	std::atomic_bool   first_hit {false};
	std::atomic<uint64_t> call_count {0};
};

SlotMeta g_slot_meta[kCapacity];

static KYTY_SYSV_ABI int64_t kyty_missing_func_return_zero()
{
	// Conservative Func answer: integer zero / null-like success. Do not guess
	// errno vs pointer vs callable-return classes from symbol names.
	return 0;
}

template <int I>
static KYTY_SYSV_ABI int64_t stub_impl()
{
	SlotMeta& meta = g_slot_meta[I];
	meta.call_count.fetch_add(1, std::memory_order_relaxed);

	bool expected = false;
	if (meta.first_hit.compare_exchange_strong(expected, true, std::memory_order_relaxed))
	{
		std::printf(FG_BRIGHT_RED "CALLED missing stub: %s [%s]" DEFAULT "\n", meta.name, meta.library);
		std::fflush(stdout);
	}
	return kyty_missing_func_return_zero();
}

template <int... Is>
static constexpr void fill_stub_table(StubFn* t, std::integer_sequence<int, Is...> /*seq*/)
{
	((t[Is] = &stub_impl<Is>), ...);
}

class StubAllocator
{
public:
	// Bind slot index to identity display fields and return the callable vaddr.
	// Index must be in [0, kCapacity). Copies name/library into owned slot meta.
	static uint64_t Bind(uint32_t index, const char* name, const char* library)
	{
		EXIT_IF(index >= kCapacity);
		CopyCStr(g_slot_meta[index].name, sizeof(g_slot_meta[index].name), name);
		CopyCStr(g_slot_meta[index].library, sizeof(g_slot_meta[index].library), library);
		g_slot_meta[index].first_hit.store(false, std::memory_order_relaxed);
		g_slot_meta[index].call_count.store(0, std::memory_order_relaxed);
		return reinterpret_cast<uint64_t>(Table()[index]);
	}

	static constexpr uint32_t Capacity() { return kCapacity; }

private:
	static StubFn* Table()
	{
		static StubFn table[kCapacity];
		static bool   init = false;
		if (!init)
		{
			fill_stub_table(table, std::make_integer_sequence<int, static_cast<int>(kCapacity)> {});
			init = true;
		}
		return table;
	}
};

// ---------------------------------------------------------------------------
// MissingImportRegistry: bounded registration + stable address ownership.
// ---------------------------------------------------------------------------

struct RegistryRecord
{
	String   canonical;
	uint64_t stub_vaddr     = 0;
};

class MissingImportRegistry
{
public:
	uint64_t ResolveOrAllocateOrAbort(const SymbolIdentity& identity)
	{
		Core::LockGuard lock(m_mutex);
		++m_resolution_attempts;

		if (const uint64_t existing = FindUnlocked(identity); existing != 0)
		{
			return existing;
		}

		Emulator::Agent::Lifecycle::EmitMissingImport(identity.canonical.C_Str());
		const PolicyDecision policy = RequestBringUp(identity.canonical.C_Str());
		const bool allow_new = policy.feature_enabled && policy.report == Core::BringUp::Decision::Continue;
		if (!allow_new)
		{
			std::fprintf(stderr,
			             "KYTY_BRINGUP: missing Func import policy halt on NEW slot "
			             "(no zero vaddr): %s\n",
			             identity.canonical.C_Str());
			std::fflush(stderr);
			EXIT("=== Missing Func import policy halt (no zero vaddr) ===\n%s\n", identity.canonical.C_Str());
		}

		if (m_used_slots >= StubAllocator::Capacity())
		{
			++m_table_overflows;
			std::fprintf(stderr,
			             "KYTY_BRINGUP: missing-import stub capacity exhausted (%u); "
			             "typed fatal unsupported (no silent fallback)\n",
			             StubAllocator::Capacity());
			std::fflush(stderr);
			EXIT("missing-import stub capacity exhausted\n");
		}

		const uint32_t index = m_used_slots;
		m_records[index].canonical = identity.canonical;
		const uint64_t vaddr = StubAllocator::Bind(index, identity.name.C_Str(), identity.library.C_Str());
		EXIT_IF(vaddr == 0);
		m_records[index].stub_vaddr = vaddr;
		++m_used_slots;
		// unique_symbols tracks distinct registered identities (== used_slots
		// for Func-only stubs; kept as a separate counter by contract).
		++m_unique_symbols;
		return vaddr;
	}

	MissingImportDiagnostics Snapshot() const
	{
		Core::LockGuard          lock(m_mutex);
		MissingImportDiagnostics out {};
		out.resolution_attempts = m_resolution_attempts;
		out.unique_symbols      = m_unique_symbols;
		out.used                = m_used_slots;
		out.table_overflows     = m_table_overflows;
		out.table_capacity      = StubAllocator::Capacity();
		return out;
	}

private:
	uint64_t FindUnlocked(const SymbolIdentity& identity) const
	{
		for (uint32_t i = 0; i < m_used_slots; ++i)
		{
			if (identity.MatchesCanonical(m_records[i].canonical))
			{
				EXIT_IF(m_records[i].stub_vaddr == 0);
				return m_records[i].stub_vaddr;
			}
		}
		return 0;
	}

	mutable Core::Mutex m_mutex;
	RegistryRecord      m_records[kCapacity] {};
	uint32_t            m_used_slots          = 0;
	uint32_t            m_unique_symbols      = 0;
	uint32_t            m_resolution_attempts = 0;
	uint32_t            m_table_overflows     = 0;
};

MissingImportRegistry& Registry()
{
	static MissingImportRegistry registry;
	return registry;
}

// ---------------------------------------------------------------------------
// ImportDiagnostics: thin publish layer over registry snapshot.
// ---------------------------------------------------------------------------

struct ImportDiagnostics
{
	static MissingImportDiagnostics Publish() { return Registry().Snapshot(); }
};

} // namespace

// --- SymbolIdentity ----------------------------------------------------------

SymbolIdentity SymbolIdentity::From(const SymbolResolve& sr, const String& canonical_name)
{
	SymbolIdentity id {};
	id.canonical            = canonical_name;
	id.name                 = sr.name;
	id.library              = sr.library;
	id.module               = sr.module;
	id.library_version      = sr.library_version;
	id.module_version_major = sr.module_version_major;
	id.module_version_minor = sr.module_version_minor;
	id.type                 = sr.type;
	return id;
}

bool SymbolIdentity::MatchesCanonical(const String& other_canonical) const
{
	return canonical == other_canonical;
}

// --- ImportPolicy ------------------------------------------------------------

PolicyDecision RequestBringUp(const char* identity_cstr)
{
	PolicyDecision out {};
	out.feature_enabled =
	    Core::BringUp::IsEnabled(Core::BringUp::Feature::MissingFunctionImport, Core::BringUp::Subsystem::Loader);
	if (!out.feature_enabled)
	{
		out.report = Core::BringUp::Decision::Halt;
		return out;
	}
	const char* id = (identity_cstr != nullptr) ? identity_cstr : "";
	out.report =
	    Core::BringUp::Report(Core::BringUp::Feature::MissingFunctionImport, Core::BringUp::Subsystem::Loader, id,
	                         __FILE__, __LINE__);
	return out;
}

// --- Public assign / diagnostics --------------------------------------------

uint64_t AssignFuncStubOrAbort(const SymbolIdentity& identity)
{
	EXIT_IF(identity.type != SymbolType::Func);
	EXIT_IF(identity.canonical.IsEmpty());
	EXIT_IF(identity.stub_abi != SymbolIdentity::StubAbiCategory::IntegerOrPointerRegisterZero);

	const uint64_t vaddr = Registry().ResolveOrAllocateOrAbort(identity);
	EXIT_IF(vaddr == 0);
	return vaddr;
}

MissingImportDiagnostics Snapshot()
{
	return ImportDiagnostics::Publish();
}

} // namespace Kyty::Loader::MissingImport

#endif // KYTY_EMU_ENABLED
