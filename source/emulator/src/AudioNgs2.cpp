#include "Emulator/Audio.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/AudioNgs2Reverb.h"
#include "Emulator/AudioNgs2Sampler.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Kernel/FileSystem.h"
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
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
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

// The guest starts a block part-way by offsetting into the data it submits: offset_bytes skips the first frames, and
// data_size and frame_count count only what follows.
struct Ngs2CustomSamplerWaveformContext
{
	uint64_t offset_bytes;
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
	void*    data;
	size_t   data_size;
	uint32_t waveform_type;
	uint32_t channels;
};
static_assert(sizeof(Ngs2RenderBufferInfoImpl) == 24);

struct Ngs2VoiceState
{
	uint32_t state_flags;
};

// Sampler voice states, sized by the caller. The 48-byte block starts with a 4-byte voice state; the 56-byte block's
// voice state adds an error code, which moves every later field by 8 bytes.
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

struct Ngs2SamplerVoiceStateWithError
{
	Ngs2VoiceState voice_state;
	int32_t        error_code;
	float          envelope_height;
	float          peak_height;
	uint32_t       reserved;
	uint64_t       num_decoded_samples;
	uint64_t       decoded_data_size;
	uint64_t       user_data;
	const void*    waveform_data;
};
static_assert(sizeof(Ngs2SamplerVoiceStateWithError) == 56);
static_assert(offsetof(Ngs2SamplerVoiceStateWithError, num_decoded_samples) == 0x18);

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

// Routing params. A patch connects port 0 of a voice to input 0 of another voice (both indices are observed as zero).
// The submixer and mastering setups carry a channel count, and the mastering output param the index of the render
// buffer the voice writes; a zero word follows each value.
struct Ngs2VoicePatchParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	uint32_t             destination_input;
	uintptr_t            destination;
};
static_assert(sizeof(Ngs2VoicePatchParam) == 24);

struct Ngs2VoicePortInfo
{
	int32_t   matrix_id;
	float     volume;
	uint32_t  num_delay_samples;
	uint32_t  destination_input;
	uintptr_t destination;
};
static_assert(sizeof(Ngs2VoicePortInfo) == 24);

struct Ngs2RoutingValueParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             value;
	uint32_t             reserved;
};
static_assert(sizeof(Ngs2RoutingValueParam) == 16);

struct Ngs2ReverbSetupParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             num_input_channels;
	uint32_t             num_output_channels;
	uint32_t             flags;
	uint32_t             reserved;
};
static_assert(sizeof(Ngs2ReverbSetupParam) == 24);

// The I3DL2 block of a reverb voice. The reserved words are not checked: the guest library writes 8 to the first.
struct Ngs2ReverbI3dl2Param
{
	Ngs2VoiceParamHeader header;
	float                wet;
	float                dry;
	int32_t              room;
	int32_t              room_hf;
	uint32_t             reflection_pattern;
	float                decay_time;
	float                decay_hf_ratio;
	int32_t              reflections;
	float                reflections_delay;
	int32_t              reverb;
	float                reverb_delay;
	float                diffusion;
	float                density;
	float                hf_reference;
	uint32_t             reserved[8];
};
static_assert(sizeof(Ngs2ReverbI3dl2Param) == 96);

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

// Gen5 rack options carry a 0x30-byte expansion after the common prefix; captured
// extended options place max_voices at +0x50 and the rack body 0x30 bytes past its
// classic offset.
constexpr size_t kNgs2RackOptionExpansionBytes = 0x30;
// Sampler rack option size. Derived, not captured as a whole: the classic 0xa8
// sampler option plus the expansion.
constexpr size_t kNgs2SamplerRackOptionSize = 0xa8 + kNgs2RackOptionExpansionBytes;

// Standard sampler parameter IDs (header.id). The guest's param builders write
// these with size 40 (setup), 32 (blocks) and 16 (pitch) at offset +0.
constexpr uint32_t kNgs2SamplerSetupId  = 0x10000000u;
constexpr uint32_t kNgs2SamplerBlocksId = 0x10000001u;
constexpr uint32_t kNgs2SamplerPitchId  = 0x10000005u;
constexpr uint32_t kNgs2VoiceParamCallback          = 0x7u;
constexpr uint32_t kNgs2VoiceParamPatch             = 0x5u;
constexpr uint32_t kNgs2SubmixerSetupId             = 0x20000000u;
constexpr uint32_t kNgs2ReverbSetupId               = 0x20010000u;
constexpr uint32_t kNgs2ReverbI3dl2Id               = 0x20010001u;
constexpr uint32_t kNgs2MasteringSetupId            = 0x30000000u;
constexpr uint32_t kNgs2MasteringOutputId           = 0x30000005u;
// Host bound on the render buffers of one SystemRender call; titles pass one or three.
constexpr uint32_t kNgs2MaxRenderBuffers            = 8;
constexpr uint32_t kNgs2VoiceCallbackFlagEnd        = 0x1u;
// Blocks add flags. RESET replaces the queued waveform; CONTINUE keeps the
// waveform open for later adds; APPEND extends the last streaming block with
// more data. Other values (including SILENCE, 0x10) are refused.
constexpr uint32_t kNgs2SamplerBlocksFlagContinue   = 0x1u;
constexpr uint32_t kNgs2SamplerBlocksFlagAppend     = 0x2u;
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
// Host bounds on the blocks one custom sampler voice holds: their count matches the end callback budget per voice, and
// their frames cap the snapshot copies (16 MiB of stereo PCM16).
constexpr size_t   kNgs2MaxCustomQueuedBlocks  = 64;
constexpr uint64_t kNgs2MaxCustomQueuedFrames  = 1ull << 22u;
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
	       Core::VirtualMemory::CopyFromGuestOrLent(destination, reinterpret_cast<uint64_t>(source), static_cast<uint64_t>(size));
}

// Copies a NUL-terminated guest string that fits in `capacity` bytes, terminator included.
static bool Ngs2CopyFromGuestString(char* destination, const char* source, size_t capacity)
{
	for (size_t i = 0; i < capacity; i++)
	{
		if (!Ngs2CopyFromGuest(destination + i, source + i, 1))
		{
			return false;
		}
		if (destination[i] == '\0')
		{
			return true;
		}
	}
	return false;
}

static bool Ngs2CopyToGuest(void* destination, const void* source, size_t size)
{
	return destination != nullptr && source != nullptr && size != 0 &&
	       Core::VirtualMemory::CopyToGuestOrLent(reinterpret_cast<uint64_t>(destination), source, static_cast<uint64_t>(size));
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
	       Core::VirtualMemory::IsRangeWritableOrLent(reinterpret_cast<uint64_t>(pointer), static_cast<uint64_t>(size));
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

// Copies `size` bytes at `offset` of a waveform image; false when they cannot be read.
using Ngs2WaveformReader = std::function<bool(size_t offset, void* destination, size_t size)>;

// VAG: big-endian 0x30-byte header, then 16-byte ADPCM frames of 28 samples interleaved per channel.
// The low flag bits of each frame mark loop start (4), repeat (2) and end (1).
static bool Ngs2ParseVag(const Ngs2WaveformReader& read, size_t data_size, Ngs2WaveformInfo* info)
{
	std::array<uint8_t, 0x30> header {};
	if (data_size < header.size() || !read(0, header.data(), header.size()) || std::memcmp(header.data(), "VAGp", 4) != 0)
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
	if (!frames.empty() && read(header.size(), frames.data(), frames.size()))
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

// The RIFF WAVE chunks a waveform needs. Only the chunks before the data chunk are read, so a
// header-only buffer is enough.
struct Ngs2RiffChunks
{
	std::array<uint8_t, 0x34> fmt {};
	uint32_t                  fmt_size = 0;
	std::array<uint32_t, 2>   fact {};
	std::array<uint8_t, 0x34> smpl {};
	bool                      has_fmt  = false;
	bool                      has_fact = false;
	bool                      has_smpl = false;
	bool                      has_data = false;
};

static bool Ngs2ReadRiffChunks(const Ngs2WaveformReader& read, size_t data_size, Ngs2RiffChunks* chunks, Ngs2WaveformInfo* info)
{
	std::array<uint8_t, 12> riff {};
	if (data_size < riff.size() || !read(0, riff.data(), riff.size()) || std::memcmp(riff.data(), "RIFF", 4) != 0 ||
	    std::memcmp(&riff[8], "WAVE", 4) != 0)
	{
		return false;
	}
	for (size_t offset = riff.size(); !chunks->has_data && offset + 8u <= data_size;)
	{
		std::array<uint8_t, 8> chunk {};
		if (!read(offset, chunk.data(), chunk.size()))
		{
			return false;
		}
		uint32_t size = 0;
		std::memcpy(&size, &chunk[4], sizeof(size));
		const size_t body      = offset + 8u;
		const size_t available = data_size - body;
		if (std::memcmp(chunk.data(), "fmt ", 4) == 0 && size >= 16u && available >= 16u)
		{
			// A plain PCM header has 16 bytes; the ATRAC9 extensible header fills all 0x34.
			chunks->fmt_size = static_cast<uint32_t>(std::min<size_t>({size, available, chunks->fmt.size()}));
			chunks->has_fmt  = read(body, chunks->fmt.data(), chunks->fmt_size);
		} else if (std::memcmp(chunk.data(), "fact", 4) == 0 && size >= 8u && available >= 8u)
		{
			chunks->has_fact = read(body, chunks->fact.data(), sizeof(chunks->fact));
		} else if (std::memcmp(chunk.data(), "smpl", 4) == 0 && size >= chunks->smpl.size() && available >= chunks->smpl.size())
		{
			chunks->has_smpl = read(body, chunks->smpl.data(), chunks->smpl.size());
		} else if (std::memcmp(chunk.data(), "data", 4) == 0)
		{
			info->data_offset = static_cast<uint32_t>(body);
			info->data_size   = size;
			chunks->has_data  = true;
		}
		offset = body + size + (size & 1u);
	}
	return chunks->has_fmt && chunks->has_data;
}

// WAVE_FORMAT_EXTENSIBLE with the ATRAC9 subformat.
static bool Ngs2SetRiffAtrac9(const Ngs2RiffChunks& chunks, Ngs2WaveformInfo* info)
{
	static constexpr uint8_t kAtrac9Guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
	                                            0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
	const auto& fmt                = chunks.fmt;
	uint16_t    channels           = 0;
	uint16_t    block_align        = 0;
	uint16_t    superframe_samples = 0;
	uint32_t    sample_rate        = 0;
	std::memcpy(&channels, &fmt[0x02], sizeof(channels));
	std::memcpy(&sample_rate, &fmt[0x04], sizeof(sample_rate));
	std::memcpy(&block_align, &fmt[0x0c], sizeof(block_align));
	std::memcpy(&superframe_samples, &fmt[0x12], sizeof(superframe_samples));
	// Config bytes: sync 0xfe, then an 11-bit frame size minus one and the log2 frames per superframe.
	const uint8_t* config           = &fmt[0x2c];
	const uint32_t frame_bytes      = ((static_cast<uint32_t>(config[2]) << 3u) | (config[3] >> 5u)) + 1u;
	const uint32_t frames_per_super = 1u << ((config[3] >> 3u) & 3u);
	if (chunks.fmt_size < fmt.size() || std::memcmp(&fmt[0x18], kAtrac9Guid, sizeof(kAtrac9Guid)) != 0 || config[0] != 0xfeu ||
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
	info->num_samples           = chunks.has_fact ? chunks.fact[0] : info->data_size / block_align * superframe_samples;
	info->delay_samples         = chunks.has_fact ? chunks.fact[1] : 0;
	return true;
}

// Uncompressed frames: format tag 1 (integer) or 3 (float), one sample per unit and per frame.
// Only the sample formats the sampler plays are accepted: 16-bit integer and 32-bit float.
static bool Ngs2SetRiffPcm(const Ngs2RiffChunks& chunks, uint16_t tag, Ngs2WaveformInfo* info)
{
	const auto& fmt         = chunks.fmt;
	uint16_t    channels    = 0;
	uint16_t    block_align = 0;
	uint16_t    bits        = 0;
	uint32_t    sample_rate = 0;
	std::memcpy(&channels, &fmt[0x02], sizeof(channels));
	std::memcpy(&sample_rate, &fmt[0x04], sizeof(sample_rate));
	std::memcpy(&block_align, &fmt[0x0c], sizeof(block_align));
	std::memcpy(&bits, &fmt[0x0e], sizeof(bits));
	const uint32_t waveform_type = (tag == 1u && bits == 16u)   ? kNgs2WaveformTypePcmI16
	                               : (tag == 3u && bits == 32u) ? kNgs2WaveformTypePcmF32
	                                                            : 0u;
	const uint32_t frame_bytes   = static_cast<uint32_t>(channels) * bits / 8u;
	if (waveform_type == 0u || channels == 0 || block_align != frame_bytes)
	{
		return false;
	}

	info->format.waveform_type  = waveform_type;
	info->format.channels       = channels;
	info->format.sample_rate    = sample_rate;
	info->audio_unit_size       = frame_bytes;
	info->audio_unit_samples    = 1;
	info->audio_units_per_frame = 1;
	info->audio_frame_size      = frame_bytes;
	info->audio_frame_samples   = 1;
	info->num_samples           = chunks.has_fact ? chunks.fact[0] : info->data_size / frame_bytes;
	return true;
}

// RIFF WAVE holding ATRAC9 or uncompressed PCM. Loop points come from a sampler chunk.
static bool Ngs2ParseRiff(const Ngs2WaveformReader& read, size_t data_size, Ngs2WaveformInfo* info)
{
	Ngs2RiffChunks chunks;
	if (!Ngs2ReadRiffChunks(read, data_size, &chunks, info))
	{
		return false;
	}
	uint16_t tag = 0;
	std::memcpy(&tag, &chunks.fmt[0x00], sizeof(tag));
	const bool parsed = tag == 0xfffeu ? Ngs2SetRiffAtrac9(chunks, info) : Ngs2SetRiffPcm(chunks, tag, info);
	if (!parsed)
	{
		return false;
	}

	uint32_t loops = 0;
	if (chunks.has_smpl)
	{
		std::memcpy(&loops, &chunks.smpl[0x1c], sizeof(loops));
		std::memcpy(&info->loop_begin, &chunks.smpl[0x2c], sizeof(info->loop_begin));
		std::memcpy(&info->loop_end, &chunks.smpl[0x30], sizeof(info->loop_end));
	}
	Ngs2SetWaveformBlocks(info, loops != 0);
	return true;
}

static bool Ngs2ParseWaveform(const Ngs2WaveformReader& read, size_t data_size, Ngs2WaveformInfo* info)
{
	if (Ngs2ParseVag(read, data_size, info) || Ngs2ParseRiff(read, data_size, info))
	{
		return true;
	}
	KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unsupported Ngs2 waveform data ignored\n");
	return false;
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

static bool Ngs2ClassicOrExpandedSize(size_t size, size_t classic_size)
{
	return size == classic_size || size == classic_size + kNgs2RackOptionExpansionBytes;
}

static bool Ngs2SupportedRackOptionSize(uint32_t rack_id, size_t size)
{
	switch (rack_id)
	{
		case 0x1000: return size == kNgs2SamplerRackOptionSize;
		case 0x2000: return Ngs2ClassicOrExpandedSize(size, sizeof(Ngs2SubmixerRackOption));
		case 0x2001: return Ngs2ClassicOrExpandedSize(size, sizeof(Ngs2ReverbRackOption));
		case 0x3000: return Ngs2ClassicOrExpandedSize(size, sizeof(Ngs2MasteringRackOption));
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

// A rack created without an option takes the default voice count: 256 for a sampler, one voice for the routing racks.
// The explicit submixer and mastering options a title passes repeat every other default field and set only the voice
// count. Custom racks have no default option.
static bool Ngs2MakeDefaultRackConfig(uint32_t rack_id, Ngs2RackConfig* config)
{
	switch (rack_id)
	{
		case 0x1000: config->max_voices = 256; break;
		case 0x2000:
		case 0x2001:
		case 0x3000: config->max_voices = 1; break;
		default: return false;
	}
	return Ngs2RackTypeFromId(rack_id, &config->type);
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

// One custom sampler block, snapshotted when the guest submits it.
struct Ngs2PcmBlock
{
	std::vector<int16_t> samples;
	uint32_t             channels    = 0;
	uint64_t             frame_count = 0;
	// The guest's block address and user data, returned unchanged by the block end callback.
	uintptr_t            guest_data = 0;
	uintptr_t            user_data  = 0;
};

struct Ngs2PcmStream
{
	uint32_t                 format_id   = 0;
	uint32_t                 channels    = 0;
	uint32_t                 sample_rate = 0;
	bool                     playing     = false;
	// Blocks play in submission order; the cursor is in the front one.
	std::deque<Ngs2PcmBlock> blocks;
	uint64_t                 queued_frames = 0;
	double                   source_frame  = 0.0;
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
	// Set when the workspace came from the guest's allocator, which takes it back at destroy.
	Ngs2BufferAllocator                                            allocator {};
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
	Ngs2BufferAllocator                                  allocator {};
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
	// Routing: port 0 feeds the patched voice at port_volume; a mastering voice writes its input to render buffer
	// output_index, whose channel count must fit output_channels once that is set. patch_handle is the destination
	// as the guest named it.
	std::weak_ptr<Ngs2VoiceRecord>     patch;
	uintptr_t                          patch_handle    = 0;
	float                              port_volume     = 1.0f;
	uint32_t                           output_channels = 0;
	uint32_t                           output_index    = 0;
	Ngs2Reverb::Reverb                 reverb;
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

// Mixes the queued blocks in order, so consecutive blocks play without a gap. A block that plays to its last frame moves
// to finished; the stream ends when no block is left.
static bool Ngs2MixPcmStream(Ngs2PcmStream* stream, double* output, uint32_t output_frames, uint32_t output_channels,
                             uint32_t output_rate, std::vector<Ngs2PcmBlock>* finished)
{
	if (stream->sample_rate == 0 || output_rate == 0 || output_channels != 2)
	{
		return false;
	}
	const double step = static_cast<double>(stream->sample_rate) / static_cast<double>(output_rate);
	for (uint32_t frame = 0; frame < output_frames; ++frame)
	{
		while (!stream->blocks.empty() && stream->source_frame >= static_cast<double>(stream->blocks.front().frame_count))
		{
			stream->source_frame -= static_cast<double>(stream->blocks.front().frame_count);
			stream->queued_frames -= stream->blocks.front().frame_count;
			finished->push_back(std::move(stream->blocks.front()));
			stream->blocks.pop_front();
		}
		if (stream->blocks.empty())
		{
			stream->playing      = false;
			stream->source_frame = 0.0;
			return false;
		}
		const Ngs2PcmBlock& block      = stream->blocks.front();
		const auto          index      = static_cast<uint64_t>(stream->source_frame);
		const float         fraction   = static_cast<float>(stream->source_frame - static_cast<double>(index));
		const auto          next_index = index + 1 < block.frame_count ? index + 1 : index;
		for (uint32_t channel = 0; channel < output_channels; ++channel)
		{
			const uint32_t source_channel = block.channels == 1 ? 0 : channel;
			const float current = static_cast<float>(block.samples[index * block.channels + source_channel]) / 32768.0f;
			const float next = static_cast<float>(block.samples[next_index * block.channels + source_channel]) / 32768.0f;
			// Not clipped: the system mix sums in double and clamps once, so opposite-sign voices still cancel before
			// the final clamp.
			output[frame * output_channels + channel] += static_cast<double>(current + (next - current) * fraction);
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
		// A destroy owner keeps one pin and waits for the count to fall to it.
		if (found->second.pins <= 1)
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

// Guest bytes of one PCM frame; 0 for coded waveforms.
static uint32_t Ngs2PcmFrameBytes(const Ngs2SamplerSetup& setup)
{
	switch (setup.waveform_type)
	{
		case kNgs2WaveformTypePcmI16: return setup.channels * static_cast<uint32_t>(sizeof(int16_t));
		case kNgs2WaveformTypePcmF32: return setup.channels * static_cast<uint32_t>(sizeof(float));
		default: return 0;
	}
}

// Float bytes a block occupies once queued: num_skip + num_samples frames, or
// only the supplied frames of a streaming block. Known from the block header
// alone, so budgets are checked before any read.
static bool Ngs2SamplerBlockBytes(const Ngs2SamplerSetup& setup, const Ngs2WaveformBlock& entry, bool streaming, uint64_t* float_bytes)
{
	const uint64_t frames = streaming ? entry.data_size / Ngs2PcmFrameBytes(setup) : static_cast<uint64_t>(entry.num_skip_samples) + entry.num_samples;
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
	if (bytes > entry.data_size || !Ngs2GuestSpanInRange(guest_data, bytes) || !Core::VirtualMemory::IsRangeReadableOrLent(guest_data, bytes))
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
		if (!Core::VirtualMemory::IsRangeReadableOrLent(address, superframe) ||
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
	block->guest_frame_bytes = Ngs2PcmFrameBytes(setup);
	return Ngs2ReadPcmBlock(setup, guest_data, entry, block);
}

// A PCM block of an open waveform whose data holds only the first part of its
// samples: the guest streams the rest with appends. It plays once, from its
// first frame, and its data is whole frames.
static bool Ngs2IsStreamingBlock(const Ngs2SamplerSetup& setup, const Ngs2WaveformBlock& entry)
{
	const uint64_t frame_bytes = Ngs2PcmFrameBytes(setup);
	return frame_bytes != 0 && entry.num_samples != 0 && entry.num_skip_samples == 0 && entry.num_repeats == 0 &&
	       entry.data_size >= frame_bytes && entry.data_size % frame_bytes == 0 && entry.data_size / frame_bytes < entry.num_samples;
}

// Reads the supplied frames of a streaming block: data_size bytes of a
// waveform of num_samples frames.
static bool Ngs2StreamingBlockFromGuest(const Ngs2SamplerSetup& setup, const void* data, const Ngs2WaveformBlock& entry,
                                        Ngs2Sampler::Block* block)
{
	Ngs2WaveformBlock supplied = entry;
	supplied.num_samples       = static_cast<uint32_t>(entry.data_size / Ngs2PcmFrameBytes(setup));
	uint64_t unused_packets    = 0;
	if (!Ngs2SamplerBlockFromGuest(setup, nullptr, data, supplied, &unused_packets, block))
	{
		return false;
	}
	block->num_samples = entry.num_samples;
	block->streaming   = true;
	return true;
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
	// A setup starts from a stopped voice with an empty queue and a unit port volume.
	voice.port_volume   = 1.0f;
	voice.event         = Ngs2VoicePlayEvent::None;
	voice.state         = Ngs2VoicePlayState::Empty;
	voice.sampler_setup = setup;
	return OK;
}

// True when bytes more float frames fit the voice's and the system's sample budgets. A reset add replaces
// the voice's queue, so only the new blocks count for it.
static bool Ngs2SampleBudgetFits(const Ngs2VoiceRecord& voice, uint64_t bytes, bool reset)
{
	const uint64_t own = reset ? 0 : voice.sampler.QueuedSampleBytes();
	if (bytes > kNgs2MaxVoiceSampleBytes - std::min(own, kNgs2MaxVoiceSampleBytes) ||
	    Ngs2QueuedSampleBytes(*voice.system, &voice) + own + bytes > kNgs2MaxSystemSampleBytes)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: NGS2 sampler waveform exceeds the host sample budget\n");
		return false;
	}
	return true;
}

// Queues new blocks, after the current ones or, with reset, in their place. A
// PCM block of an open waveform may carry only the start of its data and stream
// the rest. Budgets are checked from the block headers first; then every block
// is read (and decoded, in order, through one new decoder stream) before the
// queue changes, so a rejected add changes nothing.
static int32_t Ngs2QueueSamplerBlocks(Ngs2VoiceRecord& voice, const void* data, const std::vector<Ngs2WaveformBlock>& entries, bool reset,
                                      bool open)
{
	const auto&       setup = voice.sampler_setup;
	std::vector<bool> streaming(entries.size());
	uint64_t          bytes = 0;
	for (size_t i = 0; i < entries.size(); i++)
	{
		streaming[i]         = open && Ngs2IsStreamingBlock(setup, entries[i]);
		uint64_t block_bytes = 0;
		if (!Ngs2SamplerBlockBytes(setup, entries[i], streaming[i], &block_bytes))
		{
			return kNgs2InvalidControl;
		}
		bytes += block_bytes;
	}
	if (!Ngs2SampleBudgetFits(voice, bytes, reset))
	{
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
		const bool read = streaming[i] ? Ngs2StreamingBlockFromGuest(setup, data, entries[i], &blocks[i])
		                               : Ngs2SamplerBlockFromGuest(setup, decoder.get(), data, entries[i], &stream_packet_frames, &blocks[i]);
		if (!read)
		{
			return kNgs2InvalidControl;
		}
	}
	const bool queued = reset ? voice.sampler.ReplaceQueue(std::move(blocks)) : voice.sampler.QueueBlocks(std::move(blocks));
	return queued ? OK : kNgs2InvalidControl;
}

// Appends data pieces to the voice's streaming block. Each piece is whole
// frames that continue the waveform; its sample counts are not read.
static int32_t Ngs2AppendSamplerPieces(Ngs2VoiceRecord& voice, const void* data, const std::vector<Ngs2WaveformBlock>& entries)
{
	const auto&    setup       = voice.sampler_setup;
	const uint64_t frame_bytes = Ngs2PcmFrameBytes(setup);
	uint64_t       bytes       = 0;
	for (const auto& entry: entries)
	{
		if (frame_bytes == 0 || entry.num_skip_samples != 0 || entry.num_repeats != 0 || entry.data_size % frame_bytes != 0 ||
		    entry.data_size / frame_bytes > kNgs2MaxBlockFrames)
		{
			return kNgs2InvalidControl;
		}
		bytes += entry.data_size / frame_bytes * setup.channels * sizeof(float);
	}
	if (!Ngs2SampleBudgetFits(voice, bytes, false))
	{
		return kNgs2InvalidControl;
	}
	for (const auto& entry: entries)
	{
		if (entry.data_size == 0)
		{
			continue;
		}
		Ngs2WaveformBlock piece = entry;
		piece.num_samples       = static_cast<uint32_t>(entry.data_size / frame_bytes);
		Ngs2Sampler::Block block;
		uint64_t           unused_packets = 0;
		if (!Ngs2SamplerBlockFromGuest(setup, nullptr, data, piece, &unused_packets, &block) ||
		    !voice.sampler.AppendPiece(std::move(block.frames), block.guest_data))
		{
			return kNgs2InvalidControl;
		}
	}
	return OK;
}

// Blocks add. Without RESET the waveform must still be open, as left by the
// previous add's CONTINUE (a setup opens it). The add's CONTINUE then decides
// whether later adds are accepted.
static int32_t Ngs2AddSamplerBlocks(Ngs2VoiceRecord& voice, const Ngs2SamplerBlocksParam& param)
{
	constexpr uint32_t kKnownFlags = kNgs2SamplerBlocksFlagContinue | kNgs2SamplerBlocksFlagAppend | kNgs2SamplerBlocksFlagReset;
	const bool         reset       = (param.flags & kNgs2SamplerBlocksFlagReset) != 0;
	const bool         append      = (param.flags & kNgs2SamplerBlocksFlagAppend) != 0;
	const bool         open        = (param.flags & kNgs2SamplerBlocksFlagContinue) != 0;
	if (!voice.sampler_setup.configured || param.header.size != sizeof(param) || (param.flags & ~kKnownFlags) != 0 || (reset && append) ||
	    param.num_blocks == 0 || param.num_blocks > kNgs2MaxBlocksPerAdd || param.data == nullptr || param.blocks == nullptr)
	{
		return kNgs2InvalidControl;
	}
	if (!reset && !voice.sampler.Open())
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: NGS2 sampler blocks added after the waveform was closed\n");
		return kNgs2InvalidControl;
	}
	std::vector<Ngs2WaveformBlock> entries(param.num_blocks);
	if (!Ngs2CopyFromGuest(entries.data(), param.blocks, entries.size() * sizeof(Ngs2WaveformBlock)))
	{
		return kNgs2InvalidControl;
	}
	const int32_t result = append ? Ngs2AppendSamplerPieces(voice, param.data, entries) : Ngs2QueueSamplerBlocks(voice, param.data, entries, reset, open);
	if (result == OK)
	{
		voice.sampler.SetOpen(open);
	}
	return result;
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

// Callback registration shared by the standard and custom sampler voices: one layout, block end only.
static int32_t Ngs2ControlVoiceCallback(Ngs2VoiceRecord& voice, const Ngs2VoiceParamHeader& header, const Ngs2VoiceParamHeader* param_list)
{
	Ngs2VoiceCallbackParam param {};
	if (header.size != sizeof(param) || !Ngs2ReadGuest(&param, reinterpret_cast<const Ngs2VoiceCallbackParam*>(param_list)))
	{
		return kNgs2InvalidControl;
	}
	return Ngs2SetVoiceCallback(voice, param);
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
		case kNgs2VoiceParamCallback: return Ngs2ControlVoiceCallback(voice, header, param_list);
		default: return kNgs2InvalidControl;
	}
}

// The gain command {6, 0x100, float}: a finite, non-negative linear gain.
static bool Ngs2ReadGainCommand(const std::array<uint32_t, 3>& command, float* gain)
{
	if (command[0] != 6u || command[1] != 0x100u)
	{
		return false;
	}
	std::memcpy(gain, &command[2], sizeof(*gain));
	return std::isfinite(*gain) && *gain >= 0.0f;
}

// The event command {2, 0x400, value}: one event bit, play 0x1, stop 0x2, stop-immediate 0x4, kill 0x8, pause 0x10 or
// resume 0x20.
static bool Ngs2ReadEventCommand(const std::array<uint32_t, 3>& command, Ngs2VoicePlayEvent* event)
{
	if (command[0] != 2u || command[1] != 0x400u)
	{
		return false;
	}
	switch (command[2])
	{
		case 0x1u: *event = Ngs2VoicePlayEvent::Play; return true;
		case 0x2u: *event = Ngs2VoicePlayEvent::Stop; return true;
		case 0x4u: *event = Ngs2VoicePlayEvent::StopImm; return true;
		case 0x8u: *event = Ngs2VoicePlayEvent::Kill; return true;
		case 0x10u: *event = Ngs2VoicePlayEvent::Pause; return true;
		case 0x20u: *event = Ngs2VoicePlayEvent::Resume; return true;
		default: return false;
	}
}

static bool Ngs2IsImmediateStop(Ngs2VoicePlayEvent event)
{
	return event == Ngs2VoicePlayEvent::StopImm || event == Ngs2VoicePlayEvent::Kill;
}

// Event commands of a voice that only routes other voices (mastering, submixer, reverb). It has no source data, so
// an event changes only its state. A submixer also takes the gain command.
static int32_t Ngs2MixerRunCommand(Ngs2VoiceRecord& voice, Ngs2RackType type, const std::array<uint32_t, 3>& command)
{
	float gain = 0.0f;
	if (type == Ngs2RackType::Submixer && Ngs2ReadGainCommand(command, &gain))
	{
		voice.port_volume = gain;
		return OK;
	}
	Ngs2VoicePlayEvent event = Ngs2VoicePlayEvent::None;
	if (!Ngs2ReadEventCommand(command, &event))
	{
		return kNgs2InvalidControl;
	}
	if (type == Ngs2RackType::Reverb && Ngs2IsImmediateStop(event))
	{
		// An immediate stop ends the reverb tail as well.
		voice.reverb.Reset();
	}
	voice.event = event;
	return OK;
}

static bool Ngs2SupportedRenderChannels(uint32_t channels)
{
	return channels == 1 || channels == 2 || channels == 8;
}

// Setups of the routing voices. The bus is stereo, so a submixer admits only the stereo setup it already mixes.
static int32_t Ngs2RoutingControl(Ngs2VoiceRecord& voice, Ngs2RackType type, const Ngs2VoiceParamHeader& header,
                                  const Ngs2VoiceParamHeader* param_list)
{
	Ngs2RoutingValueParam param {};
	if (header.size != sizeof(param) || !Ngs2ReadGuest(&param, reinterpret_cast<const Ngs2RoutingValueParam*>(param_list)) ||
	    param.reserved != 0)
	{
		return kNgs2InvalidControl;
	}
	if (type == Ngs2RackType::Submixer && header.id == kNgs2SubmixerSetupId)
	{
		return param.value == 2 ? OK : kNgs2InvalidControl;
	}
	if (type == Ngs2RackType::Mastering && header.id == kNgs2MasteringSetupId && Ngs2SupportedRenderChannels(param.value))
	{
		voice.output_channels = param.value;
		return OK;
	}
	if (type == Ngs2RackType::Mastering && header.id == kNgs2MasteringOutputId && param.value < kNgs2MaxRenderBuffers)
	{
		voice.output_index = param.value;
		return OK;
	}
	return kNgs2InvalidControl;
}

// Setup and I3DL2 parameters of a reverb voice. The bus is stereo, so only the stereo setup is admitted; a setup clears
// the tail and loads the default parameters.
static int32_t Ngs2ReverbControl(Ngs2VoiceRecord& voice, const Ngs2VoiceParamHeader& header, const Ngs2VoiceParamHeader* param_list)
{
	if (header.id == kNgs2ReverbSetupId)
	{
		Ngs2ReverbSetupParam setup {};
		if (header.size != sizeof(setup) || !Ngs2ReadGuest(&setup, reinterpret_cast<const Ngs2ReverbSetupParam*>(param_list)) ||
		    setup.num_input_channels != 2 || setup.num_output_channels != 2 || setup.flags != 0 || setup.reserved != 0)
		{
			return kNgs2InvalidControl;
		}
		return voice.reverb.Configure(voice.system->option.sample_rate) ? OK : kNgs2InvalidControl;
	}
	Ngs2ReverbI3dl2Param param {};
	if (header.id != kNgs2ReverbI3dl2Id || header.size != sizeof(param) ||
	    !Ngs2ReadGuest(&param, reinterpret_cast<const Ngs2ReverbI3dl2Param*>(param_list)))
	{
		return kNgs2InvalidControl;
	}
	Ngs2Reverb::Params params;
	params.wet                = param.wet;
	params.dry                = param.dry;
	params.room               = param.room;
	params.room_hf            = param.room_hf;
	params.reflection_pattern = param.reflection_pattern;
	params.decay_time         = param.decay_time;
	params.decay_hf_ratio     = param.decay_hf_ratio;
	params.reflections        = param.reflections;
	params.reflections_delay  = param.reflections_delay;
	params.reverb             = param.reverb;
	params.reverb_delay       = param.reverb_delay;
	params.diffusion          = param.diffusion;
	params.density            = param.density;
	params.hf_reference       = param.hf_reference;
	if (!Ngs2Reverb::Normalize(&params))
	{
		return kNgs2InvalidControl;
	}
	voice.reverb.SetParams(params);
	return OK;
}

static bool Ngs2IsRoutingRack(Ngs2RackType type)
{
	return type == Ngs2RackType::Submixer || type == Ngs2RackType::Reverb || type == Ngs2RackType::Mastering;
}

// A patch names a voice, or a rack of exactly one voice: a title routes its sampler voices to its single-voice
// mastering rack by the rack handle.
static std::shared_ptr<Ngs2VoiceRecord> Ngs2ResolvePatchDestination(uintptr_t destination)
{
	if (auto voice = Ngs2AcquireVoice(destination))
	{
		return voice.Shared();
	}
	auto rack = Ngs2AcquireRack(destination);
	if (!rack || rack->voices.size() != 1)
	{
		return nullptr;
	}
	return rack->voices.front();
}

// Connects port 0 of a source or submixer voice to a routing voice of the same system. The destination is resolved
// before the state lock is taken; the graph stays acyclic.
static int32_t Ngs2PatchVoice(Ngs2VoiceRecord* voice, const Ngs2VoiceParamHeader& header, const Ngs2VoiceParamHeader* param_list)
{
	Ngs2VoicePatchParam param {};
	if (header.size != sizeof(param) || !Ngs2ReadGuest(&param, reinterpret_cast<const Ngs2VoicePatchParam*>(param_list)) ||
	    param.port != 0 || param.destination_input != 0)
	{
		return kNgs2InvalidControl;
	}
	const auto destination = Ngs2ResolvePatchDestination(param.destination);
	if (destination == nullptr || destination->system != voice->system)
	{
		return kNgs2InvalidControl;
	}
	std::lock_guard lock(voice->system->state_mutex);
	const auto source_rack      = voice->rack.lock();
	const auto destination_rack = destination->rack.lock();
	if (source_rack == nullptr || destination_rack == nullptr || source_rack->type == Ngs2RackType::Mastering ||
	    !Ngs2IsRoutingRack(destination_rack->type))
	{
		return kNgs2InvalidControl;
	}
	for (auto hop = destination; hop != nullptr; hop = hop->patch.lock())
	{
		if (hop.get() == voice)
		{
			return kNgs2InvalidControl;
		}
	}
	voice->patch        = destination;
	voice->patch_handle = param.destination;
	return OK;
}

// Event and gain commands of a standard sampler voice.
static int32_t Ngs2SamplerRunCommand(Ngs2VoiceRecord& voice, const std::array<uint32_t, 3>& command)
{
	Ngs2VoicePlayEvent event = Ngs2VoicePlayEvent::None;
	if (Ngs2ReadEventCommand(command, &event))
	{
		// Play is accepted before any block: the voice plays silence until the guest queues its waveform.
		if (Ngs2IsImmediateStop(event))
		{
			// An immediate stop drops the queued blocks as well.
			voice.sampler.Reset();
		}
		voice.event = event;
		return OK;
	}
	float gain = 0.0f;
	if (!Ngs2ReadGainCommand(command, &gain))
	{
		return kNgs2InvalidControl;
	}
	voice.port_volume = gain;
	return OK;
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

// Queues the end callback of a custom sampler block that played to its last frame.
static void Ngs2QueueBlockEnd(const std::shared_ptr<Ngs2VoiceRecord>& voice, const Ngs2PcmBlock& block, std::vector<Ngs2PendingCallback>* pending)
{
	Ngs2Sampler::Event event {};
	event.user_data  = block.user_data;
	event.guest_data = block.guest_data;
	event.guest_size = block.samples.size() * sizeof(int16_t);
	Ngs2QueueSamplerEvent(voice, event, pending);
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

// Most block end callbacks one render can produce: every queued block of a voice
// with an end callback can end at most once per render, and no block is added
// during a render.
static uint64_t Ngs2MaxEndCallbacks(const Ngs2SystemRecord& system)
{
	uint64_t count = 0;
	for (const auto& [unused_workspace, rack]: system.racks)
	{
		(void)unused_workspace;
		if (rack->type != Ngs2RackType::Sampler && rack->type != Ngs2RackType::CustomSampler)
		{
			continue;
		}
		for (const auto& voice: rack->voices)
		{
			if (voice->callback.handler == 0 || (voice->callback.flags & kNgs2VoiceCallbackFlagEnd) == 0)
			{
				continue;
			}
			count += rack->type == Ngs2RackType::Sampler ? voice->sampler.QueuedBlocks() : voice->stream.blocks.size();
		}
	}
	return count;
}

// One render buffer of a SystemRender call and the samples mixed for it.
struct Ngs2RenderTarget
{
	Ngs2RenderBufferInfoImpl info {};
	std::vector<double>      mix;
};

// A grain of the stereo bus, interleaved, indexed by the voice it feeds.
using Ngs2BusMap = std::unordered_map<const Ngs2VoiceRecord*, std::vector<double>>;

static size_t Ngs2RenderSampleBytes(uint32_t waveform_type)
{
	return waveform_type == kNgs2WaveformTypePcmI16 ? sizeof(int16_t) : sizeof(float);
}

static int32_t Ngs2ReadRenderTargets(const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info,
                                     std::vector<Ngs2RenderTarget>* targets)
{
	if (buffer_info == nullptr || num_buffer_info == 0 || num_buffer_info > kNgs2MaxRenderBuffers)
	{
		return kNgs2InvalidBufferInfo;
	}
	std::array<Ngs2RenderBufferInfoImpl, kNgs2MaxRenderBuffers> infos {};
	if (!Ngs2CopyFromGuest(infos.data(), buffer_info, sizeof(infos[0]) * num_buffer_info))
	{
		return kNgs2InvalidBufferInfo;
	}
	targets->resize(num_buffer_info);
	for (uint32_t i = 0; i < num_buffer_info; i++)
	{
		const auto& info = infos[i];
		if (info.data == nullptr)
		{
			return kNgs2InvalidBufferAddress;
		}
		if ((info.waveform_type != kNgs2WaveformTypePcmF32 && info.waveform_type != kNgs2WaveformTypePcmI16) ||
		    !Ngs2SupportedRenderChannels(info.channels))
		{
			return kNgs2InvalidBufferInfo;
		}
		(*targets)[i].info = info;
	}
	return OK;
}

static int32_t Ngs2CheckRenderTargetSizes(uint32_t grain, std::vector<Ngs2RenderTarget>* targets)
{
	for (auto& target: *targets)
	{
		const size_t samples = static_cast<size_t>(grain) * target.info.channels;
		const size_t bytes   = samples * Ngs2RenderSampleBytes(target.info.waveform_type);
		if (target.info.data_size < bytes)
		{
			return kNgs2InvalidBufferSize;
		}
		if (!Ngs2IsGuestWritable(target.info.data, bytes))
		{
			return kNgs2InvalidBufferAddress;
		}
		target.mix.assign(samples, 0.0);
	}
	return OK;
}

// Adds a voice's output at its port volume to the input of the voice its port 0 is patched to. An unpatched voice
// reaches no output.
static void Ngs2RouteBus(const Ngs2VoiceRecord& voice, const std::vector<double>& bus, Ngs2BusMap* inputs)
{
	const auto destination = voice.patch.lock();
	if (destination == nullptr)
	{
		return;
	}
	auto& input = (*inputs)[destination.get()];
	input.resize(bus.size(), 0.0);
	for (size_t i = 0; i < bus.size(); i++)
	{
		input[i] += bus[i] * static_cast<double>(voice.port_volume);
	}
}

// A mastering voice writes its own channel count; a stereo voice may also feed the front pair of an 8-channel buffer,
// whose other channels stay silent.
static bool Ngs2MasteringFitsTarget(uint32_t output_channels, uint32_t target_channels)
{
	return output_channels == 0 || output_channels == target_channels || (output_channels == 2 && target_channels == 8);
}

// Writes the stereo bus into a render buffer: stereo as is, mono as the mean of both channels, and eight channels with
// the bus on the front pair and silence elsewhere.
static void Ngs2AddBusToTarget(const std::vector<double>& bus, Ngs2RenderTarget* target)
{
	const uint32_t channels = target->info.channels;
	const size_t   frames   = bus.size() / 2;
	for (size_t frame = 0; frame < frames; frame++)
	{
		const double left  = bus[frame * 2];
		const double right = bus[frame * 2 + 1];
		double*      out   = target->mix.data() + frame * channels;
		if (channels == 1)
		{
			out[0] += (left + right) * 0.5;
			continue;
		}
		out[0] += left;
		out[1] += right;
	}
}

// Hops from a routing voice to the end of its route. Upstream voices are farther, so mixing in decreasing distance
// finishes every input of a voice before the voice itself runs.
static uint32_t Ngs2RouteDistance(const Ngs2VoiceRecord& voice)
{
	uint32_t distance = 0;
	for (auto hop = voice.patch.lock(); hop != nullptr; hop = hop->patch.lock())
	{
		distance++;
	}
	return distance;
}

// Renders every source voice into its own bus, then runs the routing voices down the graph: a playing submixer passes
// its input on at its gain, a playing reverb voice passes it on through its reverb (every grain, so a tail outlives
// its input), a playing mastering voice writes it to its render buffer. Only the system lease and the state lock are held; callbacks are
// queued, not run. system_out receives the record (not a lease) for the later dispatch.
// defer_callbacks is set when the calling thread holds the explicit system lock. Each render bounds its callback batch
// before changing voice state; when the caller holds the explicit lock, the deferred list must also have room for every
// callback the render could produce, since dispatch waits for unlock.
static int32_t Ngs2MixSystem(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info,
                             bool defer_callbacks, std::vector<Ngs2RenderTarget>* targets, std::vector<Ngs2PendingCallback>* pending,
                             std::shared_ptr<Ngs2SystemRecord>* system_out)
{
	auto system = Ngs2AcquireSystem(system_handle);
	if (!system)
	{
		return kNgs2InvalidSystem;
	}
	const int32_t read_result = Ngs2ReadRenderTargets(buffer_info, num_buffer_info, targets);
	if (read_result != OK)
	{
		return read_result;
	}

	std::lock_guard lock(system->state_mutex);
	const uint32_t grain = system->option.num_grain_samples;
	if (grain == 0 || grain > system->option.max_grain_samples || grain > kNgs2MaxGrainSamples)
	{
		return kNgs2InvalidControl;
	}
	const int32_t size_result = Ngs2CheckRenderTargetSizes(grain, targets);
	if (size_result != OK)
	{
		return size_result;
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

	// Buses sum in double, so finite voices, including large opposite-sign and high-gain ones, add without intermediate
	// rounding or clipping. Each render buffer is clamped once, when it is written.
	const size_t                                      bus_samples = static_cast<size_t>(grain) * 2u;
	Ngs2BusMap                                        inputs;
	std::vector<std::pair<uint32_t, Ngs2VoiceRecord*>> routing;
	std::vector<double>                               bus(bus_samples);
	for (const auto& [unused_workspace, rack]: system->racks)
	{
		(void)unused_workspace;
		for (const auto& voice: rack->voices)
		{
			Ngs2ApplyVoiceEvent(voice.get());
			if (Ngs2IsRoutingRack(rack->type))
			{
				routing.emplace_back(Ngs2RouteDistance(*voice), voice.get());
				continue;
			}
			std::fill(bus.begin(), bus.end(), 0.0);
			if (rack->type == Ngs2RackType::Sampler)
			{
				Ngs2RenderSamplerVoice(voice, grain, system->option.sample_rate, bus.data(), pending);
				Ngs2RouteBus(*voice, bus, &inputs);
				continue;
			}
			if (voice->state != Ngs2VoicePlayState::Playing || !voice->stream.playing)
			{
				continue;
			}
			std::vector<Ngs2PcmBlock> finished;
			const bool playing = Ngs2MixPcmStream(&voice->stream, bus.data(), grain, 2, system->option.sample_rate, &finished);
			for (const auto& block: finished)
			{
				Ngs2QueueBlockEnd(voice, block, pending);
			}
			if (!playing)
			{
				voice->state = Ngs2VoicePlayState::Stopped;
			}
			Ngs2RouteBus(*voice, bus, &inputs);
		}
	}

	std::stable_sort(routing.begin(), routing.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	for (const auto& [unused_distance, voice]: routing)
	{
		(void)unused_distance;
		const auto input = inputs.find(voice);
		const auto rack  = voice->rack.lock();
		if (rack == nullptr || voice->state != Ngs2VoicePlayState::Playing)
		{
			continue;
		}
		if (rack->type == Ngs2RackType::Reverb)
		{
			if (input == inputs.end())
			{
				std::fill(bus.begin(), bus.end(), 0.0);
			} else
			{
				bus = input->second;
			}
			voice->reverb.Process(bus.data(), grain, bus.data());
			Ngs2RouteBus(*voice, bus, &inputs);
			continue;
		}
		if (input == inputs.end())
		{
			continue;
		}
		if (rack->type == Ngs2RackType::Submixer)
		{
			Ngs2RouteBus(*voice, input->second, &inputs);
			continue;
		}
		if (rack->type != Ngs2RackType::Mastering || voice->output_index >= targets->size())
		{
			continue;
		}
		auto& target = (*targets)[voice->output_index];
		if (!Ngs2MasteringFitsTarget(voice->output_channels, target.info.channels))
		{
			return kNgs2InvalidBufferInfo;
		}
		Ngs2AddBusToTarget(input->second, &target);
	}
	*system_out = system.Shared();
	return OK;
}

// Clamps a mixed render buffer once and writes it in the buffer's sample format.
static bool Ngs2WriteRenderTarget(const Ngs2RenderTarget& target)
{
	if (target.info.waveform_type == kNgs2WaveformTypePcmI16)
	{
		std::vector<int16_t> samples(target.mix.size());
		for (size_t i = 0; i < samples.size(); i++)
		{
			samples[i] = static_cast<int16_t>(std::lround(std::clamp(target.mix[i], -1.0, 1.0) * 32767.0));
		}
		return Ngs2CopyToGuest(target.info.data, samples.data(), samples.size() * sizeof(int16_t));
	}
	std::vector<float> samples(target.mix.size());
	for (size_t i = 0; i < samples.size(); i++)
	{
		samples[i] = static_cast<float>(std::clamp(target.mix[i], -1.0, 1.0));
	}
	return Ngs2CopyToGuest(target.info.data, samples.data(), samples.size() * sizeof(float));
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

// The allocator variants ask the guest's alloc handler for the workspace the buffer-info variants take: the handler
// receives {null, size, user_data} and fills host_buffer. The free handler gets the buffer back, with the same
// user_data, when the create fails after the allocation and when the object is destroyed.
static int32_t Ngs2AllocateWorkspace(const Ngs2BufferAllocator* allocator, size_t size, Ngs2BufferAllocator* allocator_out,
                                     Ngs2ContextBufferInfo* info)
{
	if (!Ngs2ReadGuest(allocator_out, allocator) || allocator_out->alloc_handler == nullptr || allocator_out->free_handler == nullptr)
	{
		return kNgs2InvalidBufferInfo;
	}
	*info                  = {};
	info->host_buffer_size = size;
	info->user_data        = allocator_out->user_data;
	const uint64_t handler = reinterpret_cast<uint64_t>(allocator_out->alloc_handler);
	const auto     result  = static_cast<int32_t>(::Kyty::Emulator::GuestRuntimePort::Invoke(handler, reinterpret_cast<uint64_t>(info), 0, 0));
	if (result != OK)
	{
		return result;
	}
	return info->host_buffer != nullptr ? OK : kNgs2InvalidBufferAddress;
}

static void Ngs2FreeWorkspace(const Ngs2BufferAllocator& allocator, uintptr_t workspace, size_t size)
{
	if (allocator.free_handler == nullptr)
	{
		return;
	}
	Ngs2ContextBufferInfo info {};
	info.host_buffer      = reinterpret_cast<void*>(workspace);
	info.host_buffer_size = size;
	info.user_data        = allocator.user_data;
	const uint64_t handler = reinterpret_cast<uint64_t>(allocator.free_handler);
	(void)::Kyty::Emulator::GuestRuntimePort::Invoke(handler, reinterpret_cast<uint64_t>(&info), 0, 0);
}

static int32_t Ngs2CreateSystem(const Ngs2SystemOption* option, const Ngs2ContextBufferInfo& info, const Ngs2BufferAllocator& allocator,
                                uintptr_t* handle)
{
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
	system->allocator           = allocator;
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

int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption* option, const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle)
{
	PRINT_NAME();
	Ngs2ContextBufferInfo info {};
	if (!Ngs2ReadGuest(&info, buffer_info))
	{
		return kNgs2InvalidBufferInfo;
	}
	return Ngs2CreateSystem(option, info, {}, handle);
}

int KYTY_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption* option, const Ngs2BufferAllocator* allocator,
                                                 uintptr_t* handle)
{
	PRINT_NAME();
	Ngs2SystemOption system_option {};
	if (!Ngs2SnapshotSystemOption(option, &system_option))
	{
		return kNgs2InvalidOption;
	}
	Ngs2BufferAllocator   guest_allocator {};
	Ngs2ContextBufferInfo info {};
	const int32_t         allocated = Ngs2AllocateWorkspace(allocator, kNgs2SystemWorkspaceBytes, &guest_allocator, &info);
	if (allocated != OK)
	{
		return allocated;
	}
	const int32_t created = Ngs2CreateSystem(option, info, guest_allocator, handle);
	if (created != OK)
	{
		Ngs2FreeWorkspace(guest_allocator, reinterpret_cast<uintptr_t>(info.host_buffer), kNgs2SystemWorkspaceBytes);
	}
	return created;
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
	std::vector<std::shared_ptr<Ngs2RackRecord>> racks;
	{
		std::lock_guard lock(record->state_mutex);
		for (auto& [unused_workspace, rack]: record->racks)
		{
			(void)unused_workspace;
			racks.push_back(std::move(rack));
		}
		record->racks.clear();
	}
	// The racks go with their system; guest-allocated workspaces return to their allocators, outside the state lock.
	for (const auto& rack: racks)
	{
		Ngs2FreeWorkspace(rack->allocator, rack->workspace, rack->workspace_size);
	}
	Ngs2FreeWorkspace(record->allocator, record->workspace, record->workspace_size);
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

static int32_t Ngs2CreateRack(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option, const Ngs2ContextBufferInfo& info,
                              const Ngs2BufferAllocator& allocator, uintptr_t* handle)
{
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
	rack->allocator       = allocator;
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

int KYTY_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
	                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle)
{
	PRINT_NAME();
	Ngs2ContextBufferInfo info {};
	if (!Ngs2ReadGuest(&info, buffer_info))
	{
		return kNgs2InvalidBufferInfo;
	}
	return Ngs2CreateRack(system_handle, rack_id, option, info, {}, handle);
}

int KYTY_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                              const Ngs2BufferAllocator* allocator, uintptr_t* handle)
{
	PRINT_NAME();
	Ngs2RackConfig config {};
	size_t         workspace_size = 0;
	if (!Ngs2SnapshotRackConfig(rack_id, option, &config) || !Ngs2CalculateRackWorkspaceSize(config.max_voices, &workspace_size))
	{
		return kNgs2InvalidOption;
	}
	Ngs2BufferAllocator   guest_allocator {};
	Ngs2ContextBufferInfo info {};
	const int32_t         allocated = Ngs2AllocateWorkspace(allocator, workspace_size, &guest_allocator, &info);
	if (allocated != OK)
	{
		return allocated;
	}
	const int32_t created = Ngs2CreateRack(system_handle, rack_id, option, info, guest_allocator, handle);
	if (created != OK)
	{
		Ngs2FreeWorkspace(guest_allocator, reinterpret_cast<uintptr_t>(info.host_buffer), workspace_size);
	}
	return created;
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
	Ngs2FreeWorkspace(record->allocator, record->workspace, record->workspace_size);
	if (buffer_info != nullptr && !Ngs2WriteGuest(buffer_info, output))
	{
		return kNgs2InvalidOut;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info)
{
	PRINT_NAME();
	std::vector<Ngs2RenderTarget>     targets;
	std::vector<Ngs2PendingCallback>  pending;
	std::shared_ptr<Ngs2SystemRecord> system;
	// If this thread holds the explicit system lock, that lock is still held after
	// the render's own lease and state lock are released, so running a guest
	// callback would hold the state mutex across it. Those callbacks wait for
	// unlock; the mix admits the render only if they all fit (see Ngs2MixSystem).
	const bool    defer_callbacks = g_ngs_thread_locks.find(system_handle) != g_ngs_thread_locks.end();
	const int32_t mix_result =
	    Ngs2MixSystem(system_handle, buffer_info, num_buffer_info, defer_callbacks, &targets, &pending, &system);
	if (mix_result != OK)
	{
		return mix_result;
	}
	const bool    written     = std::all_of(targets.begin(), targets.end(), [](const auto& target) { return Ngs2WriteRenderTarget(target); });
	const int32_t copy_result = written ? OK : kNgs2InvalidBufferAddress;
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
	if (header.id == kNgs2VoiceParamPatch)
	{
		return Ngs2PatchVoice(voice.Get(), header, param_list);
	}
	std::lock_guard lock(voice->system->state_mutex);
	auto rack = voice->rack.lock();
	if (rack != nullptr && rack->type == Ngs2RackType::Sampler)
	{
		return Ngs2SamplerControl(*voice.Get(), header, param_list);
	}
	if (rack != nullptr && (rack->type == Ngs2RackType::Submixer || rack->type == Ngs2RackType::Mastering))
	{
		return Ngs2RoutingControl(*voice.Get(), rack->type, header, param_list);
	}
	if (rack != nullptr && rack->type == Ngs2RackType::Reverb)
	{
		return Ngs2ReverbControl(*voice.Get(), header, param_list);
	}
	if (rack == nullptr || rack->type != Ngs2RackType::CustomSampler)
	{
		return kNgs2InvalidControl;
	}

	if (header.id == kNgs2VoiceParamCallback)
	{
		return Ngs2ControlVoiceCallback(*voice.Get(), header, param_list);
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
		voice->port_volume        = 1.0f;
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
		// A block lasts as many grains as its frames; only the queued host copies are bounded.
		if (!Ngs2ReadGuest(&context, waveform.context) || context.frame_count == 0 ||
		    voice->stream.blocks.size() >= kNgs2MaxCustomQueuedBlocks ||
		    context.frame_count > kNgs2MaxCustomQueuedFrames - voice->stream.queued_frames)
		{
			return kNgs2InvalidControl;
		}
		size_t pcm_bytes   = 0;
		size_t frame_bytes = 0;
		if (!Ngs2CalculatePcmBytes(context.frame_count, voice->stream.channels, &pcm_bytes) || context.data_size != pcm_bytes ||
		    !Ngs2CalculatePcmBytes(1, voice->stream.channels, &frame_bytes) || context.offset_bytes % frame_bytes != 0 ||
		    context.offset_bytes > std::numeric_limits<uint64_t>::max() - pcm_bytes ||
		    !Ngs2GuestSpanInRange(reinterpret_cast<uintptr_t>(waveform.data), context.offset_bytes + pcm_bytes))
		{
			return kNgs2InvalidControl;
		}
		std::vector<int16_t> samples(pcm_bytes / sizeof(int16_t));
		if (!Ngs2CopyFromGuest(samples.data(), reinterpret_cast<const uint8_t*>(waveform.data) + context.offset_bytes, pcm_bytes))
		{
			return kNgs2InvalidControl;
		}
		// Blocks queue behind the one playing: the guest streams by submitting the next blocks ahead and refilling on
		// each block end callback.
		voice->stream.blocks.push_back({std::move(samples), voice->stream.channels, context.frame_count,
		                                reinterpret_cast<uintptr_t>(waveform.data), static_cast<uintptr_t>(context.user_data)});
		voice->stream.queued_frames += context.frame_count;
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
	if (rack != nullptr && rack->type != Ngs2RackType::CustomSampler)
	{
		for (size_t i = 0; i < command.size(); ++i)
		{
			voice->last_command[i] = command[i];
		}
		return Ngs2MixerRunCommand(*voice.Get(), rack->type, command);
	}
	if (rack == nullptr)
	{
		return kNgs2InvalidControl;
	}
	for (size_t i = 0; i < command.size(); ++i)
	{
		voice->last_command[i] = command[i];
	}
	Ngs2VoicePlayEvent event = Ngs2VoicePlayEvent::None;
	if (Ngs2ReadEventCommand(command, &event))
	{
		if (event == Ngs2VoicePlayEvent::Play)
		{
			if (voice->stream.format_id != 0x12u || voice->stream.blocks.empty())
			{
				return kNgs2InvalidControl;
			}
			voice->stream.playing = true;
		}
		if (Ngs2IsImmediateStop(event))
		{
			// An immediate stop drops the queued blocks as well.
			voice->stream.playing       = false;
			voice->stream.blocks.clear();
			voice->stream.queued_frames = 0;
			voice->stream.source_frame  = 0.0;
		}
		voice->event = event;
		return OK;
	}
	float gain = 0.0f;
	if (!Ngs2ReadGainCommand(command, &gain))
	{
		return kNgs2InvalidControl;
	}
	voice->port_volume = gain;
	return OK;
}

int KYTY_SYSV_ABI Ngs2ParseWaveformData(const void* data, size_t data_size, Ngs2WaveformInfo* info)
{
	PRINT_NAME();
	if (data == nullptr || info == nullptr)
	{
		return kNgs2InvalidOption;
	}
	const auto read = [data](size_t offset, void* destination, size_t size)
	{ return Ngs2CopyFromGuest(destination, static_cast<const uint8_t*>(data) + offset, size); };
	Ngs2WaveformInfo parsed {};
	if (!Ngs2ParseWaveform(read, data_size, &parsed))
	{
		return kNgs2InvalidOption;
	}
	return Ngs2WriteGuest(info, parsed) ? OK : kNgs2InvalidOut;
}

// The waveform starts `offset` bytes into the file; the returned offsets count from the file start.
int KYTY_SYSV_ABI Ngs2ParseWaveformFile(const char* path, uint64_t offset, Ngs2WaveformInfo* info)
{
	PRINT_NAME();
	std::array<char, 1024> guest_path {};
	if (path == nullptr || info == nullptr || !Ngs2CopyFromGuestString(guest_path.data(), path, guest_path.size()))
	{
		return kNgs2InvalidOption;
	}
	const std::filesystem::path host(::Kyty::Kernel::FileSystem::GetRealFilename(String::FromUtf8(guest_path.data())).utf8_str().GetData());
	std::ifstream file(host, std::ios::binary | std::ios::ate);
	const auto    file_size = file ? static_cast<uint64_t>(file.tellg()) : 0u;
	if (!file || offset > file_size)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Ngs2 waveform file %s is unreadable at offset %" PRIu64 "\n", guest_path.data(), offset);
		return kNgs2InvalidOption;
	}
	const auto read = [&file, offset](size_t at, void* destination, size_t size)
	{
		file.clear();
		file.seekg(static_cast<std::streamoff>(offset + at));
		return static_cast<bool>(file.read(static_cast<char*>(destination), static_cast<std::streamsize>(size)));
	};
	Ngs2WaveformInfo parsed {};
	if (!Ngs2ParseWaveform(read, static_cast<size_t>(file_size - offset), &parsed) || offset + parsed.data_offset > UINT32_MAX)
	{
		return kNgs2InvalidOption;
	}
	parsed.data_offset += static_cast<uint32_t>(offset);
	for (uint32_t i = 0; i < parsed.num_blocks; i++)
	{
		parsed.blocks[i].data_offset += offset;
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
	if (state == nullptr || !Ngs2IsGuestWritable(state, state_size))
	{
		return kNgs2InvalidControl;
	}
	uint32_t  state_flags     = 0;
	uint64_t  decoded_samples = 0;
	uint64_t  decoded_bytes   = 0;
	uint64_t  user_data       = 0;
	uintptr_t waveform_data   = 0;
	{
		std::lock_guard lock(voice->system->state_mutex);
		auto rack = voice->rack.lock();
		const bool sampler        = rack != nullptr && rack->type == Ngs2RackType::Sampler;
		const bool custom_sampler = rack != nullptr && rack->type == Ngs2RackType::CustomSampler;
		const bool sized          = state_size == sizeof(Ngs2SamplerVoiceState) || (sampler && state_size == sizeof(Ngs2SamplerVoiceStateWithError));
		if ((!sampler && !custom_sampler) || !sized)
		{
			return kNgs2InvalidControl;
		}
		state_flags = Ngs2GetVoiceStateFlags(*voice.Get());
		if (sampler)
		{
			decoded_samples = voice->sampler.DecodedFrames();
			decoded_bytes   = voice->sampler.DecodedBytes();
			user_data       = voice->sampler.FrontUserData();
			waveform_data   = voice->sampler.WaveformData();
		}
	}
	// waveform_data is the guest address of the next frame to play, which a
	// streaming guest compares with its own buffer to find the consumed data.
	if (state_size == sizeof(Ngs2SamplerVoiceStateWithError))
	{
		Ngs2SamplerVoiceStateWithError output {};
		output.voice_state.state_flags = state_flags;
		output.num_decoded_samples     = decoded_samples;
		output.decoded_data_size       = decoded_bytes;
		output.user_data               = user_data;
		output.waveform_data           = reinterpret_cast<const void*>(waveform_data);
		return Ngs2CopyToGuest(state, &output, sizeof(output)) ? OK : kNgs2InvalidControl;
	}
	Ngs2SamplerVoiceState output {};
	output.voice_state.state_flags = state_flags;
	output.num_decoded_samples     = decoded_samples;
	output.decoded_data_size       = decoded_bytes;
	output.user_data               = user_data;
	output.waveform_data           = reinterpret_cast<const void*>(waveform_data);
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

// Port 0 is the only port. It has no matrix (-1, as port matrix commands are refused) and no delay, and a patch
// always names input 0.
int KYTY_SYSV_ABI Ngs2VoiceGetPortInfo(uintptr_t voice_handle, uint32_t port, void* out_info, size_t out_info_size)
{
	PRINT_NAME();
	auto voice = Ngs2AcquireVoice(voice_handle);
	if (!voice)
	{
		return kNgs2InvalidVoice;
	}
	if (port != 0 || out_info == nullptr || out_info_size != sizeof(Ngs2VoicePortInfo))
	{
		return kNgs2InvalidControl;
	}
	Ngs2VoicePortInfo info {};
	info.matrix_id = -1;
	{
		std::lock_guard lock(voice->system->state_mutex);
		info.volume      = voice->port_volume;
		info.destination = voice->patch.expired() ? 0 : voice->patch_handle;
	}
	return Ngs2CopyToGuest(out_info, &info, sizeof(info)) ? OK : kNgs2InvalidControl;
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
