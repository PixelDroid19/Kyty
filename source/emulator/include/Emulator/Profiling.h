#ifndef EMULATOR_INCLUDE_EMULATOR_PROFILING_H_
#define EMULATOR_INCLUDE_EMULATOR_PROFILING_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

#define BUILD_WITH_EASY_PROFILER
#define EASY_PROFILER_STATIC

#include <easy/profiler.h> // IWYU pragma: export

//
#include "easy/details/profiler_aux.h"    // IWYU pragma: export
#include "easy/details/profiler_colors.h" // IWYU pragma: export

#include <cwchar>   // IWYU pragma: export
#include <optional> // IWYU pragma: export

// IWYU pragma: no_include "easy/profiler.h"

namespace Kyty::Profiler {

// Set at startup when the configuration selects a profiler output. Without one
// the profiler can never be enabled, so blocks are not opened at all: a
// disabled easy_profiler block still reads the clock twice for its frame
// counter.
inline bool g_blocks_enabled = false;

} // namespace Kyty::Profiler

// Same as EASY_BLOCK, but constructs and begins the block only when profiler
// blocks are enabled.
#define KYTY_PROFILER_BLOCK_IMPL(name, ...)                                                                                                \
	std::optional<::profiler::Block> EASY_UNIQUE_BLOCK(__LINE__);                                                                          \
	if (::Kyty::Profiler::g_blocks_enabled)                                                                                                \
	{                                                                                                                                      \
		EASY_LOCAL_STATIC_PTR(const ::profiler::BaseBlockDescriptor*, EASY_UNIQUE_DESC(__LINE__),                                          \
		                      ::profiler::registerDescription(::profiler::extract_enable_flag(__VA_ARGS__), EASY_UNIQUE_LINE_ID,           \
		                                                      EASY_COMPILETIME_NAME(name), __FILE__, __LINE__,                             \
		                                                      ::profiler::BlockType::Block, ::profiler::extract_color(__VA_ARGS__),        \
		                                                      ::std::is_base_of<::profiler::ForceConstStr, decltype(name)>::value));       \
		EASY_UNIQUE_BLOCK(__LINE__).emplace(EASY_UNIQUE_DESC(__LINE__), EASY_RUNTIME_NAME(name));                                          \
		::profiler::beginBlock(*EASY_UNIQUE_BLOCK(__LINE__));                                                                              \
	}

#if KYTY_COMPILER == KYTY_COMPILER_MSVC
#define KYTY_PROFILER_BLOCK(f, ...) KYTY_PROFILER_BLOCK_IMPL(f, __VA_ARGS__)
#else
#define KYTY_PROFILER_BLOCK(f, s...) KYTY_PROFILER_BLOCK_IMPL(f, ##s)
#endif

#if KYTY_COMPILER == KYTY_COMPILER_MSVC
#define KYTY_PROFILER_FUNCTION(...) KYTY_PROFILER_BLOCK_IMPL(EASY_FUNC_NAME, __VA_ARGS__)
#else
#define KYTY_PROFILER_FUNCTION(s...) KYTY_PROFILER_BLOCK_IMPL(EASY_FUNC_NAME, ##s)
#endif

#define KYTY_PROFILER_END_BLOCK                                                                                                            \
	if (::Kyty::Profiler::g_blocks_enabled)                                                                                                \
	{                                                                                                                                      \
		EASY_END_BLOCK                                                                                                                     \
	}

#define KYTY_PROFILER_THREAD(f) EASY_THREAD(f)

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_PROFILING_H_ */
