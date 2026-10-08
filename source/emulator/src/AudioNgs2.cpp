#include "Emulator/Audio.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/AudioNgs2Sampler.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <algorithm>
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
#include <string>
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

// The +16 word is the waveform type of the output; PCM_F32L is the only one accepted.
struct Ngs2RenderBufferInfoImpl
{
	float*   data;
	size_t   data_size;
	uint32_t waveform_type;
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

struct Ngs2WaveformFormat
{
	uint32_t waveform_type;
	uint32_t channels;
	uint32_t sample_rate;
	uint32_t config_data;
	uint32_t frame_offset;
	uint32_t frame_margin;
};
static_assert(sizeof(Ngs2WaveformFormat) == 24);

// Guests store the offset and size as qwords and read the repeat count at +0x10.
struct Ngs2WaveformBlock
{
	uint64_t data_offset;
	uint64_t data_size;
	uint32_t num_repeats;
	uint32_t num_skip_samples;
	uint32_t num_samples;
	uint32_t reserved;
	uint64_t user_data;
};
static_assert(sizeof(Ngs2WaveformBlock) == 40);

// Standard sampler params, laid out as the guest's param builders write them.
struct Ngs2SamplerSetupParam
{
	Ngs2VoiceParamHeader header;
	Ngs2WaveformFormat   format;
	uint32_t             flags;
	uint32_t             reserved;
};
static_assert(sizeof(Ngs2SamplerSetupParam) == 40);

struct Ngs2SamplerBlocksParam
{
	Ngs2VoiceParamHeader     header;
	const void*              data;
	uint32_t                 flags;
	uint32_t                 num_blocks;
	const Ngs2WaveformBlock* blocks;
};
static_assert(sizeof(Ngs2SamplerBlocksParam) == 32);

struct Ngs2SamplerPitchParam
{
	Ngs2VoiceParamHeader header;
	float                ratio;
	uint32_t             reserved;
};
static_assert(sizeof(Ngs2SamplerPitchParam) == 16);

struct Ngs2VoiceCallbackParam
{
	Ngs2VoiceParamHeader header;
	uintptr_t            callback;
	uintptr_t            callback_data;
	uint32_t             flags;
	uint32_t             reserved;
};
static_assert(sizeof(Ngs2VoiceCallbackParam) == 32);

// Input of a block callback handler. Only callback_data (+0), flag (+0x10) and
// user_data (+0x18) are confirmed by a native callback's reads. The other offsets
// are inferred from a classic header and are not verified on a guest: real values
// are written there, but the positions are not a confirmed native ABI.
struct Ngs2VoiceCallbackInfo
{
	uintptr_t   callback_data;
	uintptr_t   voice_handle;
	uint32_t    flag;
	uint32_t    reserved;
	uintptr_t   user_data;
	const void* block_data;
	uint32_t    block_size;
	uint32_t    num_repeated;
	uint32_t    attributes;
	uint32_t    reserved2;
};
static_assert(sizeof(Ngs2VoiceCallbackInfo) == 56);

struct Ngs2WaveformInfo
{
	Ngs2WaveformFormat format;
	uint32_t           data_offset;
	uint32_t           data_size;
	uint32_t           loop_begin;
	uint32_t           loop_end;
	uint32_t           num_samples;
	uint32_t           audio_unit_size;
	uint32_t           audio_unit_samples;
	uint32_t           audio_units_per_frame;
	uint32_t           audio_frame_size;
	uint32_t           audio_frame_samples;
	uint32_t           delay_samples;
	uint32_t           num_blocks;
	Ngs2WaveformBlock  blocks[4];
};
static_assert(sizeof(Ngs2WaveformInfo) == 232);

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

constexpr uint32_t kNgs2WaveformTypeVag    = 0x1c;
constexpr uint32_t kNgs2WaveformTypePcmI16 = 0x12;
constexpr uint32_t kNgs2WaveformTypePcmF32 = 0x18;
constexpr uint32_t kNgs2WaveformTypeAtrac9 = 0x40;
constexpr uint32_t kNgs2RepeatForever      = 0xffffffffu;

// Sampler rack option size. Derived, not captured as a whole: the classic
// 0xa8 sampler option plus the 0x30-byte common expansion that the captured
// extended rack options share, with max_voices at +0x50.
constexpr size_t kNgs2SamplerRackOptionSize = 0xd8;

// Standard sampler parameter IDs (header.id). The guest's param builders write
// these with size 40 (setup), 32 (blocks) and 16 (pitch) at offset +0.
constexpr uint32_t kNgs2SamplerSetupId  = 0x10000000u;
constexpr uint32_t kNgs2SamplerBlocksId = 0x10000001u;
constexpr uint32_t kNgs2SamplerPitchId  = 0x10000005u;
constexpr uint32_t kNgs2VoiceParamCallback          = 0x7u;
constexpr uint32_t kNgs2VoiceCallbackFlagEnd        = 0x1u;
// The only blocks flag seen at a native call site: one block added with 4, which
// replaces the waveform (RESET). Other values are refused.
constexpr uint32_t kNgs2SamplerBlocksFlagReset      = 0x4u;
constexpr uint32_t kNgs2MaxBlocksPerAdd             = 64;
constexpr uint64_t kNgs2MaxBlockFrames              = 1ull << 22;
// Host memory limits for queued float frames, checked before any guest read.
constexpr uint64_t kNgs2MaxVoiceSampleBytes         = 32ull << 20u;
constexpr uint64_t kNgs2MaxSystemSampleBytes        = 128ull << 20u;
// Host limit on callbacks held for one system while its explicit lock is held:
// one full rack's worth (64 queued blocks for each of 256 voices).
constexpr uint64_t kNgs2MaxDeferredCallbacks        = 64ull * 256ull;

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

// Whole codec frames covering `count` samples from `start`, relative to the data start.
static Ngs2WaveformBlock Ngs2MakeWaveformBlock(const Ngs2WaveformInfo& info, uint64_t start, uint64_t count)
{
	const uint64_t first       = start + info.delay_samples;
	const uint64_t begin_frame = first / info.audio_frame_samples;
	const uint64_t end_frame   = (first + count + info.audio_frame_samples - 1) / info.audio_frame_samples;
	const uint64_t offset      = std::min<uint64_t>(begin_frame * info.audio_frame_size, info.data_size);

	Ngs2WaveformBlock block {};
	block.data_offset      = offset;
	block.data_size        = std::min<uint64_t>((end_frame - begin_frame) * info.audio_frame_size, info.data_size - offset);
	block.num_skip_samples = static_cast<uint32_t>(first - begin_frame * info.audio_frame_samples);
	block.num_samples      = static_cast<uint32_t>(count);
	return block;
}

// A looped waveform plays its lead-in once, repeats the inclusive loop range and keeps the tail after it.
static void Ngs2SetWaveformBlocks(Ngs2WaveformInfo* info, bool looped)
{
	struct Range
	{
		uint64_t begin;
		uint64_t end;
		uint32_t repeats;
	};
	std::array<Range, 3> ranges {};
	uint32_t             count = 0;
	if (looped && info->loop_begin <= info->loop_end && info->loop_end < info->num_samples)
	{
		const uint64_t loop_end = static_cast<uint64_t>(info->loop_end) + 1;
		ranges[count++]         = {0, info->loop_begin, 0};
		ranges[count++]         = {info->loop_begin, loop_end, kNgs2RepeatForever};
		ranges[count++]         = {loop_end, info->num_samples, 0};
	} else
	{
		ranges[count++] = {0, info->num_samples, 0};
	}

	info->num_blocks = 0;
	for (uint32_t i = 0; i < count; i++)
	{
		if (ranges[i].end <= ranges[i].begin)
		{
			continue;
		}
		auto& block = info->blocks[info->num_blocks++];
		block       = Ngs2MakeWaveformBlock(*info, ranges[i].begin, ranges[i].end - ranges[i].begin);
		block.data_offset += info->data_offset;
		block.num_repeats = ranges[i].repeats;
	}
}

static uint32_t Ngs2ReadBe32(const uint8_t* p)
{
	return (static_cast<uint32_t>(p[0]) << 24u) | (static_cast<uint32_t>(p[1]) << 16u) | (static_cast<uint32_t>(p[2]) << 8u) | p[3];
}

// VAG: big-endian 0x30-byte header, then 16-byte ADPCM frames of 28 samples interleaved per channel.
// The low flag bits of each frame mark loop start (4), repeat (2) and end (1).
static bool Ngs2ParseVag(const void* data, size_t data_size, Ngs2WaveformInfo* info)
{
	std::array<uint8_t, 0x30> header {};
	if (data_size < header.size() || !Ngs2CopyFromGuest(header.data(), data, header.size()) || std::memcmp(header.data(), "VAGp", 4) != 0)
	{
		return false;
	}
	const uint32_t channels    = header[0x1e] == 0 ? 1u : header[0x1e];
	const uint32_t frame_bytes = 16u * channels;
	info->format.waveform_type = kNgs2WaveformTypeVag;
	info->format.channels      = channels;
	info->format.sample_rate   = Ngs2ReadBe32(&header[0x10]);
	info->data_offset          = header.size();
	info->data_size            = Ngs2ReadBe32(&header[0x0c]);
	info->num_samples          = info->data_size / frame_bytes * 28u;
	info->audio_unit_size       = 16;
	info->audio_unit_samples    = 28;
	info->audio_units_per_frame = channels;
	info->audio_frame_size      = frame_bytes;
	info->audio_frame_samples   = 28;

	std::vector<uint8_t> frames(std::min<size_t>(info->data_size, data_size - header.size()) / 16u * 16u);
	int64_t              loop_start = -1;
	bool                 looped     = false;
	if (!frames.empty() && Ngs2CopyFromGuest(frames.data(), static_cast<const uint8_t*>(data) + header.size(), frames.size()))
	{
		for (size_t frame = 0; frame < frames.size() / 16u; frame++)
		{
			const uint32_t flags = frames[frame * 16u + 1u] & 0x7u;
			if ((flags & 0x4u) != 0 && loop_start < 0)
			{
				loop_start = static_cast<int64_t>(frame / channels);
			}
			if ((flags & 0x1u) != 0)
			{
				looped = (flags & 0x2u) != 0 && loop_start >= 0;
				if (looped)
				{
					info->loop_begin = static_cast<uint32_t>(loop_start) * 28u;
					info->loop_end   = static_cast<uint32_t>(frame / channels + 1u) * 28u - 1u;
				}
				break;
			}
		}
	}
	Ngs2SetWaveformBlocks(info, looped);
	return true;
}

// RIFF WAVE with an ATRAC9 extensible format. Only the chunks before the data chunk are read, so a
// header-only buffer is enough.
static bool Ngs2ParseRiffAtrac9(const void* data, size_t data_size, Ngs2WaveformInfo* info)
{
	static constexpr uint8_t kAtrac9Guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
	                                            0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
	const auto* bytes = static_cast<const uint8_t*>(data);
	std::array<uint8_t, 12> riff {};
	if (data_size < riff.size() || !Ngs2CopyFromGuest(riff.data(), data, riff.size()) || std::memcmp(riff.data(), "RIFF", 4) != 0 ||
	    std::memcmp(&riff[8], "WAVE", 4) != 0)
	{
		return false;
	}

	std::array<uint8_t, 0x34> fmt {};
	std::array<uint32_t, 2>   fact {};
	std::array<uint8_t, 0x34> smpl {};
	bool                      has_fmt = false;
	bool                      has_fact = false;
	bool                      has_smpl = false;
	bool                      has_data = false;
	for (size_t offset = riff.size(); !has_data && offset + 8u <= data_size;)
	{
		std::array<uint8_t, 8> chunk {};
		if (!Ngs2CopyFromGuest(chunk.data(), bytes + offset, chunk.size()))
		{
			return false;
		}
		uint32_t size = 0;
		std::memcpy(&size, &chunk[4], sizeof(size));
		const size_t body = offset + 8u;
		const size_t available = data_size - body;
		if (std::memcmp(chunk.data(), "fmt ", 4) == 0 && size >= fmt.size() && available >= fmt.size())
		{
			has_fmt = Ngs2CopyFromGuest(fmt.data(), bytes + body, fmt.size());
		} else if (std::memcmp(chunk.data(), "fact", 4) == 0 && size >= 8u && available >= 8u)
		{
			has_fact = Ngs2CopyFromGuest(fact.data(), bytes + body, sizeof(fact));
		} else if (std::memcmp(chunk.data(), "smpl", 4) == 0 && size >= smpl.size() && available >= smpl.size())
		{
			has_smpl = Ngs2CopyFromGuest(smpl.data(), bytes + body, smpl.size());
		} else if (std::memcmp(chunk.data(), "data", 4) == 0)
		{
			info->data_offset = static_cast<uint32_t>(body);
			info->data_size   = size;
			has_data          = true;
		}
		offset = body + size + (size & 1u);
	}

	uint16_t tag = 0;
	uint16_t channels = 0;
	uint16_t block_align = 0;
	uint16_t superframe_samples = 0;
	uint32_t sample_rate = 0;
	if (has_fmt)
	{
		std::memcpy(&tag, &fmt[0x00], sizeof(tag));
		std::memcpy(&channels, &fmt[0x02], sizeof(channels));
		std::memcpy(&sample_rate, &fmt[0x04], sizeof(sample_rate));
		std::memcpy(&block_align, &fmt[0x0c], sizeof(block_align));
		std::memcpy(&superframe_samples, &fmt[0x12], sizeof(superframe_samples));
	}
	// Config bytes: sync 0xfe, then an 11-bit frame size minus one and the log2 frames per superframe.
	const uint8_t* config            = &fmt[0x2c];
	const uint32_t frame_bytes       = ((static_cast<uint32_t>(config[2]) << 3u) | (config[3] >> 5u)) + 1u;
	const uint32_t frames_per_super  = 1u << ((config[3] >> 3u) & 3u);
	if (!has_fmt || !has_data || tag != 0xfffeu || std::memcmp(&fmt[0x18], kAtrac9Guid, sizeof(kAtrac9Guid)) != 0 || config[0] != 0xfeu ||
	    channels == 0 || superframe_samples == 0 || block_align != frame_bytes * frames_per_super ||
	    superframe_samples % frames_per_super != 0)
	{
		return false;
	}

	info->format.waveform_type = kNgs2WaveformTypeAtrac9;
	info->format.channels      = channels;
	info->format.sample_rate   = sample_rate;
	std::memcpy(&info->format.config_data, config, sizeof(info->format.config_data));
	info->audio_unit_size       = frame_bytes;
	info->audio_unit_samples    = superframe_samples / frames_per_super;
	info->audio_units_per_frame = frames_per_super;
	info->audio_frame_size      = block_align;
	info->audio_frame_samples   = superframe_samples;
	info->num_samples           = has_fact ? fact[0] : info->data_size / block_align * superframe_samples;
	info->delay_samples         = has_fact ? fact[1] : 0;

	uint32_t loops = 0;
	if (has_smpl)
	{
		std::memcpy(&loops, &smpl[0x1c], sizeof(loops));
		std::memcpy(&info->loop_begin, &smpl[0x2c], sizeof(info->loop_begin));
		std::memcpy(&info->loop_end, &smpl[0x30], sizeof(info->loop_end));
	}
	Ngs2SetWaveformBlocks(info, loops != 0);
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
		case 0x1000: return size == kNgs2SamplerRackOptionSize;
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

namespace AudioVideoBackend = ::Kyty::Emulator::AudioVideoBackend;

// Format of a standard sampler voice. For ATRAC9 it keeps the decoder extradata;
// each waveform reset decodes through a new decoder stream, one superframe of
// superframe_bytes per packet.
struct Ngs2SamplerSetup
{
	uint32_t                                                               waveform_type    = 0;
	uint32_t                                                               channels         = 0;
	uint32_t                                                               sample_rate      = 0;
	uint32_t                                                               superframe_bytes = 0;
	std::array<uint8_t, AudioVideoBackend::ElementaryAudioDecoder::kAtrac9ExtradataSize> extradata {};
	bool                                                                   configured = false;
};

struct Ngs2VoiceCallbackRegistration
{
	uintptr_t handler = 0;
	uintptr_t data    = 0;
	uint32_t  flags   = 0;
};

struct Ngs2SystemRecord;
struct Ngs2RackRecord;
struct Ngs2VoiceRecord;

// A block end callback waiting to run. voice keeps the record's identity stable
// for the liveness check; it is not a registry pin and blocks no destroy.
struct Ngs2PendingCallback
{
	Ngs2VoiceCallbackInfo            info {};
	uintptr_t                        handler = 0;
	std::shared_ptr<Ngs2VoiceRecord> voice;
};

struct Ngs2SystemRecord
{
	std::recursive_mutex                                           state_mutex;
	// Held while one guest callback is checked and run. Destroy paths take it
	// after their pins drain, so a destroy on another thread waits for a running
	// callback, while a callback may destroy its own objects (recursive).
	std::recursive_mutex                                           dispatch_mutex;
	Ngs2SystemOption                                               option {};
	uintptr_t                                                      workspace = 0;
	size_t                                                         workspace_size = 0;
	std::unordered_map<uintptr_t, std::shared_ptr<Ngs2RackRecord>> racks;
	// Callbacks produced by a render that ran while its thread held the explicit
	// system lock. They run at unlock, never under the lock. deferred_mutex is a
	// leaf lock guarding only this list; no other lock is taken while it is held.
	std::mutex                                                     deferred_mutex;
	std::vector<Ngs2PendingCallback>                               deferred_callbacks;
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
	Ngs2SamplerSetup                   sampler_setup;
	Ngs2VoiceCallbackRegistration      callback;
	Ngs2Sampler::Playback              sampler;
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

static bool Ngs2MixPcmStream(Ngs2PcmStream* stream, double* output, uint32_t output_frames, uint32_t output_channels,
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
			// Gained but not clipped: the system mix sums in double and clamps once,
			// so opposite-sign voices still cancel before the final clamp.
			output[frame * output_channels + channel] += static_cast<double>(current + (next - current) * fraction) * static_cast<double>(stream->gain);
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
	// A callback running on another thread finishes before the records go away.
	// Closing entries take no new pins, so the predicate still holds after relock.
	lock.unlock();
	std::lock_guard dispatch(system->dispatch_mutex);
	lock.lock();
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
	// As for a system: wait for another thread's running callback, then erase.
	lock.unlock();
	std::lock_guard dispatch(rack->system->dispatch_mutex);
	lock.lock();
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

	// The record stays alive through this shared_ptr even after the lock and lease
	// are gone, so deferred callbacks can run once the lock is released.
	[[nodiscard]] std::shared_ptr<Ngs2SystemRecord> Record() const { return m_lease.Shared(); }

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

// Config bytes sit in config_data in memory order, as the waveform parse writes them.
static void Ngs2ConfigBytes(uint32_t config_data, uint8_t* config)
{
	for (size_t i = 0; i < 4; i++)
	{
		config[i] = static_cast<uint8_t>(config_data >> (8u * i));
	}
}

// FE sync, then a 4-bit rate index, a 3-bit block config and a verification bit
// that must be clear, an 11-bit frame size minus one, and a 2-bit superframe
// index that must be even (1 or 4 frames).
static bool Ngs2Atrac9SuperframeBytes(const uint8_t* config, uint32_t* superframe_bytes)
{
	if (config[0] != 0xfeu || (config[1] & 1u) != 0)
	{
		return false;
	}
	const uint32_t frame_bytes      = ((static_cast<uint32_t>(config[2]) << 3u) | (config[3] >> 5u)) + 1u;
	const uint32_t superframe_index = (config[3] >> 3u) & 3u;
	if ((superframe_index & 1u) != 0)
	{
		return false;
	}
	*superframe_bytes = frame_bytes * (1u << superframe_index);
	return true;
}

static std::unique_ptr<AudioVideoBackend::ElementaryAudioDecoder> Ngs2OpenAtrac9Decoder(const Ngs2SamplerSetup& setup)
{
	std::string error;
	auto decoder = AudioVideoBackend::ElementaryAudioDecoder::Open(AudioVideoBackend::AudioCodec::Atrac9, setup.extradata.data(),
	                                                               setup.extradata.size(), setup.superframe_bytes, setup.channels,
	                                                               setup.sample_rate, &error);
	if (decoder == nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: NGS2 ATRAC9 decoder rejected the sampler format: %s\n", error.c_str());
	}
	return decoder;
}

static bool Ngs2MakeSamplerSetup(const Ngs2WaveformFormat& format, Ngs2SamplerSetup* setup)
{
	if (format.channels == 0 || format.channels > Ngs2Sampler::kMaxChannels || format.sample_rate == 0 ||
	    format.sample_rate > Ngs2Sampler::kMaxSampleRate)
	{
		return false;
	}
	setup->waveform_type = format.waveform_type;
	setup->channels      = format.channels;
	setup->sample_rate   = format.sample_rate;
	if (format.waveform_type == kNgs2WaveformTypePcmI16 || format.waveform_type == kNgs2WaveformTypePcmF32)
	{
		setup->configured = format.config_data == 0;
		return setup->configured;
	}
	if (format.waveform_type != kNgs2WaveformTypeAtrac9)
	{
		return false;
	}
	std::array<uint8_t, 4> config {};
	Ngs2ConfigBytes(format.config_data, config.data());
	if (!Ngs2Atrac9SuperframeBytes(config.data(), &setup->superframe_bytes))
	{
		return false;
	}
	// The RIFF extradata is version, config, reserved. The standard sampler setup
	// carries no version word, so it is zero; the decoder only checks version <= 2.
	setup->extradata = {};
	std::memcpy(setup->extradata.data() + 4, config.data(), config.size());
	// Opening once checks that the decoder derives this channel count and rate.
	if (Ngs2OpenAtrac9Decoder(*setup) == nullptr)
	{
		return false;
	}
	setup->configured = true;
	return true;
}

// Float bytes a block occupies once queued: num_skip + num_samples frames.
// Known from the block header alone, so budgets are checked before any read.
static bool Ngs2SamplerBlockBytes(const Ngs2SamplerSetup& setup, const Ngs2WaveformBlock& entry, uint64_t* float_bytes)
{
	const uint64_t frames = static_cast<uint64_t>(entry.num_skip_samples) + entry.num_samples;
	if (entry.num_samples == 0 || frames > kNgs2MaxBlockFrames)
	{
		return false;
	}
	*float_bytes = frames * setup.channels * sizeof(float);
	return true;
}

// Float bytes queued by the system's sampler voices other than except.
static uint64_t Ngs2QueuedSampleBytes(const Ngs2SystemRecord& system, const Ngs2VoiceRecord* except)
{
	uint64_t bytes = 0;
	for (const auto& [unused_workspace, rack]: system.racks)
	{
		(void)unused_workspace;
		for (const auto& voice: rack->voices)
		{
			if (voice.get() != except)
			{
				bytes += voice->sampler.QueuedSampleBytes();
			}
		}
	}
	return bytes;
}

// Copies the frames a PCM block plays (skip + count) from guest memory as float.
// Float sources must be finite.
// True when [base, base + length) does not wrap past the end of the address
// space, so a guest pointer and read length can be formed without overflow.
static bool Ngs2GuestSpanInRange(uintptr_t base, uint64_t length)
{
	return length <= static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max() - base);
}

static bool Ngs2ReadPcmBlock(const Ngs2SamplerSetup& setup, uintptr_t guest_data, const Ngs2WaveformBlock& entry, Ngs2Sampler::Block* block)
{
	const uint64_t channels = setup.channels;
	const bool     is_float = setup.waveform_type == kNgs2WaveformTypePcmF32;
	const uint64_t samples  = static_cast<uint64_t>(entry.num_skip_samples) + entry.num_samples;
	const uint64_t bytes    = samples * channels * (is_float ? sizeof(float) : sizeof(int16_t));
	if (bytes > entry.data_size || !Ngs2GuestSpanInRange(guest_data, bytes) || !Core::VirtualMemory::IsRangeReadable(guest_data, bytes))
	{
		return false;
	}
	std::vector<uint8_t> raw(static_cast<size_t>(bytes));
	if (!Ngs2CopyFromGuest(raw.data(), reinterpret_cast<const void*>(guest_data), raw.size()))
	{
		return false;
	}
	block->frames.resize(static_cast<size_t>(samples * channels));
	for (size_t i = 0; i < block->frames.size(); i++)
	{
		if (is_float)
		{
			std::memcpy(&block->frames[i], raw.data() + i * sizeof(float), sizeof(float));
			if (!std::isfinite(block->frames[i]))
			{
				return false;
			}
		} else
		{
			int16_t sample = 0;
			std::memcpy(&sample, raw.data() + i * sizeof(int16_t), sizeof(int16_t));
			block->frames[i] = static_cast<float>(sample) / 32768.0f;
		}
	}
	return true;
}

// Decodes consecutive superframes from guest_data, continuing the decoder stream
// of the previous block of the same waveform, until the block's frames are
// covered. Every superframe yields at least one frame, so the packet count is
// bounded by the frames needed. stream_packet_frames holds the frame count of the
// stream's first superframe (0 before it); every later superframe of the same
// add, in any block, must match it.
static bool Ngs2DecodeAtrac9Block(AudioVideoBackend::ElementaryAudioDecoder* decoder, const Ngs2SamplerSetup& setup, uintptr_t guest_data,
                                  const Ngs2WaveformBlock& entry, uint64_t* stream_packet_frames, Ngs2Sampler::Block* block)
{
	const uint64_t       channels   = setup.channels;
	const uint64_t       needed     = static_cast<uint64_t>(entry.num_skip_samples) + entry.num_samples;
	const uint64_t       superframe = setup.superframe_bytes;
	std::vector<float>   pcm;
	std::vector<float>   packet_pcm;
	std::vector<uint8_t> bytes(static_cast<size_t>(superframe));
	pcm.reserve(static_cast<size_t>((needed + AudioVideoBackend::ElementaryAudioDecoder::kAtrac9MaxSuperframeSamples) * channels));
	for (uint64_t packet = 0; pcm.size() / channels < needed; packet++)
	{
		const uint64_t offset = packet * superframe;
		if (packet >= needed || offset > entry.data_size || superframe > entry.data_size - offset)
		{
			return false;
		}
		// Form the pointer only after the base plus this packet's end is known not
		// to wrap, so an offset that points at a readable low address is rejected.
		if (!Ngs2GuestSpanInRange(guest_data, offset + superframe))
		{
			return false;
		}
		const uintptr_t address = guest_data + static_cast<uintptr_t>(offset);
		if (!Core::VirtualMemory::IsRangeReadable(address, superframe) ||
		    !Ngs2CopyFromGuest(bytes.data(), reinterpret_cast<const void*>(address), bytes.size()) ||
		    !decoder->Decode(bytes.data(), bytes.size(), &packet_pcm) || packet_pcm.empty())
		{
			return false;
		}
		// Every superframe of the add's stream yields the same number of frames.
		const uint64_t frames_in_packet = packet_pcm.size() / channels;
		if (*stream_packet_frames == 0)
		{
			*stream_packet_frames = frames_in_packet;
		} else if (frames_in_packet != *stream_packet_frames)
		{
			return false;
		}
		for (const float sample: packet_pcm)
		{
			if (!std::isfinite(sample))
			{
				return false;
			}
		}
		pcm.insert(pcm.end(), packet_pcm.begin(), packet_pcm.end());
	}
	pcm.resize(static_cast<size_t>(needed * channels));
	block->frames = std::move(pcm);
	return true;
}

// Fills one host block from a guest block whose size was already budgeted.
static bool Ngs2SamplerBlockFromGuest(const Ngs2SamplerSetup& setup, AudioVideoBackend::ElementaryAudioDecoder* decoder, const void* data,
                                      const Ngs2WaveformBlock& entry, uint64_t* stream_packet_frames, Ngs2Sampler::Block* block)
{
	const auto base = reinterpret_cast<uintptr_t>(data);
	if (entry.data_offset > std::numeric_limits<uint64_t>::max() - base)
	{
		return false;
	}
	const auto guest_data = static_cast<uintptr_t>(base + entry.data_offset);
	block->num_skip       = entry.num_skip_samples;
	block->num_samples    = entry.num_samples;
	block->num_repeats    = entry.num_repeats;
	block->num_repeated   = 0;
	block->user_data      = entry.user_data;
	block->guest_data     = guest_data;
	block->guest_size     = entry.data_size;
	if (setup.waveform_type == kNgs2WaveformTypeAtrac9)
	{
		return decoder != nullptr && Ngs2DecodeAtrac9Block(decoder, setup, guest_data, entry, stream_packet_frames, block);
	}
	return Ngs2ReadPcmBlock(setup, guest_data, entry, block);
}

static int32_t Ngs2SetSamplerFormat(Ngs2VoiceRecord& voice, const Ngs2SamplerSetupParam& param)
{
	if (param.header.size != sizeof(param) || param.flags != 0 || param.reserved != 0 || param.format.frame_offset != 0 ||
	    param.format.frame_margin != 0)
	{
		return kNgs2InvalidControl;
	}
	Ngs2SamplerSetup setup;
	if (!Ngs2MakeSamplerSetup(param.format, &setup) || !voice.sampler.Configure(param.format.channels, param.format.sample_rate))
	{
		return kNgs2InvalidControl;
	}
	// A setup starts from a stopped voice with an empty queue.
	voice.event         = Ngs2VoicePlayEvent::None;
	voice.state         = Ngs2VoicePlayState::Empty;
	voice.sampler_setup = setup;
	return OK;
}

// Blocks add with the reset flag: the given blocks replace the voice's waveform.
// Budgets are checked from the block headers first; then every block is read
// (and decoded, in order, through one new decoder stream) before the queue is
// replaced, so a rejected add changes nothing.
static int32_t Ngs2AddSamplerBlocks(Ngs2VoiceRecord& voice, const Ngs2SamplerBlocksParam& param)
{
	const auto& setup = voice.sampler_setup;
	if (!setup.configured || param.header.size != sizeof(param) || param.flags != kNgs2SamplerBlocksFlagReset || param.num_blocks == 0 ||
	    param.num_blocks > kNgs2MaxBlocksPerAdd || param.data == nullptr || param.blocks == nullptr)
	{
		return kNgs2InvalidControl;
	}
	std::vector<Ngs2WaveformBlock> entries(param.num_blocks);
	if (!Ngs2CopyFromGuest(entries.data(), param.blocks, entries.size() * sizeof(Ngs2WaveformBlock)))
	{
		return kNgs2InvalidControl;
	}
	uint64_t bytes = 0;
	for (const auto& entry: entries)
	{
		uint64_t block_bytes = 0;
		if (!Ngs2SamplerBlockBytes(setup, entry, &block_bytes))
		{
			return kNgs2InvalidControl;
		}
		bytes += block_bytes;
	}
	// The new blocks replace this voice's queue, so only they count for it.
	if (bytes > kNgs2MaxVoiceSampleBytes || Ngs2QueuedSampleBytes(*voice.system, &voice) + bytes > kNgs2MaxSystemSampleBytes)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: NGS2 sampler waveform exceeds the host sample budget\n");
		return kNgs2InvalidControl;
	}
	std::unique_ptr<AudioVideoBackend::ElementaryAudioDecoder> decoder;
	if (setup.waveform_type == kNgs2WaveformTypeAtrac9)
	{
		decoder = Ngs2OpenAtrac9Decoder(setup);
		if (decoder == nullptr)
		{
			return kNgs2InvalidControl;
		}
	}
	// Transient host memory for one add: the decoded blocks below (within the voice
	// budget) plus one raw block read or superframe at a time; adds are serialized
	// per system by the state lock.
	std::vector<Ngs2Sampler::Block> blocks(entries.size());
	uint64_t                        stream_packet_frames = 0;
	for (size_t i = 0; i < entries.size(); i++)
	{
		if (!Ngs2SamplerBlockFromGuest(setup, decoder.get(), param.data, entries[i], &stream_packet_frames, &blocks[i]))
		{
			return kNgs2InvalidControl;
		}
	}
	return voice.sampler.ReplaceQueue(std::move(blocks)) ? OK : kNgs2InvalidControl;
}

static int32_t Ngs2SetSamplerPitch(Ngs2VoiceRecord& voice, const Ngs2SamplerPitchParam& param)
{
	if (param.header.size != sizeof(param) || param.reserved != 0)
	{
		return kNgs2InvalidControl;
	}
	if (!voice.sampler.SetPitch(param.ratio))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: NGS2 sampler pitch outside the supported 0 to 16 range\n");
		return kNgs2InvalidControl;
	}
	return OK;
}

// Only the block end callback (flag 1) is confirmed on a guest. A registration
// with any other flag, including block repeat (2), is refused.
static int32_t Ngs2SetVoiceCallback(Ngs2VoiceRecord& voice, const Ngs2VoiceCallbackParam& param)
{
	if (param.header.size != sizeof(param) || (param.flags & ~kNgs2VoiceCallbackFlagEnd) != 0 || param.reserved != 0)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: NGS2 voice callback flags 0x%x refused; only block end is supported\n", param.flags);
		return kNgs2InvalidControl;
	}
	voice.callback = {param.callback, param.callback_data, param.flags};
	return OK;
}

// Standard sampler controls. Any other ID is refused, never ignored.
static int32_t Ngs2SamplerControl(Ngs2VoiceRecord& voice, const Ngs2VoiceParamHeader& header, const Ngs2VoiceParamHeader* param_list)
{
	switch (header.id)
	{
		case kNgs2SamplerSetupId: {
			Ngs2SamplerSetupParam param {};
			if (header.size != sizeof(param) || !Ngs2ReadGuest(&param, reinterpret_cast<const Ngs2SamplerSetupParam*>(param_list)))
			{
				return kNgs2InvalidControl;
			}
			return Ngs2SetSamplerFormat(voice, param);
		}
		case kNgs2SamplerBlocksId: {
			Ngs2SamplerBlocksParam param {};
			if (header.size != sizeof(param) || !Ngs2ReadGuest(&param, reinterpret_cast<const Ngs2SamplerBlocksParam*>(param_list)))
			{
				return kNgs2InvalidControl;
			}
			return Ngs2AddSamplerBlocks(voice, param);
		}
		case kNgs2SamplerPitchId: {
			Ngs2SamplerPitchParam param {};
			if (header.size != sizeof(param) || !Ngs2ReadGuest(&param, reinterpret_cast<const Ngs2SamplerPitchParam*>(param_list)))
			{
				return kNgs2InvalidControl;
			}
			return Ngs2SetSamplerPitch(voice, param);
		}
		case kNgs2VoiceParamCallback: {
			Ngs2VoiceCallbackParam param {};
			if (header.size != sizeof(param) || !Ngs2ReadGuest(&param, reinterpret_cast<const Ngs2VoiceCallbackParam*>(param_list)))
			{
				return kNgs2InvalidControl;
			}
			return Ngs2SetVoiceCallback(voice, param);
		}
		default: return kNgs2InvalidControl;
	}
}

// Event and gain commands of a standard sampler voice.
static int32_t Ngs2SamplerRunCommand(Ngs2VoiceRecord& voice, const std::array<uint32_t, 3>& command)
{
	if (command[0] == 2u && command[1] == 0x400u)
	{
		if (command[2] == 1u)
		{
			if (!voice.sampler.HasBlocks())
			{
				return kNgs2InvalidControl;
			}
			voice.event = Ngs2VoicePlayEvent::Play;
			return OK;
		}
		if (command[2] == 8u)
		{
			// An immediate stop drops the queued blocks as well.
			voice.sampler.Reset();
			voice.event = Ngs2VoicePlayEvent::StopImm;
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
		voice.sampler.SetGain(gain);
		return OK;
	}
	return kNgs2InvalidControl;
}

// Queues the callback of a finished block. The info carries the guest's own
// block user data unchanged. Repeat passes never queue a callback, since a
// repeat registration is refused.
static void Ngs2QueueSamplerEvent(const std::shared_ptr<Ngs2VoiceRecord>& voice, const Ngs2Sampler::Event& event,
                                  std::vector<Ngs2PendingCallback>* pending)
{
	if (voice->callback.handler == 0 || (voice->callback.flags & kNgs2VoiceCallbackFlagEnd) == 0)
	{
		return;
	}
	Ngs2PendingCallback callback {};
	callback.info.callback_data = voice->callback.data;
	callback.info.voice_handle  = voice->handle;
	callback.info.flag          = kNgs2VoiceCallbackFlagEnd;
	callback.info.user_data     = event.user_data;
	callback.info.block_data    = reinterpret_cast<const void*>(event.guest_data);
	callback.info.block_size    = static_cast<uint32_t>(std::min<uint64_t>(event.guest_size, std::numeric_limits<uint32_t>::max()));
	callback.info.num_repeated  = event.num_repeated;
	callback.handler            = voice->callback.handler;
	callback.voice              = voice;
	pending->push_back(std::move(callback));
}

// Renders one grain of a standard sampler voice into the stereo sum.
static void Ngs2RenderSamplerVoice(const std::shared_ptr<Ngs2VoiceRecord>& voice, uint32_t grain, uint32_t output_rate, double* mixed,
                                   std::vector<Ngs2PendingCallback>* pending)
{
	voice->sampler.SetPlaying(voice->state == Ngs2VoicePlayState::Playing);
	std::vector<Ngs2Sampler::Event> events;
	if (!voice->sampler.Render(grain, output_rate, mixed, &events))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: NGS2 sampler voice stopped: its resampling step exceeds the host limit\n");
		voice->sampler.SetPlaying(false);
		if (voice->state == Ngs2VoicePlayState::Playing)
		{
			voice->state = Ngs2VoicePlayState::Stopped;
		}
		return;
	}
	for (const auto& event: events)
	{
		Ngs2QueueSamplerEvent(voice, event, pending);
	}
	if (voice->sampler.Ended() && voice->state == Ngs2VoicePlayState::Playing)
	{
		voice->state = Ngs2VoicePlayState::Stopped;
	}
}

// Sums every voice of the system into mixed (stereo float) and clamps once. Only
// the system lease and the state lock are held; callbacks are queued, not run.
// system_out receives the record (not a lease) for the later dispatch.
// Most block end callbacks one render can produce: every queued block of a voice
// with an end callback can end at most once per render, and no block is added
// during a render.
static uint64_t Ngs2MaxEndCallbacks(const Ngs2SystemRecord& system)
{
	uint64_t count = 0;
	for (const auto& [unused_workspace, rack]: system.racks)
	{
		(void)unused_workspace;
		if (rack->type != Ngs2RackType::Sampler)
		{
			continue;
		}
		for (const auto& voice: rack->voices)
		{
			if (voice->callback.handler != 0 && (voice->callback.flags & kNgs2VoiceCallbackFlagEnd) != 0)
			{
				count += voice->sampler.QueuedBlocks();
			}
		}
	}
	return count;
}

// defer_callbacks is set when the calling thread holds the explicit system lock.
// Each render bounds its callback batch before changing voice state. When the
// caller holds the explicit lock, the deferred list must also have room for
// every callback the render could produce, since dispatch waits for unlock.
static int32_t Ngs2MixSystem(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info,
                             bool defer_callbacks, Ngs2RenderBufferInfoImpl* render, std::vector<float>* mixed,
                             std::vector<Ngs2PendingCallback>* pending, std::shared_ptr<Ngs2SystemRecord>* system_out)
{
	auto system = Ngs2AcquireSystem(system_handle);
	if (!system)
	{
		return kNgs2InvalidSystem;
	}
	if (buffer_info == nullptr || num_buffer_info != 1)
	{
		return kNgs2InvalidBufferInfo;
	}
	if (!Ngs2CopyFromGuest(render, buffer_info, sizeof(*render)))
	{
		return kNgs2InvalidBufferInfo;
	}
	if (render->data == nullptr)
	{
		return kNgs2InvalidBufferAddress;
	}
	if (render->waveform_type != kNgs2WaveformTypePcmF32 || render->channels != 2)
	{
		return kNgs2InvalidBufferInfo;
	}

	std::lock_guard lock(system->state_mutex);
	const uint32_t grain = system->option.num_grain_samples;
	if (grain == 0 || grain > system->option.max_grain_samples || grain > kNgs2MaxGrainSamples)
	{
		return kNgs2InvalidControl;
	}
	const size_t render_size = static_cast<size_t>(grain) * 2u * sizeof(float);
	if (render->data_size < render_size)
	{
		return kNgs2InvalidBufferSize;
	}
	if (!Ngs2IsGuestWritable(render->data, render_size))
	{
		return kNgs2InvalidBufferAddress;
	}
	size_t deferred = 0;
	if (defer_callbacks)
	{
		{
			std::lock_guard deferred_lock(system->deferred_mutex);
			deferred = system->deferred_callbacks.size();
		}
	}
	if (deferred + Ngs2MaxEndCallbacks(*system.Get()) > kNgs2MaxDeferredCallbacks)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: NGS2 render refused: callback budget is full\n");
		return kNgs2InvalidControl;
	}

	// Every voice sums into this double accumulator, so finite voices,
	// including large opposite-sign and high-gain ones, add without intermediate
	// rounding or clipping. The guest float buffer is written once, clamped.
	std::vector<double> accumulator(static_cast<size_t>(grain) * 2u, 0.0);
	for (const auto& [unused_workspace, rack]: system->racks)
	{
		(void)unused_workspace;
		for (const auto& voice: rack->voices)
		{
			Ngs2ApplyVoiceEvent(voice.get());
			if (rack->type == Ngs2RackType::Sampler)
			{
				Ngs2RenderSamplerVoice(voice, grain, system->option.sample_rate, accumulator.data(), pending);
				continue;
			}
			if (voice->state == Ngs2VoicePlayState::Playing && voice->stream.playing &&
			    !Ngs2MixPcmStream(&voice->stream, accumulator.data(), grain, 2, system->option.sample_rate))
			{
				voice->state = Ngs2VoicePlayState::Stopped;
			}
		}
	}
	mixed->resize(accumulator.size());
	for (size_t i = 0; i < accumulator.size(); i++)
	{
		(*mixed)[i] = static_cast<float>(std::clamp(accumulator[i], -1.0, 1.0));
	}
	*system_out = system.Shared();
	return OK;
}

// A queued callback still applies only while its voice entry is active and holds
// the same record: a destroyed, closing or replaced voice is skipped.
static bool Ngs2CallbackTargetLive(const Ngs2PendingCallback& callback)
{
	std::lock_guard lock(g_ngs_registry_mutex);
	auto            found = g_ngs_voices.find(callback.info.voice_handle);
	return found != g_ngs_voices.end() && found->second.record == callback.voice && !found->second.closing;
}

// Runs the queued callbacks after the system lease and the state lock are
// released. Each one is checked and run under the system's dispatch mutex: a
// destroy on another thread either marks the voice closing before the check
// (the callback is skipped) or waits for the callback to return. A callback may
// destroy its own voice, rack or system; later callbacks of destroyed voices
// are skipped.
static void Ngs2DispatchCallbacks(const std::shared_ptr<Ngs2SystemRecord>& system, const std::vector<Ngs2PendingCallback>& pending)
{
	for (const auto& callback: pending)
	{
		std::lock_guard dispatch(system->dispatch_mutex);
		if (!Ngs2CallbackTargetLive(callback))
		{
			continue;
		}
		(void)::Kyty::Emulator::GuestRuntimePort::Invoke(callback.handler, reinterpret_cast<uint64_t>(&callback.info), 0, 0);
	}
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
	// Keep the record alive, release the lock and lease, then run the callbacks a
	// render deferred while the lock was held. They now run with no explicit lock
	// or lease held, so a callback may stop or destroy its own objects.
	auto system = found->second->Record();
	g_ngs_thread_locks.erase(found);
	std::vector<Ngs2PendingCallback> deferred;
	{
		std::lock_guard deferred_lock(system->deferred_mutex);
		deferred.swap(system->deferred_callbacks);
	}
	if (!deferred.empty())
	{
		Ngs2DispatchCallbacks(system, deferred);
	}
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
	// As for a system destroy, destroying a rack while this thread holds its
	// system's explicit lock is refused: the destroy would wait for pins that other
	// threads can only release after taking that lock. A short-lived lease finds
	// the owning system; it is released before the destroy marks anything closing.
	{
		auto probe = Ngs2AcquireRack(rack_handle);
		if (!probe)
		{
			return kNgs2InvalidRack;
		}
		const uintptr_t owner = probe->system->workspace;
		probe.Reset();
		if (g_ngs_thread_locks.find(owner) != g_ngs_thread_locks.end())
		{
			return kNgs2InvalidRack;
		}
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
	Ngs2RenderBufferInfoImpl          render {};
	std::vector<float>                mixed;
	std::vector<Ngs2PendingCallback>  pending;
	std::shared_ptr<Ngs2SystemRecord> system;
	// If this thread holds the explicit system lock, that lock is still held after
	// the render's own lease and state lock are released, so running a guest
	// callback would hold the state mutex across it. Those callbacks wait for
	// unlock; the mix admits the render only if they all fit (see Ngs2MixSystem).
	const bool    defer_callbacks = g_ngs_thread_locks.find(system_handle) != g_ngs_thread_locks.end();
	const int32_t mix_result =
	    Ngs2MixSystem(system_handle, buffer_info, num_buffer_info, defer_callbacks, &render, &mixed, &pending, &system);
	if (mix_result != OK)
	{
		return mix_result;
	}
	const int32_t copy_result = Ngs2CopyToGuest(render.data, mixed.data(), mixed.size() * sizeof(float)) ? OK : kNgs2InvalidBufferAddress;
	if (!pending.empty())
	{
		if (defer_callbacks)
		{
			// Room was reserved by the admission check; nothing is dropped.
			std::lock_guard deferred_lock(system->deferred_mutex);
			for (auto& callback: pending)
			{
				system->deferred_callbacks.push_back(std::move(callback));
			}
		} else
		{
			Ngs2DispatchCallbacks(system, pending);
		}
	}
	return copy_result;
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
	if (rack != nullptr && rack->type == Ngs2RackType::Sampler)
	{
		return Ngs2SamplerControl(*voice.Get(), header, param_list);
	}
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
	if (rack != nullptr && rack->type == Ngs2RackType::Sampler)
	{
		for (size_t i = 0; i < command.size(); ++i)
		{
			voice->last_command[i] = command[i];
		}
		return Ngs2SamplerRunCommand(*voice.Get(), command);
	}
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

int KYTY_SYSV_ABI Ngs2ParseWaveformData(const void* data, size_t data_size, Ngs2WaveformInfo* info)
{
	PRINT_NAME();
	if (data == nullptr || info == nullptr)
	{
		return kNgs2InvalidOption;
	}
	Ngs2WaveformInfo parsed {};
	if (!Ngs2ParseVag(data, data_size, &parsed) && !Ngs2ParseRiffAtrac9(data, data_size, &parsed))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unsupported Ngs2 waveform data ignored\n");
		return kNgs2InvalidOption;
	}
	return Ngs2WriteGuest(info, parsed) ? OK : kNgs2InvalidOut;
}

int KYTY_SYSV_ABI Ngs2CalcWaveformBlock(const Ngs2WaveformInfo* info, uint32_t sample_pos, uint32_t num_samples,
                                        Ngs2WaveformBlock* block)
{
	PRINT_NAME();
	Ngs2WaveformInfo parsed {};
	if (block == nullptr || !Ngs2ReadGuest(&parsed, info) || parsed.audio_frame_samples == 0)
	{
		return kNgs2InvalidOption;
	}
	return Ngs2WriteGuest(block, Ngs2MakeWaveformBlock(parsed, sample_pos, num_samples)) ? OK : kNgs2InvalidOut;
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
		// num_decoded_samples sits at +0x10 in the 48-byte state the guest reads.
		if (rack->type == Ngs2RackType::Sampler)
		{
			output.num_decoded_samples = voice->sampler.DecodedFrames();
		}
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
