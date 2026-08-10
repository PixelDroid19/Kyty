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

} // namespace TextToSpeech2

LIB_DEFINE(InitTextToSpeech2_1)
{
	LIB_FUNC("UOjiprYwVNw", TextToSpeech2::Initialize);
	LIB_FUNC("SoWHuVW0gpU", TextToSpeech2::Terminate);
	LIB_FUNC("X0HZNbSiqyg", TextToSpeech2::Open);
	LIB_FUNC("2jiIxUmcsGo", TextToSpeech2::Cancel);
	LIB_FUNC("08JSg9p6bgQ", TextToSpeech2::GetSpeechStatus);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
