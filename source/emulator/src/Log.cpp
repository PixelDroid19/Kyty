#include "Emulator/Log.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Config.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
// IWYU pragma: no_include <handleapi.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processenv.h>
// IWYU pragma: no_include <winbase.h>
// IWYU pragma: no_include <wincon.h>
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty {

namespace Log {

static bool                     g_log_initialized    = false;
static Core::Mutex*             g_mutex              = nullptr;
static std::atomic<Direction>   g_dir {Direction::Silent};
static Core::File*              g_file               = nullptr;
static std::atomic_bool         g_colored_printf {false};
static thread_local Core::File* g_thread_local_file  = nullptr;
static Vector<Core::File*>*     g_thread_local_files = nullptr;
// Minimum severity that is emitted. Stored as an int so the hot-path gate is a
// single relaxed atomic read; default Info keeps normal output and makes debug opt-in.
static std::atomic_int          g_min_level {static_cast<int>(Level::Info)};
// Cap File-mode logs so a long boot cannot fill the host disk. After the
// limit, further printf/emu_printf lines are dropped (direction stays File).
static constexpr uint64_t       g_file_log_max_bytes = 32ull * 1024ull * 1024ull;
static uint64_t                 g_file_log_bytes     = 0;
static bool                     g_file_log_capped    = false;

static bool EnableVTMode()
{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Set output mode to handle virtual terminal sequences
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);

	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
	if (h == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	DWORD dw_mode = 0;
	if (GetConsoleMode(h, &dw_mode) == 0)
	{
		return false;
	}

	dw_mode |= static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	return (SetConsoleMode(h, dw_mode) != 0);
#endif
	return true;
}

bool IsColoredPrintf()
{
	return g_colored_printf.load(std::memory_order_acquire);
}

String RemoveColors(const String& str)
{
	uint32_t start = 0;
	String   ret;
	for (;;)
	{
		auto index = str.FindIndex(U'\x1b', start);
		if (!str.IndexValid(index))
		{
			ret += str.Mid(start);
			break;
		}
		ret += str.Mid(start, index - start);
		index = str.FindIndex(U'm', index);
		if (!str.IndexValid(index))
		{
			break;
		}
		start = index + 1;
	}
	return ret;
}

static void Close()
{
	if (g_log_initialized)
	{
		g_dir.store(Direction::Silent, std::memory_order_release);
		g_colored_printf.store(false, std::memory_order_release);
		g_mutex->Lock();
		if (g_file != nullptr)
		{
			g_file->Flush();
			g_file->Close();
			delete g_file;
			g_file = nullptr;
		}
		if (g_thread_local_files != nullptr && !g_thread_local_files->IsEmpty())
		{
			for (auto* file: *g_thread_local_files)
			{
				file->Flush();
				file->Close();
				delete file;
			}
			g_thread_local_files->Clear();
		}
		g_mutex->Unlock();
	}
}

void LogSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	if (!g_log_initialized)
	{
		g_mutex              = new Core::Mutex;
		g_thread_local_files = new Vector<Core::File*>;
		g_log_initialized    = true;
	}

	auto dir = Config::GetPrintfDirection();
	SetDirection(dir);
	if (dir == Log::Direction::File)
	{
		SetOutputFile(Config::GetPrintfOutputFile());
	}

	// Severity gate drives which levels reach the output. Default Info keeps
	// today's behavior; set PrintfLevel = Debug in the config to surface the
	// KYTY_LOG_DEBUG diagnostics (shader dumps, command processor traces) in
	// the console, mirroring verbose modes in other emulators.
	SetMinLevel(Config::GetPrintfLevel());
}

void LogSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent)
{
	Close();
}

void LogSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
{
	Close();
}

void SetDirection(Direction dir)
{
	EXIT_IF(!Log::g_log_initialized);
	EXIT_IF(!Core::Thread::IsMainThread());

	bool colored_printf = false;
	if (dir == Direction::Console)
	{
		colored_printf = EnableVTMode();

		if (!colored_printf)
		{
			::printf("Colored printf is not supported\n");
		}
	}

	g_colored_printf.store(colored_printf, std::memory_order_release);
	g_dir.store(dir, std::memory_order_release);
}

Direction GetDirection()
{
	EXIT_IF(!Log::g_log_initialized);

	return g_dir.load(std::memory_order_acquire);
}

void SetOutputFile(const String& file_name, Core::File::Encoding enc)
{
	EXIT_IF(!Log::g_log_initialized);
	EXIT_IF(!Core::Thread::IsMainThread());
	EXIT_IF(Log::g_dir.load(std::memory_order_acquire) != Log::Direction::File);

	auto* file = new Core::File;
	file->Create(file_name);
	if (file->IsInvalid())
	{
		::printf("Can't create log file: %s\n", file_name.C_Str());
		delete file;
	} else
	{
		file->SetEncoding(enc);
		file->WriteBOM();

		Core::File* previous_file = nullptr;
		g_mutex->Lock();
		previous_file     = g_file;
		g_file            = file;
		g_file_log_bytes  = 0;
		g_file_log_capped = false;
		g_mutex->Unlock();

		if (previous_file != nullptr)
		{
			previous_file->Flush();
			previous_file->Close();
			delete previous_file;
		}
	}
}

// Returns false when File-mode output is past the soft cap (caller should skip Write).
bool FileLogAllowsWrite(uint64_t add_bytes)
{
	if (g_file == nullptr || g_file_log_capped)
	{
		return false;
	}
	if (add_bytes > g_file_log_max_bytes - g_file_log_bytes)
	{
		g_file_log_capped = true;
		const char* msg = "\n[kyty] log file soft-cap reached (32 MiB); further File logging suppressed\n";
		g_file->Write(String::FromUtf8(msg));
		g_file->Flush();
		::printf("%s", msg);
		return false;
	}
	g_file_log_bytes += add_bytes;
	return true;
}

void SetOutputThreadLocalFile(const String& file_name, Core::File::Encoding enc)
{
	EXIT_IF(!Log::g_log_initialized);
	EXIT_IF(Log::g_dir.load(std::memory_order_acquire) != Log::Direction::Directory);
	EXIT_IF(Log::g_thread_local_file != nullptr);
	EXIT_IF(g_thread_local_files == nullptr);

	Core::File::CreateDirectories(file_name.DirectoryWithoutFilename());

	auto* file = new Core::File;
	file->Create(file_name);

	if (file->IsInvalid())
	{
		::printf("Can't create log file: %s\n", file_name.C_Str());
		delete file;
		return;
	} else
	{
		file->SetEncoding(enc);
		file->WriteBOM();
	}

	g_mutex->Lock();
	if (g_dir.load(std::memory_order_acquire) != Direction::Directory || g_thread_local_files == nullptr)
	{
		g_mutex->Unlock();
		file->Close();
		delete file;
		return;
	}
	g_thread_local_file = file;
	g_thread_local_files->Add(file);
	g_mutex->Unlock();
}

void CreateThreadLocalFile()
{
	auto file_name = String::FromPrintf("%s/%d.txt", Config::GetPrintfOutputFolder().C_Str(), Core::Thread::GetThreadIdUnique());
	SetOutputThreadLocalFile(file_name, Core::File::Encoding::Utf8);
}

} // namespace Log

namespace {

// Shared emission path. All formatting and output routing happens here, and
// only when the severity gate + direction gate both pass.
void Emit(Log::Level level, const char* format, va_list args)
{
	// HLE and loader paths can report an error while the subsystem graph is
	// still being assembled. Logging must not turn that diagnostic path into a
	// process abort; the message is intentionally dropped until a sink exists.
	if (!Log::g_log_initialized)
	{
		return;
	}

	if (!Log::ShouldLog(level))
	{
		return;
	}

	const auto direction = Log::g_dir.load(std::memory_order_acquire);
	if (direction == Log::Direction::Silent)
	{
		return;
	}

	EXIT_IF(Log::g_mutex == nullptr);

	String s;
	s.Printf(format, args);

	if (!Log::g_colored_printf.load(std::memory_order_acquire))
	{
		s = Log::RemoveColors(s);
	}

	if (direction == Log::Direction::Console)
	{
		Log::g_mutex->Lock();
		::printf("%s", s.C_Str());
		Log::g_mutex->Unlock();
	} else if (direction == Log::Direction::File)
	{
		Log::g_mutex->Lock();
		if (Log::g_file != nullptr && Log::FileLogAllowsWrite(static_cast<uint64_t>(s.Size())))
		{
			Log::g_file->Write(s);
		}
		Log::g_mutex->Unlock();
	} else if (direction == Log::Direction::Directory)
	{
		if (Log::g_thread_local_file == nullptr)
		{
			Log::CreateThreadLocalFile();
		}
		Log::g_mutex->Lock();
		if (Log::g_dir.load(std::memory_order_acquire) == Log::Direction::Directory && Log::g_thread_local_file != nullptr)
		{
			Log::g_thread_local_file->Write(s);
		}
		Log::g_mutex->Unlock();
	}
}

} // namespace

void emu_printf(const char* format, ...)
{
	va_list args {};
	va_start(args, format);
	Emit(Log::Level::Info, format, args);
	va_end(args);
}

void printf(const char* format, ...)
{
	va_list args {};
	va_start(args, format);
	Emit(Log::Level::Info, format, args);
	va_end(args);
}

namespace Log {

Level GetMinLevel()
{
	return static_cast<Level>(g_min_level.load(std::memory_order_relaxed));
}

void SetMinLevel(Level level)
{
	g_min_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

bool ShouldLog(Level level)
{
	return static_cast<int>(level) >= g_min_level.load(std::memory_order_relaxed);
}

void log_printf(Level level, const char* format, ...)
{
	va_list args {};
	va_start(args, format);
	Emit(level, format, args);
	va_end(args);
}

} // namespace Log

} // namespace Kyty

#endif // KYTY_EMU_ENABLED
