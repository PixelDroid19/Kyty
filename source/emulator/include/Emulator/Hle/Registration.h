#ifndef EMULATOR_INCLUDE_EMULATOR_HLE_REGISTRATION_H_
#define EMULATOR_INCLUDE_EMULATOR_HLE_REGISTRATION_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DateTime.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Common.h"
#include "Emulator/Hle/SymbolRegistry.h"
#include "Emulator/Log.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Hle {

// HLE registration is shared by guest-library implementations and does not
// need the Libs aggregate. Keeping the timestamp provider here makes the
// registration macros usable from an isolated domain such as Graphics.
Core::Time GetTraceTime();

} // namespace Kyty::Hle

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLED g_print_name

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLE(flag) PRINT_NAME_ENABLED = flag;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_DEFINE(name) void name(::Kyty::Hle::HleSymbolRegistry* s)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_NAME(l, m)                                                                                                                     \
	[[maybe_unused]] static thread_local bool PRINT_NAME_ENABLED = true;                                                                   \
	static constexpr char                     g_library[]        = l;                                                                       \
	static constexpr char                     g_module[]         = m;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_VERSION(l, lv, m, mv1, mv2)                                                                                                    \
	LIB_NAME(l, m);                                                                                                                         \
	static constexpr int g_library_version      = lv;                                                                                       \
	static constexpr int g_module_version_major = mv1;                                                                                      \
	static constexpr int g_module_version_minor = mv2;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_USING(n)                                                                                                                       \
	using n::g_library;                                                                                                                     \
	using n::g_library_version;                                                                                                              \
	using n::g_module;                                                                                                                       \
	using n::g_module_version_major;                                                                                                         \
	using n::g_module_version_minor;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_LOAD(name) name(s)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_CHECK(ids, name)                                                                                                               \
	if (id == (ids))                                                                                                                         \
	{                                                                                                                                         \
		LIB_LOAD(name);                                                                                                                        \
		return true;                                                                                                                           \
	}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_ADD(n, f, t)                                                                                                                   \
	{                                                                                                                                         \
		::Kyty::Hle::HleSymbolResolve sr {};                                                                                                    \
		sr.name                  = n;                                                                                                          \
		sr.library               = g_library;                                                                                                  \
		sr.library_version       = g_library_version;                                                                                          \
		sr.module                = g_module;                                                                                                   \
		sr.module_version_major  = g_module_version_major;                                                                                     \
		sr.module_version_minor  = g_module_version_minor;                                                                                     \
		sr.type                  = t;                                                                                                          \
		auto            func     = reinterpret_cast<uint64_t>(f);                                                                              \
		const char32_t* dbg_name = U"" #f;                                                                                                    \
		s->AddHle(sr, func, dbg_name);                                                                                                         \
	}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_ADD_ALIASES(f, t, ...)                                                                                                         \
	{                                                                                                                                         \
		::Kyty::Hle::HleSymbolResolve sr {};                                                                                                    \
		sr.library              = g_library;                                                                                                   \
		sr.library_version      = g_library_version;                                                                                           \
		sr.module                = g_module;                                                                                                    \
		sr.module_version_major = g_module_version_major;                                                                                      \
		sr.module_version_minor = g_module_version_minor;                                                                                      \
		sr.type                 = t;                                                                                                           \
		auto            func    = reinterpret_cast<uint64_t>(f);                                                                               \
		const char32_t* dbg_name = U"" #f;                                                                                                     \
		s->AddHleAliases(sr, {__VA_ARGS__}, func, dbg_name);                                                                                   \
	}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_OBJECT(n, f) LIB_ADD(n, f, ::Kyty::Hle::HleSymbolType::Object)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC(n, f) LIB_ADD(n, f, ::Kyty::Hle::HleSymbolType::Func)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_OBJECT_ALIASES(f, ...) LIB_ADD_ALIASES(f, ::Kyty::Hle::HleSymbolType::Object, __VA_ARGS__)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC_ALIASES(f, ...) LIB_ADD_ALIASES(f, ::Kyty::Hle::HleSymbolType::Func, __VA_ARGS__)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME()                                                                                                                       \
	if (PRINT_NAME_ENABLED)                                                                                                                 \
	{                                                                                                                                         \
		if (Kyty::Log::GetDirection() != Kyty::Log::Direction::Silent)                                                                        \
		{                                                                                                                                       \
			Kyty::printf(FG_CYAN "[%d][%s] %s::%s::%s()" DEFAULT "\n", Core::Thread::GetThreadIdUnique(),                                  \
			             ::Kyty::Hle::GetTraceTime().ToString("HH24:MI:SS.FFF").C_Str(), g_library, g_module, __func__);                       \
		}                                                                                                                                       \
	}

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_HLE_REGISTRATION_H_ */
