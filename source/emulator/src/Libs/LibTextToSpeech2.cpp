#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include "Kyty/Core/VirtualMemory.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("TextToSpeech2", 1, "TextToSpeech2", 1, 1);

namespace TextToSpeech2 {

static std::atomic<bool> g_initialized {false};
static std::atomic<bool> g_open {false};

struct OpenParameters
{
	uint32_t configuration;
	uint32_t flags;
};

static std::atomic<uint64_t> g_open_configuration {0};

static int KYTY_SYSV_ABI Initialize()
{
	PRINT_NAME();
	g_initialized.store(true, std::memory_order_release);
	return OK;
}

static int KYTY_SYSV_ABI Terminate()
{
	PRINT_NAME();
	g_open.store(false, std::memory_order_release);
	g_open_configuration.store(0, std::memory_order_release);
	g_initialized.store(false, std::memory_order_release);
	return OK;
}

static int KYTY_SYSV_ABI Open(const OpenParameters* parameters)
{
	PRINT_NAME();

	if (!g_initialized.load(std::memory_order_acquire))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	OpenParameters copied {};
	if (parameters == nullptr ||
	    !Core::VirtualMemory::CopyFromGuest(&copied, reinterpret_cast<uint64_t>(parameters), sizeof(copied)))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const uint64_t configuration = static_cast<uint64_t>(copied.configuration) |
	                               (static_cast<uint64_t>(copied.flags) << 32u);
	g_open_configuration.store(configuration, std::memory_order_release);
	g_open.store(true, std::memory_order_release);

	return OK;
}

static int KYTY_SYSV_ABI GetSpeechStatus(int32_t* status)
{
	PRINT_NAME();

	if (!g_initialized.load(std::memory_order_acquire) || !g_open.load(std::memory_order_acquire))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	constexpr int32_t idle = 0;
	if (status == nullptr || !Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(status), &idle, sizeof(idle)))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	return OK;
}

static int KYTY_SYSV_ABI Cancel()
{
	PRINT_NAME();

	if (!g_initialized.load(std::memory_order_acquire) || !g_open.load(std::memory_order_acquire))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	return OK;
}

// Speech request header; 0x40 bytes are filled by callers, the rest is not
// read here.
struct SpeakParameters
{
	uint64_t text;   // UTF-16 text
	uint32_t length; // in UTF-16 units
	uint32_t flags;
	uint64_t opaque;
};

// Accepted requests finish at once: no speech is synthesized, so the status
// stays idle and no audio plays.
static int KYTY_SYSV_ABI Speak(const SpeakParameters* parameters)
{
	PRINT_NAME();

	if (!g_initialized.load(std::memory_order_acquire) || !g_open.load(std::memory_order_acquire))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	SpeakParameters copied {};
	if (parameters == nullptr ||
	    !Core::VirtualMemory::CopyFromGuest(&copied, reinterpret_cast<uint64_t>(parameters), sizeof(copied)) ||
	    (copied.text == 0 && copied.length != 0))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	KYTY_LOG_LIMIT(Log::Level::Warn, 4, "TextToSpeech2: speech is not synthesized (%u UTF-16 units, flags 0x%x)\n", copied.length,
	               copied.flags);
	return OK;
}

// Ends the session Open started; the caller terminates the library next.
static int KYTY_SYSV_ABI Close()
{
	PRINT_NAME();

	if (!g_initialized.load(std::memory_order_acquire) || !g_open.exchange(false, std::memory_order_acq_rel))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	g_open_configuration.store(0, std::memory_order_release);
	return OK;
}

} // namespace TextToSpeech2

LIB_DEFINE(InitTextToSpeech2_1)
{
	LIB_FUNC("UOjiprYwVNw", TextToSpeech2::Initialize);
	LIB_FUNC("SoWHuVW0gpU", TextToSpeech2::Terminate);
	LIB_FUNC("X0HZNbSiqyg", TextToSpeech2::Open);
	LIB_FUNC("2jiIxUmcsGo", TextToSpeech2::Cancel);
	LIB_FUNC("08JSg9p6bgQ", TextToSpeech2::GetSpeechStatus);
	LIB_FUNC("8ntsRd07EQA", TextToSpeech2::Speak);
	LIB_FUNC("t4e879M-cSw", TextToSpeech2::Close);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
