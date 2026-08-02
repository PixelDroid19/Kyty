#include "Emulator/Audio.h"
#include "Emulator/AudioHost.h"
#include "Emulator/AudioVideoBackend.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Graphics/GuestTextureLayout.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
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

namespace AvPlayer {

LIB_NAME("AvPlayer", "AvPlayer");

static_assert(sizeof(AvPlayerMemAllocator) == 40, "AvPlayerMemAllocator ABI");
static_assert(sizeof(AvPlayerFileReplacement) == 40, "AvPlayerFileReplacement ABI");
static_assert(sizeof(AvPlayerEventReplacement) == 16, "AvPlayerEventReplacement ABI");
static_assert(sizeof(AvPlayerInitData) == 120 && offsetof(AvPlayerInitData, num_output_video_framebuffers) == 104 &&
              offsetof(AvPlayerInitData, default_language) == 112, "AvPlayerInitData ABI");
static_assert(sizeof(AvPlayerThreadInfo) == 48, "AvPlayerThreadInfo ABI");
static_assert(sizeof(AvPlayerInitDataEx) == 560 && offsetof(AvPlayerInitDataEx, auto_start) == 116 &&
              offsetof(AvPlayerInitDataEx, num_output_video_framebuffers) == 552, "AvPlayerInitDataEx ABI");
static_assert(sizeof(AvPlayerSourceDetails) == 128, "AvPlayerSourceDetails ABI");
static_assert(sizeof(AvPlayerAudio) == 16 && sizeof(AvPlayerAudioEx) == 80, "AvPlayerAudio ABI");
static_assert(sizeof(AvPlayerVideoEx) == 80 && offsetof(AvPlayerVideoEx, pitch) == 36 &&
              offsetof(AvPlayerVideoEx, framerate) == 48, "AvPlayerVideoEx ABI");
static_assert(sizeof(AvPlayerStreamInfo) == 32 && offsetof(AvPlayerStreamInfo, duration) == 24, "AvPlayerStreamInfo ABI");
static_assert(sizeof(AvPlayerStreamInfoEx) == 104 && offsetof(AvPlayerStreamInfoEx, duration) == 96, "AvPlayerStreamInfoEx ABI");
static_assert(offsetof(AvPlayerFrameInfoEx, details) == 24, "AvPlayerFrameInfoEx ABI");

constexpr int32_t AVPLAYER_EVENT_STATE_STOP       = 0x01;
constexpr int32_t AVPLAYER_EVENT_STATE_READY      = 0x02;
constexpr int32_t AVPLAYER_EVENT_STATE_PLAY       = 0x03;
constexpr int32_t AVPLAYER_EVENT_STATE_PAUSE      = 0x04;
constexpr int32_t AVPLAYER_ERROR_INVALID_PARAMS   = -2140536831;
constexpr int32_t AVPLAYER_ERROR_OPERATION_FAILED = -2140536830;
constexpr int32_t AVPLAYER_ERROR_NOT_SUPPORTED    = -2140536828;
constexpr int32_t AVPLAYER_WARNING_JUMP_COMPLETE  = -2140536669;
constexpr int32_t AVPLAYER_TRICK_SPEED_NORMAL     = 100;
constexpr int32_t AVPLAYER_TRICK_SPEED_MIN        = 400;
constexpr int32_t AVPLAYER_TRICK_SPEED_MAX        = 3200;

enum AvPlayerDebuglevels
{
	AvplayerDbgNone,
	AvplayerDbgInfo,
	AvplayerDbgWarnings,
	AvplayerDbgAll
};

struct AvPlayerStartInfoEx
{
	size_t   this_size               = 0;
	uint64_t start_time_milliseconds = 0;
};

struct AvPlayerInternal
{
	String               filename;
	std::atomic_bool     closing {false};
	bool                 loop = false;
	bool                 auto_start = false;
	bool                 playing = false;
	bool                 paused = false;
	bool                 stop_fired = false;
	uint64_t             start_time_ms = 0;
	AvPlayerEventReplacement event;
	AvPlayerMemAllocator mem;
	Core::Mutex          mutex;
	std::vector<uint8_t> synthetic_storage;
	uint8_t*             synthetic_storage_data = nullptr;
	struct VideoFrameBuffer
	{
		uint8_t* data        = nullptr;
		bool     guest_owned = false;
	};
	std::vector<VideoFrameBuffer> video_frames;
	std::vector<int16_t> audio_storage;
	uint32_t             synthetic_width        = 0;
	uint32_t             synthetic_height       = 0;
	float                synthetic_frame_rate   = 0.0f;
	uint32_t             synthetic_frame_count   = 0;
	uint32_t             synthetic_obtained_num = 0;
	uint32_t             next_video_frame       = 0;
	size_t               video_frame_bytes      = 0;
	int32_t              requested_framebuffers = 0;
	String               host_filename;
	mutable std::shared_mutex decoder_mutex;
	std::unique_ptr<AudioVideoBackend::Decoder> decoder;
	uint64_t             last_media_time_ms    = 0;
	uint64_t             media_duration_ms     = 0;
	uint32_t             media_audio_channels  = 0;
	uint32_t             media_audio_rate      = 0;
};

static void rgb_to_yuv(float r, float g, float b, uint8_t* y, uint8_t* u, uint8_t* v)
{
	int yf = static_cast<int>(16.0f + 65.481f * r + 128.553f * g + 24.966f * b);
	int uf = static_cast<int>(128.0f + -37.797f * r + -74.203f * g + 112.0f * b);
	int vf = static_cast<int>(128.0f + 112.0f * r + -93.786f * g + -18.214f * b);
	*y     = (yf < 0 ? 0 : (yf > 255 ? 255 : yf));
	*u     = (uf < 0 ? 0 : (uf > 255 ? 255 : uf));
	*v     = (vf < 0 ? 0 : (vf > 255 ? 255 : vf));
}

void ConvertNv12ToRgba32(const uint8_t* nv12_data, uint32_t width, uint32_t height, uint8_t* rgba_dst)
{
	if (nv12_data == nullptr || rgba_dst == nullptr || width == 0 || height == 0)
	{
		return;
	}
	const uint8_t* y_plane  = nv12_data;
	const uint8_t* uv_plane = nv12_data + (static_cast<size_t>(width) * height);

	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const uint32_t y_idx  = y * width + x;
			const uint32_t uv_idx = (y / 2) * width + (x & ~1u);

			const float y_val = static_cast<float>(y_plane[y_idx]);
			const float u_val = static_cast<float>(uv_plane[uv_idx]);
			const float v_val = static_cast<float>(uv_plane[uv_idx + 1]);

			const float c = y_val - 16.0f;
			const float d = u_val - 128.0f;
			const float e = v_val - 128.0f;

			int r = static_cast<int>(1.164f * c + 1.596f * e);
			int g = static_cast<int>(1.164f * c - 0.392f * d - 0.813f * e);
			int b = static_cast<int>(1.164f * c + 2.017f * d);

			const uint32_t rgba_idx = y_idx * 4;
			rgba_dst[rgba_idx + 0]  = static_cast<uint8_t>(std::clamp(r, 0, 255));
			rgba_dst[rgba_idx + 1]  = static_cast<uint8_t>(std::clamp(g, 0, 255));
			rgba_dst[rgba_idx + 2]  = static_cast<uint8_t>(std::clamp(b, 0, 255));
			rgba_dst[rgba_idx + 3]  = 255;
		}
	}
}

static void draw_synthetic_frame(uint32_t width, uint32_t height, void* data, float l)
{
	constexpr int STRIPS_NUM = 5;

	size_t luma_width        = width;
	size_t luma_height       = height;
	size_t chroma_width      = luma_width / 2;
	size_t chroma_height     = luma_height / 2;
	auto*  buffer            = static_cast<uint8_t*>(data);
	auto*  luma              = buffer;
	auto*  chroma            = buffer + luma_width * luma_height;
	size_t luma_strip_size   = luma_height / STRIPS_NUM;
	size_t chroma_strip_size = chroma_height / STRIPS_NUM;

	uint8_t color[STRIPS_NUM][3] = {};

	rgb_to_yuv(l, 0, 0, &color[0][0], &color[0][1], &color[0][2]);
	rgb_to_yuv(0, l, 0, &color[1][0], &color[1][1], &color[1][2]);
	rgb_to_yuv(0, 0, l, &color[2][0], &color[2][1], &color[2][2]);
	rgb_to_yuv(0, 0, 0, &color[3][0], &color[3][1], &color[3][2]);
	rgb_to_yuv(l, l, l, &color[4][0], &color[4][1], &color[4][2]);

	// Fill complete rows instead of visiting every pixel and strip in nested
	// loops. The synthetic stream is consumed at video rate, so this path must
	// remain bounded even when the guest requests several frame buffers.
	for (int si = 0; si < STRIPS_NUM; si++)
	{
		const size_t luma_strip_offset = luma_strip_size * static_cast<size_t>(si) * luma_width;
		std::fill_n(luma + luma_strip_offset, luma_strip_size * luma_width, color[si][0]);

		const uint16_t chroma_value = static_cast<uint16_t>(color[si][1]) | (static_cast<uint16_t>(color[si][2]) << 8u);
		auto* chroma_strip = reinterpret_cast<uint16_t*>(chroma + chroma_strip_size * static_cast<size_t>(si) * luma_width);
		std::fill_n(chroma_strip, chroma_strip_size * chroma_width, chroma_value);
	}
}

static bool get_video_frame_bytes(uint32_t width, uint32_t height, size_t* out)
{
	if (out == nullptr || width == 0 || height == 0)
	{
		return false;
	}
	const size_t luma_width  = width;
	const size_t luma_height = height;
	if (luma_width > SIZE_MAX / luma_height)
	{
		return false;
	}
	const size_t luma_bytes = luma_width * luma_height;
	const size_t chroma_rows = (luma_height + 1u) / 2u;
	if (luma_width > SIZE_MAX / chroma_rows)
	{
		return false;
	}
	const size_t chroma_bytes = luma_width * chroma_rows;
	if (chroma_bytes > SIZE_MAX - luma_bytes)
	{
		return false;
	}
	*out = luma_bytes + chroma_bytes;
	return true;
}

static bool avplayer_dump_enabled()
{
	const char* value = std::getenv("KYTY_DUMP_AVPLAYER");
	return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

static bool avplayer_dump_video_enabled()
{
	if (!avplayer_dump_enabled())
	{
		return false;
	}
	static std::atomic_uint32_t count {0};
	const uint32_t ordinal = count.fetch_add(1, std::memory_order_relaxed);
	if (ordinal < 128)
	{
		return true;
	}
	if (ordinal == 128)
	{
		std::fprintf(stderr, "KYTY_DUMP_AVPLAYER further video frames suppressed\n");
	}
	return false;
}

static void avplayer_dump_call(const char* name, const AvPlayerInternal* player)
{
	if (!avplayer_dump_enabled() || name == nullptr)
	{
		return;
	}
	static std::atomic_uint32_t count {0};
	const uint32_t index = count.fetch_add(1, std::memory_order_relaxed);
	if (index < 128)
	{
		std::fprintf(stderr, "KYTY_DUMP_AVPLAYER call=%s handle=%p index=%u\n", name, static_cast<const void*>(player), index);
	}
}

static void avplayer_dump_lifetime(const char* event, const AvPlayerInternal* player)
{
	if (!avplayer_dump_enabled() || event == nullptr)
	{
		return;
	}
	static std::atomic_uint32_t count {0};
	const uint32_t index = count.fetch_add(1, std::memory_order_relaxed);
	if (index < 128)
	{
		std::fprintf(stderr, "KYTY_DUMP_AVPLAYER lifetime=%s handle=%p index=%u\n", event,
		             static_cast<const void*>(player), index);
	} else if (index == 128)
	{
		std::fprintf(stderr, "KYTY_DUMP_AVPLAYER further lifetime events suppressed\n");
	}
}

static void register_video_frame(uint8_t* frame, size_t bytes, uint32_t pitch)
{
	Graphics::GuestTextureLayoutRegisterLinear(reinterpret_cast<uint64_t>(frame), bytes, pitch);
}

static void unregister_video_frame(uint8_t* frame)
{
	Graphics::GuestTextureLayoutUnregister(reinterpret_cast<uint64_t>(frame));
}

static void release_video_frames(const AvPlayerMemAllocator& mem, std::vector<AvPlayerInternal::VideoFrameBuffer>& frames)
{
	for (const auto& frame: frames)
	{
		if (frame.data == nullptr)
		{
			continue;
		}
		unregister_video_frame(frame.data);
		if (frame.guest_owned && mem.deallocate_texture != nullptr)
		{
			Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(mem.deallocate_texture), reinterpret_cast<uint64_t>(mem.object_pointer),
			                          reinterpret_cast<uint64_t>(frame.data), 0);
		}
	}
	frames.clear();
}

static void release_synthetic_video(AvPlayerInternal* r)
{
	if (r == nullptr)
	{
		return;
	}
	release_video_frames(r->mem, r->video_frames);
	r->synthetic_storage.clear();
	r->synthetic_storage_data = nullptr;
	r->video_frame_bytes = 0;
	r->next_video_frame  = 0;
}

static bool create_synthetic_video(AvPlayerInternal* r, int32_t requested_framebuffers)
{
	uint32_t luma_width  = (r != nullptr && r->decoder != nullptr && r->synthetic_width > 0) ? r->synthetic_width : 1920;
	uint32_t luma_height = (r != nullptr && r->decoder != nullptr && r->synthetic_height > 0) ? r->synthetic_height : 1080;
	float    frame_rate  = (r != nullptr && r->decoder != nullptr && r->synthetic_frame_rate > 0.0f) ? r->synthetic_frame_rate : 59.94f;
	uint32_t frame_count = (r != nullptr && r->decoder != nullptr && r->synthetic_frame_count > 0) ? r->synthetic_frame_count : 90;
	size_t   size        = 0;

	if (r == nullptr || !get_video_frame_bytes(luma_width, luma_height, &size))
	{
		return false;
	}

	release_synthetic_video(r);
	if (r->closing.load(std::memory_order_acquire))
	{
		return false;
	}

	if (r->mem.allocate_texture != nullptr)
	{
		if (r->mem.deallocate_texture == nullptr || size > UINT32_MAX)
		{
			return false;
		}
		const int count = std::clamp(requested_framebuffers > 0 ? requested_framebuffers : 2, 2, 16);
		r->video_frames.reserve(static_cast<size_t>(count));
		for (int i = 0; i < count; i++)
		{
			auto* frame = static_cast<uint8_t*>(reinterpret_cast<void*>(
			    Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(r->mem.allocate_texture),
			                              reinterpret_cast<uint64_t>(r->mem.object_pointer),
			                              256, static_cast<uint32_t>(size))));
			if (frame == nullptr)
			{
				release_synthetic_video(r);
				return false;
			}
			r->video_frames.push_back(AvPlayerInternal::VideoFrameBuffer {frame, true});
			register_video_frame(frame, size, luma_width);
			if (r->closing.load(std::memory_order_acquire))
			{
				release_synthetic_video(r);
				return false;
			}
		}
	} else
	{
		if (size > SIZE_MAX - 255u)
		{
			return false;
		}
		r->synthetic_storage.assign(size + 255u, 0);
		const auto raw_address     = reinterpret_cast<uintptr_t>(r->synthetic_storage.data());
		const auto aligned_address = (raw_address + 255u) & ~static_cast<uintptr_t>(255u);
		r->synthetic_storage_data  = reinterpret_cast<uint8_t*>(aligned_address);
		r->video_frames.push_back(AvPlayerInternal::VideoFrameBuffer {r->synthetic_storage_data, false});
		register_video_frame(r->synthetic_storage_data, size, luma_width);
		if (r->closing.load(std::memory_order_acquire))
		{
			release_synthetic_video(r);
			return false;
		}
	}

	r->synthetic_width        = luma_width;
	r->synthetic_height       = luma_height;
	r->synthetic_frame_rate   = frame_rate;
	r->synthetic_frame_count  = frame_count;
	r->synthetic_obtained_num = 0;
	r->next_video_frame       = 0;
	r->video_frame_bytes      = size;
	r->audio_storage.assign(2 * 1024, 0);
	if (avplayer_dump_enabled())
	{
		std::fprintf(stderr, "KYTY_DUMP_AVPLAYER create frames=%zu size=%zu pitch=%u allocator=%d real=%d\n", r->video_frames.size(), size,
		             luma_width, r->mem.allocate_texture != nullptr ? 1 : 0, r->decoder != nullptr ? 1 : 0);
	}
	return true;
}

static void delete_synthetic_video(AvPlayerInternal* r)
{
	if (r == nullptr)
	{
		return;
	}
	std::vector<AvPlayerInternal::VideoFrameBuffer> frames;
	std::vector<uint8_t>                         storage;
	AvPlayerMemAllocator                          mem;
	{
		std::unique_lock<std::shared_mutex> decoder_lock(r->decoder_mutex);
		{
			Core::LockGuard lock(r->mutex);
			r->decoder.reset();
			mem = r->mem;
			frames.swap(r->video_frames);
			storage.swap(r->synthetic_storage);
			r->synthetic_storage_data = nullptr;
			r->audio_storage.clear();
			r->synthetic_width        = 0;
			r->synthetic_height       = 0;
			r->synthetic_frame_rate   = 0.0f;
			r->synthetic_frame_count  = 0;
			r->synthetic_obtained_num = 0;
			r->next_video_frame       = 0;
			r->video_frame_bytes      = 0;
			r->host_filename          = String();
			r->last_media_time_ms     = 0;
			r->media_duration_ms      = 0;
			r->media_audio_channels   = 0;
			r->media_audio_rate       = 0;
		}
	}
	release_video_frames(mem, frames);
}

static void fill_video_ex(const AvPlayerInternal* r, AvPlayerVideoEx* video)
{
	std::memset(video, 0, sizeof(*video));
	video->width                 = r->synthetic_width;
	video->height                = r->synthetic_height;
	video->aspect_ratio          = static_cast<float>(r->synthetic_width) / static_cast<float>(r->synthetic_height);
	video->language_code[0]      = 'u';
	video->language_code[1]      = 'n';
	video->language_code[2]      = 'd';
	video->pitch                 = r->synthetic_width;
	video->luma_bit_depth        = 8;
	video->chroma_bit_depth      = 8;
	video->video_full_tange_flag = 0;
	video->framerate             = static_cast<double>(r->synthetic_frame_rate);
}

static void fill_audio(AvPlayerAudio* audio, uint32_t size, uint32_t channels = 2, uint32_t sample_rate = 48000)
{
	std::memset(audio, 0, sizeof(*audio));
	audio->channel_count    = static_cast<uint16_t>(channels);
	audio->sample_rate      = sample_rate;
	audio->size             = size;
	audio->language_code[0] = 'u';
	audio->language_code[1] = 'n';
	audio->language_code[2] = 'd';
}

static void fill_audio_ex(AvPlayerAudioEx* audio, uint32_t size, uint32_t channels = 2, uint32_t sample_rate = 48000)
{
	std::memset(audio, 0, sizeof(*audio));
	audio->channel_count    = static_cast<uint16_t>(channels);
	audio->sample_rate      = sample_rate;
	audio->size             = size;
	audio->language_code[0] = 'u';
	audio->language_code[1] = 'n';
	audio->language_code[2] = 'd';
}

static String sanitize_avplayer_uri(const char* name, uint32_t length = 0)
{
	constexpr size_t kMaxUriLength = 4096;
	if (name == nullptr)
	{
		return String();
	}
	const size_t source_length = length != 0 ? static_cast<size_t>(length) : ::strnlen(name, kMaxUriLength + 1);
	if (source_length == 0 || source_length > kMaxUriLength)
	{
		return String();
	}
	std::string s(name, source_length);

	if (s.rfind("file:///", 0) == 0)
	{
		s = s.substr(7);
	} else if (s.rfind("file://", 0) == 0)
	{
		s = s.substr(7);
	}

	std::string decoded;
	decoded.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
		    std::isxdigit(static_cast<unsigned char>(s[i + 2])))
		{
			int value = 0;
			std::sscanf(s.substr(i + 1, 2).c_str(), "%x", &value);
			decoded.push_back(static_cast<char>(value));
			i += 2;
		} else
		{
			decoded.push_back(s[i]);
		}
	}
	String guest_path = String::FromUtf8(decoded.c_str());

	if (Core::File::IsFileExisting(guest_path))
	{
		return guest_path;
	}

	if (LibKernel::FileSystem::IsMounted())
	{
		String real_path = LibKernel::FileSystem::GetRealFilename(guest_path);
		if (!real_path.IsEmpty() && Core::File::IsFileExisting(real_path))
		{
			return real_path;
		}

		String resolved_path = LibKernel::FileSystem::PreferHostApp0DataSegment(guest_path, real_path);
		if (!resolved_path.IsEmpty() && Core::File::IsFileExisting(resolved_path))
		{
			return resolved_path;
		}
	}

	return guest_path;
}

static bool synthetic_is_playing(const AvPlayerInternal* r)
{
	if (r == nullptr)
	{
		return false;
	}
	if (r->decoder != nullptr)
	{
		return !r->decoder->EndOfStream();
	}
	return r->synthetic_obtained_num < r->synthetic_frame_count;
}

static uint64_t current_time_ms(const AvPlayerInternal* r)
{
	if (r == nullptr)
	{
		return 0;
	}
	if (r->decoder != nullptr)
	{
		return r->start_time_ms + r->last_media_time_ms;
	}
	if (r->synthetic_frame_rate <= 0.0f)
	{
		return r->start_time_ms;
	}
	return r->start_time_ms +
	       static_cast<uint64_t>(1000.0f * (static_cast<float>(r->synthetic_obtained_num) / r->synthetic_frame_rate));
}

static void emit_event(AvPlayerInternal* h, int32_t event_id, void* data = nullptr)
{
	AvPlayerEventCallback callback = nullptr;
	void*                 obj_ptr  = nullptr;
	if (h != nullptr && !h->closing.load(std::memory_order_acquire))
	{
		Core::LockGuard lock(h->mutex);
		callback = h->event.event_callback;
		obj_ptr  = h->event.object_pointer;
	}
	if (callback != nullptr)
	{
		const uint64_t result = Loader::GuestCall::Invoke4(reinterpret_cast<uint64_t>(callback),
		                                                   reinterpret_cast<uint64_t>(obj_ptr),
		                                                   static_cast<uint64_t>(event_id),
		                                                   0,
		                                                   reinterpret_cast<uint64_t>(data));
		if (avplayer_dump_enabled())
		{
			std::fprintf(stderr, "KYTY_DUMP_AVPLAYER event handle=%p id=0x%x callback=%p data=%p result=0x%" PRIx64 "\n",
			             static_cast<void*>(h), event_id, reinterpret_cast<void*>(callback), data, result);
		}
	}
}

static void stop_once_after_eof(AvPlayerInternal* h)
{
	bool fire_stop = false;
	if (h == nullptr)
	{
		return;
	}
	{
		Core::LockGuard lock(h->mutex);
		if (h->playing && !synthetic_is_playing(h))
		{
			if (h->loop)
			{
				h->synthetic_obtained_num = 0;
			} else if (!h->stop_fired)
			{
				h->playing    = false;
				h->stop_fired = true;
				fire_stop     = true;
			}
		}
	}
	if (fire_stop)
	{
		emit_event(h, AVPLAYER_EVENT_STATE_STOP);
	}
}

static bool get_synthetic_video(AvPlayerInternal* r, AvPlayerFrameInfoEx* info)
{
	if (r == nullptr || info == nullptr)
	{
		return false;
	}
	std::shared_lock<std::shared_mutex> decoder_lock(r->decoder_mutex);
	uint8_t*                             frame       = nullptr;
	uint64_t                             timestamp   = 0;
	uint32_t                             width       = 0;
	uint32_t                             height      = 0;
	float                                frame_rate  = 0.0f;
	uint32_t                             frame_index = 0;
	uint32_t                             frame_count = 0;
	size_t                               frame_bytes = 0;
	AudioVideoBackend::Decoder*           decoder    = nullptr;

	{
		Core::LockGuard lock(r->mutex);
		if (!r->playing || r->paused || r->video_frames.empty() || !synthetic_is_playing(r))
		{
			return false;
		}
		timestamp   = current_time_ms(r);
		width       = r->synthetic_width;
		height      = r->synthetic_height;
		frame_rate  = r->synthetic_frame_rate;
		frame_index = r->synthetic_obtained_num;
		frame_count = r->synthetic_frame_count;
		frame_bytes = r->video_frame_bytes;
		decoder     = r->decoder.get();
	}

	const bool is_real = decoder != nullptr;
	if (is_real)
	{
		AudioVideoBackend::VideoFrame decoded;
		if (!decoder->TryReadVideoFrame(&decoded) || decoded.width != width || decoded.height != height || decoded.pitch != width ||
		    decoded.data.size() != frame_bytes)
		{
			return false;
		}
		{
			Core::LockGuard lock(r->mutex);
			frame = r->video_frames[r->next_video_frame++ % r->video_frames.size()].data;
		}
		if (frame == nullptr)
		{
			return false;
		}
		Graphics::GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(frame), decoded.data.size());
		std::memcpy(frame, decoded.data.data(), decoded.data.size());
		timestamp = decoded.timestamp_ms;
		Core::LockGuard lock(r->mutex);
		r->synthetic_obtained_num++;
		r->last_media_time_ms = timestamp;
	} else
	{
		{
			Core::LockGuard lock(r->mutex);
			frame = r->video_frames[r->next_video_frame++ % r->video_frames.size()].data;
		}
		if (frame == nullptr)
		{
			return false;
		}
		const float pos = (frame_count != 0 ? static_cast<float>(frame_index) / static_cast<float>(frame_count) : 0.0f);
		float       level = 1.0f;

		if (pos < 0.2f)
		{
			level = pos * pos * ((1.0f / 0.2f) * (1.0f / 0.2f));
		} else if (pos > 0.5f)
		{
			level = 1.0f - (1.0f - pos * (1.0f / 0.5f)) * (1.0f - pos * (1.0f / 0.5f));
		}

		Graphics::GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(frame), frame_bytes);
		draw_synthetic_frame(width, height, frame, level * 0.7f);
		Core::LockGuard lock(r->mutex);
		r->synthetic_obtained_num++;
	}

	std::memset(info, 0, sizeof(*info));
	info->data                                = frame;
	info->time_stamp                          = timestamp;
	info->details.video.width                 = width;
	info->details.video.height                = height;
	info->details.video.aspect_ratio          = (height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 16.0f / 9.0f);
	info->details.video.language_code[0]      = 'u';
	info->details.video.language_code[1]      = 'n';
	info->details.video.language_code[2]      = 'd';
	info->details.video.pitch                 = width;
	info->details.video.luma_bit_depth        = 8;
	info->details.video.chroma_bit_depth      = 8;
	info->details.video.video_full_tange_flag = 0;
	info->details.video.framerate             = static_cast<double>(frame_rate);

	if (avplayer_dump_video_enabled())
	{
		std::fprintf(stderr, "KYTY_DUMP_AVPLAYER video handle=%p frame=%u data=%p time=%" PRIu64 " size=%ux%u real=%d\n",
		             static_cast<void*>(r), frame_index + 1, info->data, info->time_stamp, width, height, is_real ? 1 : 0);
	}
	return true;
}

using AvPlayerRef = std::shared_ptr<AvPlayerInternal>;

static std::mutex                                      g_avplayer_registry_mutex;
static std::unordered_map<AvPlayerInternal*, AvPlayerRef> g_avplayer_registry;

static void finalize_player(AvPlayerInternal* player)
{
	if (player == nullptr)
	{
		return;
	}
	player->closing.store(true, std::memory_order_release);
	avplayer_dump_lifetime("finalize-begin", player);
	delete_synthetic_video(player);
	avplayer_dump_lifetime("finalize-end", player);
	delete player;
}

static AvPlayerInternal* register_player(AvPlayerRef player)
{
	if (!player)
	{
		return nullptr;
	}
	AvPlayerInternal* raw = player.get();
	{
		std::lock_guard<std::mutex> lock(g_avplayer_registry_mutex);
		g_avplayer_registry.emplace(raw, std::move(player));
	}
	avplayer_dump_lifetime("register", raw);
	return raw;
}

static AvPlayerRef acquire_player(AvPlayerInternal* handle)
{
	if (handle == nullptr)
	{
		avplayer_dump_lifetime("reject", handle);
		return {};
	}
	AvPlayerRef player;
	{
		std::lock_guard<std::mutex> lock(g_avplayer_registry_mutex);
		auto                        it = g_avplayer_registry.find(handle);
		if (it != g_avplayer_registry.end())
		{
			player = it->second;
		}
	}
	if (!player)
	{
		avplayer_dump_lifetime("reject", handle);
		return {};
	}
	return player;
}

static AvPlayerRef remove_player(AvPlayerInternal* handle)
{
	if (handle == nullptr)
	{
		avplayer_dump_lifetime("reject", handle);
		return {};
	}
	AvPlayerRef player;
	bool        rejected = false;
	{
		std::lock_guard<std::mutex> lock(g_avplayer_registry_mutex);
		auto                        it = g_avplayer_registry.find(handle);
		if (it == g_avplayer_registry.end())
		{
			rejected = true;
		} else
		{
			player = std::move(it->second);
			g_avplayer_registry.erase(it);
		}
	}
	if (rejected)
	{
		avplayer_dump_lifetime("reject", handle);
		return {};
	}
	player->closing.store(true, std::memory_order_release);
	avplayer_dump_lifetime("close-remove", handle);
	return player;
}

static AvPlayerRef create_player(const AvPlayerMemAllocator& mem, const AvPlayerEventReplacement& event, bool auto_start,
	                             int32_t requested_framebuffers)
{
	AvPlayerRef r(new AvPlayerInternal, finalize_player);
	r->mem                    = mem;
	r->event                  = event;
	r->auto_start             = auto_start;
	r->requested_framebuffers = requested_framebuffers;
	r->synthetic_width        = 1920;
	r->synthetic_height       = 1080;
	r->synthetic_frame_rate   = 59.94f;
	r->synthetic_frame_count  = 90;
	r->synthetic_obtained_num = 0;
	r->next_video_frame       = 0;
	r->audio_storage.assign(2 * 1024, 0);
	return r;
}

AvPlayerInternal* KYTY_SYSV_ABI AvPlayerInit(AvPlayerInitData* init)
{
	PRINT_NAME();
	if (init == nullptr)
	{
		return nullptr;
	}
	return register_player(create_player(init->memory_replacement, init->event_replacement, init->auto_start != 0,
	                                     init->num_output_video_framebuffers));
}

int KYTY_SYSV_ABI AvPlayerInitEx(const AvPlayerInitDataEx* init, AvPlayerInternal** handle)
{
	PRINT_NAME();
	if (init == nullptr || handle == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	*handle = register_player(create_player(init->memory_replacement, init->event_replacement, init->auto_start != 0,
	                                        init->num_output_video_framebuffers));
	return *handle == nullptr ? AVPLAYER_ERROR_OPERATION_FAILED : 0;
}

int KYTY_SYSV_ABI AvPlayerPostInit(AvPlayerInternal* h, const void* post_init)
{
	PRINT_NAME();
	(void)post_init;
	auto player = acquire_player(h);
	return player == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS : 0;
}

static int start_player(AvPlayerInternal* h);

static int add_source(AvPlayerInternal* h, const char* raw_filename, uint32_t length = 0)
{
	if (h == nullptr || raw_filename == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	if (h->closing.load(std::memory_order_acquire))
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	String clean_filename = sanitize_avplayer_uri(raw_filename, length);
	if (clean_filename.IsEmpty())
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	delete_synthetic_video(h);

	String host_filename = clean_filename;
	if (!Core::File::IsFileExisting(clean_filename) && LibKernel::FileSystem::IsMounted())
	{
		String mounted_filename = LibKernel::FileSystem::GetRealFilename(clean_filename);
		if (!mounted_filename.IsEmpty())
		{
			host_filename = mounted_filename;
		}
	}
	std::string decoder_error;
	auto decoder = AudioVideoBackend::Decoder::Open(host_filename.C_Str(), &decoder_error);
	if (decoder == nullptr || !decoder->GetStreamInfo().has_video)
	{
		if (avplayer_dump_enabled())
		{
			std::fprintf(stderr, "KYTY_DUMP_AVPLAYER backend=error path=%s reason=%s\n", host_filename.C_Str(),
			             decoder_error.empty() ? "media has no supported video stream" : decoder_error.c_str());
		}
		return AVPLAYER_ERROR_OPERATION_FAILED;
	}
	bool auto_start = false;
	{
		Core::LockGuard lock(h->mutex);
		h->filename               = clean_filename;
		h->host_filename          = host_filename;
		h->synthetic_obtained_num = 0;
		h->last_media_time_ms     = 0;
		h->media_duration_ms      = 0;
		h->media_audio_channels   = 0;
		h->media_audio_rate       = 0;
		h->playing                = false;
		h->paused                 = false;
		h->stop_fired             = false;
		if (decoder != nullptr)
		{
			h->decoder = std::move(decoder);
			const auto& stream_info = h->decoder->GetStreamInfo();
			h->synthetic_width      = stream_info.video_width;
			h->synthetic_height     = stream_info.video_height;
			h->synthetic_frame_rate = static_cast<float>(stream_info.video_frame_rate > 0.0 ? stream_info.video_frame_rate : 60.0);
			h->media_duration_ms    = stream_info.duration_ms;
			h->media_audio_channels = stream_info.audio_channels;
			h->media_audio_rate     = stream_info.audio_sample_rate;
			const double estimated_frames = stream_info.duration_ms > 0
			                                   ? (static_cast<double>(stream_info.duration_ms) / 1000.0) * h->synthetic_frame_rate
			                                   : 300.0;
			h->synthetic_frame_count = static_cast<uint32_t>(std::clamp(estimated_frames, 1.0, static_cast<double>(UINT32_MAX)));
			if (avplayer_dump_enabled())
			{
				std::fprintf(stderr, "KYTY_DUMP_AVPLAYER backend=%s path=%s %ux%u fps=%.3f duration=%" PRIu64 " audio=%u/%u\n",
				             AudioVideoBackend::Decoder::BackendName(), host_filename.C_Str(), h->synthetic_width, h->synthetic_height,
				             h->synthetic_frame_rate, h->media_duration_ms, h->media_audio_channels, h->media_audio_rate);
			}
		}
		auto_start                = h->auto_start;
	}

	if (!create_synthetic_video(h, h->requested_framebuffers))
	{
		return AVPLAYER_ERROR_OPERATION_FAILED;
	}
	if (h->closing.load(std::memory_order_acquire))
	{
		return AVPLAYER_ERROR_OPERATION_FAILED;
	}
	emit_event(h, AVPLAYER_EVENT_STATE_READY);
	if (h->closing.load(std::memory_order_acquire))
	{
		return AVPLAYER_ERROR_OPERATION_FAILED;
	}
	if (avplayer_dump_enabled())
	{
		std::fprintf(stderr, "KYTY_DUMP_AVPLAYER source handle=%p auto_start=%d\n", static_cast<void*>(h), auto_start ? 1 : 0);
	}
	if (auto_start)
	{
		return start_player(h);
	}
	return 0;
}

int KYTY_SYSV_ABI AvPlayerAddSource(AvPlayerInternal* h, const char* filename)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	return player == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS : add_source(player.get(), filename);
}

int KYTY_SYSV_ABI AvPlayerAddSourceEx(AvPlayerInternal* h, uint32_t uri_type, const AvPlayerSourceDetails* source_details)
{
	PRINT_NAME();
	if (uri_type != AvPlayerUriTypeSource || source_details == nullptr || source_details->uri.name == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	auto player = acquire_player(h);
	return player == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS : add_source(player.get(), source_details->uri.name, source_details->uri.length);
}

static int stream_count(AvPlayerInternal* h)
{
	if (h->filename.IsEmpty())
	{
		return 0;
	}
	return h->media_audio_channels != 0 ? 2 : 1;
}

int KYTY_SYSV_ABI AvPlayerStreamCount(AvPlayerInternal* h)
{
	PRINT_NAME();
	avplayer_dump_call("stream_count", h);
	auto player = acquire_player(h);
	return player == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS : stream_count(player.get());
}

int KYTY_SYSV_ABI AvPlayerGetStreamInfo(AvPlayerInternal* h, uint32_t stream_id, AvPlayerStreamInfo* info)
{
	PRINT_NAME();
	avplayer_dump_call("stream_info", h);
	auto player = acquire_player(h);
	if (player == nullptr || info == nullptr || stream_id >= static_cast<uint32_t>(stream_count(player.get())))
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h = player.get();
	std::memset(info, 0, sizeof(*info));
	info->type     = (stream_id == 0 ? AvPlayerStreamVideo : AvPlayerStreamAudio);
	info->duration = h->media_duration_ms != 0
	                     ? h->media_duration_ms
	                     : static_cast<uint64_t>(1000.0f * (static_cast<float>(h->synthetic_frame_count) /
	                                                          (h->synthetic_frame_rate > 0.0f ? h->synthetic_frame_rate : 60.0f)));
	if (stream_id == 0)
	{
		info->details.video.width        = h->synthetic_width;
		info->details.video.height       = h->synthetic_height;
		info->details.video.aspect_ratio = static_cast<float>(h->synthetic_width) / static_cast<float>(h->synthetic_height);
		info->details.video.language_code[0]  = 'u';
		info->details.video.language_code[1]  = 'n';
		info->details.video.language_code[2]  = 'd';
	} else
	{
		fill_audio(&info->details.audio, static_cast<uint32_t>(h->audio_storage.size() * sizeof(int16_t)),
		           h->media_audio_channels != 0 ? h->media_audio_channels : 2,
		           h->media_audio_rate != 0 ? h->media_audio_rate : 48000);
	}
	return 0;
}

int KYTY_SYSV_ABI AvPlayerGetStreamInfoEx(AvPlayerInternal* h, uint32_t stream_id, AvPlayerStreamInfoEx* info)
{
	PRINT_NAME();
	avplayer_dump_call("stream_info_ex", h);
	auto player = acquire_player(h);
	if (player == nullptr || info == nullptr || stream_id >= static_cast<uint32_t>(stream_count(player.get())))
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h = player.get();
	std::memset(info, 0, sizeof(*info));
	info->this_size = sizeof(*info);
	info->type      = (stream_id == 0 ? AvPlayerStreamVideo : AvPlayerStreamAudio);
	info->duration  = h->media_duration_ms != 0
	                      ? h->media_duration_ms
	                      : static_cast<uint64_t>(1000.0f * (static_cast<float>(h->synthetic_frame_count) /
	                                                           (h->synthetic_frame_rate > 0.0f ? h->synthetic_frame_rate : 60.0f)));
	if (stream_id == 0)
	{
		fill_video_ex(h, &info->details.video);
	} else
	{
		fill_audio_ex(&info->details.audio, static_cast<uint32_t>(h->audio_storage.size() * sizeof(int16_t)),
		              h->media_audio_channels != 0 ? h->media_audio_channels : 2,
		              h->media_audio_rate != 0 ? h->media_audio_rate : 48000);
	}
	return 0;
}

int KYTY_SYSV_ABI AvPlayerEnableStream(AvPlayerInternal* h, uint32_t stream_id)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	return player == nullptr || stream_id > 1 ? AVPLAYER_ERROR_INVALID_PARAMS : 0;
}

int KYTY_SYSV_ABI AvPlayerDisableStream(AvPlayerInternal* h, uint32_t stream_id)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	return player == nullptr || stream_id > 1 ? AVPLAYER_ERROR_INVALID_PARAMS : 0;
}

int KYTY_SYSV_ABI AvPlayerChangeStream(AvPlayerInternal* h, uint32_t old_stream_id, uint32_t new_stream_id)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	return player == nullptr || old_stream_id > 1 || new_stream_id > 1 ? AVPLAYER_ERROR_INVALID_PARAMS : 0;
}

static int start_player(AvPlayerInternal* h)
{
	if (h->closing.load(std::memory_order_acquire))
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	{
		Core::LockGuard lock(h->mutex);
		h->playing           = true;
		h->paused            = false;
		h->stop_fired        = false;
		h->synthetic_obtained_num = 0;
		h->start_time_ms     = 0;
	}
	emit_event(h, AVPLAYER_EVENT_STATE_PLAY);
	if (avplayer_dump_enabled())
	{
		std::fprintf(stderr, "KYTY_DUMP_AVPLAYER start handle=%p\n", static_cast<void*>(h));
	}
	return 0;
}

int KYTY_SYSV_ABI AvPlayerStart(AvPlayerInternal* h)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	return player == nullptr ? AVPLAYER_ERROR_INVALID_PARAMS : start_player(player.get());
}

int KYTY_SYSV_ABI AvPlayerStartEx(AvPlayerInternal* h, const void* start_info_ex)
{
	PRINT_NAME();
	avplayer_dump_call("start_ex_call", h);
	auto player = acquire_player(h);
	if (player == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h = player.get();
	const auto* info = static_cast<const AvPlayerStartInfoEx*>(start_info_ex);
	{
		Core::LockGuard lock(h->mutex);
		h->start_time_ms     = (info != nullptr ? info->start_time_milliseconds : 0);
		h->synthetic_obtained_num = static_cast<uint32_t>(
		    std::min<uint64_t>(h->synthetic_frame_count, static_cast<uint64_t>(h->start_time_ms * h->synthetic_frame_rate / 1000.0f)));
		h->playing    = true;
		h->paused     = false;
		h->stop_fired = false;
	}
	emit_event(h, AVPLAYER_EVENT_STATE_PLAY);
	if (avplayer_dump_enabled())
	{
		std::fprintf(stderr, "KYTY_DUMP_AVPLAYER start_ex handle=%p time=%" PRIu64 "\n", static_cast<void*>(h), h->start_time_ms);
	}
	return 0;
}

int KYTY_SYSV_ABI AvPlayerStop(AvPlayerInternal* h)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	if (player == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h = player.get();
	{
		Core::LockGuard lock(h->mutex);
		h->playing    = false;
		h->paused     = false;
		h->stop_fired = true;
	}
	emit_event(h, AVPLAYER_EVENT_STATE_STOP);
	return 0;
}

int KYTY_SYSV_ABI AvPlayerPause(AvPlayerInternal* h)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	if (player == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h = player.get();
	{
		Core::LockGuard lock(h->mutex);
		h->paused = true;
	}
	emit_event(h, AVPLAYER_EVENT_STATE_PAUSE);
	return 0;
}

int KYTY_SYSV_ABI AvPlayerResume(AvPlayerInternal* h)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	if (player == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h = player.get();
	{
		Core::LockGuard lock(h->mutex);
		h->paused = false;
	}
	emit_event(h, AVPLAYER_EVENT_STATE_PLAY);
	return 0;
}

int KYTY_SYSV_ABI AvPlayerSetLooping(AvPlayerInternal* h, Bool loop)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	if (player == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h = player.get();
	Core::LockGuard lock(h->mutex);
	h->loop = (loop != 0);
	return 0;
}

int KYTY_SYSV_ABI AvPlayerSetAvSyncMode(AvPlayerInternal* h, uint32_t sync_mode)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	return player == nullptr || sync_mode > 1 ? AVPLAYER_ERROR_INVALID_PARAMS : 0;
}

int KYTY_SYSV_ABI AvPlayerSetAvailableBandwidth(AvPlayerInternal* h, uint32_t start_bandwidth, uint32_t minimum_bandwidth,
                                                uint32_t maximum_bandwidth)
{
	PRINT_NAME();
	(void)start_bandwidth;
	auto player = acquire_player(h);
	return player == nullptr || (minimum_bandwidth != 0 && maximum_bandwidth != 0 && minimum_bandwidth > maximum_bandwidth)
	           ? AVPLAYER_ERROR_INVALID_PARAMS
	           : 0;
}

int KYTY_SYSV_ABI AvPlayerSetTrickSpeed(AvPlayerInternal* h, int32_t trick_speed)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	if (player == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	if (trick_speed != AVPLAYER_TRICK_SPEED_NORMAL &&
	    (trick_speed > -AVPLAYER_TRICK_SPEED_MIN && trick_speed < AVPLAYER_TRICK_SPEED_MIN))
	{
		return AVPLAYER_ERROR_NOT_SUPPORTED;
	}
	return 0;
}

static Bool get_video_data_ex(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info);

Bool KYTY_SYSV_ABI AvPlayerGetVideoData(AvPlayerInternal* h, AvPlayerFrameInfo* video_info)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	if (player == nullptr || video_info == nullptr)
	{
		return 0;
	}
	AvPlayerFrameInfoEx ex {};
	Bool                ok = get_video_data_ex(player.get(), &ex);
	if (ok == 0)
	{
		return 0;
	}
	std::memset(video_info, 0, sizeof(*video_info));
	video_info->data                       = ex.data;
	video_info->time_stamp                 = ex.time_stamp;
	video_info->details.video.width        = ex.details.video.width;
	video_info->details.video.height       = ex.details.video.height;
	video_info->details.video.aspect_ratio = ex.details.video.aspect_ratio;
	std::memcpy(video_info->details.video.language_code, ex.details.video.language_code, 4);
	return 1;
}

static Bool get_video_data_ex(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info)
{
	if (video_info == nullptr)
	{
		return 0;
	}
	if (get_synthetic_video(h, video_info))
	{
		return 1;
	}
	stop_once_after_eof(h);
	return 0;
}

Bool KYTY_SYSV_ABI AvPlayerGetVideoDataEx(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info)
{
	PRINT_NAME();
	avplayer_dump_call("video_call", h);
	auto player = acquire_player(h);
	return player == nullptr ? 0 : get_video_data_ex(player.get(), video_info);
}

Bool KYTY_SYSV_ABI AvPlayerGetAudioData(AvPlayerInternal* h, AvPlayerFrameInfo* audio_info)
{
	PRINT_NAME();
	avplayer_dump_call("audio_call", h);
	auto player = acquire_player(h);
	if (player == nullptr || audio_info == nullptr)
	{
		return 0;
	}
	h = player.get();
	std::shared_lock<std::shared_mutex> decoder_lock(h->decoder_mutex);
	AudioVideoBackend::Decoder*          decoder = nullptr;
	uint64_t                             timestamp = 0;
	uint64_t                             start_time = 0;
	uint32_t                             channels = 2;
	uint32_t                             rate = 48000;
	{
		Core::LockGuard lock(h->mutex);
		if (!h->playing || h->paused)
		{
			return 0;
		}
		start_time = h->start_time_ms;
		timestamp  = current_time_ms(h);
		channels   = h->media_audio_channels != 0 ? h->media_audio_channels : channels;
		rate       = h->media_audio_rate != 0 ? h->media_audio_rate : rate;
		decoder    = h->decoder.get();
	}

	if (decoder != nullptr)
	{
		AudioVideoBackend::AudioFrame decoded;
		if (!decoder->TryReadAudioFrame(&decoded) || decoded.data.empty())
		{
			return 0;
		}
		Core::LockGuard lock(h->mutex);
		h->audio_storage = std::move(decoded.data);
		timestamp        = start_time + decoded.timestamp_ms;
		channels         = decoded.channels;
		rate             = decoded.sample_rate;
	} else
	{
		Core::LockGuard lock(h->mutex);
		if (h->audio_storage.empty())
		{
			return 0;
		}
		std::memset(audio_info, 0, sizeof(*audio_info));
		audio_info->data       = h->audio_storage.data();
		audio_info->time_stamp = timestamp;
		fill_audio(&audio_info->details.audio, static_cast<uint32_t>(h->audio_storage.size() * sizeof(int16_t)), channels, rate);
		return 1;
	}

	Core::LockGuard lock(h->mutex);
	if (h->audio_storage.empty())
	{
		return 0;
	}
	std::memset(audio_info, 0, sizeof(*audio_info));
	audio_info->data       = h->audio_storage.data();
	audio_info->time_stamp = timestamp;
	fill_audio(&audio_info->details.audio, static_cast<uint32_t>(h->audio_storage.size() * sizeof(int16_t)), channels, rate);
	return 1;
}

Bool KYTY_SYSV_ABI AvPlayerIsActive(AvPlayerInternal* h)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	if (player == nullptr)
	{
		return 0;
	}
	h = player.get();
	{
		Core::LockGuard lock(h->mutex);
		if (h->playing && (h->paused || synthetic_is_playing(h)))
		{
			return 1;
		}
	}
	stop_once_after_eof(h);
	return 0;
}

uint64_t KYTY_SYSV_ABI AvPlayerCurrentTime(AvPlayerInternal* h)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	if (player == nullptr)
	{
		return 0;
	}
	h = player.get();
	Core::LockGuard lock(h->mutex);
	return current_time_ms(h);
}

int KYTY_SYSV_ABI AvPlayerJumpToTime(AvPlayerInternal* h, uint64_t time_ms)
{
	PRINT_NAME();
	auto player = acquire_player(h);
	if (player == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	h = player.get();
	{
		Core::LockGuard lock(h->mutex);
		h->start_time_ms     = time_ms;
		h->synthetic_obtained_num = static_cast<uint32_t>(
		    std::min<uint64_t>(h->synthetic_frame_count, static_cast<uint64_t>(time_ms * h->synthetic_frame_rate / 1000.0f)));
		h->stop_fired = false;
	}
	int32_t warning = AVPLAYER_WARNING_JUMP_COMPLETE;
	emit_event(h, 0x20, &warning);
	return 0;
}

int KYTY_SYSV_ABI AvPlayerClose(AvPlayerInternal* h)
{
	PRINT_NAME();
	auto player = remove_player(h);
	if (player == nullptr)
	{
		return AVPLAYER_ERROR_INVALID_PARAMS;
	}
	return 0;
}

int KYTY_SYSV_ABI AvPlayerSetLogCallback(void* callback, void* user_data)
{
	PRINT_NAME();
	(void)callback;
	(void)user_data;
	return 0;
}

} // namespace AvPlayer

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

namespace Ngs2 {

LIB_NAME("Ngs2", "Ngs2");

struct Ngs2SystemOption
{
	size_t   size              = 0;
	char     name[16]          = {};
	uint32_t flags             = 0;
	uint32_t max_grain_samples = 0;
	uint32_t num_grain_samples = 0;
	uint32_t sample_rate       = 0;
	uint32_t reserved[6]       = {};
};

struct Ngs2RackOption
{
	size_t   size                   = 0;
	char     name[16]               = {};
	uint32_t flags                  = 0;
	uint32_t max_grain_samples      = 0;
	uint32_t max_voices             = 0;
	uint32_t max_input_delay_blocks = 0;
	uint32_t max_matrices           = 0;
	uint32_t max_ports              = 0;
	uint32_t reserved[20]           = {};
};

struct Ngs2MasteringRackOption
{
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SubmixerRackOption
{
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       max_envelope_points   = 0;
	uint32_t       max_filters           = 0;
	uint32_t       max_inputs            = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SamplerRackOption
{
	Ngs2RackOption rack_option;
	uint32_t       max_channel_works        = 0;
	uint32_t       max_codec_caches         = 0;
	uint32_t       max_waveform_blocks      = 0;
	uint32_t       max_envelope_points      = 0;
	uint32_t       max_filters              = 0;
	uint32_t       max_atrac9_decoders      = 0;
	uint32_t       max_atrac9_channel_works = 0;
	uint32_t       max_ajm_atrac9_decoders  = 0;
	uint32_t       num_peak_meter_blocks    = 0;
};

struct Ngs2ReverbRackOption
{
	Ngs2RackOption rack_option;
	uint32_t       max_channels = 0;
	uint32_t       reverb_size  = 0;
};

struct Ngs2CustomModuleOption
{
	uint32_t size = 0;
};

struct Ngs2CustomRackModuleInfo
{
	const Ngs2CustomModuleOption* option           = nullptr;
	uint32_t                      module_id        = 0;
	uint32_t                      source_buffer_id = 0;
	uint32_t                      extra_buffer_id  = 0;
	uint32_t                      dest_buffer_id   = 0;
	uint32_t                      state_offset     = 0;
	uint32_t                      state_size       = 0;
	uint32_t                      reserved         = 0;
	uint32_t                      reserved2        = 0;
};

struct Ngs2CustomRackPortInfo
{
	uint32_t source_buffer_id = 0;
	uint32_t reserved         = 0;
};

struct Ngs2CustomRackOption
{
	Ngs2RackOption           rack_option;
	uint32_t                 state_size  = 0;
	uint32_t                 num_buffers = 0;
	uint32_t                 num_modules = 0;
	uint32_t                 reserved    = 0;
	Ngs2CustomRackModuleInfo module[24];
	Ngs2CustomRackPortInfo   port[16];
};

struct Ngs2CustomSubmixerRackOption
{
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channels = 0;
	uint32_t             max_inputs   = 0;
};

union Ngs2RackOptionUnion
{
	Ngs2RackOption               common;
	Ngs2SamplerRackOption        sampler;
	Ngs2MasteringRackOption      mastering;
	Ngs2SubmixerRackOption       submixer;
	Ngs2ReverbRackOption         reverb;
	Ngs2CustomSubmixerRackOption custom_submixer;
};

struct Ngs2ContextBufferInfo
{
	void*     host_buffer      = nullptr;
	size_t    host_buffer_size = 0;
	uintptr_t reserved[5]      = {};
	uintptr_t user_data        = 0;
};

using Ngs2BufferAllocHandler = int32_t KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);
using Ngs2BufferFreeHandler  = int32_t  KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);

struct Ngs2BufferAllocator
{
	Ngs2BufferAllocHandler alloc_handler = nullptr;
	Ngs2BufferFreeHandler  free_handler  = nullptr;
	uintptr_t              user_data     = 0;
};

struct Ngs2Internal
{
	Ngs2SystemOption    option;
	Ngs2BufferAllocator allocator;
	Ngs2Internal*       next = nullptr;
	Core::Mutex         mutex;
};

enum class Ngs2RackType
{
	Sampler,
	Submixer,
	Mastering,
	Reverb,
	CustomSampler,
	CustomSubmixer,
};

struct Ngs2RackInternal
{
	Ngs2Internal*       ngs  = nullptr;
	Ngs2RackInternal*   next = nullptr;
	Ngs2RackType        type = Ngs2RackType::Sampler;
	Ngs2RackOptionUnion option;
	Ngs2BufferAllocator allocator;
};

enum class Ngs2VoicePlayState
{
	Empty,
	Playing,
	Paused,
	Stopped
};

enum class Ngs2VoicePlayEvent
{
	None,
	Play,
	Pause,
	Resume,
	Stop,
	StopImm,
	Kill
};

struct Ngs2VoiceInternal
{
	Ngs2VoicePlayEvent event           = Ngs2VoicePlayEvent::None;
	Ngs2VoicePlayState state           = Ngs2VoicePlayState::Empty;
	Ngs2RackInternal*  rack            = nullptr;
	uint32_t           last_command[3] = {};
	// Render ticks spent in Playing (approximate natural end for short voices).
	uint32_t play_ticks = 0;
};

struct Ngs2PcmBlock
{
	const int16_t* samples = nullptr;
	uint64_t       frames  = 0;
};

struct Ngs2PcmStream
{
	uint32_t                  format_id   = 0;
	uint32_t                  channels    = 0;
	uint32_t                  sample_rate = 0;
	float                     gain        = 1.0f;
	bool                      playing     = false;
	std::vector<Ngs2PcmBlock> blocks;
	size_t                    block_index  = 0;
	double                    source_frame = 0.0;
};

struct Ngs2VoiceParamHeader
{
	uint16_t size;
	int16_t  next;
	uint32_t id;
};

struct Ngs2CustomSamplerFormatParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             format_id;
	uint32_t             channels;
	uint32_t             sample_rate;
	uint32_t             reserved[5];
};
static_assert(sizeof(Ngs2CustomSamplerFormatParam) == 40);

struct Ngs2CustomSamplerWaveformContext
{
	uint64_t offset_frames;
	uint64_t data_size;
	uint64_t reserved;
	uint64_t frame_count;
	uint64_t user_data;
};
static_assert(sizeof(Ngs2CustomSamplerWaveformContext) == 40);

struct Ngs2CustomSamplerWaveformParam
{
	Ngs2VoiceParamHeader                    header;
	const int16_t*                          data;
	uint32_t                                flags;
	uint32_t                                block_count;
	const Ngs2CustomSamplerWaveformContext* context;
};
static_assert(sizeof(Ngs2CustomSamplerWaveformParam) == 32);

struct Ngs2RenderBufferInfoImpl
{
	float*   data;
	size_t   data_size;
	uint32_t size;
	uint32_t channels;
};
static_assert(sizeof(Ngs2RenderBufferInfoImpl) == 24);

struct Ngs2VoiceEventParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             event_id;
};

struct Ngs2VoicePatchParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	uint32_t             dest_input_id;
	uintptr_t            dest_handle;
};

struct Ngs2VoicePortMatrixParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	int32_t              matrix_id;
};

// Common voice-control param id 0x0007. Layout matches the PS4 NGS2
// VoiceCallback parameter block (header + handler + data + flags + reserved).
// Observed at the Gen5 frontier with size 32 immediately after a custom-sampler
// class param (0x40010000). Handler invocation is not performed until a
// guest-visible dependency on the callback is evidenced.
struct Ngs2VoiceCallbackParam
{
	Ngs2VoiceParamHeader header;
	uintptr_t            callback_handler;
	uintptr_t            callback_data;
	uint32_t             flags;
	uint32_t             reserved;
};

struct Ngs2VoiceState
{
	uint32_t state_flags;
};

struct Ngs2SamplerVoiceState
{
	Ngs2VoiceState voice_state;
	float          envelope_height;
	float          peak_height;
	uint32_t       reserved;
	uint64_t       num_decoded_samples;
	uint64_t       decoded_data_size;
	uint64_t       user_data;
	const void*    waveform_data;
};

static Ngs2Internal*                                         g_ngs_list   = nullptr;
static Ngs2RackInternal*                                     g_racks_list = nullptr;
static std::unordered_map<Ngs2VoiceInternal*, Ngs2PcmStream> g_pcm_streams;

static uint32_t Ngs2GetVoiceStateFlags(const Ngs2VoiceInternal* voice)
{
	EXIT_IF(voice == nullptr);
	switch (voice->state)
	{
		case Ngs2VoicePlayState::Empty: return 0;
		case Ngs2VoicePlayState::Playing: return 0x3;
		case Ngs2VoicePlayState::Paused: return 0x5;
		case Ngs2VoicePlayState::Stopped: return 0xb;
	}
	EXIT("unknown voice state\n");
	return 0;
}

static bool Ngs2MixPcmStream(Ngs2PcmStream* stream, float* output, uint32_t output_frames, uint32_t output_channels, uint32_t output_rate)
{
	EXIT_IF(stream == nullptr || output == nullptr);
	EXIT_IF(stream->channels != 1 && stream->channels != 2);
	EXIT_IF(output_channels != 2 || stream->sample_rate == 0 || output_rate == 0);

	const double step = static_cast<double>(stream->sample_rate) / static_cast<double>(output_rate);
	for (uint32_t frame = 0; frame < output_frames; frame++)
	{
		while (stream->block_index < stream->blocks.size() &&
		       stream->source_frame >= static_cast<double>(stream->blocks[stream->block_index].frames))
		{
			stream->source_frame -= static_cast<double>(stream->blocks[stream->block_index].frames);
			stream->block_index++;
		}
		if (stream->block_index >= stream->blocks.size())
		{
			stream->playing = false;
			return false;
		}

		const auto& block      = stream->blocks[stream->block_index];
		const auto  index      = static_cast<uint64_t>(stream->source_frame);
		const float fraction   = static_cast<float>(stream->source_frame - static_cast<double>(index));
		const auto  next_index = (index + 1 < block.frames ? index + 1 : index);
		for (uint32_t channel = 0; channel < 2; channel++)
		{
			const uint32_t source_channel = (stream->channels == 1 ? 0 : channel);
			const float    current        = static_cast<float>(block.samples[index * stream->channels + source_channel]) / 32768.0f;
			const float    next           = static_cast<float>(block.samples[next_index * stream->channels + source_channel]) / 32768.0f;
			const float    sample         = (current + (next - current) * fraction) * stream->gain;
			float&         dest           = output[frame * output_channels + channel];
			dest += sample;
			if (dest > 1.0f)
			{
				dest = 1.0f;
			} else if (dest < -1.0f)
			{
				dest = -1.0f;
			}
		}
		stream->source_frame += step;
	}
	return true;
}

static const Ngs2RackOption* Ngs2ResolveRackOption(uint32_t rack_id, const Ngs2RackOption* option,
                                                   Ngs2MasteringRackOption* default_mastering)
{
	if (option != nullptr)
	{
		return option;
	}
	if (rack_id != 0x3000 || default_mastering == nullptr)
	{
		return nullptr;
	}

	default_mastering->rack_option.size       = sizeof(Ngs2MasteringRackOption);
	default_mastering->rack_option.max_voices = 1;
	return &default_mastering->rack_option;
}

static uint32_t Ngs2GetRackMaxVoices(uint32_t rack_id, const Ngs2RackOption* option)
{
	EXIT_IF(option == nullptr);

	// Gen5 rack options may insert a 0x30-byte extension after `size` before the
	// common name/flags/max_voices fields. The base Ngs2RackOption is 0x80 bytes
	// on this ABI; any option->size >= 0xb0 (0x80+0x30) is treated as extended.
	// Observed: custom sampler 0x4001 size 0x518 → max_voices at +0x50 = 256;
	// reverb 0x2001 size 0xb8 → same offset (standard option->max_voices is 0).
	const bool extended = (option->size >= 0xb0u);
	if (extended)
	{
		uint32_t max_voices = 0;
		memcpy(&max_voices, reinterpret_cast<const uint8_t*>(option) + 0x50, sizeof(max_voices));
		if (max_voices != 0)
		{
			return max_voices;
		}
	}

	// Fallback: classic prefix layout (max_voices at +0x20 after size+name+flags+grain).
	(void)rack_id;
	return option->max_voices;
}

int KYTY_SYSV_ABI Ngs2SystemQueryBufferSize(const Ngs2SystemOption* option, Ngs2ContextBufferInfo* buffer_info)
{
	PRINT_NAME();

	constexpr int32_t NGS2_ERROR_INVALID_OUT_ADDRESS = static_cast<int32_t>(0x804a0053u);
	constexpr int32_t NGS2_ERROR_INVALID_OPTION_SIZE = static_cast<int32_t>(0x804a0081u);

	if (buffer_info == nullptr)
	{
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	if (option != nullptr && option->size != sizeof(Ngs2SystemOption))
	{
		return NGS2_ERROR_INVALID_OPTION_SIZE;
	}

	buffer_info->host_buffer      = nullptr;
	buffer_info->host_buffer_size = sizeof(Ngs2Internal);
	for (auto& value: buffer_info->reserved)
	{
		value = 0;
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption* option, const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle)
{
	PRINT_NAME();

	constexpr int32_t NGS2_ERROR_INVALID_OUT_ADDRESS    = static_cast<int32_t>(0x804a0053u);
	constexpr int32_t NGS2_ERROR_INVALID_OPTION_SIZE    = static_cast<int32_t>(0x804a0081u);
	constexpr int32_t NGS2_ERROR_INVALID_BUFFER_INFO    = static_cast<int32_t>(0x804a0206u);
	constexpr int32_t NGS2_ERROR_INVALID_BUFFER_ADDRESS = static_cast<int32_t>(0x804a0207u);
	constexpr int32_t NGS2_ERROR_INVALID_BUFFER_SIZE    = static_cast<int32_t>(0x804a0209u);

	if (buffer_info == nullptr)
	{
		return NGS2_ERROR_INVALID_BUFFER_INFO;
	}
	if (handle == nullptr)
	{
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	if (option != nullptr && option->size != sizeof(Ngs2SystemOption))
	{
		return NGS2_ERROR_INVALID_OPTION_SIZE;
	}
	if (buffer_info->host_buffer == nullptr)
	{
		return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
	}
	if (buffer_info->host_buffer_size < sizeof(Ngs2Internal))
	{
		return NGS2_ERROR_INVALID_BUFFER_SIZE;
	}

	auto* ngs = new (buffer_info->host_buffer) Ngs2Internal;
	if (option != nullptr)
	{
		ngs->option = *option;
	} else
	{
		ngs->option.size              = sizeof(Ngs2SystemOption);
		ngs->option.max_grain_samples = 512;
		ngs->option.num_grain_samples = 256;
		ngs->option.sample_rate       = 48000;
	}

	ngs->next  = g_ngs_list;
	g_ngs_list = ngs;
	*handle    = reinterpret_cast<uintptr_t>(ngs);

	return OK;
}

static bool Ngs2ValidateSystem(uintptr_t system_handle, Ngs2Internal** out_ngs)
{
	if (system_handle == 0 || out_ngs == nullptr)
	{
		return false;
	}
	auto* ngs = reinterpret_cast<Ngs2Internal*>(system_handle);
	for (auto* it = g_ngs_list; it != nullptr; it = it->next)
	{
		if (it == ngs)
		{
			*out_ngs = ngs;
			return true;
		}
	}
	return false;
}

int KYTY_SYSV_ABI Ngs2SystemDestroy(uintptr_t system_handle)
{
	PRINT_NAME();
	Ngs2Internal* ngs = nullptr;
	if (!Ngs2ValidateSystem(system_handle, &ngs))
	{
		return static_cast<int32_t>(0x804a0201u);
	}
	Core::LockGuard lock(ngs->mutex);
	auto**          link = &g_racks_list;
	while (*link != nullptr)
	{
		if ((*link)->ngs == ngs)
		{
			auto* rack   = *link;
			*link        = rack->next;
			auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
			for (uint32_t i = 0; i < rack->option.common.max_voices; i++)
			{
				g_pcm_streams.erase(voices + i);
			}
			rack->~Ngs2RackInternal();
			continue;
		}
		link = &(*link)->next;
	}
	auto** sys_link = &g_ngs_list;
	while (*sys_link != nullptr && *sys_link != ngs)
	{
		sys_link = &(*sys_link)->next;
	}
	if (*sys_link != nullptr)
	{
		*sys_link = ngs->next;
	}
	ngs->~Ngs2Internal();
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemLock(uintptr_t system_handle)
{
	PRINT_NAME();
	Ngs2Internal* ngs = nullptr;
	if (!Ngs2ValidateSystem(system_handle, &ngs))
	{
		return static_cast<int32_t>(0x804a0201u);
	}
	ngs->mutex.Lock();
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemUnlock(uintptr_t system_handle)
{
	PRINT_NAME();
	Ngs2Internal* ngs = nullptr;
	if (!Ngs2ValidateSystem(system_handle, &ngs))
	{
		return static_cast<int32_t>(0x804a0201u);
	}
	ngs->mutex.Unlock();
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemSetGrainSamples(uintptr_t system_handle, uint32_t grain_samples)
{
	PRINT_NAME();
	Ngs2Internal* ngs = nullptr;
	if (!Ngs2ValidateSystem(system_handle, &ngs))
	{
		return static_cast<int32_t>(0x804a0201u);
	}
	if (grain_samples > 0 && grain_samples <= 8192)
	{
		ngs->option.num_grain_samples = grain_samples;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemSetSampleRate(uintptr_t system_handle, uint32_t sample_rate)
{
	PRINT_NAME();
	Ngs2Internal* ngs = nullptr;
	if (!Ngs2ValidateSystem(system_handle, &ngs))
	{
		return static_cast<int32_t>(0x804a0201u);
	}
	if (sample_rate > 0)
	{
		ngs->option.sample_rate = sample_rate;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2PanInit(void* /*pan_param*/)
{
	PRINT_NAME();
	return OK;
}

int KYTY_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option, Ngs2ContextBufferInfo* buffer_info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);

	Ngs2MasteringRackOption default_mastering {};
	option = Ngs2ResolveRackOption(rack_id, option, &default_mastering);
	EXIT_NOT_IMPLEMENTED(option == nullptr);

	printf("\t rack_id    = 0x%" PRIx32 "\n", rack_id);
	const uint32_t max_voices = Ngs2GetRackMaxVoices(rack_id, option);
	printf("\t max_voices = %u\n", max_voices);

	buffer_info->host_buffer_size = sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal) * max_voices;

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption* option, const Ngs2BufferAllocator* allocator, uintptr_t* handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(option == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	printf("\t name              = %.16s\n", option->name);
	printf("\t flags             = %u\n", option->flags);
	printf("\t max_grain_samples = %u\n", option->max_grain_samples);
	printf("\t num_grain_samples = %u\n", option->num_grain_samples);
	printf("\t sample_rate       = %u\n", option->sample_rate);
	printf("\t alloc_handler     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->alloc_handler));
	printf("\t free_handler      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->free_handler));
	printf("\t user_data         = 0x%016" PRIx64 "\n", static_cast<uint64_t>(allocator->user_data));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = sizeof(Ngs2Internal);
	buf.user_data        = allocator->user_data;

	int result = allocator->alloc_handler(&buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	auto* ngs = new (buf.host_buffer) Ngs2Internal;

	ngs->option    = *option;
	ngs->allocator = *allocator;

	ngs->next  = g_ngs_list;
	g_ngs_list = ngs;

	*handle = reinterpret_cast<uintptr_t>(ngs);

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer_size == 0);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	Ngs2MasteringRackOption default_mastering {};
	option = Ngs2ResolveRackOption(rack_id, option, &default_mastering);
	EXIT_NOT_IMPLEMENTED(option == nullptr);

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));

	printf("\t rack_id                = 0x%" PRIx32 "\n", rack_id);
	printf("\t option_size            = 0x%016" PRIx64 "\n", static_cast<uint64_t>(option->size));
	printf("\t name                   = %.16s\n", option->name);
	printf("\t flags                  = %u\n", option->flags);
	printf("\t max_grain_samples      = %u\n", option->max_grain_samples);
	printf("\t max_voices             = %u\n", option->max_voices);
	printf("\t max_input_delay_blocks = %u\n", option->max_input_delay_blocks);
	printf("\t max_matrices           = %u\n", option->max_matrices);
	printf("\t max_ports              = %u\n", option->max_ports);
	printf("\t host_buffer            = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer_info->host_buffer));
	printf("\t host_buffer_size      = 0x%016" PRIx64 "\n", static_cast<uint64_t>(buffer_info->host_buffer_size));

	auto* ngs    = reinterpret_cast<Ngs2Internal*>(system_handle);
	auto* rack   = static_cast<Ngs2RackInternal*>(buffer_info->host_buffer);
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);

	Core::LockGuard lock(ngs->mutex);

	switch (rack_id)
	{
		case 0x1000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SamplerRackOption));
			rack->option.sampler = *reinterpret_cast<const Ngs2SamplerRackOption*>(option);
			rack->type           = Ngs2RackType::Sampler;
			break;
		case 0x2000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SubmixerRackOption));
			rack->option.submixer = *reinterpret_cast<const Ngs2SubmixerRackOption*>(option);
			rack->type            = Ngs2RackType::Submixer;
			break;
		case 0x2001:
			// Gen5 appends an opaque 0x30-byte extension to the reverb option.
			// The common and reverb fields consumed here retain their prefix layout.
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2ReverbRackOption) && option->size != 0xb8);
			rack->option.reverb = *reinterpret_cast<const Ngs2ReverbRackOption*>(option);
			rack->type          = Ngs2RackType::Reverb;
			break;
		case 0x3000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2MasteringRackOption));
			rack->option.mastering = *reinterpret_cast<const Ngs2MasteringRackOption*>(option);
			rack->type             = Ngs2RackType::Mastering;
			break;
		case 0x4001:
			// The Gen5 custom-sampler option extends the common ABI to 0x518
			// bytes. Only the common prefix is consumed here; the undocumented
			// extension remains opaque until a supported operation needs it.
			EXIT_NOT_IMPLEMENTED(option->size != 0x518);
			rack->option.common = *option;
			rack->type          = Ngs2RackType::CustomSampler;
			break;
		case 0x4002:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomSubmixerRackOption));
			rack->option.custom_submixer = *reinterpret_cast<const Ngs2CustomSubmixerRackOption*>(option);
			rack->type                   = Ngs2RackType::CustomSubmixer;
			break;
		default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
	}

	printf("\t type                   = %s\n", Core::EnumName(rack->type).C_Str());

	rack->allocator                = Ngs2BufferAllocator();
	rack->ngs                      = ngs;
	rack->option.common.max_voices = Ngs2GetRackMaxVoices(rack_id, option);

	rack->next   = g_racks_list;
	g_racks_list = rack;

	for (uint32_t i = 0; i < rack->option.common.max_voices; i++)
	{
		voices[i].rack  = rack;
		voices[i].event = Ngs2VoicePlayEvent::None;
		voices[i].state = Ngs2VoicePlayState::Empty;
	}

	*handle = reinterpret_cast<uintptr_t>(rack);

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                              const Ngs2BufferAllocator* allocator, uintptr_t* handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(option == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));

	printf("\t rack_id                = 0x%" PRIx32 "\n", rack_id);
	printf("\t name                   = %.16s\n", option->name);
	printf("\t flags                  = %u\n", option->flags);
	printf("\t max_grain_samples      = %u\n", option->max_grain_samples);
	printf("\t max_voices             = %u\n", option->max_voices);
	printf("\t max_input_delay_blocks = %u\n", option->max_input_delay_blocks);
	printf("\t max_matrices           = %u\n", option->max_matrices);
	printf("\t max_ports              = %u\n", option->max_ports);
	printf("\t alloc_handler          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->alloc_handler));
	printf("\t free_handler           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->free_handler));
	printf("\t user_data              = 0x%016" PRIx64 "\n", static_cast<uint64_t>(allocator->user_data));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = 0;
	buf.user_data        = allocator->user_data;

	Ngs2RackQueryBufferSize(rack_id, option, &buf);

	EXIT_NOT_IMPLEMENTED(buf.host_buffer_size == 0);

	int result = allocator->alloc_handler(&buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	result = Ngs2RackCreate(system_handle, rack_id, option, &buf, handle);

	if (result == OK)
	{
		auto* rack      = static_cast<Ngs2RackInternal*>(buf.host_buffer);
		rack->allocator = *allocator;
	}

	return result;
}

int KYTY_SYSV_ABI Ngs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info)
{
	PRINT_NAME();

	if (rack_handle == 0)
	{
		return static_cast<int32_t>(0x804a0261u);
	}

	auto* rack = reinterpret_cast<Ngs2RackInternal*>(rack_handle);
	auto* ngs  = rack->ngs;
	EXIT_IF(ngs == nullptr);

	Ngs2ContextBufferInfo released {};
	released.host_buffer      = rack;
	released.host_buffer_size = sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal) * rack->option.common.max_voices;
	released.user_data        = rack->allocator.user_data;
	const auto allocator      = rack->allocator;

	{
		Core::LockGuard lock(ngs->mutex);

		auto** link = &g_racks_list;
		while (*link != nullptr && *link != rack)
		{
			link = &(*link)->next;
		}
		if (*link == nullptr)
		{
			return static_cast<int32_t>(0x804a0261u);
		}
		*link = rack->next;

		auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
		for (uint32_t i = 0; i < rack->option.common.max_voices; i++)
		{
			g_pcm_streams.erase(voices + i);
		}
		rack->~Ngs2RackInternal();
	}

	if (allocator.free_handler != nullptr)
	{
		return allocator.free_handler(&released);
	}
	if (buffer_info != nullptr)
	{
		*buffer_info = released;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);
	EXIT_NOT_IMPLEMENTED(num_buffer_info != 1);

	auto*       ngs    = reinterpret_cast<Ngs2Internal*>(system_handle);
	const auto* render = reinterpret_cast<const Ngs2RenderBufferInfoImpl*>(buffer_info);
	EXIT_NOT_IMPLEMENTED(render->data == nullptr);
	EXIT_NOT_IMPLEMENTED(render->size != sizeof(Ngs2RenderBufferInfoImpl));
	EXIT_NOT_IMPLEMENTED(render->channels != 2);
	EXIT_NOT_IMPLEMENTED(ngs->option.num_grain_samples == 0 || ngs->option.sample_rate == 0);
	const size_t render_size = static_cast<size_t>(ngs->option.num_grain_samples) * render->channels * sizeof(float);
	EXIT_NOT_IMPLEMENTED(render->data_size < render_size);
	std::memset(render->data, 0, render_size);

	Core::LockGuard lock(ngs->mutex);

	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next)
	{
		if (rack->ngs == ngs)
		{
			auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);

			for (uint32_t i = 0; i < rack->option.common.max_voices; i++)
			{
				auto& voice = voices[i];
				switch (voice.event)
				{
					case Ngs2VoicePlayEvent::None:
						// Keep Playing until a Stop event or natural end (below). Only
						// advance Stopped → Empty when idle so callers that poll state
						// still observe a finished voice.
						if (voice.state == Ngs2VoicePlayState::Stopped)
						{
							voice.state = Ngs2VoicePlayState::Empty;
						}
						break;
					case Ngs2VoicePlayEvent::Play:
						if (voice.state == Ngs2VoicePlayState::Empty)
						{
							voice.state      = Ngs2VoicePlayState::Playing;
							voice.play_ticks = 0;
						}
						break;
					case Ngs2VoicePlayEvent::Pause:
						if (voice.state == Ngs2VoicePlayState::Playing)
						{
							voice.state = Ngs2VoicePlayState::Paused;
						}
						break;
					case Ngs2VoicePlayEvent::Resume:
						if (voice.state == Ngs2VoicePlayState::Paused)
						{
							voice.state = Ngs2VoicePlayState::Playing;
						}
						break;
					case Ngs2VoicePlayEvent::Stop:
						if (voice.state == Ngs2VoicePlayState::Playing)
						{
							voice.state = Ngs2VoicePlayState::Stopped;
						}
						break;
					case Ngs2VoicePlayEvent::StopImm:
					case Ngs2VoicePlayEvent::Kill: voice.state = Ngs2VoicePlayState::Empty; break;
				}
				voice.event = Ngs2VoicePlayEvent::None;

				auto stream_it = g_pcm_streams.find(&voice);
				if (voice.state == Ngs2VoicePlayState::Playing && stream_it != g_pcm_streams.end() && stream_it->second.playing)
				{
					if (!Ngs2MixPcmStream(&stream_it->second, render->data, ngs->option.num_grain_samples, render->channels,
					                      ngs->option.sample_rate))
					{
						voice.state = Ngs2VoicePlayState::Stopped;
					}
				} else if (voice.state == Ngs2VoicePlayState::Playing && stream_it == g_pcm_streams.end())
				{
					// Preserve state timing for rack types whose PCM contract is still opaque.
					voice.play_ticks++;
					if (voice.play_ticks >= 400)
					{
						voice.state      = Ngs2VoicePlayState::Stopped;
						voice.play_ticks = 0;
					}
				}
			}
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id, uintptr_t* handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	if (rack_handle == 0)
	{
		*handle = 0;
		return static_cast<int32_t>(0x804a0261u);
	}

	printf("\t voice_id = %u\n", voice_id);

	auto* rack   = reinterpret_cast<Ngs2RackInternal*>(rack_handle);
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack_handle + sizeof(Ngs2RackInternal));

	const uint32_t max_voices = rack->option.common.max_voices;
	printf("\t max_voices = %u\n", max_voices);

	// Ordinary invalid index is a guest error, not an emulator invariant break.
	// Captured: reverb rack 0x2001 extended option size 0xb8 → max_voices 16 at
	// +0x50; GetVoiceHandle for voice_id in [0,15] must succeed after Create.
	if (max_voices == 0 || voice_id >= max_voices)
	{
		*handle = 0;
		return static_cast<int32_t>(0x804a0300u); // SCE_NGS2_ERROR_INVALID_VOICE_HANDLE
	}

	EXIT_IF(voices[voice_id].rack != rack);

	*handle = reinterpret_cast<uintptr_t>(voices + voice_id);

	return OK;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int KYTY_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(param_list == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Core::LockGuard lock(voice->rack->ngs->mutex);

	const auto* param = param_list;

	for (;;)
	{
		printf("\t id   = 0x%08" PRIx32 "\n", param->id);
		printf("\t size = %" PRIu16 "\n", param->size);
		printf("\t next = %" PRId16 "\n", param->next);

		auto rack_id = param->id >> 16u;

		EXIT_NOT_IMPLEMENTED(((param->id >> 15u) & 0x1u) != 0);

		switch (rack_id)
		{
			case 0x0000:
			{
				auto cid = param->id & 0x7fffu;
				switch (cid)
				{
					case 0x0002:
					{
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortMatrixParam));
						const auto* pm = reinterpret_cast<const Ngs2VoicePortMatrixParam*>(param);
						printf("\t port      = %u\n", pm->port);
						printf("\t matrix_id = %d\n", pm->matrix_id);
						break;
					}
					case 0x0005:
					{
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePatchParam));
						const auto* patch = reinterpret_cast<const Ngs2VoicePatchParam*>(param);
						printf("\t connect->port          = %u\n", patch->port);
						printf("\t connect->dest_input_id = %u\n", patch->dest_input_id);
						printf("\t connect->dest_handle   = 0x%016" PRIx64 "\n", patch->dest_handle);
						break;
					}
					case 0x0006:
					{
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceEventParam));
						const auto* event = reinterpret_cast<const Ngs2VoiceEventParam*>(param);
						switch (event->event_id)
						{
							case 0: voice->event = Ngs2VoicePlayEvent::Play; break;
							case 1: voice->event = Ngs2VoicePlayEvent::Stop; break;
							case 2: voice->event = Ngs2VoicePlayEvent::StopImm; break;
							case 3: voice->event = Ngs2VoicePlayEvent::Kill; break;
							case 4: voice->event = Ngs2VoicePlayEvent::Pause; break;
							case 5: voice->event = Ngs2VoicePlayEvent::Resume; break;
							default: EXIT("unknown event_id: 0x%08" PRIx32 "\n", event->event_id);
						}
						printf("\t event = %u\n", event->event_id);
						break;
					}
					case 0x0007:
					{
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceCallbackParam));
						const auto* cb = reinterpret_cast<const Ngs2VoiceCallbackParam*>(param);
						printf("\t callback_handler = 0x%016" PRIx64 "\n", static_cast<uint64_t>(cb->callback_handler));
						printf("\t callback_data    = 0x%016" PRIx64 "\n", static_cast<uint64_t>(cb->callback_data));
						printf("\t flags            = 0x%08" PRIx32 "\n", cb->flags);
						break;
					}
					default: EXIT("unknown id: 0x%04" PRIx32 "\n", cid);
				}
				break;
			}
			case 0x1000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Sampler); break;
			case 0x2000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Submixer); break;
			case 0x2001: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Reverb); break;
			case 0x3000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Mastering); break;
			// 0x4000 class params are used both for CustomSubmixer (historical
			// Kyty path) and for CustomSampler module params. Observed Gen5
			// sequence on a CustomSampler voice: 0x40010000 → 0x00000007 →
			// 0x40010001 → 0x00000005 → 0x40001300 (size 48). Type-check only
			// until a field of the 48-byte block is shown to affect guest state.
			case 0x4000:
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSubmixer && voice->rack->type != Ngs2RackType::CustomSampler);
				break;
			// Gen5 custom-sampler rack (created via rack_id 0x4001). Observed
			// VoiceControl param id 0x40010000 with size 40 after logo path.
			// Accept like other rack-class ids: type-check only until a field
			// of this 40-byte block is shown to affect guest-visible state.
			case 0x4001:
			{
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSampler);
				const uint32_t control_id = param->id & 0xffffu;
				if (control_id == 0)
				{
					EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2CustomSamplerFormatParam));
					const auto* format = reinterpret_cast<const Ngs2CustomSamplerFormatParam*>(param);
					EXIT_NOT_IMPLEMENTED(format->format_id != 0x12u);
					EXIT_NOT_IMPLEMENTED(format->channels != 1 && format->channels != 2);
					EXIT_NOT_IMPLEMENTED(format->sample_rate != 44100);
					auto& stream       = g_pcm_streams[voice];
					stream             = Ngs2PcmStream {};
					stream.format_id   = format->format_id;
					stream.channels    = format->channels;
					stream.sample_rate = format->sample_rate;
				} else if (control_id == 1)
				{
					EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2CustomSamplerWaveformParam));
					const auto* waveform = reinterpret_cast<const Ngs2CustomSamplerWaveformParam*>(param);
					auto&       stream   = g_pcm_streams[voice];
					EXIT_NOT_IMPLEMENTED(stream.format_id != 0x12u || stream.channels == 0 || stream.sample_rate == 0);
					if (waveform->data == nullptr && waveform->context == nullptr)
					{
						stream.blocks.clear();
						stream.block_index  = 0;
						stream.source_frame = 0.0;
						stream.playing      = false;
					} else
					{
						EXIT_NOT_IMPLEMENTED(waveform->data == nullptr || waveform->context == nullptr);
						EXIT_NOT_IMPLEMENTED(waveform->flags != 0x11u || waveform->block_count != 1u);
						const uint64_t bytes_per_frame = static_cast<uint64_t>(stream.channels) * sizeof(int16_t);
						EXIT_NOT_IMPLEMENTED(waveform->context->frame_count > UINT64_MAX / bytes_per_frame);
						EXIT_NOT_IMPLEMENTED(waveform->context->data_size != waveform->context->frame_count * bytes_per_frame);
						stream.blocks.push_back({waveform->data, waveform->context->frame_count});
					}
				} else
				{
					// Other module controls retain their established opaque handling until
					// a guest-visible effect identifies their contract.
				}
				break;
			}
			case 0x4002: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSubmixer); break;
			default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
		}

		if (param->next == 0)
		{
			break;
		}
		param = reinterpret_cast<const Ngs2VoiceParamHeader*>(reinterpret_cast<uintptr_t>(param) + param->next);
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceRunCommands(uintptr_t voice_handle, const void* commands, uint32_t num_commands)
{
	PRINT_NAME();

	constexpr int32_t NGS2_ERROR_INVALID_VOICE_HANDLE    = static_cast<int32_t>(0x804a0300u);
	constexpr int32_t NGS2_ERROR_INVALID_CONTROL_ADDRESS = static_cast<int32_t>(0x804a0309u);

	if (voice_handle == 0)
	{
		return NGS2_ERROR_INVALID_VOICE_HANDLE;
	}
	if (num_commands == 0)
	{
		return OK;
	}
	if (commands == nullptr)
	{
		return NGS2_ERROR_INVALID_CONTROL_ADDRESS;
	}

	EXIT_NOT_IMPLEMENTED(num_commands != 1);

	auto*           voice   = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);
	const auto*     command = static_cast<const uint32_t*>(commands);
	Core::LockGuard lock(voice->rack->ngs->mutex);
	for (int i = 0; i < 3; i++)
	{
		voice->last_command[i] = command[i];
	}
	if (voice->rack->type == Ngs2RackType::CustomSampler)
	{
		auto stream_it = g_pcm_streams.find(voice);
		if (command[0] == 2u && command[1] == 0x400u && stream_it != g_pcm_streams.end())
		{
			if (command[2] == 1u)
			{
				voice->event              = Ngs2VoicePlayEvent::Play;
				stream_it->second.playing = true;
			} else if (command[2] == 8u)
			{
				voice->event                   = Ngs2VoicePlayEvent::StopImm;
				stream_it->second.playing      = false;
				stream_it->second.block_index  = 0;
				stream_it->second.source_frame = 0.0;
			}
		} else if (command[0] == 6u && command[1] == 0x100u && stream_it != g_pcm_streams.end())
		{
			float gain = 0.0f;
			std::memcpy(&gain, &command[2], sizeof(gain));
			EXIT_NOT_IMPLEMENTED(!std::isfinite(gain) || gain < 0.0f);
			stream_it->second.gain = gain;
		}
	}

	printf("\t command = {%08" PRIx32 ", %08" PRIx32 ", %08" PRIx32 "}\n", voice->last_command[0], voice->last_command[1],
	       voice->last_command[2]);
	return OK;
}

// Layout from PS4 NGS2 geom headers (vector = 3 floats; see OrbisNgs2Geom*Param).
struct Ngs2GeomVector
{
	float x = 0;
	float y = 0;
	float z = 0;
};
struct Ngs2GeomCone
{
	float inner_level = 0;
	float inner_angle = 0;
	float outer_level = 0;
	float outer_angle = 0;
};
struct Ngs2GeomRolloff
{
	uint32_t model              = 0;
	float    max_distance       = 0;
	float    rolloff_factor     = 0;
	float    reference_distance = 0;
};
struct Ngs2GeomListenerParam
{
	Ngs2GeomVector position {};
	Ngs2GeomVector orient_front {};
	Ngs2GeomVector orient_up {};
	Ngs2GeomVector velocity {};
	float          sound_speed = 0;
	uint32_t       reserved[2] = {};
};
struct Ngs2GeomSourceParam
{
	Ngs2GeomVector  position {};
	Ngs2GeomVector  velocity {};
	Ngs2GeomVector  direction {};
	Ngs2GeomCone    cone {};
	Ngs2GeomRolloff rolloff {};
	float           doppler_factor = 0;
	float           fbw_level      = 0;
	float           lfe_level      = 0;
	float           max_level      = 0;
	float           min_level      = 0;
	float           radius         = 0;
	uint32_t        num_speakers   = 0;
	uint32_t        matrix_format  = 0;
	uint32_t        reserved[2]    = {};
};
struct Ngs2GeomListenerWork
{
	float          matrix[4][4] = {};
	Ngs2GeomVector velocity {};
	float          sound_speed = 0;
	uint32_t       coordinate  = 0;
	uint32_t       reserved[3] = {};
};

int KYTY_SYSV_ABI Ngs2GeomResetSourceParam(void* out_source_param)
{
	PRINT_NAME();

	// Sony reset: zero then set identity-ish defaults for a non-spatialised source.
	if (out_source_param != nullptr)
	{
		auto* p                       = static_cast<Ngs2GeomSourceParam*>(out_source_param);
		*p                            = Ngs2GeomSourceParam {};
		p->cone.inner_level           = 1.0f;
		p->cone.outer_level           = 1.0f;
		p->cone.outer_angle           = 3.14159265f; // 180 deg — omnidirectional default
		p->rolloff.max_distance       = 1000.0f;
		p->rolloff.rolloff_factor     = 1.0f;
		p->rolloff.reference_distance = 1.0f;
		p->doppler_factor             = 1.0f;
		p->fbw_level                  = 1.0f;
		p->lfe_level                  = 1.0f;
		p->max_level                  = 1.0f;
		p->min_level                  = 0.0f;
		p->radius                     = 0.0f;
		p->num_speakers               = 2;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomResetListenerParam(void* out_listener_param)
{
	PRINT_NAME();

	if (out_listener_param != nullptr)
	{
		auto* p           = static_cast<Ngs2GeomListenerParam*>(out_listener_param);
		*p                = Ngs2GeomListenerParam {};
		p->orient_front.z = -1.0f; // look down -Z
		p->orient_up.y    = 1.0f;
		p->sound_speed    = 340.0f;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomCalcListener(const void* listener_param, void* out_work, uint32_t flags)
{
	PRINT_NAME();

	printf("\t flags = %u\n", flags);
	if (out_work != nullptr)
	{
		auto* w = static_cast<Ngs2GeomListenerWork*>(out_work);
		*w      = Ngs2GeomListenerWork {};
		// Identity 4x4
		w->matrix[0][0] = w->matrix[1][1] = w->matrix[2][2] = w->matrix[3][3] = 1.0f;
		if (listener_param != nullptr)
		{
			const auto* p  = static_cast<const Ngs2GeomListenerParam*>(listener_param);
			w->velocity    = p->velocity;
			w->sound_speed = p->sound_speed;
		} else
		{
			w->sound_speed = 340.0f;
		}
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomApply(const void* listener_work, const void* source_param, void* out_attrib, uint32_t flags)
{
	PRINT_NAME();

	printf("\t flags = %u\n", flags);
	(void)listener_work;
	(void)source_param;
	if (out_attrib != nullptr)
	{
		// Attribute is large (level matrix); zero the first 8 floats of levels + pitch.
		std::memset(out_attrib, 0, 4 + 8 * 8 * 4);
		*static_cast<float*>(out_attrib) = 1.0f; // pitchRatio
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state, size_t state_size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Core::LockGuard lock(voice->rack->ngs->mutex);

	switch (voice->rack->type)
	{
		// Gen5 CustomSampler (rack 0x4001) GetState was observed with state_size 48,
		// matching the standard Sampler voice-state block on this ABI. Fill the
		// same fields; do not invent the larger PS4 custom-state layout (80).
		case Ngs2RackType::CustomSampler:
		case Ngs2RackType::Sampler:
		{
			EXIT_NOT_IMPLEMENTED(state_size != sizeof(Ngs2SamplerVoiceState));
			auto* sampler                    = reinterpret_cast<Ngs2SamplerVoiceState*>(state);
			sampler->voice_state.state_flags = Ngs2GetVoiceStateFlags(voice);
			sampler->envelope_height         = 1.0f;
			sampler->peak_height             = 0.0f;
			sampler->reserved                = 0;
			sampler->num_decoded_samples     = 0;
			sampler->decoded_data_size       = 0;
			sampler->user_data               = 0;
			sampler->waveform_data           = nullptr;
			printf("\t state_flags = %u\n", sampler->voice_state.state_flags);
			break;
		}
		default: EXIT("unknown type: %s\n", Core::EnumName(voice->rack->type).C_Str());
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state_flags == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Core::LockGuard lock(voice->rack->ngs->mutex);

	*state_flags = Ngs2GetVoiceStateFlags(voice);
	printf("\t state_flags = %u\n", *state_flags);

	return OK;
}

} // namespace Ngs2

} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED
