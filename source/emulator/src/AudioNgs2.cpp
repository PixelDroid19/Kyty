#include "Emulator/Audio.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <array>
#include <cmath>
#include <condition_variable>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>
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

struct Ngs2ContextBufferInfo
{
	void*     host_buffer      = nullptr;
	size_t    host_buffer_size = 0;
	uintptr_t reserved[5]      = {};
	uintptr_t user_data        = 0;
};
static_assert(sizeof(Ngs2ContextBufferInfo) == 64);

using Ngs2BufferAllocHandler = int32_t KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);
using Ngs2BufferFreeHandler  = int32_t KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);

struct Ngs2BufferAllocator
{
	Ngs2BufferAllocHandler alloc_handler = nullptr;
	Ngs2BufferFreeHandler  free_handler  = nullptr;
	uintptr_t              user_data     = 0;
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

struct Ngs2VoiceParamHeader
{
	uint16_t size;
	int16_t  next;
	uint32_t id;
};
static_assert(sizeof(Ngs2VoiceParamHeader) == 8);

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
static_assert(sizeof(Ngs2SamplerVoiceState) == 48);

namespace {

constexpr int32_t kNgs2InvalidOut           = static_cast<int32_t>(0x804a0053u);
constexpr int32_t kNgs2InvalidOption        = static_cast<int32_t>(0x804a0081u);
constexpr int32_t kNgs2InvalidSystem        = static_cast<int32_t>(0x804a0201u);
constexpr int32_t kNgs2InvalidBufferInfo    = static_cast<int32_t>(0x804a0206u);
constexpr int32_t kNgs2InvalidBufferAddress = static_cast<int32_t>(0x804a0207u);
constexpr int32_t kNgs2InvalidBufferSize    = static_cast<int32_t>(0x804a0209u);
constexpr int32_t kNgs2InvalidRack          = static_cast<int32_t>(0x804a0261u);
constexpr int32_t kNgs2InvalidVoice         = static_cast<int32_t>(0x804a0300u);
constexpr int32_t kNgs2InvalidControl       = static_cast<int32_t>(0x804a0309u);

constexpr uint32_t kNgs2DefaultMaxGrainSamples = 512;
constexpr uint32_t kNgs2DefaultGrainSamples    = 256;
constexpr uint32_t kNgs2DefaultSampleRate      = 48000;
constexpr uint32_t kNgs2MaxGrainSamples        = 8192;
constexpr uint32_t kNgs2MaxRackVoices          = 256;
constexpr size_t   kNgs2MaxRackOptionBytes     = 0x518;

// Workspaces are guest ABI identities only. These opaque slots are never
// dereferenced or populated by HLE state; all mutable state lives in host
// records below.
constexpr size_t kNgs2SystemWorkspaceBytes      = sizeof(uintptr_t);
constexpr size_t kNgs2RackWorkspaceHeaderBytes  = sizeof(uintptr_t);
constexpr size_t kNgs2VoiceWorkspaceSlotBytes   = sizeof(uintptr_t);

static bool Ngs2CopyFromGuest(void* destination, const void* source, size_t size)
{
	return destination != nullptr && source != nullptr && size != 0 &&
	       Core::VirtualMemory::CopyFromGuest(destination, reinterpret_cast<uint64_t>(source), static_cast<uint64_t>(size));
}

static bool Ngs2CopyToGuest(void* destination, const void* source, size_t size)
{
	return destination != nullptr && source != nullptr && size != 0 &&
	       Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(destination), source, static_cast<uint64_t>(size));
}

template <typename T> static bool Ngs2ReadGuest(T* destination, const T* source)
{
	return Ngs2CopyFromGuest(destination, source, sizeof(T));
}

template <typename T> static bool Ngs2WriteGuest(T* destination, const T& source)
{
	return Ngs2CopyToGuest(destination, &source, sizeof(T));
}

static bool Ngs2IsGuestWritable(const void* pointer, size_t size)
{
	return pointer != nullptr && size != 0 &&
	       Core::VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(pointer), static_cast<uint64_t>(size));
}

static bool Ngs2CalculatePcmBytes(uint64_t frames, uint32_t channels, size_t* bytes_out)
{
	if (bytes_out == nullptr || frames == 0 || channels == 0)
	{
		return false;
	}
	constexpr size_t kBytesPerSample = sizeof(int16_t);
	if (channels > std::numeric_limits<size_t>::max() / kBytesPerSample)
	{
		return false;
	}
	const size_t bytes_per_frame = static_cast<size_t>(channels) * kBytesPerSample;
	if (frames > std::numeric_limits<size_t>::max() / bytes_per_frame)
	{
		return false;
	}
	*bytes_out = static_cast<size_t>(frames) * bytes_per_frame;
	return true;
}

static bool Ngs2CalculateRackWorkspaceSize(uint32_t max_voices, size_t* size_out)
{
	if (size_out == nullptr || max_voices > kNgs2MaxRackVoices ||
	    max_voices > (std::numeric_limits<size_t>::max() - kNgs2RackWorkspaceHeaderBytes) / kNgs2VoiceWorkspaceSlotBytes)
	{
		return false;
	}
	*size_out = kNgs2RackWorkspaceHeaderBytes + static_cast<size_t>(max_voices) * kNgs2VoiceWorkspaceSlotBytes;
	return true;
}

static bool Ngs2SnapshotSystemOption(const Ngs2SystemOption* option, Ngs2SystemOption* snapshot)
{
	if (snapshot == nullptr)
	{
		return false;
	}
	*snapshot = {};
	if (option == nullptr)
	{
		snapshot->size              = sizeof(*snapshot);
		snapshot->max_grain_samples = kNgs2DefaultMaxGrainSamples;
		snapshot->num_grain_samples = kNgs2DefaultGrainSamples;
		snapshot->sample_rate       = kNgs2DefaultSampleRate;
		return true;
	}
	if (!Ngs2ReadGuest(snapshot, option) || snapshot->size != sizeof(*snapshot))
	{
		return false;
	}
	return snapshot->max_grain_samples != 0 && snapshot->max_grain_samples <= kNgs2MaxGrainSamples &&
	       snapshot->num_grain_samples != 0 && snapshot->num_grain_samples <= snapshot->max_grain_samples &&
	       snapshot->num_grain_samples <= kNgs2MaxGrainSamples && snapshot->sample_rate == kNgs2DefaultSampleRate;
}

static bool Ngs2SupportedRackOptionSize(uint32_t rack_id, size_t size)
{
	switch (rack_id)
	{
		case 0x1000: return size == sizeof(Ngs2SamplerRackOption);
		case 0x2000: return size == sizeof(Ngs2SubmixerRackOption);
		case 0x2001: return size == sizeof(Ngs2ReverbRackOption) || size == 0xb8;
		case 0x3000: return size == sizeof(Ngs2MasteringRackOption);
		case 0x4001: return size == 0x518;
		case 0x4002: return size == sizeof(Ngs2CustomSubmixerRackOption);
		default: return false;
	}
}

static bool Ngs2RackTypeFromId(uint32_t rack_id, Ngs2RackType* type_out)
{
	if (type_out == nullptr)
	{
		return false;
	}
	switch (rack_id)
	{
		case 0x1000: *type_out = Ngs2RackType::Sampler; return true;
		case 0x2000: *type_out = Ngs2RackType::Submixer; return true;
		case 0x2001: *type_out = Ngs2RackType::Reverb; return true;
		case 0x3000: *type_out = Ngs2RackType::Mastering; return true;
		case 0x4001: *type_out = Ngs2RackType::CustomSampler; return true;
		case 0x4002: *type_out = Ngs2RackType::CustomSubmixer; return true;
		default: return false;
	}
}

struct Ngs2RackConfig
{
	Ngs2RackType                              type = Ngs2RackType::Sampler;
	uint32_t                                  max_voices = 0;
	size_t                                    option_size = 0;
	std::array<uint8_t, kNgs2MaxRackOptionBytes> option_bytes {};
};

static bool Ngs2MakeDefaultRackConfig(uint32_t rack_id, Ngs2RackConfig* config)
{
	if (config == nullptr || rack_id != 0x3000)
	{
		return false;
	}
	Ngs2MasteringRackOption option {};
	option.rack_option.size       = sizeof(option);
	option.rack_option.max_voices = 1;
	config->type                  = Ngs2RackType::Mastering;
	config->max_voices            = 1;
	config->option_size           = sizeof(option);
	std::memcpy(config->option_bytes.data(), &option, sizeof(option));
	return true;
}

static bool Ngs2SnapshotRackConfig(uint32_t rack_id, const Ngs2RackOption* option, Ngs2RackConfig* config)
{
	if (config == nullptr)
	{
		return false;
	}
	*config = {};
	if (option == nullptr)
	{
		return Ngs2MakeDefaultRackConfig(rack_id, config);
	}

	size_t option_size = 0;
	if (!Ngs2CopyFromGuest(&option_size, option, sizeof(option_size)) || !Ngs2SupportedRackOptionSize(rack_id, option_size) ||
	    option_size > config->option_bytes.size() || !Ngs2CopyFromGuest(config->option_bytes.data(), option, option_size) ||
	    !Ngs2RackTypeFromId(rack_id, &config->type))
	{
		return false;
	}

	uint32_t max_voices = 0;
	if (option_size >= 0xb0)
	{
		std::memcpy(&max_voices, config->option_bytes.data() + 0x50, sizeof(max_voices));
	}
	if (max_voices == 0)
	{
		std::memcpy(&max_voices, config->option_bytes.data() + offsetof(Ngs2RackOption, max_voices), sizeof(max_voices));
	}
	if (max_voices > kNgs2MaxRackVoices)
	{
		return false;
	}
	config->max_voices = max_voices;
	config->option_size = option_size;
	return true;
}

struct Ngs2PcmStream
{
	uint32_t             format_id   = 0;
	uint32_t             channels    = 0;
	uint32_t             sample_rate = 0;
	float                gain        = 1.0f;
	bool                 playing     = false;
	std::vector<int16_t> samples;
	uint64_t             frame_count  = 0;
	double               source_frame = 0.0;
};

struct Ngs2SystemRecord;
struct Ngs2RackRecord;
struct Ngs2VoiceRecord;

struct Ngs2SystemRecord
{
	std::recursive_mutex                                           state_mutex;
	Ngs2SystemOption                                               option {};
	uintptr_t                                                      workspace = 0;
	size_t                                                         workspace_size = 0;
	std::unordered_map<uintptr_t, std::shared_ptr<Ngs2RackRecord>> racks;
};

struct Ngs2RackRecord
{
	std::shared_ptr<Ngs2SystemRecord>                   system;
	uintptr_t                                            workspace = 0;
	size_t                                               workspace_size = 0;
	Ngs2RackType                                         type = Ngs2RackType::Sampler;
	uint32_t                                             max_voices = 0;
	size_t                                               option_size = 0;
	std::array<uint8_t, kNgs2MaxRackOptionBytes>        option_snapshot {};
	std::vector<std::shared_ptr<Ngs2VoiceRecord>>       voices;
};

struct Ngs2VoiceRecord
{
	std::shared_ptr<Ngs2SystemRecord> system;
	std::weak_ptr<Ngs2RackRecord>     rack;
	uintptr_t                          handle = 0;
	uint32_t                           voice_id = 0;
	Ngs2VoicePlayEvent                 event = Ngs2VoicePlayEvent::None;
	Ngs2VoicePlayState                 state = Ngs2VoicePlayState::Empty;
	uint32_t                           last_command[3] = {};
	uint32_t                           play_ticks = 0;
	Ngs2PcmStream                      stream;
};

static uint32_t Ngs2GetVoiceStateFlags(const Ngs2VoiceRecord& voice)
{
	switch (voice.state)
	{
		case Ngs2VoicePlayState::Empty: return 0;
		case Ngs2VoicePlayState::Playing: return 0x3;
		case Ngs2VoicePlayState::Paused: return 0x5;
		case Ngs2VoicePlayState::Stopped: return 0xb;
	}
	return 0;
}

static bool Ngs2MixPcmStream(Ngs2PcmStream* stream, float* output, uint32_t output_frames, uint32_t output_channels,
	                            uint32_t output_rate)
{
	if (stream == nullptr || output == nullptr || (stream->channels != 1 && stream->channels != 2) || output_channels != 2 ||
	    stream->sample_rate == 0 || output_rate == 0 || stream->frame_count == 0)
	{
		return false;
	}
	size_t expected_bytes = 0;
	if (!Ngs2CalculatePcmBytes(stream->frame_count, stream->channels, &expected_bytes) ||
	    stream->samples.size() != expected_bytes / sizeof(int16_t))
	{
		return false;
	}

	const double step = static_cast<double>(stream->sample_rate) / static_cast<double>(output_rate);
	for (uint32_t frame = 0; frame < output_frames; ++frame)
	{
		if (stream->source_frame >= static_cast<double>(stream->frame_count))
		{
			stream->playing = false;
			return false;
		}
		const auto  index      = static_cast<uint64_t>(stream->source_frame);
		const float fraction   = static_cast<float>(stream->source_frame - static_cast<double>(index));
		const auto  next_index = index + 1 < stream->frame_count ? index + 1 : index;
		for (uint32_t channel = 0; channel < output_channels; ++channel)
		{
			const uint32_t source_channel = stream->channels == 1 ? 0 : channel;
			const float current = static_cast<float>(stream->samples[index * stream->channels + source_channel]) / 32768.0f;
			const float next = static_cast<float>(stream->samples[next_index * stream->channels + source_channel]) / 32768.0f;
			float&      dest = output[frame * output_channels + channel];
			dest += (current + (next - current) * fraction) * stream->gain;
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

enum class Ngs2ObjectKind: uint8_t
{
	System,
	Rack,
	Voice
};

struct Ngs2LeaseIdentity
{
	Ngs2ObjectKind kind = Ngs2ObjectKind::System;
	uintptr_t      handle = 0;
	uint64_t       generation = 0;

	[[nodiscard]] explicit operator bool() const { return handle != 0 && generation != 0; }
};

template <typename T> struct Ngs2RegistryEntry
{
	std::shared_ptr<T> record;
	uint64_t           generation = 0;
	uint32_t           pins = 0;
	bool               closing = false;
};

std::mutex                                                          g_ngs_registry_mutex;
std::condition_variable                                             g_ngs_registry_changed;
std::unordered_map<uintptr_t, Ngs2RegistryEntry<Ngs2SystemRecord>> g_ngs_systems;
std::unordered_map<uintptr_t, Ngs2RegistryEntry<Ngs2RackRecord>>   g_ngs_racks;
std::unordered_map<uintptr_t, Ngs2RegistryEntry<Ngs2VoiceRecord>>  g_ngs_voices;
uint64_t                                                            g_ngs_next_generation = 1;

static uint64_t Ngs2NextGenerationLocked()
{
	const uint64_t generation = g_ngs_next_generation++;
	if (g_ngs_next_generation == 0)
	{
		g_ngs_next_generation = 1;
	}
	return generation;
}

static void Ngs2ReleaseLease(Ngs2LeaseIdentity identity)
{
	if (!identity)
	{
		return;
	}
	std::lock_guard lock(g_ngs_registry_mutex);
	auto release = [&](auto& records)
	{
		auto found = records.find(identity.handle);
		EXIT_IF(found == records.end() || found->second.generation != identity.generation || found->second.pins == 0);
		found->second.pins--;
		if (found->second.pins == 0)
		{
			g_ngs_registry_changed.notify_all();
		}
	};
	switch (identity.kind)
	{
		case Ngs2ObjectKind::System: release(g_ngs_systems); break;
		case Ngs2ObjectKind::Rack: release(g_ngs_racks); break;
		case Ngs2ObjectKind::Voice: release(g_ngs_voices); break;
	}
}

template <typename T> class Ngs2Lease
{
public:
	Ngs2Lease() = default;
	Ngs2Lease(std::shared_ptr<T> record, Ngs2LeaseIdentity identity): m_record(std::move(record)), m_identity(identity) {}
	Ngs2Lease(Ngs2Lease&& other) noexcept: m_record(std::move(other.m_record)), m_identity(other.m_identity)
	{
		other.m_identity = {};
	}
	Ngs2Lease& operator=(Ngs2Lease&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_record         = std::move(other.m_record);
			m_identity       = other.m_identity;
			other.m_identity = {};
		}
		return *this;
	}
	~Ngs2Lease() { Reset(); }

	Ngs2Lease(const Ngs2Lease&)            = delete;
	Ngs2Lease& operator=(const Ngs2Lease&) = delete;

	[[nodiscard]] explicit operator bool() const { return m_record != nullptr && static_cast<bool>(m_identity); }
	[[nodiscard]] T* operator->() const { return m_record.get(); }
	[[nodiscard]] T* Get() const { return m_record.get(); }
	[[nodiscard]] const std::shared_ptr<T>& Shared() const { return m_record; }
	[[nodiscard]] const Ngs2LeaseIdentity& Identity() const { return m_identity; }

	void Reset()
	{
		if (m_identity)
		{
			const auto identity = m_identity;
			m_identity          = {};
			Ngs2ReleaseLease(identity);
			m_record.reset();
		}
	}

	// A destroy path removes its own registry entry after waiting for every
	// other pin. It must disarm the owner lease before its destructor runs.
	void Disarm() { m_identity = {}; }

private:
	std::shared_ptr<T> m_record;
	Ngs2LeaseIdentity  m_identity {};
};

using Ngs2SystemLease = Ngs2Lease<Ngs2SystemRecord>;
using Ngs2RackLease   = Ngs2Lease<Ngs2RackRecord>;
using Ngs2VoiceLease  = Ngs2Lease<Ngs2VoiceRecord>;

static Ngs2SystemLease Ngs2AcquireSystem(uintptr_t handle)
{
	if (handle == 0)
	{
		return {};
	}
	std::lock_guard lock(g_ngs_registry_mutex);
	auto found = g_ngs_systems.find(handle);
	if (found == g_ngs_systems.end() || found->second.closing || found->second.pins == UINT32_MAX)
	{
		return {};
	}
	found->second.pins++;
	return {found->second.record, {Ngs2ObjectKind::System, handle, found->second.generation}};
}

static Ngs2RackLease Ngs2AcquireRack(uintptr_t handle)
{
	if (handle == 0)
	{
		return {};
	}
	std::lock_guard lock(g_ngs_registry_mutex);
	auto found = g_ngs_racks.find(handle);
	if (found == g_ngs_racks.end() || found->second.closing || found->second.pins == UINT32_MAX)
	{
		return {};
	}
	found->second.pins++;
	return {found->second.record, {Ngs2ObjectKind::Rack, handle, found->second.generation}};
}

static Ngs2VoiceLease Ngs2AcquireVoice(uintptr_t handle)
{
	if (handle == 0)
	{
		return {};
	}
	std::lock_guard lock(g_ngs_registry_mutex);
	auto found = g_ngs_voices.find(handle);
	if (found == g_ngs_voices.end() || found->second.closing || found->second.pins == UINT32_MAX)
	{
		return {};
	}
	found->second.pins++;
	return {found->second.record, {Ngs2ObjectKind::Voice, handle, found->second.generation}};
}

// Creation first reserves an entry as closing, writes its guest output, then
// activates it. A guessed workspace handle can therefore never race a partly
// initialized host record into public use.
static bool Ngs2ReserveSystem(uintptr_t handle, const std::shared_ptr<Ngs2SystemRecord>& record)
{
	std::lock_guard lock(g_ngs_registry_mutex);
	if (handle == 0 || record == nullptr || g_ngs_systems.find(handle) != g_ngs_systems.end())
	{
		return false;
	}
	g_ngs_systems.emplace(handle, Ngs2RegistryEntry<Ngs2SystemRecord> {record, Ngs2NextGenerationLocked(), 0, true});
	return true;
}

static bool Ngs2ActivateSystem(uintptr_t handle, const std::shared_ptr<Ngs2SystemRecord>& record)
{
	std::lock_guard lock(g_ngs_registry_mutex);
	auto found = g_ngs_systems.find(handle);
	if (found == g_ngs_systems.end() || found->second.record.get() != record.get() || !found->second.closing)
	{
		return false;
	}
	found->second.closing = false;
	g_ngs_registry_changed.notify_all();
	return true;
}

static void Ngs2CancelSystemReservation(uintptr_t handle, const std::shared_ptr<Ngs2SystemRecord>& record)
{
	std::lock_guard lock(g_ngs_registry_mutex);
	auto found = g_ngs_systems.find(handle);
	if (found != g_ngs_systems.end() && found->second.record.get() == record.get() && found->second.closing && found->second.pins == 0)
	{
		g_ngs_systems.erase(found);
		g_ngs_registry_changed.notify_all();
	}
}

static bool Ngs2ReserveRack(const Ngs2SystemLease& system, const std::shared_ptr<Ngs2RackRecord>& rack)
{
	if (!system || rack == nullptr)
	{
		return false;
	}
	std::lock_guard lock(g_ngs_registry_mutex);
	auto active_system = g_ngs_systems.find(system.Identity().handle);
	if (active_system == g_ngs_systems.end() || active_system->second.record.get() != system.Get() ||
	    active_system->second.generation != system.Identity().generation || active_system->second.closing ||
	    g_ngs_racks.find(rack->workspace) != g_ngs_racks.end())
	{
		return false;
	}
	for (const auto& voice: rack->voices)
	{
		if (voice == nullptr || g_ngs_voices.find(voice->handle) != g_ngs_voices.end())
		{
			return false;
		}
	}
	g_ngs_racks.emplace(rack->workspace, Ngs2RegistryEntry<Ngs2RackRecord> {rack, Ngs2NextGenerationLocked(), 0, true});
	for (const auto& voice: rack->voices)
	{
		g_ngs_voices.emplace(voice->handle, Ngs2RegistryEntry<Ngs2VoiceRecord> {voice, Ngs2NextGenerationLocked(), 0, true});
	}
	return true;
}

static bool Ngs2ActivateRack(const Ngs2SystemLease& system, const std::shared_ptr<Ngs2RackRecord>& rack)
{
	if (!system || rack == nullptr)
	{
		return false;
	}
	std::lock_guard lock(g_ngs_registry_mutex);
	auto active_system = g_ngs_systems.find(system.Identity().handle);
	auto rack_entry    = g_ngs_racks.find(rack->workspace);
	if (active_system == g_ngs_systems.end() || active_system->second.record.get() != system.Get() ||
	    active_system->second.generation != system.Identity().generation || active_system->second.closing ||
	    rack_entry == g_ngs_racks.end() || rack_entry->second.record.get() != rack.get() || !rack_entry->second.closing)
	{
		return false;
	}
	for (const auto& voice: rack->voices)
	{
		auto voice_entry = g_ngs_voices.find(voice->handle);
		if (voice_entry == g_ngs_voices.end() || voice_entry->second.record.get() != voice.get() || !voice_entry->second.closing)
		{
			return false;
		}
	}
	rack_entry->second.closing = false;
	for (const auto& voice: rack->voices)
	{
		g_ngs_voices.find(voice->handle)->second.closing = false;
	}
	g_ngs_registry_changed.notify_all();
	return true;
}

static void Ngs2CancelRackReservation(const std::shared_ptr<Ngs2RackRecord>& rack)
{
	if (rack == nullptr)
	{
		return;
	}
	std::lock_guard lock(g_ngs_registry_mutex);
	for (const auto& voice: rack->voices)
	{
		auto found = g_ngs_voices.find(voice->handle);
		if (found != g_ngs_voices.end() && found->second.record.get() == voice.get() && found->second.closing && found->second.pins == 0)
		{
			g_ngs_voices.erase(found);
		}
	}
	auto rack_entry = g_ngs_racks.find(rack->workspace);
	if (rack_entry != g_ngs_racks.end() && rack_entry->second.record.get() == rack.get() && rack_entry->second.closing &&
	    rack_entry->second.pins == 0)
	{
		g_ngs_racks.erase(rack_entry);
	}
	g_ngs_registry_changed.notify_all();
}

// A destroy owner holds one private pin. The condition variable waits for all
// other leases without a registry lock ever being held under a system lock.
static Ngs2SystemLease Ngs2BeginSystemDestroy(uintptr_t handle)
{
	std::lock_guard lock(g_ngs_registry_mutex);
	auto found = g_ngs_systems.find(handle);
	if (found == g_ngs_systems.end() || found->second.closing || found->second.pins == UINT32_MAX)
	{
		return {};
	}
	found->second.closing = true;
	found->second.pins++;
	auto system = found->second.record;
	for (auto& [unused_handle, entry]: g_ngs_racks)
	{
		(void)unused_handle;
		if (entry.record->system.get() == system.get())
		{
			entry.closing = true;
		}
	}
	for (auto& [unused_handle, entry]: g_ngs_voices)
	{
		(void)unused_handle;
		if (entry.record->system.get() == system.get())
		{
			entry.closing = true;
		}
	}
	return {std::move(system), {Ngs2ObjectKind::System, handle, found->second.generation}};
}

static void Ngs2WaitAndEraseSystem(Ngs2SystemLease* owner)
{
	if (owner == nullptr || !*owner)
	{
		return;
	}
	const auto system = owner->Shared();
	const auto handle = owner->Identity().handle;
	std::unique_lock lock(g_ngs_registry_mutex);
	g_ngs_registry_changed.wait(lock,
	                            [&]
	                            {
				auto system_entry = g_ngs_systems.find(handle);
				if (system_entry == g_ngs_systems.end() || system_entry->second.record.get() != system.get() ||
				    system_entry->second.pins != 1)
				{
					return false;
				}
				for (const auto& [unused_handle, entry]: g_ngs_racks)
				{
					(void)unused_handle;
					if (entry.record->system.get() == system.get() && entry.pins != 0)
					{
						return false;
					}
				}
				for (const auto& [unused_handle, entry]: g_ngs_voices)
				{
					(void)unused_handle;
					if (entry.record->system.get() == system.get() && entry.pins != 0)
					{
						return false;
					}
				}
				return true;
			});
	for (auto it = g_ngs_voices.begin(); it != g_ngs_voices.end();)
	{
		if (it->second.record->system.get() == system.get())
		{
			it = g_ngs_voices.erase(it);
		} else
		{
			++it;
		}
	}
	for (auto it = g_ngs_racks.begin(); it != g_ngs_racks.end();)
	{
		if (it->second.record->system.get() == system.get())
		{
			it = g_ngs_racks.erase(it);
		} else
		{
			++it;
		}
	}
	g_ngs_systems.erase(handle);
	owner->Disarm();
	g_ngs_registry_changed.notify_all();
}

static Ngs2RackLease Ngs2BeginRackDestroy(uintptr_t handle)
{
	std::lock_guard lock(g_ngs_registry_mutex);
	auto found = g_ngs_racks.find(handle);
	if (found == g_ngs_racks.end() || found->second.closing || found->second.pins == UINT32_MAX)
	{
		return {};
	}
	auto rack          = found->second.record;
	auto system_entry  = g_ngs_systems.find(rack->system->workspace);
	if (system_entry == g_ngs_systems.end() || system_entry->second.record.get() != rack->system.get() || system_entry->second.closing)
	{
		return {};
	}
	found->second.closing = true;
	found->second.pins++;
	for (auto& [unused_handle, entry]: g_ngs_voices)
	{
		(void)unused_handle;
		auto voice_rack = entry.record->rack.lock();
		if (voice_rack.get() == rack.get())
		{
			entry.closing = true;
		}
	}
	return {std::move(rack), {Ngs2ObjectKind::Rack, handle, found->second.generation}};
}

static void Ngs2WaitAndEraseRack(Ngs2RackLease* owner)
{
	if (owner == nullptr || !*owner)
	{
		return;
	}
	const auto rack   = owner->Shared();
	const auto handle = owner->Identity().handle;
	std::unique_lock lock(g_ngs_registry_mutex);
	g_ngs_registry_changed.wait(lock,
	                            [&]
	                            {
				auto rack_entry = g_ngs_racks.find(handle);
				if (rack_entry == g_ngs_racks.end() || rack_entry->second.record.get() != rack.get() || rack_entry->second.pins != 1)
				{
					return false;
				}
				for (const auto& [unused_handle, entry]: g_ngs_voices)
				{
					(void)unused_handle;
					auto voice_rack = entry.record->rack.lock();
					if (voice_rack.get() == rack.get() && entry.pins != 0)
					{
						return false;
					}
				}
				return true;
			});
	for (auto it = g_ngs_voices.begin(); it != g_ngs_voices.end();)
	{
		auto voice_rack = it->second.record->rack.lock();
		if (voice_rack.get() == rack.get())
		{
			it = g_ngs_voices.erase(it);
		} else
		{
			++it;
		}
	}
	g_ngs_racks.erase(handle);
	owner->Disarm();
	g_ngs_registry_changed.notify_all();
}

static bool Ngs2MakeVoiceHandle(const Ngs2RackRecord& rack, uint32_t voice_id, uintptr_t* handle_out)
{
	if (handle_out == nullptr || voice_id >= rack.max_voices ||
	    voice_id > (std::numeric_limits<size_t>::max() - kNgs2RackWorkspaceHeaderBytes) / kNgs2VoiceWorkspaceSlotBytes)
	{
		return false;
	}
	const size_t offset = kNgs2RackWorkspaceHeaderBytes + static_cast<size_t>(voice_id) * kNgs2VoiceWorkspaceSlotBytes;
	if (offset > rack.workspace_size || kNgs2VoiceWorkspaceSlotBytes > rack.workspace_size - offset ||
	    rack.workspace > std::numeric_limits<uintptr_t>::max() - offset)
	{
		return false;
	}
	*handle_out = rack.workspace + offset;
	return true;
}

class Ngs2HeldSystemLock
{
public:
	explicit Ngs2HeldSystemLock(Ngs2SystemLease&& lease): m_lease(std::move(lease)), m_state_lock(m_lease->state_mutex) {}

private:
	// Destruction reverses this order: state lock first, then the pinned lease.
	Ngs2SystemLease                         m_lease;
	std::unique_lock<std::recursive_mutex> m_state_lock;
};

thread_local std::unordered_map<uintptr_t, std::unique_ptr<Ngs2HeldSystemLock>> g_ngs_thread_locks;

static void Ngs2ApplyVoiceEvent(Ngs2VoiceRecord* voice)
{
	if (voice == nullptr)
	{
		return;
	}
	switch (voice->event)
	{
		case Ngs2VoicePlayEvent::None:
			if (voice->state == Ngs2VoicePlayState::Stopped)
			{
				voice->state = Ngs2VoicePlayState::Empty;
			}
			break;
		case Ngs2VoicePlayEvent::Play:
			if (voice->state == Ngs2VoicePlayState::Empty)
			{
				voice->state      = Ngs2VoicePlayState::Playing;
				voice->play_ticks = 0;
			}
			break;
		case Ngs2VoicePlayEvent::Pause:
			if (voice->state == Ngs2VoicePlayState::Playing)
			{
				voice->state = Ngs2VoicePlayState::Paused;
			}
			break;
		case Ngs2VoicePlayEvent::Resume:
			if (voice->state == Ngs2VoicePlayState::Paused)
			{
				voice->state = Ngs2VoicePlayState::Playing;
			}
			break;
		case Ngs2VoicePlayEvent::Stop:
			if (voice->state == Ngs2VoicePlayState::Playing)
			{
				voice->state = Ngs2VoicePlayState::Stopped;
			}
			break;
		case Ngs2VoicePlayEvent::StopImm:
		case Ngs2VoicePlayEvent::Kill: voice->state = Ngs2VoicePlayState::Empty; break;
	}
	voice->event = Ngs2VoicePlayEvent::None;
}

} // namespace

int KYTY_SYSV_ABI Ngs2SystemQueryBufferSize(const Ngs2SystemOption* option, Ngs2ContextBufferInfo* buffer_info)
{
	PRINT_NAME();

	Ngs2SystemOption system_option {};
	if (!Ngs2SnapshotSystemOption(option, &system_option))
	{
		return kNgs2InvalidOption;
	}
	Ngs2ContextBufferInfo output {};
	if (!Ngs2ReadGuest(&output, buffer_info))
	{
		return kNgs2InvalidOut;
	}
	output.host_buffer      = nullptr;
	output.host_buffer_size = kNgs2SystemWorkspaceBytes;
	for (auto& reserved: output.reserved)
	{
		reserved = 0;
	}
	return Ngs2WriteGuest(buffer_info, output) ? OK : kNgs2InvalidOut;
}

int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption* option, const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle)
{
	PRINT_NAME();

	Ngs2ContextBufferInfo info {};
	if (!Ngs2ReadGuest(&info, buffer_info))
	{
		return kNgs2InvalidBufferInfo;
	}
	if (handle == nullptr || !Ngs2IsGuestWritable(handle, sizeof(*handle)))
	{
		return kNgs2InvalidOut;
	}
	Ngs2SystemOption system_option {};
	if (!Ngs2SnapshotSystemOption(option, &system_option))
	{
		return kNgs2InvalidOption;
	}
	if (info.host_buffer == nullptr || !Ngs2IsGuestWritable(info.host_buffer, kNgs2SystemWorkspaceBytes))
	{
		return kNgs2InvalidBufferAddress;
	}
	if (info.host_buffer_size < kNgs2SystemWorkspaceBytes)
	{
		return kNgs2InvalidBufferSize;
	}

	const uintptr_t public_handle = reinterpret_cast<uintptr_t>(info.host_buffer);
	auto system                 = std::make_shared<Ngs2SystemRecord>();
	system->option              = system_option;
	system->workspace           = public_handle;
	system->workspace_size      = kNgs2SystemWorkspaceBytes;
	if (!Ngs2ReserveSystem(public_handle, system))
	{
		return kNgs2InvalidBufferAddress;
	}
	if (!Ngs2WriteGuest(handle, public_handle))
	{
		Ngs2CancelSystemReservation(public_handle, system);
		return kNgs2InvalidOut;
	}
	if (!Ngs2ActivateSystem(public_handle, system))
	{
		Ngs2CancelSystemReservation(public_handle, system);
		return kNgs2InvalidSystem;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemDestroy(uintptr_t system_handle)
{
	PRINT_NAME();
	if (g_ngs_thread_locks.find(system_handle) != g_ngs_thread_locks.end())
	{
		return kNgs2InvalidSystem;
	}
	auto system = Ngs2BeginSystemDestroy(system_handle);
	if (!system)
	{
		return kNgs2InvalidSystem;
	}
	auto record = system.Shared();
	Ngs2WaitAndEraseSystem(&system);
	std::lock_guard lock(record->state_mutex);
	record->racks.clear();
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemLock(uintptr_t system_handle)
{
	PRINT_NAME();
	if (g_ngs_thread_locks.find(system_handle) != g_ngs_thread_locks.end())
	{
		return kNgs2InvalidSystem;
	}
	auto system = Ngs2AcquireSystem(system_handle);
	if (!system)
	{
		return kNgs2InvalidSystem;
	}
	auto held = std::unique_ptr<Ngs2HeldSystemLock>(new (std::nothrow) Ngs2HeldSystemLock(std::move(system)));
	if (held == nullptr)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	g_ngs_thread_locks.emplace(system_handle, std::move(held));
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemUnlock(uintptr_t system_handle)
{
	PRINT_NAME();
	auto found = g_ngs_thread_locks.find(system_handle);
	if (found == g_ngs_thread_locks.end())
	{
		return kNgs2InvalidSystem;
	}
	g_ngs_thread_locks.erase(found);
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemSetGrainSamples(uintptr_t system_handle, uint32_t grain_samples)
{
	PRINT_NAME();
	auto system = Ngs2AcquireSystem(system_handle);
	if (!system)
	{
		return kNgs2InvalidSystem;
	}
	std::lock_guard lock(system->state_mutex);
	if (grain_samples == 0 || grain_samples > system->option.max_grain_samples || grain_samples > kNgs2MaxGrainSamples)
	{
		return kNgs2InvalidControl;
	}
	system->option.num_grain_samples = grain_samples;
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemSetSampleRate(uintptr_t system_handle, uint32_t sample_rate)
{
	PRINT_NAME();
	auto system = Ngs2AcquireSystem(system_handle);
	if (!system)
	{
		return kNgs2InvalidSystem;
	}
	if (sample_rate != kNgs2DefaultSampleRate)
	{
		return kNgs2InvalidControl;
	}
	std::lock_guard lock(system->state_mutex);
	system->option.sample_rate = sample_rate;
	return OK;
}

int KYTY_SYSV_ABI Ngs2PanInit(void* pan_param)
{
	PRINT_NAME();
	(void)pan_param;
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option, Ngs2ContextBufferInfo* buffer_info)
{
	PRINT_NAME();

	Ngs2ContextBufferInfo output {};
	if (!Ngs2ReadGuest(&output, buffer_info))
	{
		return kNgs2InvalidOut;
	}
	Ngs2RackConfig config {};
	if (!Ngs2SnapshotRackConfig(rack_id, option, &config))
	{
		return kNgs2InvalidOption;
	}
	size_t workspace_size = 0;
	if (!Ngs2CalculateRackWorkspaceSize(config.max_voices, &workspace_size))
	{
		return kNgs2InvalidOption;
	}
	output.host_buffer_size = workspace_size;
	return Ngs2WriteGuest(buffer_info, output) ? OK : kNgs2InvalidOut;
}

int KYTY_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption* option, const Ngs2BufferAllocator* allocator,
                                                 uintptr_t* handle)
{
	PRINT_NAME();
	(void)option;
	(void)allocator;
	(void)handle;
	// Allocator function-pointer invocation/lifetime is not established.
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
	                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle)
{
	PRINT_NAME();

	Ngs2ContextBufferInfo info {};
	if (!Ngs2ReadGuest(&info, buffer_info))
	{
		return kNgs2InvalidBufferInfo;
	}
	if (handle == nullptr || !Ngs2IsGuestWritable(handle, sizeof(*handle)))
	{
		return kNgs2InvalidOut;
	}
	auto system = Ngs2AcquireSystem(system_handle);
	if (!system)
	{
		return kNgs2InvalidSystem;
	}
	Ngs2RackConfig config {};
	if (!Ngs2SnapshotRackConfig(rack_id, option, &config))
	{
		return kNgs2InvalidOption;
	}
	size_t workspace_size = 0;
	if (!Ngs2CalculateRackWorkspaceSize(config.max_voices, &workspace_size))
	{
		return kNgs2InvalidOption;
	}
	if (info.host_buffer == nullptr || !Ngs2IsGuestWritable(info.host_buffer, workspace_size))
	{
		return kNgs2InvalidBufferAddress;
	}
	if (info.host_buffer_size < workspace_size)
	{
		return kNgs2InvalidBufferSize;
	}

	auto rack             = std::make_shared<Ngs2RackRecord>();
	rack->system          = system.Shared();
	rack->workspace       = reinterpret_cast<uintptr_t>(info.host_buffer);
	rack->workspace_size  = workspace_size;
	rack->type            = config.type;
	rack->max_voices      = config.max_voices;
	rack->option_size     = config.option_size;
	rack->option_snapshot = config.option_bytes;
	rack->voices.reserve(config.max_voices);

	for (uint32_t voice_id = 0; voice_id < config.max_voices; ++voice_id)
	{
		uintptr_t voice_handle = 0;
		if (!Ngs2MakeVoiceHandle(*rack, voice_id, &voice_handle))
		{
			return kNgs2InvalidBufferSize;
		}
		auto voice      = std::make_shared<Ngs2VoiceRecord>();
		voice->system   = system.Shared();
		voice->rack     = rack;
		voice->handle   = voice_handle;
		voice->voice_id = voice_id;
		rack->voices.push_back(std::move(voice));
	}

	// The system lease stays pinned while this host record is linked. No
	// registry lock is held while taking the per-system state lock.
	{
		std::lock_guard lock(system->state_mutex);
		auto [unused, inserted] = system->racks.emplace(rack->workspace, rack);
		(void)unused;
		if (!inserted)
		{
			return kNgs2InvalidBufferAddress;
		}
	}
	if (!Ngs2ReserveRack(system, rack))
	{
		std::lock_guard lock(system->state_mutex);
		system->racks.erase(rack->workspace);
		return kNgs2InvalidSystem;
	}

	const uintptr_t public_handle = rack->workspace;
	if (!Ngs2WriteGuest(handle, public_handle))
	{
		Ngs2CancelRackReservation(rack);
		std::lock_guard lock(system->state_mutex);
		system->racks.erase(rack->workspace);
		return kNgs2InvalidOut;
	}
	if (!Ngs2ActivateRack(system, rack))
	{
		Ngs2CancelRackReservation(rack);
		std::lock_guard lock(system->state_mutex);
		system->racks.erase(rack->workspace);
		return kNgs2InvalidSystem;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                              const Ngs2BufferAllocator* allocator, uintptr_t* handle)
{
	PRINT_NAME();
	(void)system_handle;
	(void)rack_id;
	(void)option;
	(void)allocator;
	(void)handle;
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info)
{
	PRINT_NAME();
	if (buffer_info != nullptr && !Ngs2IsGuestWritable(buffer_info, sizeof(*buffer_info)))
	{
		return kNgs2InvalidOut;
	}
	auto rack = Ngs2BeginRackDestroy(rack_handle);
	if (!rack)
	{
		return kNgs2InvalidRack;
	}
	auto record = rack.Shared();
	{
		std::lock_guard lock(record->system->state_mutex);
		auto found = record->system->racks.find(record->workspace);
		if (found != record->system->racks.end() && found->second.get() == record.get())
		{
			record->system->racks.erase(found);
		}
	}

	Ngs2ContextBufferInfo output {};
	output.host_buffer      = reinterpret_cast<void*>(record->workspace);
	output.host_buffer_size = record->workspace_size;
	Ngs2WaitAndEraseRack(&rack);
	if (buffer_info != nullptr && !Ngs2WriteGuest(buffer_info, output))
	{
		return kNgs2InvalidOut;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info)
{
	PRINT_NAME();
	auto system = Ngs2AcquireSystem(system_handle);
	if (!system)
	{
		return kNgs2InvalidSystem;
	}
	if (buffer_info == nullptr || num_buffer_info != 1)
	{
		return kNgs2InvalidBufferInfo;
	}
	Ngs2RenderBufferInfoImpl render {};
	if (!Ngs2CopyFromGuest(&render, buffer_info, sizeof(render)))
	{
		return kNgs2InvalidBufferInfo;
	}
	if (render.data == nullptr)
	{
		return kNgs2InvalidBufferAddress;
	}
	if (render.size != sizeof(render) || render.channels != 2)
	{
		return kNgs2InvalidBufferInfo;
	}

	uint32_t           grain = 0;
	std::vector<float> mixed;
	{
		std::lock_guard lock(system->state_mutex);
		grain = system->option.num_grain_samples;
		if (grain == 0 || grain > system->option.max_grain_samples || grain > kNgs2MaxGrainSamples)
		{
			return kNgs2InvalidControl;
		}
		const size_t render_size = static_cast<size_t>(grain) * 2u * sizeof(float);
		if (render.data_size < render_size)
		{
			return kNgs2InvalidBufferSize;
		}
		if (!Ngs2IsGuestWritable(render.data, render_size))
		{
			return kNgs2InvalidBufferAddress;
		}

		mixed.assign(static_cast<size_t>(grain) * 2u, 0.0f);
		for (const auto& [unused_workspace, rack]: system->racks)
		{
			(void)unused_workspace;
			for (const auto& voice: rack->voices)
			{
				Ngs2ApplyVoiceEvent(voice.get());
				if (voice->state == Ngs2VoicePlayState::Playing && voice->stream.playing)
				{
					if (!Ngs2MixPcmStream(&voice->stream, mixed.data(), grain, 2, system->option.sample_rate))
					{
						voice->state = Ngs2VoicePlayState::Stopped;
					}
				}
			}
		}
	}

	return Ngs2CopyToGuest(render.data, mixed.data(), mixed.size() * sizeof(float)) ? OK : kNgs2InvalidBufferAddress;
}

int KYTY_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id, uintptr_t* handle)
{
	PRINT_NAME();
	if (handle == nullptr || !Ngs2IsGuestWritable(handle, sizeof(*handle)))
	{
		return kNgs2InvalidOut;
	}
	auto rack = Ngs2AcquireRack(rack_handle);
	if (!rack)
	{
		const uintptr_t invalid_handle = 0;
		return Ngs2WriteGuest(handle, invalid_handle) ? kNgs2InvalidRack : kNgs2InvalidOut;
	}
	std::lock_guard lock(rack->system->state_mutex);
	if (voice_id >= rack->max_voices || voice_id >= rack->voices.size())
	{
		const uintptr_t invalid_handle = 0;
		return Ngs2WriteGuest(handle, invalid_handle) ? kNgs2InvalidVoice : kNgs2InvalidOut;
	}
	return Ngs2WriteGuest(handle, rack->voices[voice_id]->handle) ? OK : kNgs2InvalidOut;
}

int KYTY_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list)
{
	PRINT_NAME();
	Ngs2VoiceParamHeader header {};
	if (!Ngs2ReadGuest(&header, param_list) || header.next != 0)
	{
		return kNgs2InvalidControl;
	}
	auto voice = Ngs2AcquireVoice(voice_handle);
	if (!voice)
	{
		return kNgs2InvalidVoice;
	}
	std::lock_guard lock(voice->system->state_mutex);
	auto rack = voice->rack.lock();
	if (rack == nullptr || rack->type != Ngs2RackType::CustomSampler)
	{
		return kNgs2InvalidControl;
	}

	if (header.id == 0x40010000u)
	{
		Ngs2CustomSamplerFormatParam format {};
		if (header.size != sizeof(format) || !Ngs2ReadGuest(&format, reinterpret_cast<const Ngs2CustomSamplerFormatParam*>(param_list)) ||
		    format.header.next != 0 || format.header.id != header.id || format.format_id != 0x12u ||
		    (format.channels != 1 && format.channels != 2) || format.sample_rate != 44100u)
		{
			return kNgs2InvalidControl;
		}
		voice->stream             = {};
		voice->stream.format_id   = format.format_id;
		voice->stream.channels    = format.channels;
		voice->stream.sample_rate = format.sample_rate;
		return OK;
	}

	if (header.id == 0x40010001u)
	{
		Ngs2CustomSamplerWaveformParam waveform {};
		if (header.size != sizeof(waveform) || !Ngs2ReadGuest(&waveform, reinterpret_cast<const Ngs2CustomSamplerWaveformParam*>(param_list)) ||
		    waveform.header.next != 0 || waveform.header.id != header.id || waveform.data == nullptr || waveform.context == nullptr ||
		    waveform.flags != 0x11u || waveform.block_count != 1u || voice->stream.format_id != 0x12u ||
		    (voice->stream.channels != 1 && voice->stream.channels != 2) || voice->stream.sample_rate != 44100u)
		{
			return kNgs2InvalidControl;
		}

		Ngs2CustomSamplerWaveformContext context {};
		if (!Ngs2ReadGuest(&context, waveform.context) || context.offset_frames != 0 || context.frame_count == 0 ||
		    context.frame_count > voice->system->option.max_grain_samples || context.frame_count > kNgs2MaxGrainSamples)
		{
			return kNgs2InvalidControl;
		}
		size_t pcm_bytes = 0;
		if (!Ngs2CalculatePcmBytes(context.frame_count, voice->stream.channels, &pcm_bytes) || context.data_size != pcm_bytes)
		{
			return kNgs2InvalidControl;
		}
		std::vector<int16_t> samples(pcm_bytes / sizeof(int16_t));
		if (!Ngs2CopyFromGuest(samples.data(), waveform.data, pcm_bytes))
		{
			return kNgs2InvalidControl;
		}
		voice->stream.samples      = std::move(samples);
		voice->stream.frame_count  = context.frame_count;
		voice->stream.source_frame = 0.0;
		voice->stream.playing      = false;
		return OK;
	}

	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2VoiceRunCommands(uintptr_t voice_handle, const void* commands, uint32_t num_commands)
{
	PRINT_NAME();
	if (num_commands != 1 || commands == nullptr)
	{
		return kNgs2InvalidControl;
	}
	std::array<uint32_t, 3> command {};
	if (!Ngs2CopyFromGuest(command.data(), commands, sizeof(command)))
	{
		return kNgs2InvalidControl;
	}
	auto voice = Ngs2AcquireVoice(voice_handle);
	if (!voice)
	{
		return kNgs2InvalidVoice;
	}
	std::lock_guard lock(voice->system->state_mutex);
	auto rack = voice->rack.lock();
	if (rack == nullptr || rack->type != Ngs2RackType::CustomSampler)
	{
		return kNgs2InvalidControl;
	}
	for (size_t i = 0; i < command.size(); ++i)
	{
		voice->last_command[i] = command[i];
	}
	if (command[0] == 2u && command[1] == 0x400u)
	{
		if (command[2] == 1u)
		{
			if (voice->stream.format_id != 0x12u || voice->stream.samples.empty() || voice->stream.frame_count == 0)
			{
				return kNgs2InvalidControl;
			}
			voice->event          = Ngs2VoicePlayEvent::Play;
			voice->stream.playing = true;
			return OK;
		}
		if (command[2] == 8u)
		{
			voice->event              = Ngs2VoicePlayEvent::StopImm;
			voice->stream.playing      = false;
			voice->stream.source_frame = 0.0;
			return OK;
		}
		return kNgs2InvalidControl;
	}
	if (command[0] == 6u && command[1] == 0x100u)
	{
		float gain = 0.0f;
		std::memcpy(&gain, &command[2], sizeof(gain));
		if (!std::isfinite(gain) || gain < 0.0f)
		{
			return kNgs2InvalidControl;
		}
		voice->stream.gain = gain;
		return OK;
	}
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2GeomResetSourceParam(void* out_source_param)
{
	PRINT_NAME();
	(void)out_source_param;
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2GeomResetListenerParam(void* out_listener_param)
{
	PRINT_NAME();
	(void)out_listener_param;
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2GeomCalcListener(const void* listener_param, void* out_work, uint32_t flags)
{
	PRINT_NAME();
	(void)listener_param;
	(void)out_work;
	(void)flags;
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2GeomApply(const void* listener_work, const void* source_param, void* out_attrib, uint32_t flags)
{
	PRINT_NAME();
	(void)listener_work;
	(void)source_param;
	(void)out_attrib;
	(void)flags;
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state, size_t state_size)
{
	PRINT_NAME();
	auto voice = Ngs2AcquireVoice(voice_handle);
	if (!voice)
	{
		return kNgs2InvalidVoice;
	}
	if (state == nullptr || state_size != sizeof(Ngs2SamplerVoiceState) || !Ngs2IsGuestWritable(state, state_size))
	{
		return kNgs2InvalidControl;
	}
	Ngs2SamplerVoiceState output {};
	{
		std::lock_guard lock(voice->system->state_mutex);
		auto rack = voice->rack.lock();
		if (rack == nullptr || (rack->type != Ngs2RackType::CustomSampler && rack->type != Ngs2RackType::Sampler))
		{
			return kNgs2InvalidControl;
		}
		output.voice_state.state_flags = Ngs2GetVoiceStateFlags(*voice.Get());
	}
	return Ngs2CopyToGuest(state, &output, sizeof(output)) ? OK : kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags)
{
	PRINT_NAME();
	auto voice = Ngs2AcquireVoice(voice_handle);
	if (!voice)
	{
		return kNgs2InvalidVoice;
	}
	if (state_flags == nullptr || !Ngs2IsGuestWritable(state_flags, sizeof(*state_flags)))
	{
		return kNgs2InvalidControl;
	}
	uint32_t output = 0;
	{
		std::lock_guard lock(voice->system->state_mutex);
		output = Ngs2GetVoiceStateFlags(*voice.Get());
	}
	return Ngs2WriteGuest(state_flags, output) ? OK : kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2RackGetInfo(uintptr_t rack_handle, void* out_info, size_t info_size)
{
	PRINT_NAME();
	auto rack = Ngs2AcquireRack(rack_handle);
	if (!rack)
	{
		return kNgs2InvalidRack;
	}
	(void)out_info;
	(void)info_size;
	// The output layout and size relation remain unmeasured. In particular, do
	// not memset a guest-provided size; that would turn an unknown ABI into a
	// guest-controlled write primitive.
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2VoiceGetPortInfo(uintptr_t voice_handle, uint32_t port, void* out_info, size_t out_info_size)
{
	PRINT_NAME();
	auto voice = Ngs2AcquireVoice(voice_handle);
	if (!voice)
	{
		return kNgs2InvalidVoice;
	}
	(void)port;
	(void)out_info;
	(void)out_info_size;
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2VoiceQueryInfo(uintptr_t voice_handle, uint32_t query_type, const void* param, void* out_info)
{
	PRINT_NAME();
	auto voice = Ngs2AcquireVoice(voice_handle);
	if (!voice)
	{
		return kNgs2InvalidVoice;
	}
	(void)query_type;
	(void)param;
	(void)out_info;
	return kNgs2InvalidControl;
}

int KYTY_SYSV_ABI Ngs2PanGetVolumeMatrix(void* work, const void* params, uint32_t num_params, uint32_t matrix_format,
	                                         float* out_volume_matrix)
{
	PRINT_NAME();
	(void)work;
	(void)params;
	(void)num_params;
	(void)matrix_format;
	(void)out_volume_matrix;
	return kNgs2InvalidControl;
}

} // namespace Ngs2

} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED
