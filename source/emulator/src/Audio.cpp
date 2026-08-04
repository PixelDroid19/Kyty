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
#include "Emulator/Loader/GuestCall.h"

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
// Kyty::Libs::AudioVideoBackend is the canonical host-side decoder namespace.
// This local alias keeps the guest-facing implementation readable without changing its ABI.
namespace AudioVideoBackend = ::Kyty::Libs::AudioVideoBackend;

static std::shared_ptr<HostAudio> g_host_audio;

KYTY_SUBSYSTEM_INIT(Audio)
{
	EXIT_IF(std::atomic_load(&g_host_audio) != nullptr);
	std::string error;
	auto        audio = HostAudio::Create(&error);
	if (audio == nullptr)
	{
		KYTY_SUBSYSTEM_FAIL("%s\n", error.c_str());
	}
	std::atomic_store(&g_host_audio, std::move(audio));
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Audio)
{
	auto audio = std::atomic_exchange(&g_host_audio, std::shared_ptr<HostAudio> {});
	if (audio != nullptr)
	{
		audio->Shutdown();
	}
}

KYTY_SUBSYSTEM_DESTROY(Audio)
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

	printf("\t user_id = %d\n", user_id);
	printf("\t type    = %d\n", type);
	printf("\t index   = %d\n", index);
	printf("\t len     = %u\n", len);
	printf("\t freq    = %u\n", freq);

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

	printf("\t param   = %u (%s)\n", param, Core::EnumName(format).C_Str());

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

	printf("\t output  = %" PRIu16 "\n", state->output);
	printf("\t channel = %" PRIu8 "\n", state->channel);

	return OK;
}

int KYTY_SYSV_ABI AudioOutSetVolume(int handle, uint32_t flag, int* vol)
{
	PRINT_NAME();

	printf("\t handle = %d\n", handle);
	printf("\t flag   = %u\n", flag);

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
		printf("\t handle[%u] = %d\n", i, param[i].handle);
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

	printf("\t handle = %d\n", handle);

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
	printf("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	if (param == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	// Peek leading size-like field if present (common SCE param header).
	uint64_t leading = 0;
	std::memcpy(&leading, param, sizeof(leading));
	printf("\t leading = 0x%016" PRIx64 "\n", leading);
	return OK;
}

// sceAudioOut2ContextQueryMemory (NID pDmme7Bgm6E)
int KYTY_SYSV_ABI AudioOut2ContextQueryMemory(const void* param, uint64_t* size_out)
{
	PRINT_NAME();
	printf("\t param    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	printf("\t size_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(size_out));
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
	printf("\t param      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	printf("\t buffer     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer));
	printf("\t size       = 0x%016" PRIx64 "\n", size);
	printf("\t handle_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handle_out));
	if (handle_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (!g_audio_out2_ready)
	{
		printf("\t note: ContextCreate before Initialize\n");
	}
	const int32_t id = AllocContext();
	if (id == 0)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	g_contexts[id - 1].buffer = buffer;
	g_contexts[id - 1].size   = size;
	*handle_out               = id;
	printf("\t handle     = %d\n", id);
	return OK;
}

// sceAudioOut2ContextDestroy (NID on6ZH7Abo10)
int KYTY_SYSV_ABI AudioOut2ContextDestroy(int32_t handle)
{
	PRINT_NAME();
	printf("\t handle = %d\n", handle);
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
	printf("\t handle = %d\n", handle);
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
	printf("\t handle = %d\n", handle);
	printf("\t data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
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
	printf("\t handle    = %d\n", handle);
	printf("\t used      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(used));
	printf("\t available = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(available));
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
	printf("\t context  = %d\n", context);
	printf("\t param    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	printf("\t port_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(port_out));
	if (port_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (context < 1 || context > kMaxContexts || !g_contexts[context - 1].used)
	{
		// Some titles create a port before a host-tracked context handle is
		// established; still allocate so boot can continue with evidence.
		printf("\t note: PortCreate with unknown context\n");
	}
	const int32_t id = AllocPort(context);
	if (id == 0)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	*port_out = id;
	printf("\t port     = %d\n", id);
	return OK;
}

// sceAudioOut2PortDestroy (NID cd+Rtw+D1x8)
int KYTY_SYSV_ABI AudioOut2PortDestroy(int32_t port)
{
	PRINT_NAME();
	printf("\t port = %d\n", port);
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
	printf("\t port = %d\n", port);
	printf("\t attr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(attr));
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
	printf("\t port      = %d\n", port);
	printf("\t state_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(state_out));
	if (port < 1 || port > kMaxPorts || !g_ports[port - 1].used || state_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	constexpr size_t kPortStateSize = 0x20;
	void*            start          = nullptr;
	void*            end            = nullptr;
	if (LibKernel::Memory::KernelQueryMemoryProtection(state_out, &start, &end, nullptr) != OK ||
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
int KYTY_SYSV_ABI AudioOut2UserCreate(int user_id, const void* param, int32_t* user_out)
{
	PRINT_NAME();
	printf("\t user_id  = %d\n", user_id);
	printf("\t param    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	printf("\t user_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_out));
	if (user_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	void* output_start = nullptr;
	void* output_end   = nullptr;
	if (LibKernel::Memory::KernelQueryMemoryProtection(user_out, &output_start, &output_end, nullptr) != OK ||
	    reinterpret_cast<uintptr_t>(user_out) > reinterpret_cast<uintptr_t>(output_end) - sizeof(*user_out) + 1)
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	const int32_t id = AllocUser(user_id);
	if (id == 0)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	*user_out = id;
	printf("\t user     = %d\n", id);
	return OK;
}

// sceAudioOut2UserDestroy (NID IaZXJ9M79uo)
int KYTY_SYSV_ABI AudioOut2UserDestroy(int32_t user)
{
	PRINT_NAME();
	printf("\t user = %d\n", user);
	if (user < 1 || user > kMaxUsers || !g_users[user - 1].used)
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

	printf("\t user_id = %d\n", user_id);
	printf("\t type    = %u\n", type);
	printf("\t index   = %d\n", index);
	printf("\t len     = %u\n", len);
	printf("\t freq    = %u\n", freq);

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

	printf("\t param   = %u (%s)\n", param, Core::EnumName(format).C_Str());

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

	printf("\t mem_block = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(mem_block));
	printf("\t mem_size = %" PRIu32 "\n", mem_size);
	printf("\t app_type = %" PRId32 "\n", app_type);

	return OK;
}

} // namespace VoiceQoS

namespace Ajm {

LIB_NAME("Ajm", "Ajm");

namespace {

constexpr int32_t  AJM_ERROR_INVALID_CONTEXT          = static_cast<int32_t>(0x80930002u);
constexpr int32_t  AJM_ERROR_INVALID_INSTANCE         = static_cast<int32_t>(0x80930003u);
constexpr int32_t  AJM_ERROR_INVALID_BATCH            = static_cast<int32_t>(0x80930004u);
constexpr int32_t  AJM_ERROR_INVALID_PARAMETER        = static_cast<int32_t>(0x80930005u);
constexpr int32_t  AJM_ERROR_OUT_OF_RESOURCES         = static_cast<int32_t>(0x80930007u);
constexpr int32_t  AJM_ERROR_CODEC_ALREADY_REGISTERED = static_cast<int32_t>(0x80930009u);
constexpr int32_t  AJM_ERROR_CODEC_NOT_REGISTERED     = static_cast<int32_t>(0x8093000au);
constexpr int32_t  AJM_ERROR_WRONG_REVISION_FLAG      = static_cast<int32_t>(0x8093000bu);
constexpr int32_t  AJM_ERROR_MALFORMED_BATCH          = static_cast<int32_t>(0x80930011u);
constexpr uint32_t AJM_MAX_CODEC_TYPE                 = 25;
constexpr int64_t  AJM_GEN5_INITIALIZATION_FLAGS      = INT64_C(0x300000000);
constexpr uint32_t AJM_MAX_INSTANCE_SLOT              = 0x2fff;
constexpr uint32_t AJM_INSTANCE_SLOT_MASK             = 0x3fff;
constexpr uint64_t AJM_FLAG_RUN_GET_CODEC_INFO        = 1ull << 11u;
constexpr uint64_t AJM_FLAG_RUN_MULTIPLE_FRAMES       = 1ull << 12u;
constexpr uint64_t AJM_FLAG_SIDEBAND_RESAMPLE_INFO    = 1ull << 44u;
constexpr uint64_t AJM_FLAG_SIDEBAND_GAPLESS          = 1ull << 45u;
constexpr uint64_t AJM_FLAG_SIDEBAND_FORMAT           = 1ull << 46u;
constexpr uint64_t AJM_FLAG_SIDEBAND_STREAM           = 1ull << 47u;
constexpr uint32_t AJM_SIDEBAND_RESULT_SIZE           = 8u;
constexpr uint32_t AJM_SIDEBAND_RESAMPLE_INFO_SIZE    = 40u;
constexpr uint32_t AJM_SIDEBAND_GAPLESS_SIZE          = 8u;
constexpr uint32_t AJM_SIDEBAND_FORMAT_SIZE           = 24u;
constexpr uint32_t AJM_SIDEBAND_STREAM_SIZE           = 16u;
constexpr uint32_t AJM_SIDEBAND_MULTIPLE_FRAMES_SIZE  = 8u;

struct AjmContextState
{
	std::unordered_set<uint32_t>           registered_codecs;
	std::unordered_map<uint32_t, uint32_t> instances_by_slot;
	std::unordered_set<uint32_t>           completed_batches;
	uint32_t                               next_instance_slot = 0;
	uint32_t                               next_batch_id      = 0;
};

struct AjmBatch2DecodeJob
{
	uint32_t               instance        = 0;
	uint32_t               data_input_size = 0;
	std::vector<AjmBuffer> data_outputs;
	void*                  result          = nullptr;
	uint64_t               sideband_flags  = 0;
	void*                  sideband_output = nullptr;
	uint32_t               sideband_size   = 0;
};

struct AjmDecodeResult
{
	int32_t  result                = 0;
	int32_t  codec_result          = 0;
	uint32_t input_bytes_consumed  = 0;
	uint32_t output_bytes_produced = 0;
	uint64_t total_decoded_samples = 0;
	uint32_t decoded_frames        = 0;
	uint32_t reserved              = 0;
};

static_assert(sizeof(AjmDecodeResult) == 32);

struct AjmStatisticsResult
{
	int32_t  result = 0;
	int32_t  codec_result = 0;
	float    engine_usage_batch = 0.0f;
	float    engine_usage_interval[3] {};
	uint32_t memory[6] {};
};

static_assert(sizeof(AjmStatisticsResult) == 48);

std::mutex                                                     g_ajm_mutex;
std::unordered_map<uint32_t, AjmContextState>                  g_ajm_contexts;
std::unordered_map<uintptr_t, std::vector<AjmBatch2DecodeJob>> g_ajm_batch2_jobs;
std::unordered_map<uint32_t, uint64_t>                         g_ajm_decoded_samples;

constexpr size_t AJM_MAX_BATCH2_BUFFER_SIZE = 64u * 1024u * 1024u;

bool ValidateAjmBuffers(const AjmBuffer* buffers, size_t count, uint32_t* total_size)
{
	if (total_size == nullptr || (buffers == nullptr && count != 0))
	{
		return false;
	}
	uint64_t total = 0;
	for (size_t index = 0; index < count; index++)
	{
		if (buffers[index].address == nullptr || buffers[index].size > AJM_MAX_BATCH2_BUFFER_SIZE)
		{
			return false;
		}
		total += buffers[index].size;
		if (total > AJM_MAX_BATCH2_BUFFER_SIZE)
		{
			return false;
		}
	}
	*total_size = static_cast<uint32_t>(total);
	return true;
}

void WriteAjmResampleInfoResult(void* result)
{
	if (result == nullptr)
	{
		return;
	}

	const float ratio = 1.0f;
	std::memset(result, 0, AJM_SIDEBAND_RESULT_SIZE + AJM_SIDEBAND_RESAMPLE_INFO_SIZE);
	std::memcpy(static_cast<uint8_t*>(result) + AJM_SIDEBAND_RESULT_SIZE, &ratio, sizeof(ratio));
}

void WriteAjmFormatResult(void* result)
{
	if (result == nullptr)
	{
		return;
	}

	auto* const output = static_cast<uint8_t*>(result);
	std::memset(output, 0, AJM_SIDEBAND_RESULT_SIZE + AJM_SIDEBAND_FORMAT_SIZE);
	const uint32_t channel_format = 2u;
	const uint32_t channel_count  = 3u;
	const uint32_t sample_rate    = 48000u;
	std::memcpy(output + AJM_SIDEBAND_RESULT_SIZE + 0u, &channel_format, sizeof(channel_format));
	std::memcpy(output + AJM_SIDEBAND_RESULT_SIZE + 4u, &channel_count, sizeof(channel_count));
	std::memcpy(output + AJM_SIDEBAND_RESULT_SIZE + 8u, &sample_rate, sizeof(sample_rate));
}

void WriteAjmGaplessResult(void* result)
{
	if (result != nullptr)
	{
		std::memset(result, 0, AJM_SIDEBAND_RESULT_SIZE + AJM_SIDEBAND_GAPLESS_SIZE);
	}
}

bool QueueAjmDataJob(void* batch, uint32_t instance, uint32_t input_size, const AjmBuffer* outputs, size_t output_count, void* result,
                     uint64_t sideband_flags = 0, void* sideband_output = nullptr, uint32_t sideband_size = 0)
{
	if (batch == nullptr || instance == 0 || (outputs == nullptr && output_count != 0))
	{
		return false;
	}

	AjmBatch2DecodeJob job {};
	job.instance        = instance;
	job.data_input_size = input_size;
	if (output_count != 0)
	{
		job.data_outputs.assign(outputs, outputs + output_count);
	}
	job.result          = result;
	job.sideband_flags  = sideband_flags;
	job.sideband_output = sideband_output;
	job.sideband_size   = sideband_size;

	std::scoped_lock lock(g_ajm_mutex);
	g_ajm_batch2_jobs[reinterpret_cast<uintptr_t>(batch)].push_back(std::move(job));
	return true;
}

uint32_t AllocateContextId()
{
	uint32_t id = 1;
	while (g_ajm_contexts.find(id) != g_ajm_contexts.end())
	{
		++id;
	}
	return id;
}

constexpr uint32_t MakeAjmChunkHeader(uint32_t identifier, uint32_t payload = 0)
{
	return (identifier & 0x3fu) | ((payload & 0xfffffu) << 6u);
}

void WriteAjmU32(uint8_t* destination, uint32_t value)
{
	std::memcpy(destination, &value, sizeof(value));
}

uint32_t ReadAjmU32(const uint8_t* source)
{
	uint32_t value = 0;
	std::memcpy(&value, source, sizeof(value));
	return value;
}

void* ReadAjmPointer(const uint8_t* source)
{
	void* value = nullptr;
	std::memcpy(&value, source, sizeof(value));
	return value;
}

void WriteAjmPointer(uint8_t* destination, const void* value)
{
	std::memcpy(destination, &value, sizeof(value));
}

uint8_t* WriteAjmBufferChunk(uint8_t* destination, uint32_t identifier, void* address, size_t size)
{
	if (size > UINT32_MAX)
	{
		return nullptr;
	}
	WriteAjmU32(destination, MakeAjmChunkHeader(identifier));
	WriteAjmU32(destination + 4u, static_cast<uint32_t>(size));
	WriteAjmPointer(destination + 8u, address);
	return destination + 16u;
}

struct AjmOutputChunk
{
	void*    address = nullptr;
	uint32_t size    = 0;
	bool     control = false;
};

uint32_t AjmCodecInfoSize(uint32_t instance)
{
	switch (instance >> 14u)
	{
		case 0u: return 16u;
		case 1u: return 16u;
		case 2u: return 8u;
		case 24u: return 4u;
		default: return 0u;
	}
}

void WriteAjmRunSideband(uint64_t flags, void* output, uint32_t output_size, uint32_t instance, uint64_t input_size,
                         uint64_t data_output_size, uint64_t total_decoded_samples)
{
	if (output == nullptr || output_size == 0)
	{
		return;
	}

	std::memset(output, 0, output_size);
	auto*    sideband        = static_cast<uint8_t*>(output);
	uint32_t sideband_offset = AJM_SIDEBAND_RESULT_SIZE;

	if ((flags & AJM_FLAG_RUN_GET_CODEC_INFO) != 0)
	{
		const uint32_t codec_info_size = AjmCodecInfoSize(instance);
		if (codec_info_size != 0 && output_size >= sideband_offset + codec_info_size)
		{
			sideband_offset += codec_info_size;
		}
	}
	if ((flags & AJM_FLAG_SIDEBAND_RESAMPLE_INFO) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_RESAMPLE_INFO_SIZE)
	{
		const float ratio = 1.0f;
		std::memcpy(sideband + sideband_offset, &ratio, sizeof(ratio));
		sideband_offset += AJM_SIDEBAND_RESAMPLE_INFO_SIZE;
	}
	if ((flags & AJM_FLAG_SIDEBAND_GAPLESS) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_GAPLESS_SIZE)
	{
		sideband_offset += AJM_SIDEBAND_GAPLESS_SIZE;
	}
	if ((flags & AJM_FLAG_SIDEBAND_FORMAT) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_FORMAT_SIZE)
	{
		WriteAjmU32(sideband + sideband_offset + 0u, 2u);
		WriteAjmU32(sideband + sideband_offset + 4u, 3u);
		WriteAjmU32(sideband + sideband_offset + 8u, 48000u);
		sideband_offset += AJM_SIDEBAND_FORMAT_SIZE;
	}
	if ((flags & AJM_FLAG_SIDEBAND_STREAM) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_STREAM_SIZE)
	{
		WriteAjmU32(sideband + sideband_offset, static_cast<uint32_t>(std::min<uint64_t>(input_size, INT32_MAX)));
		WriteAjmU32(sideband + sideband_offset + 4u, static_cast<uint32_t>(std::min<uint64_t>(data_output_size, INT32_MAX)));
		std::memcpy(sideband + sideband_offset + 8u, &total_decoded_samples, sizeof(total_decoded_samples));
		sideband_offset += AJM_SIDEBAND_STREAM_SIZE;
	}
	if ((flags & AJM_FLAG_RUN_MULTIPLE_FRAMES) != 0 && output_size >= sideband_offset + AJM_SIDEBAND_MULTIPLE_FRAMES_SIZE)
	{
		WriteAjmU32(sideband + sideband_offset, input_size != 0 ? 1u : 0u);
	}
}

bool ProcessAjmJobBuffer(const uint8_t* job, uint32_t job_size, uint32_t instance, const AjmContextState& context)
{
	if (instance != 0x80000u)
	{
		const uint32_t slot = instance & AJM_INSTANCE_SLOT_MASK;
		if (slot == 0 || context.instances_by_slot.find(slot) == context.instances_by_slot.end())
		{
			return false;
		}
	}

	uint64_t                    flags       = 0;
	uint64_t                    input_size  = 0;
	uint64_t                    output_size = 0;
	std::vector<AjmOutputChunk> outputs;
	uint32_t                    offset = 0;
	while (offset < job_size)
	{
		if (job_size - offset < sizeof(uint32_t))
		{
			return false;
		}
		const uint32_t header     = ReadAjmU32(job + offset);
		const uint32_t identifier = header & 0x3fu;
		if (identifier == 3u || identifier == 4u)
		{
			if (job_size - offset < 8u)
			{
				return false;
			}
			flags = (static_cast<uint64_t>((header >> 6u) & 0xfffffu) << 32u) | ReadAjmU32(job + offset + 4u);
			offset += 8u;
			continue;
		}
		if (identifier != 1u && identifier != 2u && identifier != 6u && identifier != 17u && identifier != 18u)
		{
			return false;
		}
		if (job_size - offset < 16u)
		{
			return false;
		}
		const uint32_t size    = ReadAjmU32(job + offset + 4u);
		void* const    address = ReadAjmPointer(job + offset + 8u);
		if (size != 0 && address == nullptr)
		{
			return false;
		}
		if (identifier == 1u)
		{
			input_size += size;
		} else if (identifier == 17u)
		{
			output_size += size;
			outputs.push_back({address, size, false});
		} else if (identifier == 18u)
		{
			outputs.push_back({address, size, true});
		}
		offset += 16u;
	}

	for (const auto& output: outputs)
	{
		if (output.size == 0)
		{
			continue;
		}
		std::memset(output.address, 0, output.size);
		if (!output.control)
		{
			continue;
		}

		WriteAjmRunSideband(flags, output.address, output.size, instance, input_size, output_size, output_size / 4u);
	}

	return true;
}

bool ProcessAjmBatchBuffer(const uint8_t* batch, uint32_t batch_size, const AjmContextState& context)
{
	uint32_t offset = 0;
	while (offset < batch_size)
	{
		if (batch_size - offset < 8u)
		{
			return false;
		}
		const uint32_t header     = ReadAjmU32(batch + offset);
		const uint32_t identifier = header & 0x3fu;
		const uint32_t payload    = (header >> 6u) & 0xfffffu;
		const uint32_t size       = ReadAjmU32(batch + offset + 4u);
		offset += 8u;
		if (size > batch_size - offset)
		{
			return false;
		}
		if (identifier == 0u)
		{
			if (!ProcessAjmJobBuffer(batch + offset, size, payload, context))
			{
				return false;
			}
		} else if (identifier != 7u)
		{
			return false;
		}
		offset += size;
	}
	return offset == batch_size;
}

} // namespace

int KYTY_SYSV_ABI AjmInitialize(int64_t reserved, uint32_t* context)
{
	PRINT_NAME();

	if (context == nullptr || (reserved != 0 && reserved != AJM_GEN5_INITIALIZATION_FLAGS))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(g_ajm_mutex);
	*context = AllocateContextId();
	g_ajm_contexts.emplace(*context, AjmContextState {});

	return OK;
}

int KYTY_SYSV_ABI AjmFinalize(uint32_t context)
{
	PRINT_NAME();
	printf("\t context = %u\n", context);

	std::scoped_lock lock(g_ajm_mutex);
	if (g_ajm_contexts.erase(context) == 0)
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmModuleRegister(uint32_t context, uint32_t codec, int64_t reserved)
{
	PRINT_NAME();

	if (codec >= AJM_MAX_CODEC_TYPE || reserved != 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (!context_it->second.registered_codecs.insert(codec).second)
	{
		return AJM_ERROR_CODEC_ALREADY_REGISTERED;
	}

	printf("\t codec = %u\n", codec);

	switch (codec)
	{
		case 1: printf("\t %s\n", "ATRAC9 decoder"); break;
		case 2: printf("\t %s\n", "MPEG4-AAC decoder"); break;
		case 0: printf("\t %s\n", "MP3 decoder"); break;
		case 4: printf("\t %s\n", "CELP8 encoder"); break;
		case 3: printf("\t %s\n", "CELP8 decoder"); break;
		case 13: printf("\t %s\n", "CELP16 encoder"); break;
		case 12: printf("\t %s\n", "CELP16 decoder"); break;
		default: printf("\t codec %u\n", codec); break;
	}

	return OK;
}

int KYTY_SYSV_ABI AjmModuleUnregister(uint32_t context, uint32_t codec)
{
	PRINT_NAME();
	printf("\t context = %u\n", context);
	printf("\t codec   = %u\n", codec);

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (context_it->second.registered_codecs.erase(codec) == 0)
	{
		return AJM_ERROR_CODEC_NOT_REGISTERED;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmMemoryRegister(uint32_t context, const void* address, size_t num_pages)
{
	PRINT_NAME();

	if (address == nullptr || num_pages == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(g_ajm_mutex);
	if (g_ajm_contexts.find(context) == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}

	// AJM hardware requires explicit shared-memory registration. HLE decoders
	// already share the guest address space, so the validated range needs no
	// additional host mapping.
	return OK;
}

int KYTY_SYSV_ABI AjmBatchInitializeBuffer(void* buffer, size_t buffer_size, void* control)
{
	PRINT_NAME();

	if (buffer == nullptr || buffer_size == 0 || control == nullptr)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	// Gen5 Wwise supplies its own batch storage and control object. Kyty's HLE
	// batch parser has no hardware-side context to construct here.
	return OK;
}

int KYTY_SYSV_ABI AjmInstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance)
{
	PRINT_NAME();

	if (codec >= AJM_MAX_CODEC_TYPE || instance == nullptr)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	if ((flags & 0x7u) == 0)
	{
		return AJM_ERROR_WRONG_REVISION_FLAG;
	}

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	auto& state = context_it->second;
	if (state.registered_codecs.find(codec) == state.registered_codecs.end())
	{
		return AJM_ERROR_CODEC_NOT_REGISTERED;
	}
	if (state.instances_by_slot.size() >= AJM_MAX_INSTANCE_SLOT)
	{
		return AJM_ERROR_OUT_OF_RESOURCES;
	}

	uint32_t slot = state.next_instance_slot;
	do
	{
		slot = slot % AJM_MAX_INSTANCE_SLOT + 1;
	} while (state.instances_by_slot.find(slot) != state.instances_by_slot.end());

	const uint32_t instance_id = (codec << 14u) | slot;
	state.instances_by_slot.emplace(slot, instance_id);
	state.next_instance_slot = slot;
	*instance                = instance_id;

	printf("\t context  = %u\n", context);
	printf("\t codec    = %u\n", codec);
	printf("\t flags    = 0x%016" PRIx64 "\n", flags);
	printf("\t instance = 0x%08" PRIx32 "\n", instance_id);

	return OK;
}

int KYTY_SYSV_ABI AjmInstanceDestroy(uint32_t context, uint32_t instance)
{
	PRINT_NAME();

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}

	const uint32_t slot = instance & AJM_INSTANCE_SLOT_MASK;
	if (slot == 0 || context_it->second.instances_by_slot.erase(slot) == 0)
	{
		return AJM_ERROR_INVALID_INSTANCE;
	}
	g_ajm_decoded_samples.erase(instance);
	for (auto& [batch, jobs]: g_ajm_batch2_jobs)
	{
		(void)batch;
		jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [instance](const auto& job) { return job.instance == instance; }), jobs.end());
	}

	printf("\t context  = %u\n", context);
	printf("\t instance = 0x%08" PRIx32 "\n", instance);
	return OK;
}

void* KYTY_SYSV_ABI AjmBatchJobControlBufferRa(void* buffer, uint32_t instance, uint64_t flags, void* sideband_input,
                                               size_t sideband_input_size, void* sideband_output, size_t sideband_output_size,
                                               void* return_address)
{
	PRINT_NAME();
	if (buffer == nullptr || sideband_input_size > UINT32_MAX || sideband_output_size > UINT32_MAX)
	{
		return nullptr;
	}

	auto* const begin   = static_cast<uint8_t*>(buffer);
	auto*       current = begin + 8u;
	if (return_address != nullptr)
	{
		current = WriteAjmBufferChunk(current, 6u, return_address, 0);
	}
	current = WriteAjmBufferChunk(current, 2u, sideband_input, sideband_input_size);
	if (current == nullptr)
	{
		return nullptr;
	}

	constexpr uint64_t control_flags_mask = 0x000060000000e7ffull;
	flags &= control_flags_mask;
	WriteAjmU32(current, MakeAjmChunkHeader(3u, static_cast<uint32_t>(flags >> 32u)));
	WriteAjmU32(current + 4u, static_cast<uint32_t>(flags));
	current += 8u;
	current = WriteAjmBufferChunk(current, 18u, sideband_output, sideband_output_size);
	if (current == nullptr)
	{
		return nullptr;
	}

	WriteAjmU32(begin, MakeAjmChunkHeader(0u, instance));
	WriteAjmU32(begin + 4u, static_cast<uint32_t>(current - begin - 8u));
	return current;
}

void* KYTY_SYSV_ABI AjmBatchJobInlineBuffer(void* buffer, const void* data_input, size_t data_input_size, const void** batch_address)
{
	PRINT_NAME();
	if (buffer == nullptr || batch_address == nullptr || (data_input == nullptr && data_input_size != 0) ||
	    data_input_size > UINT32_MAX - 7u)
	{
		return nullptr;
	}

	auto* const  begin        = static_cast<uint8_t*>(buffer);
	const size_t aligned_size = (data_input_size + 7u) & ~size_t {7u};
	WriteAjmU32(begin, MakeAjmChunkHeader(7u));
	WriteAjmU32(begin + 4u, static_cast<uint32_t>(aligned_size));
	*batch_address = begin + 8u;
	if (data_input_size != 0)
	{
		std::memcpy(begin + 8u, data_input, data_input_size);
	}
	if (aligned_size > data_input_size)
	{
		std::memset(begin + 8u + data_input_size, 0, aligned_size - data_input_size);
	}
	return begin + 8u + aligned_size;
}

void* KYTY_SYSV_ABI AjmBatchJobRunBufferRa(void* buffer, uint32_t instance, uint64_t flags, void* data_input, size_t data_input_size,
                                           void* data_output, size_t data_output_size, void* sideband_output, size_t sideband_output_size,
                                           void* return_address)
{
	PRINT_NAME();
	if (buffer == nullptr || data_input_size > UINT32_MAX || data_output_size > UINT32_MAX || sideband_output_size > UINT32_MAX)
	{
		return nullptr;
	}

	auto* const begin   = static_cast<uint8_t*>(buffer);
	auto*       current = begin + 8u;
	if (return_address != nullptr)
	{
		current = WriteAjmBufferChunk(current, 6u, return_address, 0);
	}
	current = WriteAjmBufferChunk(current, 1u, data_input, data_input_size);

	constexpr uint64_t run_flags_mask = 0x0000e00000001fffull;
	flags &= run_flags_mask;
	WriteAjmU32(current, MakeAjmChunkHeader(4u, static_cast<uint32_t>(flags >> 32u)));
	WriteAjmU32(current + 4u, static_cast<uint32_t>(flags));
	current += 8u;
	current = WriteAjmBufferChunk(current, 17u, data_output, data_output_size);
	current = WriteAjmBufferChunk(current, 18u, sideband_output, sideband_output_size);

	WriteAjmU32(begin, MakeAjmChunkHeader(0u, instance));
	WriteAjmU32(begin + 4u, static_cast<uint32_t>(current - begin - 8u));
	return current;
}

void* KYTY_SYSV_ABI AjmBatchJobRunSplitBufferRa(void* buffer, uint32_t instance, uint64_t flags, const AjmBuffer* data_input_buffers,
                                                size_t data_input_buffer_count, const AjmBuffer* data_output_buffers,
                                                size_t data_output_buffer_count, void* sideband_output, size_t sideband_output_size,
                                                void* return_address)
{
	PRINT_NAME();
	if (buffer == nullptr || (data_input_buffers == nullptr && data_input_buffer_count != 0) ||
	    (data_output_buffers == nullptr && data_output_buffer_count != 0) || sideband_output_size > UINT32_MAX)
	{
		return nullptr;
	}

	auto* const begin   = static_cast<uint8_t*>(buffer);
	auto*       current = begin + 8u;
	if (return_address != nullptr)
	{
		current = WriteAjmBufferChunk(current, 6u, return_address, 0);
	}
	for (size_t index = 0; index < data_input_buffer_count; ++index)
	{
		current = WriteAjmBufferChunk(current, 1u, data_input_buffers[index].address, data_input_buffers[index].size);
		if (current == nullptr)
		{
			return nullptr;
		}
	}

	constexpr uint64_t run_flags_mask = 0x0000e00000001fffull;
	flags &= run_flags_mask;
	WriteAjmU32(current, MakeAjmChunkHeader(4u, static_cast<uint32_t>(flags >> 32u)));
	WriteAjmU32(current + 4u, static_cast<uint32_t>(flags));
	current += 8u;
	for (size_t index = 0; index < data_output_buffer_count; ++index)
	{
		current = WriteAjmBufferChunk(current, 17u, data_output_buffers[index].address, data_output_buffers[index].size);
		if (current == nullptr)
		{
			return nullptr;
		}
	}
	current = WriteAjmBufferChunk(current, 18u, sideband_output, sideband_output_size);
	if (current == nullptr)
	{
		return nullptr;
	}

	WriteAjmU32(begin, MakeAjmChunkHeader(0u, instance));
	WriteAjmU32(begin + 4u, static_cast<uint32_t>(current - begin - 8u));
	return current;
}

int KYTY_SYSV_ABI AjmBatchStartBuffer(uint32_t context, uint8_t* batch_buffer, uint32_t batch_size, int priority, AjmBatchError* error,
                                      uint32_t* batch_id)
{
	PRINT_NAME();
	if (error != nullptr)
	{
		std::memset(error, 0, sizeof(*error));
	}
	if (batch_buffer == nullptr || batch_id == nullptr || (batch_size & 7u) != 0 || priority < 0)
	{
		return (batch_size & 7u) != 0 ? AJM_ERROR_MALFORMED_BATCH : AJM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (!ProcessAjmBatchBuffer(batch_buffer, batch_size, context_it->second))
	{
		return AJM_ERROR_MALFORMED_BATCH;
	}

	auto& state = context_it->second;
	do
	{
		++state.next_batch_id;
		if (state.next_batch_id == 0)
		{
			++state.next_batch_id;
		}
	} while (state.completed_batches.find(state.next_batch_id) != state.completed_batches.end());
	state.completed_batches.insert(state.next_batch_id);
	*batch_id = state.next_batch_id;
	return OK;
}

int KYTY_SYSV_ABI AjmBatchWait(uint32_t context, uint32_t batch_id, uint32_t timeout, AjmBatchError* error)
{
	PRINT_NAME();
	(void)timeout;
	if (error != nullptr)
	{
		std::memset(error, 0, sizeof(*error));
	}

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (context_it->second.completed_batches.erase(batch_id) == 0)
	{
		return AJM_ERROR_INVALID_BATCH;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchCancel(uint32_t context, uint32_t batch_id)
{
	PRINT_NAME();
	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}
	if (context_it->second.completed_batches.erase(batch_id) == 0)
	{
		return AJM_ERROR_INVALID_BATCH;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobInitialize(void* batch, uint32_t instance, const void* config, size_t config_size, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || config == nullptr || config_size == 0 || config_size > AJM_MAX_BATCH2_BUFFER_SIZE)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	if (result != nullptr)
	{
		std::memset(result, 0, AJM_SIDEBAND_RESULT_SIZE);
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobClearContext(void* batch, uint32_t instance, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	if (result != nullptr)
	{
		std::memset(result, 0, AJM_SIDEBAND_RESULT_SIZE);
	}

	std::scoped_lock lock(g_ajm_mutex);
	g_ajm_decoded_samples.erase(instance);
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobSetGaplessDecode(void* batch, uint32_t instance, const void* config, uint64_t enabled, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || config == nullptr || enabled > 1)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	if (result != nullptr)
	{
		std::memset(result, 0, AJM_SIDEBAND_RESULT_SIZE);
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobGetGaplessDecode(void* batch, uint32_t instance, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	WriteAjmGaplessResult(result);
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobSetResampleParameters(void* batch, uint32_t instance, float ratio, uint32_t flags, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || !std::isfinite(ratio))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	if (result != nullptr)
	{
		std::memset(result, 0, AJM_SIDEBAND_RESULT_SIZE);
	}

	printf("\t instance = 0x%08" PRIx32 "\n", instance);
	printf("\t ratio    = %f\n", static_cast<double>(ratio));
	printf("\t flags    = 0x%08" PRIx32 "\n", flags);
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobSetResampleParametersEx(void* batch, uint32_t instance, float ratio_start,
                                                     float ratio_change_per_sample, uint32_t flags, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || !std::isfinite(ratio_start) || !std::isfinite(ratio_change_per_sample))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	if (result != nullptr)
	{
		std::memset(result, 0, AJM_SIDEBAND_RESULT_SIZE);
	}

	printf("\t instance                = 0x%08" PRIx32 "\n", instance);
	printf("\t ratio_start             = %f\n", static_cast<double>(ratio_start));
	printf("\t ratio_change_per_sample = %f\n", static_cast<double>(ratio_change_per_sample));
	printf("\t flags                   = 0x%08" PRIx32 "\n", flags);
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobGetResampleInfo(void* batch, uint32_t instance, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	WriteAjmResampleInfoResult(result);
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobDecode(void* batch, uint32_t instance, const void* data_input, size_t data_input_size, void* data_output,
                                    size_t data_output_size, void* result, void* return_address, uint64_t reserved, void* result_alias)
{
	PRINT_NAME();
	(void)return_address;
	(void)reserved;
	(void)result_alias;
	if (batch == nullptr || instance == 0 || (data_input == nullptr && data_input_size != 0) || data_output == nullptr ||
	    data_input_size > AJM_MAX_BATCH2_BUFFER_SIZE || data_output_size > AJM_MAX_BATCH2_BUFFER_SIZE)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	const AjmBuffer output {data_output, data_output_size};
	if (!QueueAjmDataJob(batch, instance, static_cast<uint32_t>(data_input_size), &output, 1u, result))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobDecodeSingle(void* batch, uint32_t instance, const void* data_input, size_t data_input_size,
                                          void* data_output, size_t data_output_size, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || (data_input == nullptr && data_input_size != 0) || data_output == nullptr ||
	    data_input_size > AJM_MAX_BATCH2_BUFFER_SIZE || data_output_size > AJM_MAX_BATCH2_BUFFER_SIZE)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	const AjmBuffer output {data_output, data_output_size};
	if (!QueueAjmDataJob(batch, instance, static_cast<uint32_t>(data_input_size), &output, 1u, result))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobDecodeSplit(void* batch, uint32_t instance, const AjmBuffer* data_input_buffers,
                                         size_t data_input_buffer_count, const AjmBuffer* data_output_buffers,
                                         size_t data_output_buffer_count, void* result)
{
	PRINT_NAME();
	uint32_t input_size  = 0;
	uint32_t output_size = 0;
	if (batch == nullptr || instance == 0 || !ValidateAjmBuffers(data_input_buffers, data_input_buffer_count, &input_size) ||
	    !ValidateAjmBuffers(data_output_buffers, data_output_buffer_count, &output_size) || output_size == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	if (!QueueAjmDataJob(batch, instance, input_size, data_output_buffers, data_output_buffer_count, result))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobEncode(void* batch, uint32_t instance, const void* data_input, size_t data_input_size, void* data_output,
                                    size_t data_output_size, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || (data_input == nullptr && data_input_size != 0) || data_output == nullptr ||
	    data_input_size > AJM_MAX_BATCH2_BUFFER_SIZE || data_output_size > AJM_MAX_BATCH2_BUFFER_SIZE)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	const AjmBuffer output {data_output, data_output_size};
	if (!QueueAjmDataJob(batch, instance, static_cast<uint32_t>(data_input_size), &output, 1u, result))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobGetInfo(void* batch, uint32_t instance, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	WriteAjmFormatResult(result);
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobGetCodecInfo(void* batch, uint32_t instance, void* result, size_t result_size)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || (result == nullptr && result_size != 0))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	if (result != nullptr)
	{
		std::memset(result, 0, result_size);
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobGetStatistics(void* batch, float interval, void* result)
{
	PRINT_NAME();
	if (batch == nullptr || !std::isfinite(interval))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	if (result != nullptr)
	{
		const AjmStatisticsResult statistics {};
		std::memcpy(result, &statistics, sizeof(statistics));
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobControl(void* batch, uint32_t instance, uint64_t flags, const void* sideband_input,
                                     size_t sideband_input_size, void* sideband_output, size_t sideband_output_size)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || (sideband_input == nullptr && sideband_input_size != 0) ||
	    sideband_output_size > UINT32_MAX)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	constexpr uint64_t control_flags_mask = 0x000060000000e7ffull;
	WriteAjmRunSideband(flags & control_flags_mask, sideband_output, static_cast<uint32_t>(sideband_output_size), instance, 0, 0, 0);
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobRun(void* batch, uint32_t instance, uint64_t flags, const void* data_input, size_t data_input_size,
                                 void* data_output, size_t data_output_size, void* sideband_output, size_t sideband_output_size)
{
	PRINT_NAME();
	if (batch == nullptr || instance == 0 || (data_input == nullptr && data_input_size != 0) || data_output == nullptr ||
	    data_input_size > AJM_MAX_BATCH2_BUFFER_SIZE || data_output_size > AJM_MAX_BATCH2_BUFFER_SIZE || sideband_output_size > UINT32_MAX)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	constexpr uint64_t run_flags_mask = 0x0000e00000001fffull;
	const AjmBuffer    output {data_output, data_output_size};
	if (!QueueAjmDataJob(batch, instance, static_cast<uint32_t>(data_input_size), &output, 1u, nullptr, flags & run_flags_mask,
	                     sideband_output, static_cast<uint32_t>(sideband_output_size)))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchJobRunSplit(void* batch, uint32_t instance, uint64_t flags, const AjmBuffer* data_input_buffers,
                                      size_t data_input_buffer_count, const AjmBuffer* data_output_buffers,
                                      size_t data_output_buffer_count, void* sideband_output, size_t sideband_output_size)
{
	PRINT_NAME();
	uint32_t input_size  = 0;
	uint32_t output_size = 0;
	if (batch == nullptr || instance == 0 || !ValidateAjmBuffers(data_input_buffers, data_input_buffer_count, &input_size) ||
	    !ValidateAjmBuffers(data_output_buffers, data_output_buffer_count, &output_size) || output_size == 0 ||
	    sideband_output_size > UINT32_MAX)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	constexpr uint64_t run_flags_mask = 0x0000e00000001fffull;
	if (!QueueAjmDataJob(batch, instance, input_size, data_output_buffers, data_output_buffer_count, nullptr, flags & run_flags_mask,
	                     sideband_output, static_cast<uint32_t>(sideband_output_size)))
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}
	return OK;
}

int KYTY_SYSV_ABI AjmBatchStart(uint32_t context, void* batch, int priority, AjmBatchError* error, uint32_t* batch_id)
{
	PRINT_NAME();
	if (error != nullptr)
	{
		std::memset(error, 0, sizeof(*error));
	}
	if (batch == nullptr || batch_id == nullptr || priority < 0)
	{
		return AJM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(g_ajm_mutex);
	auto             context_it = g_ajm_contexts.find(context);
	if (context_it == g_ajm_contexts.end())
	{
		return AJM_ERROR_INVALID_CONTEXT;
	}

	auto jobs_it = g_ajm_batch2_jobs.find(reinterpret_cast<uintptr_t>(batch));
	if (jobs_it != g_ajm_batch2_jobs.end())
	{
		for (const auto& job: jobs_it->second)
		{
			const uint32_t slot = job.instance & AJM_INSTANCE_SLOT_MASK;
			if (slot == 0 || context_it->second.instances_by_slot.find(slot) == context_it->second.instances_by_slot.end())
			{
				return AJM_ERROR_INVALID_INSTANCE;
			}
		}
		for (const auto& job: jobs_it->second)
		{
			uint64_t output_size = 0;
			for (const auto& output: job.data_outputs)
			{
				std::memset(output.address, 0, output.size);
				output_size += output.size;
			}
			auto& total = g_ajm_decoded_samples[job.instance];
			total += output_size / 4u;
			if (job.result != nullptr)
			{
				const AjmDecodeResult decode_result {
				    0, 0, job.data_input_size, static_cast<uint32_t>(std::min<uint64_t>(output_size, UINT32_MAX)), total,
				    job.data_input_size != 0 ? 1u : 0u, 0};
				std::memcpy(job.result, &decode_result, sizeof(decode_result));
			}
			WriteAjmRunSideband(job.sideband_flags, job.sideband_output, job.sideband_size, job.instance, job.data_input_size, output_size, total);
		}
		g_ajm_batch2_jobs.erase(jobs_it);
	}

	auto& state = context_it->second;
	do
	{
		++state.next_batch_id;
		if (state.next_batch_id == 0)
		{
			++state.next_batch_id;
		}
	} while (state.completed_batches.find(state.next_batch_id) != state.completed_batches.end());
	state.completed_batches.insert(state.next_batch_id);
	*batch_id = state.next_batch_id;
	return OK;
}

const char* KYTY_SYSV_ABI AjmStrError(int error)
{
	PRINT_NAME();
	printf("\t error = %d\n", error);
	return "AJM";
}

} // namespace Ajm

namespace Audio3d {

LIB_NAME("Audio3d", "Audio3d");

namespace Semaphore = LibKernel::Semaphore;

struct Audio3dOpenParameters
{
	size_t   size        = 0x20;
	uint32_t granularity = 256;
	uint32_t rate        = 0;
	uint32_t max_objects = 512;
	uint32_t queue_depth = 2;
	uint32_t buffer_mode = 2;
	uint32_t pad         = 0;
	uint32_t num_beds    = 2;
};

constexpr int AUDIO3D_ERROR_INVALID_PARAMETER = static_cast<int32_t>(0x80ea0004u);
constexpr int AUDIO3D_ERROR_INVALID_PORT      = static_cast<int32_t>(0x80ea0002u);
constexpr int AUDIO3D_ERROR_OUT_OF_RESOURCES  = static_cast<int32_t>(0x80ea0006u);

struct Audio3dData
{
	enum class State
	{
		Empty,
		Ready,
		Play
	};

	std::atomic<State> state = State::Empty;
};

struct Audio3dInternal
{
	Audio3dData*          data                        = nullptr;
	Core::Mutex*          data_mutex                  = nullptr;
	uint64_t              data_delay                  = 0;
	Semaphore::KernelSema playback_sema               = nullptr;
	Audio3dOpenParameters params                      = {};
	int                   user_id                     = 0;
	float                 late_reverb_level           = 0.0f;
	float                 downmix_spread_radius       = 2.0f;
	int                   downmix_spread_height_aware = 0;
	uint32_t              data_index                  = 0;
	bool                  used                        = false;
	std::atomic_bool      playback_finished           = false;
	int                   audio_out_handles[32]       = {};
	uint32_t              audio_out_handle_count      = 0;
};

constexpr uint32_t MAX_PORTS = 4;

static Audio3dInternal g_ports[MAX_PORTS] = {};

static void playback_simulate(void* arg)
{
	auto* port = static_cast<Audio3dInternal*>(arg);
	EXIT_IF(port == nullptr);
	EXIT_IF(port->data_mutex == nullptr);
	EXIT_IF(port->data == nullptr);

	for (;;)
	{
		int result = Semaphore::KernelWaitSema(port->playback_sema, 1, nullptr);

		if (result != OK)
		{
			break;
		}

		Audio3dData* play_data = nullptr;

		port->data_mutex->Lock();
		{
			for (uint32_t i = 0; i < port->params.queue_depth; i++)
			{
				uint32_t index = (port->data_index + i) % port->params.queue_depth;

				if (port->data[index].state == Audio3dData::State::Play)
				{
					play_data = &port->data[index];
					break;
				}
			}
		}
		port->data_mutex->Unlock();

		EXIT_IF(play_data == nullptr);

		if (play_data != nullptr)
		{
			// TODO(): Audio output is not yet implemented, so simulate audio delay
			Core::Thread::SleepMicro(port->data_delay);
			play_data->state = Audio3dData::State::Empty;
		}
	}

	port->playback_finished = true;
}

int KYTY_SYSV_ABI Audio3dInitialize(int64_t reserved)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(reserved != 0);

	return OK;
}

void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p)
{
	PRINT_NAME();

	if (p == nullptr)
	{
		return;
	}

	const Audio3dOpenParameters defaults {};
	std::memcpy(p, &defaults, defaults.size);
}

int KYTY_SYSV_ABI Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters, uint32_t* id)
{
	PRINT_NAME();

	if (parameters == nullptr || id == nullptr)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	*id = UINT32_MAX;

	Audio3dOpenParameters effective {};
	effective.size = sizeof(effective);
	switch (parameters->size & ~size_t {7})
	{
		case 0x10:
			effective.granularity = parameters->granularity;
			effective.rate        = parameters->rate;
			effective.buffer_mode = 0;
			break;
		case 0x18:
			effective.granularity = parameters->granularity;
			effective.rate        = parameters->rate;
			effective.max_objects = parameters->max_objects;
			effective.queue_depth = parameters->queue_depth;
			effective.buffer_mode = 1;
			break;
		case 0x20:
			effective.granularity = parameters->granularity;
			effective.rate        = parameters->rate;
			effective.max_objects = parameters->max_objects;
			effective.queue_depth = parameters->queue_depth;
			effective.buffer_mode = parameters->buffer_mode;
			effective.pad         = parameters->pad;
			break;
		case 0x28: effective = *parameters; break;
		default: return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	printf("\t user_id     = %d\n", user_id);
	printf("\t granularity = %u\n", effective.granularity);
	printf("\t rate        = %u\n", effective.rate);
	printf("\t max_objects = %u\n", effective.max_objects);
	printf("\t queue_depth = %u\n", effective.queue_depth);
	printf("\t buffer_mode = %u\n", effective.buffer_mode);
	printf("\t num_beds    = %u\n", effective.num_beds);

	if ((user_id != 255 && user_id != 1) || effective.rate != 0 || effective.granularity < 0x100 || (effective.granularity & 0xffu) != 0 ||
	    effective.max_objects == 0 || effective.queue_depth == 0 || effective.buffer_mode > 2)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	uint32_t port = 0;
	for (; port < MAX_PORTS; port++)
	{
		if (!g_ports[port].used)
		{
			break;
		}
	}

	if (port >= MAX_PORTS)
	{
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}

	g_ports[port].user_id = user_id;
	g_ports[port].params  = effective;
	g_ports[port].used    = true;

	EXIT_IF(g_ports[port].data != nullptr);
	EXIT_IF(g_ports[port].data_mutex != nullptr);
	EXIT_IF(g_ports[port].playback_sema != nullptr);

	g_ports[port].data       = new Audio3dData[effective.queue_depth];
	g_ports[port].data_index = 0;
	g_ports[port].data_mutex = new Core::Mutex;
	g_ports[port].data_delay = (1000000 * static_cast<uint64_t>(effective.granularity)) / 48000;

	for (uint32_t d = 0; d < effective.queue_depth; d++)
	{
		g_ports[port].data[d].state = Audio3dData::State::Empty;
	}

	int result = Semaphore::KernelCreateSema(&g_ports[port].playback_sema, "audio3d_play", 0x01, 0, static_cast<int>(effective.queue_depth),
	                                         nullptr);
	EXIT_NOT_IMPLEMENTED(result != OK);

	g_ports[port].playback_finished = false;
	Core::Thread playback_thread(playback_simulate, &g_ports[port]);
	playback_thread.Detach();

	*id = port;

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id, const void* attribute, size_t attribute_size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(attribute == nullptr);

	printf("\t attribute_id = 0x%" PRIx32 "\n", attribute_id);

	switch (attribute_id)
	{
		case 0x10001:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].late_reverb_level = *static_cast<const float*>(attribute);
			printf("\t late_reverb_level = %f\n", g_ports[port_id].late_reverb_level);
			break;
		case 0x10002:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].downmix_spread_radius = *static_cast<const float*>(attribute);
			printf("\t downmix_spread_radius = %f\n", g_ports[port_id].downmix_spread_radius);
			break;
		case 0x10003:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].downmix_spread_height_aware = *static_cast<const int*>(attribute);
			printf("\t downmix_spread_height_aware = %d\n", g_ports[port_id].downmix_spread_height_aware);
			break;
		default: EXIT("unknown attribute: 0x%" PRIx32 "\n", attribute_id);
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level, uint32_t* queue_available)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(queue_level == nullptr && queue_available == nullptr);

	auto* port = &g_ports[port_id];

	uint32_t empty_num = 0;

	port->data_mutex->Lock();
	{
		for (uint32_t i = 0; i < port->params.queue_depth; i++)
		{
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Empty)
			{
				empty_num++;
			} else
			{
				break;
			}
		}
	}
	port->data_mutex->Unlock();

	EXIT_IF(empty_num > port->params.queue_depth);

	printf("\t queue_available = %u\n", empty_num);

	if (queue_level != nullptr)
	{
		*queue_level = port->params.queue_depth - empty_num;
	}
	if (queue_available != nullptr)
	{
		*queue_available = empty_num;
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetAttributesSupported(uint32_t port_id, uint32_t* capabilities, uint32_t* num_capabilities)
{
	if (num_capabilities == nullptr)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	if (port_id >= MAX_PORTS || !g_ports[port_id].used)
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	static constexpr uint32_t supported[] = {1u, 3u, 9u};
	if (capabilities == nullptr)
	{
		*num_capabilities = std::size(supported);
		return OK;
	}

	const uint32_t count = std::min(*num_capabilities, static_cast<uint32_t>(std::size(supported)));
	std::copy_n(supported, count, capabilities);
	*num_capabilities = count;
	return OK;
}

int KYTY_SYSV_ABI Audio3dAudioOutOpen(uint32_t port_id, int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param)
{
	if (port_id >= MAX_PORTS || !g_ports[port_id].used)
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	auto& port = g_ports[port_id];
	if (len != port.params.granularity || port.audio_out_handle_count >= std::size(port.audio_out_handles))
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	const int handle = AudioOut::AudioOutOpen(user_id, type, index, len, freq, param & 0xffu);
	if (handle < 0)
	{
		return handle;
	}
	port.audio_out_handles[port.audio_out_handle_count++] = handle;
	return handle;
}

int KYTY_SYSV_ABI Audio3dAudioOutOutput(int handle, const void* data)
{
	if (data == nullptr)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	for (const auto& port: g_ports)
	{
		if (!port.used)
		{
			continue;
		}
		for (uint32_t index = 0; index < port.audio_out_handle_count; ++index)
		{
			if (port.audio_out_handles[index] == handle)
			{
				return AudioOut::AudioOutOutput(handle, data);
			}
		}
	}
	return AUDIO3D_ERROR_INVALID_PORT;
}

int KYTY_SYSV_ABI Audio3dAudioOutClose(int handle)
{
	for (auto& port: g_ports)
	{
		if (!port.used)
		{
			continue;
		}
		for (uint32_t index = 0; index < port.audio_out_handle_count; ++index)
		{
			if (port.audio_out_handles[index] != handle)
			{
				continue;
			}

			const int result = AudioOut::AudioOutClose(handle);
			if (result != OK)
			{
				return result;
			}

			std::move(port.audio_out_handles + index + 1, port.audio_out_handles + port.audio_out_handle_count,
			          port.audio_out_handles + index);
			port.audio_out_handles[--port.audio_out_handle_count] = 0;
			return OK;
		}
	}
	return AUDIO3D_ERROR_INVALID_PORT;
}

int KYTY_SYSV_ABI Audio3dPortAdvance(uint32_t port_id)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);

	auto* port = &g_ports[port_id];

	port->data_mutex->Lock();
	{
		uint32_t current_index = port->data_index;
		uint32_t next_index    = (current_index + 1) % port->params.queue_depth;

		if (port->data[current_index].state == Audio3dData::State::Empty)
		{
			port->data[current_index].state = Audio3dData::State::Ready;
		}

		EXIT_NOT_IMPLEMENTED(port->data[current_index].state != Audio3dData::State::Ready);

		port->data_index = next_index;

		printf("\t %u -> %u\n", current_index, next_index);
	}
	port->data_mutex->Unlock();

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortPush(uint32_t port_id, uint32_t blocking)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);

	auto* port = &g_ports[port_id];

	EXIT_NOT_IMPLEMENTED(blocking != 1);

	printf("\t blocking = %u\n", blocking);

	int          data_num   = 0;
	Audio3dData* first_data = nullptr;

	port->data_mutex->Lock();
	{
		first_data = port->data + port->data_index;

		for (uint32_t i = 0; i < port->params.queue_depth; i++)
		{
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Ready)
			{
				port->data[index].state = Audio3dData::State::Play;
				data_num++;
			}
		}
	}
	port->data_mutex->Unlock();

	printf("\t push num = %d\n", data_num);

	if (data_num > 0)
	{
		Semaphore::KernelSignalSema(port->playback_sema, data_num);

		if (blocking == 1)
		{
			auto wait_time = port->data_delay / 8;
			while (first_data->state != Audio3dData::State::Empty)
			{
				Core::Thread::SleepMicro(wait_time);
			}
		}
	}

	return OK;
}

} // namespace Audio3d


} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED
