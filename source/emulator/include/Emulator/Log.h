#ifndef EMULATOR_INCLUDE_EMULATOR_LOG_H_
#define EMULATOR_INCLUDE_EMULATOR_LOG_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Subsystems.h"

//#include "Emulator/Config.h"

//#define KYTY_LOG_ENABLED

#include <atomic> // IWYU pragma: keep

#define CSI               "\x1b["    // NOLINT
#define DEFAULT           CSI "0m"   // NOLINT // Returns all attributes to the default state prior to modification
#define BOLD              CSI "1m"   // NOLINT // Applies brightness/intensity flag to foreground color
#define NO_BOLD           CSI "22m"  // NOLINT // Removes brightness/intensity flag from foreground color
#define UNDERLINE         CSI "4m"   // NOLINT // Adds underline
#define NO_UNDERLINE      CSI "24m"  // NOLINT // Removes underline
#define NEGATIVE          CSI "7m"   // NOLINT // Swaps foreground and background colors
#define POSITIVE          CSI "27m"  // NOLINT // Returns foreground/background to normal
#define FG_BLACK          CSI "30m"  // NOLINT // Applies non-bold/bright black to foreground
#define FG_RED            CSI "31m"  // NOLINT // Applies non-bold/bright red to foreground
#define FG_GREEN          CSI "32m"  // NOLINT // Applies non-bold/bright green to foreground
#define FG_YELLOW         CSI "33m"  // NOLINT // Applies non-bold/bright yellow to foreground
#define FG_BLUE           CSI "34m"  // NOLINT // Applies non-bold/bright blue to foreground
#define FG_MAGENTA        CSI "35m"  // NOLINT // Applies non-bold/bright magenta to foreground
#define FG_CYAN           CSI "36m"  // NOLINT // Applies non-bold/bright cyan to foreground
#define FG_WHITE          CSI "37m"  // NOLINT // Applies non-bold/bright white to foreground
#define FG_EXTENDED       CSI "38m"  // NOLINT // Applies extended color value to the foreground (see details below)
#define FG_DEFAULT        CSI "39m"  // NOLINT // Applies only the foreground portion of the defaults (see 0)
#define BG_BLACK          CSI "40m"  // NOLINT // Applies non-bold/bright black to background
#define BG_RED            CSI "41m"  // NOLINT // Applies non-bold/bright red to background
#define BG_GREEN          CSI "42m"  // NOLINT // Applies non-bold/bright green to background
#define BG_YELLOW         CSI "43m"  // NOLINT // Applies non-bold/bright yellow to background
#define BG_BLUE           CSI "44m"  // NOLINT // Applies non-bold/bright blue to background
#define BG_MAGENTA        CSI "45m"  // NOLINT // Applies non-bold/bright magenta to background
#define BG_CYAN           CSI "46m"  // NOLINT // Applies non-bold/bright cyan to background
#define BG_WHITE          CSI "47m"  // NOLINT // Applies non-bold/bright white to background
#define BG_EXTENDED       CSI "48m"  // NOLINT // Applies extended color value to the background (see details below)
#define BG_DEFAULT        CSI "49m"  // NOLINT // Applies only the background portion of the defaults (see 0)
#define FG_BRIGHT_BLACK   CSI "90m"  // NOLINT // Applies bold/bright black to foreground
#define FG_BRIGHT_RED     CSI "91m"  // NOLINT // Applies bold/bright red to foreground
#define FG_BRIGHT_GREEN   CSI "92m"  // NOLINT // Applies bold/bright green to foreground
#define FG_BRIGHT_YELLOW  CSI "93m"  // NOLINT // Applies bold/bright yellow to foreground
#define FG_BRIGHT_BLUE    CSI "94m"  // NOLINT // Applies bold/bright blue to foreground
#define FG_BRIGHT_MAGENTA CSI "95m"  // NOLINT // Applies bold/bright magenta to foreground
#define FG_BRIGHT_CYAN    CSI "96m"  // NOLINT // Applies bold/bright cyan to foreground
#define FG_BRIGHT_WHITE   CSI "97m"  // NOLINT // Applies bold/bright white to foreground
#define BG_BRIGHT_BLACK   CSI "100m" // NOLINT // Applies bold/bright black to background
#define BG_BRIGHT_RED     CSI "101m" // NOLINT // Applies bold/bright red to background
#define BG_BRIGHT_GREEN   CSI "102m" // NOLINT // Applies bold/bright green to background
#define BG_BRIGHT_YELLOW  CSI "103m" // NOLINT // Applies bold/bright yellow to background
#define BG_BRIGHT_BLUE    CSI "104m" // NOLINT // Applies bold/bright blue to background
#define BG_BRIGHT_MAGENTA CSI "105m" // NOLINT // Applies bold/bright magenta to background
#define BG_BRIGHT_CYAN    CSI "106m" // NOLINT // Applies bold/bright cyan to background
#define BG_BRIGHT_WHITE   CSI "107m" // NOLINT // Applies bold/bright white to background

namespace Kyty {

namespace Log {

KYTY_SUBSYSTEM_DEFINE(Log);

enum class Direction
{
	Silent,
	Console,
	File,
	Directory
};

// Severity levels gate log emission before any formatting happens. The gate is
// a single relaxed atomic read, so a log call that does not pass costs the same
// as one branch -- the slow path (String::Printf + color strip + mutex) only
// runs when the message is actually emitted.
enum class Level : int
{
	Debug = 0,
	Info  = 1,
	Warn  = 2,
	Error = 3
};

Direction GetDirection();
void      SetDirection(Direction dir);
void      SetOutputFile(const String& file_name, Core::File::Encoding enc = Core::File::Encoding::Utf8);

bool   IsColoredPrintf();
String RemoveColors(const String& str);

// Minimum severity that gets emitted. Defaults to Info; debug output is opt-in.
Level GetMinLevel();
void  SetMinLevel(Level level);

// Cheap pre-format gate: true iff `level` is at or above the current minimum.
// Safe to call from any thread and from hot paths.
[[nodiscard]] bool ShouldLog(Level level);

// Level-aware logger. Formats and emits only when `ShouldLog(level)` holds.
// `format` may contain the ANSI color macros defined above.
void log_printf(Level level, const char* format, ...) KYTY_FORMAT_PRINTF(2, 3);

} // namespace Log

void printf(const char* format, ...) KYTY_FORMAT_PRINTF(1, 2);
void emu_printf(const char* format, ...) KYTY_FORMAT_PRINTF(1, 2);

} // namespace Kyty

// Convenience wrappers for the level-aware logger. The implementation checks
// the gate before formatting, so non-emitting levels avoid String/lock work.
#define KYTY_LOG_ERROR(...) ::Kyty::Log::log_printf(::Kyty::Log::Level::Error, __VA_ARGS__)
#define KYTY_LOG_WARN(...)  ::Kyty::Log::log_printf(::Kyty::Log::Level::Warn, __VA_ARGS__)
#define KYTY_LOG_INFO(...)  ::Kyty::Log::log_printf(::Kyty::Log::Level::Info, __VA_ARGS__)
#define KYTY_LOG_DEBUG(...) ::Kyty::Log::log_printf(::Kyty::Log::Level::Debug, __VA_ARGS__)

// Rate-limited variant (prosper-style log-once): emits at most `max_count`
// times per call site, then goes silent. The counter is a function-local
// static, so each macro invocation site has its own budget. Signed counter
// keeps the gate correct past zero (no unsigned wrap re-emitting).
#define KYTY_LOG_LIMIT(level, max_count, ...)                                                                          \
	do                                                                                                                 \
	{                                                                                                                  \
		static ::std::atomic_int kyty_log_remaining {(max_count)};                                                     \
		if (::Kyty::Log::ShouldLog(level) && kyty_log_remaining.fetch_sub(1, ::std::memory_order_relaxed) > 0)         \
		{                                                                                                              \
			::Kyty::Log::log_printf(level, __VA_ARGS__);                                                               \
		}                                                                                                              \
	} while (0)

#endif /* EMULATOR_INCLUDE_EMULATOR_LOG_H_ */
