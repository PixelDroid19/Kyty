#include "Emulator/Audio.h"
#include "Emulator/AudioHost.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/VideoFrameMemory.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

namespace VideoFrameMemory = Kyty::Emulator::VideoFrameMemory;
// Keep the guest-facing implementation readable while the decoder remains a
// neutral host runtime service.
namespace AudioVideoBackend = ::Kyty::Emulator::AudioVideoBackend;

static std::shared_ptr<HostAudio> g_host_audio;

void AudioSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	EXIT_IF(std::atomic_load(&g_host_audio) != nullptr);
	std::string error;
	auto        audio = HostAudio::Create(&error);
	if (audio == nullptr)
	{
		this->Fail("%s\n", error.c_str());
		return;
	}
	std::atomic_store(&g_host_audio, std::move(audio));
}

void AudioSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent)
{
	auto audio = std::atomic_exchange(&g_host_audio, std::shared_ptr<HostAudio> {});
	if (audio != nullptr)
	{
		audio->Shutdown();
	}
}

void AudioSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
{
	auto audio = std::atomic_exchange(&g_host_audio, std::shared_ptr<HostAudio> {});
	if (audio != nullptr)
	{
		audio->Shutdown();
	}
}

namespace AudioOut {

LIB_NAME("AudioOut", "AudioOut");

void AudioOutSetHostPaused(bool paused)
{
	auto audio = std::atomic_load(&g_host_audio);
	if (audio != nullptr)
	{
		audio->SetHostPaused(paused);
	}
}

struct AudioOutOutputParam
{
	int         handle;
	const void* ptr;
};

struct AudioOutPortState
{
	uint16_t output;
	uint8_t  channel;
	uint8_t  reserved1[1];
	int16_t  volume;
	uint16_t reroute_counter;
	uint64_t flag;
	uint64_t reserved2[2];
};

int KYTY_SYSV_ABI AudioOutInit()
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI AudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t user_id = %d\n", user_id);
	KYTY_LOG_DEBUG("\t type    = %d\n", type);
	KYTY_LOG_DEBUG("\t index   = %d\n", index);
	KYTY_LOG_DEBUG("\t len     = %u\n", len);
	KYTY_LOG_DEBUG("\t freq    = %u\n", freq);

	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 1);
	// Port types observed on Gen5 titles: 0 MAIN, 1 BGM, 3 PERSONAL, 4 PADSPK,
	// 10 (pad/haptic-adjacent), and 126 for Audio3D output.
	EXIT_NOT_IMPLEMENTED(type != 0 && type != 1 && type != 3 && type != 4 && type != 10 && type != 126);
	EXIT_NOT_IMPLEMENTED(index != 0);

	HostAudio::Format format = HostAudio::Format::Unknown;

	switch (param)
	{
		case 0: format = HostAudio::Format::Signed16bitMono; break;
		case 1: format = HostAudio::Format::Signed16bitStereo; break;
		case 2: format = HostAudio::Format::Signed16bit8Ch; break;
		case 3: format = HostAudio::Format::FloatMono; break;
		case 4: format = HostAudio::Format::FloatStereo; break;
		case 5: format = HostAudio::Format::Float8Ch; break;
		case 6: format = HostAudio::Format::Signed16bit8ChStd; break;
		case 7: format = HostAudio::Format::Float8ChStd; break;
		default:;
	}

	KYTY_LOG_DEBUG("\t param   = %u (%s)\n", param, Core::EnumName(format).C_Str());

	EXIT_NOT_IMPLEMENTED(format == HostAudio::Format::Unknown);

	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr)
	{
		return AUDIO_OUT_ERROR_PORT_FULL;
	}
	auto id = audio->AudioOutOpen(type, len, freq, format);

	if (!id.IsValid())
	{
		return AUDIO_OUT_ERROR_PORT_FULL;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI AudioOutClose(int handle)
{
	PRINT_NAME();

	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr || !audio->AudioOutClose(HostAudio::Id(handle)))
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOutGetPortState(int handle, AudioOutPortState* state)
{
	PRINT_NAME();

	int type         = 0;
	int channels_num = 0;

	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr || !audio->AudioOutGetStatus(HostAudio::Id(handle), &type, &channels_num))
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	state->reroute_counter = 0;
	state->volume          = 127;

	switch (type)
	{
		case 0:
		case 1:
		case 2:
		case 126:
			state->output  = 1;
			state->channel = (channels_num > 2 ? 2 : channels_num);
			break;
		case 3:
		case 127:
			state->output  = 0;
			state->channel = 0;
			break;
		case 4:
		case 10:
			// PADSPK and Gen5 type-10 pad/haptic ports report tertiary/pad routing.
			state->output  = 4;
			state->channel = 1;
			break;
		default: EXIT("unknown port type: %d\n", type);
	}

	KYTY_LOG_DEBUG("\t output  = %" PRIu16 "\n", state->output);
	KYTY_LOG_DEBUG("\t channel = %" PRIu8 "\n", state->channel);

	return OK;
}

int KYTY_SYSV_ABI AudioOutSetVolume(int handle, uint32_t flag, int* vol)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	KYTY_LOG_DEBUG("\t flag   = %u\n", flag);

	EXIT_NOT_IMPLEMENTED(vol == nullptr);

	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr || !audio->AudioOutSetVolume(HostAudio::Id(handle), flag, vol))
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOutOutputs(AudioOutOutputParam* param, uint32_t num)
{
	PRINT_NAME();
	if (param == nullptr || num == 0 || num > HostAudio::OUT_PORTS_MAX)
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	for (uint32_t i = 0; i < num; i++)
	{
		KYTY_LOG_DEBUG("\t handle[%u] = %d\n", i, param[i].handle);
	}

	HostAudio::OutputParam params[HostAudio::OUT_PORTS_MAX];
	auto                   audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr)
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	for (uint32_t i = 0; i < num; i++)
	{
		params[i].handle = HostAudio::Id(param[i].handle);
		params[i].data   = param[i].ptr;
	}

	uint32_t samples_num = 0;
	return audio->AudioOutOutputs(params, num, &samples_num) ? static_cast<int>(samples_num) : AUDIO_OUT_ERROR_INVALID_PORT;
}

int KYTY_SYSV_ABI AudioOutOutput(int handle, const void* ptr)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t handle = %d\n", handle);

	// EXIT_NOT_IMPLEMENTED(ptr == nullptr);

	HostAudio::OutputParam params[1];
	auto                   audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr)
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	params[0].handle = HostAudio::Id(handle);
	params[0].data   = ptr;

	uint32_t samples_num = 0;
	return audio->AudioOutOutputs(params, 1, &samples_num) ? static_cast<int>(samples_num) : AUDIO_OUT_ERROR_INVALID_PORT;
}

} // namespace AudioOut

namespace AudioOut2 {

LIB_NAME("AudioOut2", "AudioOut");

// Host-side AudioOut2 object table. Guest receives opaque positive handles.
// Layout/API reimplemented from public export names (SCE NID encoding) and
// observed Gen5 import/call order; no third-party implementation code copied.
constexpr int32_t  kMaxContexts         = 8;
constexpr int32_t  kMaxPorts            = 16;
constexpr int32_t  kMaxUsers            = 8;
constexpr uint64_t kDefaultContextBytes = 0x10000; // host workspace until layout is measured
constexpr uint32_t kDefaultQueueDepth   = 4;

struct ContextSlot
{
	bool     used       = false;
	void*    buffer     = nullptr;
	uint64_t size       = 0;
	uint32_t queue_used = 0;
};

struct PortSlot
{
	bool    used    = false;
	int32_t context = 0;
	// PortGetState fields (0x20-byte guest state blob).
	uint16_t output   = 0x01;
	uint8_t  channels = 2;
	int16_t  status   = -1;
};

struct UserSlot
{
	bool used    = false;
	int  user_id = 0;
};

static ContextSlot g_contexts[kMaxContexts];
static PortSlot    g_ports[kMaxPorts];
static UserSlot    g_users[kMaxUsers];
static bool        g_audio_out2_ready = false;

static int32_t AllocContext()
{
	for (int32_t i = 0; i < kMaxContexts; i++)
	{
		if (!g_contexts[i].used)
		{
			g_contexts[i]      = ContextSlot {};
			g_contexts[i].used = true;
			return i + 1; // guest handles are 1-based
		}
	}
	return 0;
}

static int32_t AllocPort(int32_t context)
{
	for (int32_t i = 0; i < kMaxPorts; i++)
	{
		if (!g_ports[i].used)
		{
			g_ports[i]         = PortSlot {};
			g_ports[i].used    = true;
			g_ports[i].context = context;
			return i + 1;
		}
	}
	return 0;
}

static int32_t AllocUser(int user_id)
{
	for (int32_t i = 0; i < kMaxUsers; i++)
	{
		if (!g_users[i].used)
		{
			g_users[i]         = UserSlot {};
			g_users[i].used    = true;
			g_users[i].user_id = user_id;
			return i + 1;
		}
	}
	return 0;
}

// sceAudioOut2Initialize (NID g2tViFIohHE)
int KYTY_SYSV_ABI AudioOut2Initialize()
{
	PRINT_NAME();
	g_audio_out2_ready = true;
	return OK;
}

// sceAudioOut2ContextResetParam (NID t5YrizufpQc)
// Guest passes a context-param blob; official ResetParam fills defaults.
// Without a measured sizeof(param), leave memory unchanged and succeed so the
// title can write its own fields after the call (observed boot pattern).
int KYTY_SYSV_ABI AudioOut2ContextResetParam(void* param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	if (param == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	// Peek leading size-like field if present (common SCE param header).
	uint64_t leading = 0;
	std::memcpy(&leading, param, sizeof(leading));
	KYTY_LOG_DEBUG("\t leading = 0x%016" PRIx64 "\n", leading);
	return OK;
}

// sceAudioOut2ContextQueryMemory (NID pDmme7Bgm6E)
int KYTY_SYSV_ABI AudioOut2ContextQueryMemory(const void* param, uint64_t* size_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t param    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	KYTY_LOG_DEBUG("\t size_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(size_out));
	if (size_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	*size_out = kDefaultContextBytes;
	return OK;
}

// sceAudioOut2ContextCreate (NID 0x6o1VVAYSY)
int KYTY_SYSV_ABI AudioOut2ContextCreate(const void* param, void* buffer, uint64_t size, int32_t* handle_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t param      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	KYTY_LOG_DEBUG("\t buffer     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer));
	KYTY_LOG_DEBUG("\t size       = 0x%016" PRIx64 "\n", size);
	KYTY_LOG_DEBUG("\t handle_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handle_out));
	if (handle_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (!g_audio_out2_ready)
	{
		KYTY_LOG_DEBUG("\t note: ContextCreate before Initialize\n");
	}
	const int32_t id = AllocContext();
	if (id == 0)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	g_contexts[id - 1].buffer = buffer;
	g_contexts[id - 1].size   = size;
	*handle_out               = id;
	KYTY_LOG_DEBUG("\t handle     = %d\n", id);
	return OK;
}

// sceAudioOut2ContextDestroy (NID on6ZH7Abo10)
int KYTY_SYSV_ABI AudioOut2ContextDestroy(int32_t handle)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	if (handle < 1 || handle > kMaxContexts || !g_contexts[handle - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	g_contexts[handle - 1] = ContextSlot {};
	return OK;
}

// sceAudioOut2ContextAdvance (NID PE2zHMqLSHs)
int KYTY_SYSV_ABI AudioOut2ContextAdvance(int32_t handle)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	if (handle < 1 || handle > kMaxContexts || !g_contexts[handle - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (g_contexts[handle - 1].queue_used > 0)
	{
		g_contexts[handle - 1].queue_used--;
	}
	return OK;
}

// sceAudioOut2ContextPush (NID aII9h5nli9U)
int KYTY_SYSV_ABI AudioOut2ContextPush(int32_t handle, const void* data)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	KYTY_LOG_DEBUG("\t data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	if (handle < 1 || handle > kMaxContexts || !g_contexts[handle - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (g_contexts[handle - 1].queue_used < kDefaultQueueDepth)
	{
		g_contexts[handle - 1].queue_used++;
	}
	return OK;
}

// sceAudioOut2ContextGetQueueLevel (NID R7d0F1g2qsU)
int KYTY_SYSV_ABI AudioOut2ContextGetQueueLevel(int32_t handle, uint32_t* used, uint32_t* available)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle    = %d\n", handle);
	KYTY_LOG_DEBUG("\t used      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(used));
	KYTY_LOG_DEBUG("\t available = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(available));
	if (handle < 1 || handle > kMaxContexts || !g_contexts[handle - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	const uint32_t q = g_contexts[handle - 1].queue_used;
	if (used != nullptr)
	{
		*used = q;
	}
	if (available != nullptr)
	{
		*available = (q < kDefaultQueueDepth) ? (kDefaultQueueDepth - q) : 0;
	}
	return OK;
}

// sceAudioOut2PortCreate (NID JK2wamZPzwM)
int KYTY_SYSV_ABI AudioOut2PortCreate(int32_t context, const void* param, int32_t* port_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t context  = %d\n", context);
	KYTY_LOG_DEBUG("\t param    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	KYTY_LOG_DEBUG("\t port_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(port_out));
	if (port_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (context < 1 || context > kMaxContexts || !g_contexts[context - 1].used)
	{
		// Some titles create a port before a host-tracked context handle is
		// established; still allocate so boot can continue with evidence.
		KYTY_LOG_DEBUG("\t note: PortCreate with unknown context\n");
	}
	const int32_t id = AllocPort(context);
	if (id == 0)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	*port_out = id;
	KYTY_LOG_DEBUG("\t port     = %d\n", id);
	return OK;
}

// sceAudioOut2PortDestroy (NID cd+Rtw+D1x8)
int KYTY_SYSV_ABI AudioOut2PortDestroy(int32_t port)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t port = %d\n", port);
	if (port < 1 || port > kMaxPorts || !g_ports[port - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	g_ports[port - 1] = PortSlot {};
	return OK;
}

// sceAudioOut2PortSetAttributes (NID 8XTArSPyWHk)
// attr points at tagged entries { int32 id; int32 pad; uint64 value; } (16 B).
// When attr is a single opaque pointer from older call sites, treat as no-op success.
int KYTY_SYSV_ABI AudioOut2PortSetAttributes(int32_t port, const void* attr)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t port = %d\n", port);
	KYTY_LOG_DEBUG("\t attr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(attr));
	if (port < 1 || port > kMaxPorts || !g_ports[port - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	// Successful SetAttributes clears the previous "unset" status so GetState
	// reports a ready port.
	g_ports[port - 1].status = 0;
	if (attr != nullptr)
	{
		// Best-effort first entry: id at +0, value at +8.
		const auto*   words        = static_cast<const uint32_t*>(attr);
		const int32_t attribute_id = static_cast<int32_t>(words[0]);
		uint64_t      value        = 0;
		std::memcpy(&value, static_cast<const uint8_t*>(attr) + 8, sizeof(value));
		auto& p = g_ports[port - 1];
		switch (attribute_id)
		{
			case 0:
			case 1: p.output = static_cast<uint16_t>(value); break;
			case 2: p.channels = static_cast<uint8_t>(value); break;
			case 3:
			case 4: p.status = static_cast<int16_t>(value); break;
			default: break;
		}
	}
	return OK;
}

// sceAudioOut2PortGetState (NID gatEUKG+Ea4)
int KYTY_SYSV_ABI AudioOut2PortGetState(int32_t port, void* state_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t port      = %d\n", port);
	KYTY_LOG_DEBUG("\t state_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(state_out));
	if (port < 1 || port > kMaxPorts || !g_ports[port - 1].used || state_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	constexpr size_t kPortStateSize = 0x20;
	void*            start          = nullptr;
	void*            end            = nullptr;
	if (Kernel::Memory::KernelQueryMemoryProtection(state_out, &start, &end, nullptr) != OK ||
	    reinterpret_cast<uintptr_t>(state_out) > reinterpret_cast<uintptr_t>(end) - kPortStateSize + 1)
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	const auto& p = g_ports[port - 1];
	uint8_t     blob[kPortStateSize] {};
	blob[0] = static_cast<uint8_t>(p.output & 0xffu);
	blob[1] = static_cast<uint8_t>((p.output >> 8) & 0xffu);
	blob[2] = p.channels;
	blob[4] = static_cast<uint8_t>(static_cast<uint16_t>(p.status) & 0xffu);
	blob[5] = static_cast<uint8_t>((static_cast<uint16_t>(p.status) >> 8) & 0xffu);
	std::memcpy(state_out, blob, kPortStateSize);
	return OK;
}

// sceAudioOut2UserCreate (NID xywYcRB7nbQ)
int KYTY_SYSV_ABI AudioOut2UserCreate(uint32_t user_id, uintptr_t* user_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t user_id  = %u\n", user_id);
	KYTY_LOG_DEBUG("\t user_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_out));
	if (user_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	void* output_start = nullptr;
	void* output_end   = nullptr;
	if (Kernel::Memory::KernelQueryMemoryProtection(user_out, &output_start, &output_end, nullptr) != OK ||
	    reinterpret_cast<uintptr_t>(user_out) > reinterpret_cast<uintptr_t>(output_end) - sizeof(*user_out) + 1)
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	const int32_t id = AllocUser(static_cast<int>(user_id));
	if (id == 0)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	*user_out = static_cast<uintptr_t>(id);
	KYTY_LOG_DEBUG("\t user     = %d\n", id);
	return OK;
}

// sceAudioOut2UserDestroy (NID IaZXJ9M79uo)
int KYTY_SYSV_ABI AudioOut2UserDestroy(uintptr_t user)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t user = 0x%016" PRIxPTR "\n", user);
	if (user < 1 || user > static_cast<uintptr_t>(kMaxUsers) || !g_users[user - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	g_users[user - 1] = UserSlot {};
	return OK;
}

} // namespace AudioOut2

namespace AudioIn {

LIB_NAME("AudioIn", "AudioIn");

int KYTY_SYSV_ABI AudioInOpen(int user_id, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t user_id = %d\n", user_id);
	KYTY_LOG_DEBUG("\t type    = %u\n", type);
	KYTY_LOG_DEBUG("\t index   = %d\n", index);
	KYTY_LOG_DEBUG("\t len     = %u\n", len);
	KYTY_LOG_DEBUG("\t freq    = %u\n", freq);

	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 1);
	EXIT_NOT_IMPLEMENTED(type != 1);
	EXIT_NOT_IMPLEMENTED(index != 0);

	HostAudio::Format format = HostAudio::Format::Unknown;

	switch (param)
	{
		case 0: format = HostAudio::Format::Signed16bitMono; break;
		case 2: format = HostAudio::Format::Signed16bitStereo; break;
		default:;
	}

	KYTY_LOG_DEBUG("\t param   = %u (%s)\n", param, Core::EnumName(format).C_Str());

	EXIT_NOT_IMPLEMENTED(format == HostAudio::Format::Unknown);

	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr)
	{
		return AUDIO_IN_ERROR_PORT_FULL;
	}
	auto id = audio->AudioInOpen(type, len, freq, format);

	if (!id.IsValid())
	{
		return AUDIO_IN_ERROR_PORT_FULL;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI AudioInInput(int handle, void* dest)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(dest == nullptr);

	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr || !audio->AudioInValid(HostAudio::Id(handle)))
	{
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}

	return static_cast<int>(audio->AudioInInput(HostAudio::Id(handle), dest));
}

} // namespace AudioIn

namespace VoiceQoS {

LIB_NAME("VoiceQoS", "VoiceQoS");

int KYTY_SYSV_ABI VoiceQoSInit(void* mem_block, uint32_t mem_size, int32_t app_type)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t mem_block = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(mem_block));
	KYTY_LOG_DEBUG("\t mem_size = %" PRIu32 "\n", mem_size);
	KYTY_LOG_DEBUG("\t app_type = %" PRId32 "\n", app_type);

	return OK;
}

} // namespace VoiceQoS



} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED
