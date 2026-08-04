#include "Emulator/Audio.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <cmath>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

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

	KYTY_LOG_DEBUG("\t rack_id    = 0x%" PRIx32 "\n", rack_id);
	const uint32_t max_voices = Ngs2GetRackMaxVoices(rack_id, option);
	KYTY_LOG_DEBUG("\t max_voices = %u\n", max_voices);

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

	KYTY_LOG_DEBUG("\t name              = %.16s\n", option->name);
	KYTY_LOG_DEBUG("\t flags             = %u\n", option->flags);
	KYTY_LOG_DEBUG("\t max_grain_samples = %u\n", option->max_grain_samples);
	KYTY_LOG_DEBUG("\t num_grain_samples = %u\n", option->num_grain_samples);
	KYTY_LOG_DEBUG("\t sample_rate       = %u\n", option->sample_rate);
	KYTY_LOG_DEBUG("\t alloc_handler     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->alloc_handler));
	KYTY_LOG_DEBUG("\t free_handler      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->free_handler));
	KYTY_LOG_DEBUG("\t user_data         = 0x%016" PRIx64 "\n", static_cast<uint64_t>(allocator->user_data));

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

	KYTY_LOG_DEBUG("\t rack_id                = 0x%" PRIx32 "\n", rack_id);
	KYTY_LOG_DEBUG("\t option_size            = 0x%016" PRIx64 "\n", static_cast<uint64_t>(option->size));
	KYTY_LOG_DEBUG("\t name                   = %.16s\n", option->name);
	KYTY_LOG_DEBUG("\t flags                  = %u\n", option->flags);
	KYTY_LOG_DEBUG("\t max_grain_samples      = %u\n", option->max_grain_samples);
	KYTY_LOG_DEBUG("\t max_voices             = %u\n", option->max_voices);
	KYTY_LOG_DEBUG("\t max_input_delay_blocks = %u\n", option->max_input_delay_blocks);
	KYTY_LOG_DEBUG("\t max_matrices           = %u\n", option->max_matrices);
	KYTY_LOG_DEBUG("\t max_ports              = %u\n", option->max_ports);
	KYTY_LOG_DEBUG("\t host_buffer            = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer_info->host_buffer));
	KYTY_LOG_DEBUG("\t host_buffer_size      = 0x%016" PRIx64 "\n", static_cast<uint64_t>(buffer_info->host_buffer_size));

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

	KYTY_LOG_DEBUG("\t type                   = %s\n", Core::EnumName(rack->type).C_Str());

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

	KYTY_LOG_DEBUG("\t rack_id                = 0x%" PRIx32 "\n", rack_id);
	KYTY_LOG_DEBUG("\t name                   = %.16s\n", option->name);
	KYTY_LOG_DEBUG("\t flags                  = %u\n", option->flags);
	KYTY_LOG_DEBUG("\t max_grain_samples      = %u\n", option->max_grain_samples);
	KYTY_LOG_DEBUG("\t max_voices             = %u\n", option->max_voices);
	KYTY_LOG_DEBUG("\t max_input_delay_blocks = %u\n", option->max_input_delay_blocks);
	KYTY_LOG_DEBUG("\t max_matrices           = %u\n", option->max_matrices);
	KYTY_LOG_DEBUG("\t max_ports              = %u\n", option->max_ports);
	KYTY_LOG_DEBUG("\t alloc_handler          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->alloc_handler));
	KYTY_LOG_DEBUG("\t free_handler           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->free_handler));
	KYTY_LOG_DEBUG("\t user_data              = 0x%016" PRIx64 "\n", static_cast<uint64_t>(allocator->user_data));

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

	KYTY_LOG_DEBUG("\t voice_id = %u\n", voice_id);

	auto* rack   = reinterpret_cast<Ngs2RackInternal*>(rack_handle);
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack_handle + sizeof(Ngs2RackInternal));

	const uint32_t max_voices = rack->option.common.max_voices;
	KYTY_LOG_DEBUG("\t max_voices = %u\n", max_voices);

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
		KYTY_LOG_DEBUG("\t id   = 0x%08" PRIx32 "\n", param->id);
		KYTY_LOG_DEBUG("\t size = %" PRIu16 "\n", param->size);
		KYTY_LOG_DEBUG("\t next = %" PRId16 "\n", param->next);

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
						KYTY_LOG_DEBUG("\t port      = %u\n", pm->port);
						KYTY_LOG_DEBUG("\t matrix_id = %d\n", pm->matrix_id);
						break;
					}
					case 0x0005:
					{
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePatchParam));
						const auto* patch = reinterpret_cast<const Ngs2VoicePatchParam*>(param);
						KYTY_LOG_DEBUG("\t connect->port          = %u\n", patch->port);
						KYTY_LOG_DEBUG("\t connect->dest_input_id = %u\n", patch->dest_input_id);
						KYTY_LOG_DEBUG("\t connect->dest_handle   = 0x%016" PRIx64 "\n", patch->dest_handle);
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
						KYTY_LOG_DEBUG("\t event = %u\n", event->event_id);
						break;
					}
					case 0x0007:
					{
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceCallbackParam));
						const auto* cb = reinterpret_cast<const Ngs2VoiceCallbackParam*>(param);
						KYTY_LOG_DEBUG("\t callback_handler = 0x%016" PRIx64 "\n", static_cast<uint64_t>(cb->callback_handler));
						KYTY_LOG_DEBUG("\t callback_data    = 0x%016" PRIx64 "\n", static_cast<uint64_t>(cb->callback_data));
						KYTY_LOG_DEBUG("\t flags            = 0x%08" PRIx32 "\n", cb->flags);
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

	KYTY_LOG_DEBUG("\t command = {%08" PRIx32 ", %08" PRIx32 ", %08" PRIx32 "}\n", voice->last_command[0], voice->last_command[1],
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

	KYTY_LOG_DEBUG("\t flags = %u\n", flags);
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

	KYTY_LOG_DEBUG("\t flags = %u\n", flags);
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
			KYTY_LOG_DEBUG("\t state_flags = %u\n", sampler->voice_state.state_flags);
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
	KYTY_LOG_DEBUG("\t state_flags = %u\n", *state_flags);

	return OK;
}

} // namespace Ngs2

} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED
