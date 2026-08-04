#include "Emulator/Audio.h"
#include "Emulator/AudioVideoBackend.h"
#include "Emulator/VideoFrameMemory.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

namespace VideoFrameMemory = Kyty::Emulator::VideoFrameMemory;
namespace GuestRuntimePort = ::Kyty::Emulator::GuestRuntimePort;
// Kyty::Libs::AudioVideoBackend is the canonical host-side decoder namespace.
// This local alias keeps the guest-facing implementation readable without changing its ABI.
namespace AudioVideoBackend = ::Kyty::Libs::AudioVideoBackend;

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
	bool                 source_failed = false;
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
		KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER further video frames suppressed\n");
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
		KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER call=%s handle=%p index=%u\n", name, static_cast<const void*>(player), index);
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
		KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER lifetime=%s handle=%p index=%u\n", event,
		             static_cast<const void*>(player), index);
	} else if (index == 128)
	{
		KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER further lifetime events suppressed\n");
	}
}

static void register_video_frame(uint8_t* frame, size_t bytes, uint32_t pitch)
{
	VideoFrameMemory::RegisterLinearFrame(reinterpret_cast<uint64_t>(frame), bytes, pitch);
}

static void unregister_video_frame(uint8_t* frame)
{
	VideoFrameMemory::UnregisterFrame(reinterpret_cast<uint64_t>(frame));
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
			GuestRuntimePort::Invoke(reinterpret_cast<uint64_t>(mem.deallocate_texture), reinterpret_cast<uint64_t>(mem.object_pointer),
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
			    GuestRuntimePort::Invoke(reinterpret_cast<uint64_t>(r->mem.allocate_texture),
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
		KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER create frames=%zu size=%zu pitch=%u allocator=%d real=%d\n", r->video_frames.size(), size,
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
			r->source_failed          = false;
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
	if (r->source_failed)
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
		const uint64_t result = GuestRuntimePort::Invoke4(reinterpret_cast<uint64_t>(callback),
		                                                   reinterpret_cast<uint64_t>(obj_ptr),
		                                                   static_cast<uint64_t>(event_id),
		                                                   0,
		                                                   reinterpret_cast<uint64_t>(data));
		if (avplayer_dump_enabled())
		{
			KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER event handle=%p id=0x%x callback=%p data=%p result=0x%" PRIx64 "\n",
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
		VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(frame), decoded.data.size());
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

		VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(frame), frame_bytes);
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
		KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER video handle=%p frame=%u data=%p time=%" PRIu64 " size=%ux%u real=%d\n",
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
			KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER backend=error path=%s reason=%s\n", host_filename.C_Str(),
			             decoder_error.empty() ? "media has no supported video stream" : decoder_error.c_str());
		}
		bool auto_start = false;
		{
			Core::LockGuard lock(h->mutex);
			h->filename               = clean_filename;
			h->host_filename          = host_filename;
			h->synthetic_width        = 1920;
			h->synthetic_height       = 1080;
			h->synthetic_frame_rate   = 30.0f;
			h->synthetic_frame_count  = 0;
			h->synthetic_obtained_num = 0;
			h->last_media_time_ms     = 0;
			h->media_duration_ms      = 0;
			h->media_audio_channels   = 0;
			h->media_audio_rate       = 0;
			h->playing                = false;
			h->paused                 = false;
			h->stop_fired             = false;
			h->source_failed          = true;
			auto_start                = h->auto_start;
		}
		if (avplayer_dump_enabled())
		{
			KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER source handle=%p auto_start=%d empty=1\n", static_cast<void*>(h),
			             auto_start ? 1 : 0);
		}
		emit_event(h, AVPLAYER_EVENT_STATE_READY);
		if (h->closing.load(std::memory_order_acquire))
		{
			return AVPLAYER_ERROR_OPERATION_FAILED;
		}
		return auto_start ? start_player(h) : 0;
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
		h->source_failed          = false;
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
				KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER backend=%s path=%s %ux%u fps=%.3f duration=%" PRIu64 " audio=%u/%u\n",
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
		KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER source handle=%p auto_start=%d\n", static_cast<void*>(h), auto_start ? 1 : 0);
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
		KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER start handle=%p\n", static_cast<void*>(h));
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
		KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER start_ex handle=%p time=%" PRIu64 "\n", static_cast<void*>(h), h->start_time_ms);
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

} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED
