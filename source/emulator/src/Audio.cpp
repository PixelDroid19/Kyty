#include "Emulator/Audio.h"
#include "Emulator/AudioHost.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/VideoFrameMemory.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
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

	if (user_id != 255 && user_id != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	// Port types observed on Gen5 titles: 0 MAIN, 1 BGM, 3 PERSONAL, 4 PADSPK,
	// 10 (pad/haptic-adjacent), and 126 for Audio3D output.
	if (type != 0 && type != 1 && type != 3 && type != 4 && type != 10 && type != 126) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (index != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

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

	if (format == HostAudio::Format::Unknown) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

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

	if (state == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

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

	if (vol == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

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
// Contracts reimplemented from public Gen5 export names and live-observed
// parameter layouts and call order: PortSetAttributes → Advance → Push.
constexpr int32_t  kMaxContexts         = 8;
constexpr int32_t  kMaxPorts            = 32;
constexpr int32_t  kMaxUsers            = 8;
constexpr uint64_t kDefaultContextBytes = 0x10000;
constexpr uint32_t kDefaultQueueDepth   = 4;
constexpr uint32_t kDefaultGrain        = 256;
constexpr uint32_t kDefaultSampleRate   = 48000;
// Port flag bit 1 reserves ~20 dB of digital headroom for platform mastering;
// restore it at the host boundary so MAIN beds are not 10× quieter.
constexpr uint32_t kPortFlag20DbHeadroom = 1u << 1;
constexpr float    kHeadroomGain         = 10.0f;
// AudioOut2 attribute entry: {u32 id, u32 pad, void* value, size_t value_size}.
constexpr size_t kAttributeStride = 0x18;
// data_format bits 0..6: 0 = f32, 1 = s16; bits 8..15: channel count.
constexpr uint32_t kDataFormatTypeMask = 0x7fu;

struct ContextSlot
{
	bool     used        = false;
	uint64_t generation  = 0;
	void*    buffer      = nullptr;
	uint64_t size        = 0;
	uint32_t queue_used  = 0;
	uint32_t queue_depth = kDefaultQueueDepth;
	uint32_t grain       = kDefaultGrain;
	uint32_t sample_rate = kDefaultSampleRate;
	// Host MAIN sink for this context (HostAudio handle as int; 0 = none).
	int host_handle = 0;
};

struct PortSlot
{
	bool     used         = false;
	int32_t  context      = 0;
	uint16_t type         = 0; // 0 = MAIN speaker bed
	uint32_t data_format  = 0x200; // f32 stereo default
	uint32_t sample_rate  = kDefaultSampleRate;
	uint32_t flags        = 0;
	// Current grain published by attribute id 0. It is copied out of guest
	// memory before publication so Push never retains a guest pointer.
	std::vector<uint8_t> pcm;
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
static std::mutex  g_audio_out2_mutex;
static uint64_t    g_next_context_generation = 1;
// C++-only host-state regression control; never registered as a guest export.
static std::atomic_bool g_audio_out2_fail_next_submit {false};

static int32_t AllocContextLocked()
{
	for (int32_t i = 0; i < kMaxContexts; i++)
	{
		if (!g_contexts[i].used)
		{
			g_contexts[i]            = ContextSlot {};
			g_contexts[i].used       = true;
			g_contexts[i].generation = g_next_context_generation++;
			if (g_next_context_generation == 0)
			{
				g_next_context_generation = 1;
			}
			return i + 1; // guest handles are 1-based
		}
	}
	return 0;
}

static int32_t AllocPortLocked(int32_t context)
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

static int32_t AllocUserLocked(int user_id)
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

static bool DecodeDataFormat(uint32_t data_format, uint32_t* channels_out, uint32_t* sample_bytes_out, bool* is_float_out)
{
	if (channels_out == nullptr || sample_bytes_out == nullptr || is_float_out == nullptr)
	{
		return false;
	}
	uint32_t channels = (data_format >> 8u) & 0xffu;
	if (channels == 0)
	{
		channels = 2;
	}
	// AudioOut's established host path covers only the documented channel
	// layouts. Do not silently fold an unmeasured layout into stereo.
	if (channels != 1 && channels != 2 && channels != 8)
	{
		return false;
	}
	const uint32_t data_type = data_format & kDataFormatTypeMask;
	if (data_type == 0)
	{
		*is_float_out     = true;
		*sample_bytes_out = 4;
	} else if (data_type == 1)
	{
		*is_float_out     = false;
		*sample_bytes_out = 2;
	} else
	{
		return false;
	}
	*channels_out = channels;
	return true;
}

static bool IsGuestWritableRange(const void* pointer, size_t size)
{
	return pointer != nullptr && size != 0 &&
	       Core::VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(pointer), static_cast<uint64_t>(size));
}

static bool CopyFromGuest(void* destination, const void* source, size_t size)
{
	return destination != nullptr && source != nullptr && size != 0 &&
	       Core::VirtualMemory::CopyFromGuest(destination, reinterpret_cast<uint64_t>(source), static_cast<uint64_t>(size));
}

static bool CopyToGuest(void* destination, const void* source, size_t size)
{
	return destination != nullptr && source != nullptr && size != 0 &&
	       Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(destination), source, static_cast<uint64_t>(size));
}

static bool CalculatePcmGrainBytes(uint32_t grain, uint32_t channels, uint32_t sample_bytes, size_t* bytes_out)
{
	if (bytes_out == nullptr || grain == 0 || channels == 0 || sample_bytes == 0)
	{
		return false;
	}
	constexpr size_t max_size = std::numeric_limits<size_t>::max();
	if (channels > max_size / sample_bytes)
	{
		return false;
	}
	const size_t frame_bytes = static_cast<size_t>(channels) * sample_bytes;
	if (grain > max_size / frame_bytes)
	{
		return false;
	}
	*bytes_out = static_cast<size_t>(grain) * frame_bytes;
	return true;
}

static float ReadNormalizedSample(const uint8_t* frame, uint32_t channel, uint32_t sample_bytes, bool is_float)
{
	const uint8_t* sample = frame + static_cast<size_t>(channel) * sample_bytes;
	if (is_float)
	{
		float value = 0.0f;
		std::memcpy(&value, sample, sizeof(value));
		return std::isfinite(value) ? value : 0.0f;
	}
	int16_t value = 0;
	std::memcpy(&value, sample, sizeof(value));
	return static_cast<float>(value) * (1.0f / 32768.0f);
}

// Fold one source frame into stereo L/R. Identity for mono/stereo; first-pair
// plus centre bleed for wider beds (MAIN only; object/aux ports stay unmixed).
static void MixFrameToStereo(const uint8_t* frame, uint32_t channels, uint32_t sample_bytes, bool is_float, float gain,
                             float* left, float* right)
{
	if (channels == 1)
	{
		const float m = ReadNormalizedSample(frame, 0, sample_bytes, is_float) * gain;
		*left += m;
		*right += m;
		return;
	}
	const float fl = ReadNormalizedSample(frame, 0, sample_bytes, is_float) * gain;
	const float fr = ReadNormalizedSample(frame, 1, sample_bytes, is_float) * gain;
	*left += fl;
	*right += fr;
	if (channels >= 3)
	{
		const float centre = ReadNormalizedSample(frame, 2, sample_bytes, is_float) * gain * 0.70710678f;
		*left += centre;
		*right += centre;
	}
	if (channels >= 6)
	{
		// 5.1-ish: LFE / surrounds contribute gently so wide beds are not lost.
		const float lfe = ReadNormalizedSample(frame, 3, sample_bytes, is_float) * gain * 0.5f;
		*left += lfe;
		*right += lfe;
		const float sl = ReadNormalizedSample(frame, 4, sample_bytes, is_float) * gain * 0.70710678f;
		const float sr = ReadNormalizedSample(frame, 5, sample_bytes, is_float) * gain * 0.70710678f;
		*left += sl;
		*right += sr;
	}
}

static bool EnsureHostSinkLocked(ContextSlot* ctx)
{
	if (ctx == nullptr)
	{
		return false;
	}
	if (ctx->host_handle != 0)
	{
		return true;
	}
	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr)
	{
		return false;
	}
	// Type 0 is the primary output and opens the host output device.
	const auto id = audio->AudioOutOpen(0, ctx->grain, ctx->sample_rate, HostAudio::Format::FloatStereo);
	if (!id.IsValid())
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 4, "AudioOut2: host sink open failed (grain=%u rate=%u)\n", ctx->grain,
		               ctx->sample_rate);
		return false;
	}
	ctx->host_handle = id.ToInt();
	KYTY_LOG_DEBUG("\t AudioOut2 host sink handle = %d\n", ctx->host_handle);
	return true;
}

static void CloseHostSinkLocked(ContextSlot* ctx)
{
	if (ctx == nullptr || ctx->host_handle == 0)
	{
		return;
	}
	auto audio = std::atomic_load(&g_host_audio);
	if (audio != nullptr)
	{
		audio->AudioOutClose(HostAudio::Id(ctx->host_handle));
	}
	ctx->host_handle = 0;
}

static bool SubmitMixedGrain(int host_handle, const float* mix, uint32_t frames)
{
	if (host_handle == 0 || mix == nullptr || frames == 0)
	{
		return false;
	}
	if (g_audio_out2_fail_next_submit.exchange(false, std::memory_order_acq_rel))
	{
		return false;
	}
	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr)
	{
		return false;
	}
	HostAudio::OutputParam params[1];
	params[0].handle = HostAudio::Id(host_handle);
	params[0].data   = mix;
	uint32_t samples = 0;
	return audio->AudioOutOutputs(params, 1, &samples);
}

// sceAudioOut2Initialize (NID g2tViFIohHE)
int KYTY_SYSV_ABI AudioOut2Initialize()
{
	PRINT_NAME();
	std::lock_guard lock(g_audio_out2_mutex);
	g_audio_out2_ready = true;
	return OK;
}

// sceAudioOut2ContextResetParam (NID t5YrizufpQc)
int KYTY_SYSV_ABI AudioOut2ContextResetParam(void* param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	// This output block has no size argument and no measured field layout.
	// Reject both null and non-null forms without touching guest memory.
	(void)param;
	return LibKernel::KERNEL_ERROR_EINVAL;
}

// sceAudioOut2ContextQueryMemory (NID pDmme7Bgm6E)
int KYTY_SYSV_ABI AudioOut2ContextQueryMemory(const void* param, uint64_t* size_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t param    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	KYTY_LOG_DEBUG("\t size_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(size_out));
	// The size depends on the unmeasured ContextParam layout. Returning a host
	// default would make guest allocation decisions from invented ABI data.
	(void)param;
	(void)size_out;
	return LibKernel::KERNEL_ERROR_EINVAL;
}

// sceAudioOut2ContextCreate (NID 0x6o1VVAYSY)
int KYTY_SYSV_ABI AudioOut2ContextCreate(const void* param, void* buffer, uint64_t size, int32_t* handle_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t param      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	KYTY_LOG_DEBUG("\t buffer     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer));
	KYTY_LOG_DEBUG("\t size       = 0x%016" PRIx64 "\n", size);
	KYTY_LOG_DEBUG("\t handle_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handle_out));
	// The required ContextParam and workspace semantics are unmeasured. Do not
	// create a public context from a null/default interpretation either.
	(void)param;
	(void)buffer;
	(void)size;
	(void)handle_out;
	return LibKernel::KERNEL_ERROR_EINVAL;
}

namespace HostStateTest {

int CreateContext()
{
	// C++ test seam only. This bypasses no guest ABI: the exported
	// AudioOut2ContextCreate remains a strict unsupported operation.
	std::lock_guard lock(g_audio_out2_mutex);
	return AllocContextLocked();
}

void FailNextSubmit()
{
	g_audio_out2_fail_next_submit.store(true, std::memory_order_release);
}

int GetContextSinkHandle(int context)
{
	std::lock_guard lock(g_audio_out2_mutex);
	if (context < 1 || context > kMaxContexts || !g_contexts[context - 1].used)
	{
		return 0;
	}
	return g_contexts[context - 1].host_handle;
}

void FillContextQueue(int context)
{
	std::lock_guard lock(g_audio_out2_mutex);
	if (context < 1 || context > kMaxContexts || !g_contexts[context - 1].used)
	{
		return;
	}
	auto& ctx      = g_contexts[context - 1];
	ctx.queue_used = ctx.queue_depth != 0 ? ctx.queue_depth : kDefaultQueueDepth;
}

} // namespace HostStateTest

// sceAudioOut2ContextDestroy (NID on6ZH7Abo10)
int KYTY_SYSV_ABI AudioOut2ContextDestroy(int32_t handle)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	std::lock_guard lock(g_audio_out2_mutex);
	if (handle < 1 || handle > kMaxContexts || !g_contexts[handle - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	CloseHostSinkLocked(&g_contexts[handle - 1]);
	for (auto& port: g_ports)
	{
		if (port.used && port.context == handle)
		{
			port = PortSlot {};
		}
	}
	g_contexts[handle - 1] = ContextSlot {};
	return OK;
}

// sceAudioOut2ContextAdvance (NID PE2zHMqLSHs)
// Updates the public queue clock; PCM submission lives in ContextPush.
int KYTY_SYSV_ABI AudioOut2ContextAdvance(int32_t handle)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	std::lock_guard lock(g_audio_out2_mutex);
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
// ABI: (ctx, blocking). Mix MAIN ports' published PCM grains into a stereo bed
// and deliver through HostAudio. Second argument is a blocking flag, not data.
int KYTY_SYSV_ABI AudioOut2ContextPush(int32_t handle, uint32_t blocking)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d blocking = %u\n", handle, blocking);

	if (blocking > 1)
	{
		return AUDIO_OUT_ERROR_INVALID_FLAG;
	}

	std::unique_lock lock(g_audio_out2_mutex);
	if (handle < 1 || handle > kMaxContexts || !g_contexts[handle - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	auto& ctx = g_contexts[handle - 1];
	const uint64_t context_generation = ctx.generation;
	const uint32_t grain = (ctx.grain >= 64 && ctx.grain <= 4096) ? ctx.grain : kDefaultGrain;
	const uint32_t sample_rate = ctx.sample_rate != 0 ? ctx.sample_rate : kDefaultSampleRate;
	const uint32_t queue_depth = ctx.queue_depth != 0 ? ctx.queue_depth : kDefaultQueueDepth;

	// Blocking-when-full: wait one grain when the emulated queue is saturated.
	while (ctx.queue_used >= queue_depth)
	{
		if (blocking == 0)
		{
			return AUDIO_OUT_ERROR_PORT_FULL;
		}
		lock.unlock();
		const auto ns = std::chrono::nanoseconds(static_cast<int64_t>(grain) * 1'000'000'000LL /
		                                         static_cast<int64_t>(sample_rate));
		std::this_thread::sleep_for(ns);
		lock.lock();
		if (!ctx.used || ctx.generation != context_generation)
		{
			return LibKernel::KERNEL_ERROR_EINVAL;
		}
		if (ctx.queue_used > 0)
		{
			ctx.queue_used--;
		}
	}

	std::vector<float> mix(static_cast<size_t>(grain) * 2u, 0.0f);
	std::vector<PortSlot*> consumed_ports;

	for (auto& port: g_ports)
	{
		if (!port.used || port.context != handle || port.pcm.empty())
		{
			continue;
		}
		// Only MAIN (type 0) drives the host speakers. Aux/personal/object
		// ports stay unmixed until their routing contracts are evidenced.
		if (port.type != 0)
		{
			continue;
		}
		uint32_t channels     = 0;
		uint32_t sample_bytes = 0;
		bool     is_float     = false;
		if (!DecodeDataFormat(port.data_format, &channels, &sample_bytes, &is_float))
		{
			return AUDIO_OUT_ERROR_INVALID_FORMAT;
		}
		size_t grain_bytes = 0;
		if (!CalculatePcmGrainBytes(grain, channels, sample_bytes, &grain_bytes) || port.pcm.size() != grain_bytes)
		{
			return LibKernel::KERNEL_ERROR_EINVAL;
		}
		const float  gain        = (port.flags & kPortFlag20DbHeadroom) != 0 ? kHeadroomGain : 1.0f;
		const auto*  base        = port.pcm.data();
		const size_t frame_bytes = static_cast<size_t>(channels) * sample_bytes;
		for (uint32_t frame = 0; frame < grain; frame++)
		{
			const uint8_t* src = base + static_cast<size_t>(frame) * frame_bytes;
			MixFrameToStereo(src, channels, sample_bytes, is_float, gain, &mix[frame * 2u], &mix[frame * 2u + 1u]);
		}
		consumed_ports.push_back(&port);
	}

	if (consumed_ports.empty())
	{
		return OK;
	}
	// Keeping the context lock through submission prevents close/recreate from
	// rebinding this grain to a recycled sink handle.
	if (!EnsureHostSinkLocked(&ctx) || !SubmitMixedGrain(ctx.host_handle, mix.data(), grain))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "AudioOut2: host submit failed for ctx %d\n", handle);
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}
	for (auto* port: consumed_ports)
	{
		port->pcm.clear();
	}
	ctx.queue_used++;

	return OK;
}

// sceAudioOut2ContextGetQueueLevel (NID R7d0F1g2qsU)
int KYTY_SYSV_ABI AudioOut2ContextGetQueueLevel(int32_t handle, uint32_t* used, uint32_t* available)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle    = %d\n", handle);
	KYTY_LOG_DEBUG("\t used      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(used));
	KYTY_LOG_DEBUG("\t available = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(available));
	if ((used != nullptr && !IsGuestWritableRange(used, sizeof(*used))) ||
	    (available != nullptr && !IsGuestWritableRange(available, sizeof(*available))))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::lock_guard lock(g_audio_out2_mutex);
	if (handle < 1 || handle > kMaxContexts || !g_contexts[handle - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	const auto&    ctx = g_contexts[handle - 1];
	const uint32_t q   = ctx.queue_used;
	const uint32_t depth = ctx.queue_depth != 0 ? ctx.queue_depth : kDefaultQueueDepth;
	const uint32_t available_value = (q < depth) ? (depth - q) : 0;
	if ((used != nullptr && !CopyToGuest(used, &q, sizeof(q))) ||
	    (available != nullptr && !CopyToGuest(available, &available_value, sizeof(available_value))))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return OK;
}

// sceAudioOut2PortCreate (NID JK2wamZPzwM)
// portParam: u16 type @0, u16 pad @2, u32 data_format @4, u32 sample_rate @8, u32 flags @0xc.
int KYTY_SYSV_ABI AudioOut2PortCreate(int32_t context, const void* param, int32_t* port_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t context  = %d\n", context);
	KYTY_LOG_DEBUG("\t param    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	KYTY_LOG_DEBUG("\t port_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(port_out));
	if (port_out == nullptr || !IsGuestWritableRange(port_out, sizeof(*port_out)))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	uint16_t type        = 0;
	uint32_t data_format = 0x200;
	uint32_t sample_rate = kDefaultSampleRate;
	uint32_t flags       = 0;
	if (param != nullptr)
	{
		constexpr size_t kPortParamSize = 0x10;
		uint8_t          param_bytes[kPortParamSize] {};
		if (!CopyFromGuest(param_bytes, param, sizeof(param_bytes)))
		{
			return LibKernel::KERNEL_ERROR_EINVAL;
		}
		std::memcpy(&type, param_bytes + 0, sizeof(type));
		std::memcpy(&data_format, param_bytes + 4, sizeof(data_format));
		uint32_t freq = 0;
		std::memcpy(&freq, param_bytes + 8, sizeof(freq));
		if (freq >= 8000 && freq <= 192000)
		{
			sample_rate = freq;
		}
		std::memcpy(&flags, param_bytes + 12, sizeof(flags));
	}

	uint32_t channels     = 2;
	uint32_t sample_bytes = 4;
	bool     is_float     = true;
	if (!DecodeDataFormat(data_format, &channels, &sample_bytes, &is_float))
	{
		return AUDIO_OUT_ERROR_INVALID_FORMAT;
	}

	std::lock_guard lock(g_audio_out2_mutex);
	if (context < 1 || context > kMaxContexts || !g_contexts[context - 1].used)
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}
	const int32_t id = AllocPortLocked(context);
	if (id == 0)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	auto& port        = g_ports[id - 1];
	port.type         = type;
	port.data_format  = data_format;
	port.sample_rate  = sample_rate;
	port.flags        = flags;
	port.channels     = static_cast<uint8_t>(std::min<uint32_t>(channels, 255));
	port.output       = 0x01;
	port.status       = -1;
	port.pcm.clear();
	if (!CopyToGuest(port_out, &id, sizeof(id)))
	{
		port = PortSlot {};
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	KYTY_LOG_DEBUG("\t port=%d type=0x%x format=0x%x rate=%u flags=0x%x ch=%u\n", id, type, data_format,
	               sample_rate, flags, channels);
	return OK;
}

// sceAudioOut2PortDestroy (NID cd+Rtw+D1x8)
int KYTY_SYSV_ABI AudioOut2PortDestroy(int32_t port)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t port = %d\n", port);
	std::lock_guard lock(g_audio_out2_mutex);
	if (port < 1 || port > kMaxPorts || !g_ports[port - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	g_ports[port - 1] = PortSlot {};
	return OK;
}

// sceAudioOut2PortSetAttributes (NID 8XTArSPyWHk)
// Attribute id 0 = per-grain PCM pointer. value points at a guest qword that
// holds the address of the interleaved grain (double-indirect).
int KYTY_SYSV_ABI AudioOut2PortSetAttributes(int32_t port, const void* attrs, uint32_t count)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t port  = %d\n", port);
	KYTY_LOG_DEBUG("\t attrs = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(attrs));
	KYTY_LOG_DEBUG("\t count = %u\n", count);

	if (count == 0)
	{
		std::lock_guard lock(g_audio_out2_mutex);
		if (port < 1 || port > kMaxPorts || !g_ports[port - 1].used)
		{
			return LibKernel::KERNEL_ERROR_EINVAL;
		}
		auto& p = g_ports[port - 1];
		p.status = 0; // configured / ready
		return OK;
	}
	constexpr uint32_t kMaxAttributeEntries = 32;
	if (attrs == nullptr || count != 1 || count > kMaxAttributeEntries || count > std::numeric_limits<size_t>::max() / kAttributeStride)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	const size_t attrs_size = static_cast<size_t>(count) * kAttributeStride;
	std::vector<uint8_t> attrs_snapshot(attrs_size);
	if (!CopyFromGuest(attrs_snapshot.data(), attrs, attrs_snapshot.size()))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	std::lock_guard lock(g_audio_out2_mutex);
	if (port < 1 || port > kMaxPorts || !g_ports[port - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	auto& p = g_ports[port - 1];
	if (p.context < 1 || p.context > kMaxContexts || !g_contexts[p.context - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const auto* entry = attrs_snapshot.data();
	uint32_t    id    = 0;
	uint64_t    vptr  = 0;
	uint64_t    vsize = 0;
	std::memcpy(&id, entry + 0, sizeof(id));
	std::memcpy(&vptr, entry + 8, sizeof(vptr));
	std::memcpy(&vsize, entry + 16, sizeof(vsize));

	if (id != 0 || vsize != sizeof(uintptr_t) || vptr == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	uintptr_t pcm_address = 0;
	if (!CopyFromGuest(&pcm_address, reinterpret_cast<const void*>(static_cast<uintptr_t>(vptr)), sizeof(pcm_address)) ||
	    pcm_address == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	uint32_t channels     = 0;
	uint32_t sample_bytes = 0;
	bool     is_float     = false;
	if (!DecodeDataFormat(p.data_format, &channels, &sample_bytes, &is_float))
	{
		return AUDIO_OUT_ERROR_INVALID_FORMAT;
	}
	size_t grain_bytes = 0;
	if (!CalculatePcmGrainBytes(g_contexts[p.context - 1].grain, channels, sample_bytes, &grain_bytes))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::vector<uint8_t> pcm_snapshot(grain_bytes);
	if (!CopyFromGuest(pcm_snapshot.data(), reinterpret_cast<const void*>(pcm_address), pcm_snapshot.size()))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	p.pcm = std::move(pcm_snapshot);
	KYTY_LOG_DEBUG("\t copied pcm grain = 0x%016" PRIxPTR "\n", pcm_address);
	p.status = 0; // configured / ready
	return OK;
}

// sceAudioOut2PortGetState (NID gatEUKG+Ea4)
int KYTY_SYSV_ABI AudioOut2PortGetState(int32_t port, void* state_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t port      = %d\n", port);
	KYTY_LOG_DEBUG("\t state_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(state_out));
	constexpr size_t kPortStateSize = 0x20;
	if (port < 1 || port > kMaxPorts || state_out == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (!IsGuestWritableRange(state_out, kPortStateSize))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::lock_guard lock(g_audio_out2_mutex);
	if (!g_ports[port - 1].used)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	// Fixed 0x20-byte connected-port state.
	const auto&      p              = g_ports[port - 1];
	uint8_t          blob[kPortStateSize] {};
	blob[0] = static_cast<uint8_t>(p.output & 0xffu);
	blob[1] = static_cast<uint8_t>((p.output >> 8) & 0xffu);
	blob[2] = p.channels;
	// Volume field: -1 means N/A for MAIN (observed Gen5 convention).
	const int16_t volume = -1;
	blob[4]              = static_cast<uint8_t>(static_cast<uint16_t>(volume) & 0xffu);
	blob[5]              = static_cast<uint8_t>((static_cast<uint16_t>(volume) >> 8) & 0xffu);
	return CopyToGuest(state_out, blob, sizeof(blob)) ? OK : LibKernel::KERNEL_ERROR_EINVAL;
}

// sceAudioOut2UserCreate (NID xywYcRB7nbQ)
int KYTY_SYSV_ABI AudioOut2UserCreate(uint32_t user_id, uintptr_t* user_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t user_id  = %u\n", user_id);
	KYTY_LOG_DEBUG("\t user_out = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_out));
	if (user_out == nullptr || !IsGuestWritableRange(user_out, sizeof(*user_out)))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::lock_guard lock(g_audio_out2_mutex);
	const int32_t   id = AllocUserLocked(static_cast<int>(user_id));
	if (id == 0)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	const uintptr_t user = static_cast<uintptr_t>(id);
	if (!CopyToGuest(user_out, &user, sizeof(user)))
	{
		g_users[id - 1] = UserSlot {};
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	KYTY_LOG_DEBUG("\t user     = %d\n", id);
	return OK;
}

// sceAudioOut2UserDestroy (NID IaZXJ9M79uo)
int KYTY_SYSV_ABI AudioOut2UserDestroy(uintptr_t user)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t user = 0x%016" PRIxPTR "\n", user);
	std::lock_guard lock(g_audio_out2_mutex);
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

	if (user_id != 255 && user_id != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (type != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (index != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	HostAudio::Format format = HostAudio::Format::Unknown;

	switch (param)
	{
		case 0: format = HostAudio::Format::Signed16bitMono; break;
		case 2: format = HostAudio::Format::Signed16bitStereo; break;
		default:;
	}

	KYTY_LOG_DEBUG("\t param   = %u (%s)\n", param, Core::EnumName(format).C_Str());

	if (format == HostAudio::Format::Unknown) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

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

	if (dest == nullptr)
	{
		return AUDIO_IN_ERROR_INVALID_POINTER;
	}

	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr || !audio->AudioInValid(HostAudio::Id(handle)))
	{
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}

	return static_cast<int>(audio->AudioInInput(HostAudio::Id(handle), dest));
}

// sceAudioInClose (NID Jh6WbHhnI68).
int KYTY_SYSV_ABI AudioInClose(int handle)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t handle = %d\n", handle);

	auto audio = std::atomic_load(&g_host_audio);
	if (audio == nullptr || !audio->AudioInClose(HostAudio::Id(handle)))
	{
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}

	return OK;
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
