#include "Emulator/AudioVideoBackend.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>

#include "libatrac9.h"

#if defined(KYTY_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace Kyty::Emulator::AudioVideoBackend {

#if defined(KYTY_HAVE_FFMPEG)
struct VideoFormatSelection
{
	AVPixelFormat format          = AV_PIX_FMT_NONE;
	bool          require_hardware = false;
};
#endif

struct Decoder::State
{
	Status     status = Status::Unavailable;
	std::string error;
	StreamInfo info;
	std::unique_ptr<InputSource> source;

	mutable std::mutex mutex;
	std::condition_variable condition;
	bool decode_done = false;

#if defined(KYTY_HAVE_FFMPEG)
	AVFormatContext* format = nullptr;
	AVIOContext*    input_io = nullptr;
	int64_t         input_size = 0;
	int64_t         input_position = 0;
	AVCodecContext*  video  = nullptr;
	AVCodecContext*  audio  = nullptr;
	AVPacket*        packet = nullptr;
	AVFrame*         video_frame = nullptr;
	AVFrame*         transferred_video_frame = nullptr;
	AVFrame*         audio_frame = nullptr;
	AVBufferRef*     hardware_device = nullptr;
	SwsContext*      scaler = nullptr;
	SwrContext*      resampler = nullptr;
	AVChannelLayout  output_layout {};
	VideoFormatSelection video_format_selection {};
	AVPixelFormat    hardware_video_format = AV_PIX_FMT_NONE;

	int video_stream = -1;
	int audio_stream = -1;
	bool demux_eof   = false;
	bool video_eof   = false;
	bool audio_eof   = false;
	bool flushed     = false;

	uint64_t next_video_timestamp = 0;
	uint64_t next_audio_timestamp = 0;
	// Frames that end at or before this position precede the last seek target.
	uint64_t seek_floor_ms = 0;

	std::thread worker;
	bool stop_requested = false;
	std::deque<VideoFrame> video_queue;
	std::deque<AudioFrame> audio_queue;
#endif
};

static void SetError(Decoder::State* state, Status status, const char* message)
{
	if (state == nullptr)
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->status = status;
		state->error  = message != nullptr ? message : "FFmpeg operation failed";
		state->decode_done = true;
	}
	state->condition.notify_all();
}

#if defined(KYTY_HAVE_FFMPEG)

namespace {

constexpr uint32_t kOutputSampleRate = 48000;
constexpr uint32_t kOutputChannels   = 2;
constexpr size_t   kVideoQueueCapacity = 6;
constexpr size_t   kAudioQueueCapacity = 32;

bool SoftwareDecodeAllowed()
{
	const char* value = std::getenv("KYTY_AVPLAYER_ALLOW_SOFTWARE");
	return value == nullptr || value[0] == '\0' || std::strcmp(value, "0") != 0;
}

uint64_t TimestampMilliseconds(int64_t timestamp, AVRational time_base, uint64_t fallback)
{
	if (timestamp == AV_NOPTS_VALUE || time_base.num <= 0 || time_base.den <= 0)
	{
		return fallback;
	}
	const double milliseconds = static_cast<double>(timestamp) * av_q2d(time_base) * 1000.0;
	if (!std::isfinite(milliseconds) || milliseconds < 0.0)
	{
		return fallback;
	}
	return static_cast<uint64_t>(milliseconds + 0.5);
}

const char* ErrorText(int error, char* buffer, size_t buffer_size)
{
	if (buffer == nullptr || buffer_size == 0)
	{
		return "unknown FFmpeg error";
	}
	buffer[0] = '\0';
	if (av_strerror(error, buffer, buffer_size) != 0)
	{
		std::snprintf(buffer, buffer_size, "FFmpeg error %d", error);
	}
	return buffer;
}

} // namespace

static AVPixelFormat SelectVideoFormat(AVCodecContext* context, const AVPixelFormat* formats)
{
	if (context == nullptr || formats == nullptr)
	{
		return AV_PIX_FMT_NONE;
	}
	auto* selection = static_cast<VideoFormatSelection*>(context->opaque);
	if (selection == nullptr)
	{
		return AV_PIX_FMT_NONE;
	}
	if (selection->format != AV_PIX_FMT_NONE)
	{
		for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
		{
			if (*format == selection->format)
			{
				return *format;
			}
		}
	}
	if (selection->require_hardware)
	{
		return AV_PIX_FMT_NONE;
	}
	for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
	{
		const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*format);
		if (descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
		{
			return *format;
		}
	}
	return AV_PIX_FMT_NONE;
}

static void SetFfmpegError(Decoder::State* state, Status status, int error, const char* operation)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = {};
	std::string message = operation != nullptr ? operation : "FFmpeg operation";
	message += ": ";
	message += ErrorText(error, text, sizeof(text));
	SetError(state, status, message.c_str());
}

static void FreeState(Decoder::State* state)
{
	if (state == nullptr)
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->stop_requested = true;
	}
	state->condition.notify_all();
	if (state->worker.joinable())
	{
		// The decode worker runs guest read callbacks, so waiting for it is a safe point.
		Kernel::PthreadInterruptibleScope any_syscall;
		state->worker.join();
	}
	if (state->resampler != nullptr)
	{
		swr_free(&state->resampler);
	}
	if (state->scaler != nullptr)
	{
		sws_freeContext(state->scaler);
		state->scaler = nullptr;
	}
	if (state->video_frame != nullptr)
	{
		av_frame_free(&state->video_frame);
	}
	if (state->transferred_video_frame != nullptr)
	{
		av_frame_free(&state->transferred_video_frame);
	}
	if (state->audio_frame != nullptr)
	{
		av_frame_free(&state->audio_frame);
	}
	if (state->packet != nullptr)
	{
		av_packet_free(&state->packet);
	}
	if (state->video != nullptr)
	{
		avcodec_free_context(&state->video);
	}
	if (state->audio != nullptr)
	{
		avcodec_free_context(&state->audio);
	}
	if (state->hardware_device != nullptr)
	{
		av_buffer_unref(&state->hardware_device);
	}
	if (state->format != nullptr)
	{
		avformat_close_input(&state->format);
	}
	if (state->input_io != nullptr)
	{
		av_freep(&state->input_io->buffer);
		avio_context_free(&state->input_io);
	}
	state->source.reset();
	if (state->output_layout.nb_channels != 0)
	{
		av_channel_layout_uninit(&state->output_layout);
	}
	state->video_queue.clear();
	state->audio_queue.clear();
}

static bool ConfigureAudio(Decoder::State* state)
{
	if (state == nullptr || state->audio == nullptr)
	{
		return true;
	}
	av_channel_layout_default(&state->output_layout, kOutputChannels);
	const int result = swr_alloc_set_opts2(&state->resampler,
	                                       &state->output_layout,
	                                       AV_SAMPLE_FMT_S16,
	                                       kOutputSampleRate,
	                                       &state->audio->ch_layout,
	                                       state->audio->sample_fmt,
	                                       state->audio->sample_rate,
	                                       0,
	                                       nullptr);
	if (result < 0 || state->resampler == nullptr)
	{
		SetFfmpegError(state, Status::OpenFailed, result, "configure audio resampler");
		return false;
	}
	if (swr_init(state->resampler) < 0)
	{
		SetError(state, Status::OpenFailed, "initialize audio resampler");
		return false;
	}
	return true;
}

static bool OpenCodec(AVFormatContext* format, int stream_index, AVCodecContext** context, const AVCodec** codec,
                      Decoder::State* state, bool video_codec)
{
	if (format == nullptr || context == nullptr || codec == nullptr || state == nullptr || stream_index < 0)
	{
		return false;
	}
	*codec = nullptr;
	const AVStream* stream = format->streams[stream_index];
	const int result = av_find_best_stream(format, stream->codecpar->codec_type, stream_index, -1, codec, 0);
	if (result < 0 || *codec == nullptr)
	{
		SetFfmpegError(state, Status::OpenFailed, result, "find media stream");
		return false;
	}
	if (video_codec)
	{
		state->video_format_selection = VideoFormatSelection {};
		state->video_format_selection.require_hardware = !SoftwareDecodeAllowed();
		for (int index = 0;; ++index)
		{
			const AVCodecHWConfig* config = avcodec_get_hw_config(*codec, index);
			if (config == nullptr)
			{
				break;
			}
			if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
			    config->device_type == AV_HWDEVICE_TYPE_VAAPI)
			{
				state->video_format_selection.format = config->pix_fmt;
				break;
			}
		}
		if (state->video_format_selection.format == AV_PIX_FMT_NONE && state->video_format_selection.require_hardware)
		{
			SetError(state, Status::OpenFailed, "video codec has no VA-API hardware configuration");
			return false;
		}
		if (state->video_format_selection.format != AV_PIX_FMT_NONE)
		{
			const char* device = std::getenv("KYTY_AVPLAYER_VAAPI_DEVICE");
			if (device != nullptr && device[0] == '\0')
			{
				device = nullptr;
			}
			const int device_result = av_hwdevice_ctx_create(&state->hardware_device, AV_HWDEVICE_TYPE_VAAPI, device, nullptr, 0);
			if (device_result < 0)
			{
				if (state->video_format_selection.require_hardware)
				{
					SetFfmpegError(state, Status::OpenFailed, device_result, "create VA-API device");
					return false;
				}
				state->video_format_selection.format = AV_PIX_FMT_NONE;
			}
		}
		if (std::getenv("KYTY_DUMP_AVPLAYER") != nullptr)
		{
			KYTY_LOG_DEBUG( "KYTY_DUMP_AVPLAYER video_decode=%s format=%s software_allowed=%d\n",
			             state->hardware_device != nullptr ? "vaapi" : "software",
			             state->video_format_selection.format == AV_PIX_FMT_NONE
			                 ? "software"
			                 : av_get_pix_fmt_name(state->video_format_selection.format),
			             state->video_format_selection.require_hardware ? 0 : 1);
		}
	}
	*context = avcodec_alloc_context3(*codec);
	if (*context == nullptr)
	{
		SetError(state, Status::OpenFailed, "allocate codec context");
		return false;
	}
	if (avcodec_parameters_to_context(*context, stream->codecpar) < 0)
	{
		SetError(state, Status::OpenFailed, "open media codec");
		return false;
	}
	if (video_codec)
	{
		(*context)->opaque = &state->video_format_selection;
		(*context)->get_format = SelectVideoFormat;
		if (state->hardware_device != nullptr)
		{
			(*context)->hw_device_ctx = av_buffer_ref(state->hardware_device);
			state->hardware_video_format = state->video_format_selection.format;
		}
	}
	if (avcodec_open2(*context, *codec, nullptr) < 0)
	{
		SetError(state, Status::OpenFailed, "open media codec");
		return false;
	}
	return true;
}

// Copies a decoded picture into tightly packed NV12 (the pitch is the width).
static bool CopyNv12Picture(const AVFrame* source, uint32_t width, uint32_t height, SwsContext** scaler, VideoFrame* frame,
                            const char** error)
{
	const size_t chroma_height = (static_cast<size_t>(height) + 1u) / 2u;
	if (width == 0 || height == 0 || static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / height)
	{
		*error = "decoded video dimensions overflow";
		return false;
	}
	const size_t luma_bytes = static_cast<size_t>(width) * height;
	if (static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / chroma_height ||
	    luma_bytes > std::numeric_limits<size_t>::max() - static_cast<size_t>(width) * chroma_height)
	{
		*error = "decoded video buffer size overflow";
		return false;
	}
	frame->width  = width;
	frame->height = height;
	frame->pitch  = width;
	frame->data.resize(luma_bytes + static_cast<size_t>(width) * chroma_height);
	const bool copied_nv12 = source->format == AV_PIX_FMT_NV12 && source->width == static_cast<int>(width) &&
	                         source->height == static_cast<int>(height) && source->data[0] != nullptr && source->data[1] != nullptr &&
	                         source->linesize[0] >= static_cast<int>(width) && source->linesize[1] >= static_cast<int>(width);
	if (copied_nv12)
	{
		for (uint32_t row = 0; row < height; ++row)
		{
			std::memcpy(frame->data.data() + static_cast<size_t>(row) * width,
			            source->data[0] + static_cast<size_t>(row) * source->linesize[0], width);
		}
		for (size_t row = 0; row < chroma_height; ++row)
		{
			std::memcpy(frame->data.data() + luma_bytes + row * width,
			            source->data[1] + row * static_cast<size_t>(source->linesize[1]), width);
		}
		return true;
	}
	*scaler = sws_getCachedContext(*scaler, source->width, source->height, static_cast<AVPixelFormat>(source->format),
	                               static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr,
	                               nullptr);
	if (*scaler == nullptr)
	{
		*error = "initialize video scaler";
		return false;
	}
	uint8_t* destination[4]       = {frame->data.data(), frame->data.data() + luma_bytes, nullptr, nullptr};
	int      destination_pitch[4] = {static_cast<int>(width), static_cast<int>(width), 0, 0};
	if (sws_scale(*scaler, source->data, source->linesize, 0, source->height, destination, destination_pitch) <= 0)
	{
		*error = "convert video frame to NV12";
		return false;
	}
	return true;
}

static bool ConvertVideo(Decoder::State* state)
{
	AVFrame* source = state->video_frame;
	if (source == nullptr || state->video == nullptr || source->width <= 0 || source->height <= 0)
	{
		SetError(state, Status::DecodeFailed, "invalid decoded video frame");
		return false;
	}
	if (state->hardware_device != nullptr && source->format == state->hardware_video_format)
	{
		if (state->transferred_video_frame == nullptr)
		{
			SetError(state, Status::DecodeFailed, "missing VA-API transfer frame");
			return false;
		}
		av_frame_unref(state->transferred_video_frame);
		const int transfer = av_hwframe_transfer_data(state->transferred_video_frame, source, 0);
		if (transfer < 0)
		{
			SetFfmpegError(state, Status::DecodeFailed, transfer, "transfer VA-API video frame");
			return false;
		}
		source = state->transferred_video_frame;
	}
	const uint64_t timestamp_ms = TimestampMilliseconds(source->best_effort_timestamp,
	                                                    state->format->streams[state->video_stream]->time_base,
	                                                    state->next_video_timestamp);
	state->next_video_timestamp = timestamp_ms +
	                              (state->info.video_frame_rate > 0.0
	                                   ? static_cast<uint64_t>(1000.0 / state->info.video_frame_rate + 0.5)
	                                   : 0);
	if (state->next_video_timestamp <= state->seek_floor_ms)
	{
		return true;
	}
	VideoFrame  frame;
	const char* copy_error = nullptr;
	if (!CopyNv12Picture(source, static_cast<uint32_t>(state->info.video_width), static_cast<uint32_t>(state->info.video_height),
	                     &state->scaler, &frame, &copy_error))
	{
		SetError(state, Status::DecodeFailed, copy_error);
		return false;
	}
	frame.timestamp_ms = timestamp_ms;
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		if (state->stop_requested)
		{
			return false;
		}
		state->condition.wait(lock, [state] {
			return state->stop_requested || state->video_queue.size() < kVideoQueueCapacity;
		});
		if (state->stop_requested)
		{
			return false;
		}
		state->video_queue.push_back(std::move(frame));
	}
	state->condition.notify_all();
	return true;
}

static bool ConvertAudio(Decoder::State* state)
{
	AVFrame* source = state->audio_frame;
	if (source == nullptr || state->resampler == nullptr || source->nb_samples <= 0)
	{
		return true;
	}
	const int delay = swr_get_delay(state->resampler, state->audio->sample_rate);
	const int samples = static_cast<int>(av_rescale_rnd(delay + source->nb_samples,
	                                                    kOutputSampleRate,
	                                                    state->audio->sample_rate,
	                                                    AV_ROUND_UP));
	if (samples <= 0 || static_cast<size_t>(samples) > std::numeric_limits<size_t>::max() / kOutputChannels)
	{
		SetError(state, Status::DecodeFailed, "decoded audio buffer size overflow");
		return false;
	}
	AudioFrame frame;
	frame.channels    = kOutputChannels;
	frame.sample_rate = kOutputSampleRate;
	frame.timestamp_ms = TimestampMilliseconds(source->best_effort_timestamp,
	                                             state->format->streams[state->audio_stream]->time_base,
	                                             state->next_audio_timestamp);
	frame.data.resize(static_cast<size_t>(samples) * kOutputChannels);
	uint8_t* destination[1] = {reinterpret_cast<uint8_t*>(frame.data.data())};
	const int converted = swr_convert(state->resampler,
	                                  destination,
	                                  samples,
	                                  const_cast<const uint8_t**>(source->extended_data),
	                                  source->nb_samples);
	if (converted < 0)
	{
		SetFfmpegError(state, Status::DecodeFailed, converted, "convert audio frame");
		return false;
	}
	frame.data.resize(static_cast<size_t>(converted) * kOutputChannels);
	state->next_audio_timestamp = frame.timestamp_ms +
	                              static_cast<uint64_t>(static_cast<double>(converted) * 1000.0 / kOutputSampleRate + 0.5);
	if (!frame.data.empty() && state->next_audio_timestamp > state->seek_floor_ms)
	{
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->audio_queue.size() >= kAudioQueueCapacity)
			{
				state->audio_queue.pop_front();
			}
			state->audio_queue.push_back(std::move(frame));
		}
		state->condition.notify_all();
	}
	return true;
}

static bool DrainVideo(Decoder::State* state)
{
	if (state == nullptr || state->video == nullptr)
	{
		return true;
	}
	for (;;)
	{
		const int result = avcodec_receive_frame(state->video, state->video_frame);
		if (result == 0)
		{
			if (!ConvertVideo(state))
			{
				return false;
			}
			av_frame_unref(state->video_frame);
			continue;
		}
		if (result == AVERROR(EAGAIN))
		{
			return true;
		}
		if (result == AVERROR_EOF)
		{
			state->video_eof = true;
			return true;
		}
		SetFfmpegError(state, Status::DecodeFailed, result, "receive video frame");
		return false;
	}
}

static bool DrainAudio(Decoder::State* state)
{
	if (state == nullptr || state->audio == nullptr)
	{
		return true;
	}
	for (;;)
	{
		const int result = avcodec_receive_frame(state->audio, state->audio_frame);
		if (result == 0)
		{
			if (!ConvertAudio(state))
			{
				return false;
			}
			av_frame_unref(state->audio_frame);
			continue;
		}
		if (result == AVERROR(EAGAIN))
		{
			return true;
		}
		if (result == AVERROR_EOF)
		{
			state->audio_eof = true;
			return true;
		}
		SetFfmpegError(state, Status::DecodeFailed, result, "receive audio frame");
		return false;
	}
}

static bool FlushCodecs(Decoder::State* state)
{
	if (state->flushed)
	{
		return true;
	}
	state->flushed = true;
	if (state->video != nullptr)
	{
		const int result = avcodec_send_packet(state->video, nullptr);
		if (result < 0 && result != AVERROR_EOF && result != AVERROR(EAGAIN))
		{
			SetFfmpegError(state, Status::DecodeFailed, result, "flush video decoder");
			return false;
		}
	}
	if (state->audio != nullptr)
	{
		const int result = avcodec_send_packet(state->audio, nullptr);
		if (result < 0 && result != AVERROR_EOF && result != AVERROR(EAGAIN))
		{
			SetFfmpegError(state, Status::DecodeFailed, result, "flush audio decoder");
			return false;
		}
	}
	return DrainVideo(state) && DrainAudio(state);
}

static bool Pump(Decoder::State* state)
{
	if (state == nullptr || state->demux_eof)
	{
		return FlushCodecs(state);
	}
	const int result = av_read_frame(state->format, state->packet);
	if (result < 0)
	{
		if (result != AVERROR_EOF)
		{
			SetFfmpegError(state, Status::DecodeFailed, result, "read media packet");
			return false;
		}
		state->demux_eof = true;
		return FlushCodecs(state);
	}
	if (state->packet->stream_index == state->video_stream && state->video != nullptr)
	{
		const int send_result = avcodec_send_packet(state->video, state->packet);
		if (send_result < 0 && send_result != AVERROR(EAGAIN))
		{
			SetFfmpegError(state, Status::DecodeFailed, send_result, "send video packet");
			av_packet_unref(state->packet);
			return false;
		}
	}
	if (state->packet->stream_index == state->audio_stream && state->audio != nullptr)
	{
		const int send_result = avcodec_send_packet(state->audio, state->packet);
		if (send_result < 0 && send_result != AVERROR(EAGAIN))
		{
			SetFfmpegError(state, Status::DecodeFailed, send_result, "send audio packet");
			av_packet_unref(state->packet);
			return false;
		}
	}
	av_packet_unref(state->packet);
	return DrainVideo(state) && DrainAudio(state);
}

static void DecodeLoop(Decoder::State* state)
{
	if (state == nullptr)
	{
		return;
	}
	for (;;)
	{
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->stop_requested || state->demux_eof)
			{
				break;
			}
		}
		if (!Pump(state))
		{
			break;
		}
	}
	{
		// A stop requested by Seek() is a pause in decoding, not the end of the stream.
		std::lock_guard<std::mutex> lock(state->mutex);
		state->decode_done = state->decode_done || !state->stop_requested;
	}
	state->condition.notify_all();
}

static bool RepositionInput(Decoder::State* state, uint64_t time_ms)
{
	if (time_ms > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / 1000))
	{
		SetError(state, Status::DecodeFailed, "seek position overflow");
		return false;
	}
	const int64_t target = av_rescale(static_cast<int64_t>(time_ms), AV_TIME_BASE, 1000);
	const int     result = avformat_seek_file(state->format, -1, std::numeric_limits<int64_t>::min(), target, target, 0);
	if (result < 0)
	{
		SetFfmpegError(state, Status::DecodeFailed, result, "seek media input");
		return false;
	}
	if (state->video != nullptr)
	{
		avcodec_flush_buffers(state->video);
	}
	if (state->audio != nullptr)
	{
		avcodec_flush_buffers(state->audio);
	}
	if (state->resampler != nullptr && swr_init(state->resampler) < 0)
	{
		SetError(state, Status::DecodeFailed, "reset audio resampler");
		return false;
	}
	state->demux_eof            = false;
	state->video_eof            = state->video == nullptr;
	state->audio_eof            = state->audio == nullptr;
	state->flushed              = false;
	state->next_video_timestamp = time_ms;
	state->next_audio_timestamp = time_ms;
	state->seek_floor_ms        = time_ms;
	return true;
}

#endif

bool Decoder::IsAvailable()
{
#if defined(KYTY_HAVE_FFMPEG)
	return true;
#else
	return false;
#endif
}

const char* Decoder::BackendName()
{
#if defined(KYTY_HAVE_FFMPEG)
	return "ffmpeg";
#else
	return "unavailable";
#endif
}

Decoder::Decoder(): state_(std::make_unique<State>()) {}

Decoder::~Decoder()
{
	Close();
}

#if defined(KYTY_HAVE_FFMPEG)
static int ReadInputSource(void* opaque, uint8_t* destination, int size)
{
	auto* state = static_cast<Decoder::State*>(opaque);
	if (size <= 0 || destination == nullptr)
	{
		return AVERROR(EINVAL);
	}
	if (state->input_position == state->input_size)
	{
		return AVERROR_EOF;
	}
	const auto requested = static_cast<uint32_t>(std::min<int64_t>(size, state->input_size - state->input_position));
	const int read = state->source->ReadAt(static_cast<uint64_t>(state->input_position), destination, requested);
	if (read < 0 || static_cast<uint32_t>(read) > requested)
	{
		return AVERROR(EIO);
	}
	state->input_position += read;
	return read == 0 ? AVERROR_EOF : read;
}

static int64_t SeekInputSource(void* opaque, int64_t offset, int whence)
{
	auto* state = static_cast<Decoder::State*>(opaque);
	whence &= ~AVSEEK_FORCE;
	if (whence == AVSEEK_SIZE)
	{
		return state->input_size;
	}
	int64_t origin = 0;
	switch (whence)
	{
		case SEEK_SET: break;
		case SEEK_CUR: origin = state->input_position; break;
		case SEEK_END: origin = state->input_size; break;
		default: return AVERROR(EINVAL);
	}
	if (offset < -origin || offset > state->input_size - origin)
	{
		return AVERROR(EINVAL);
	}
	state->input_position = origin + offset;
	return state->input_position;
}

static bool OpenInputSource(Decoder::State* state, const char* host_path)
{
	if (state->source == nullptr)
	{
		if (host_path == nullptr || host_path[0] == '\0')
		{
			SetError(state, Status::InvalidArgument, "empty media path");
			return false;
		}
		const int result = avformat_open_input(&state->format, host_path, nullptr, nullptr);
		if (result < 0) { SetFfmpegError(state, Status::OpenFailed, result, "open media file"); }
		return result >= 0;
	}
	const uint64_t size = state->source->Size();
	if (size == 0 || size > static_cast<uint64_t>(INT64_MAX))
	{
		SetError(state, Status::InvalidArgument, "invalid media source size");
		return false;
	}
	state->input_size = static_cast<int64_t>(size);
	state->format = avformat_alloc_context();
	constexpr int kInputBufferSize = 64 * 1024;
	auto* bytes = static_cast<uint8_t*>(av_malloc(kInputBufferSize));
	if (state->format == nullptr || bytes == nullptr)
	{
		av_free(bytes);
		SetError(state, Status::OpenFailed, "allocate media source buffer");
		return false;
	}
	state->input_io = avio_alloc_context(bytes, kInputBufferSize, 0, state, ReadInputSource, nullptr, SeekInputSource);
	if (state->input_io == nullptr)
	{
		av_free(bytes);
		SetError(state, Status::OpenFailed, "allocate media source context");
		return false;
	}
	state->format->pb = state->input_io;
	state->format->flags |= AVFMT_FLAG_CUSTOM_IO;
	const int result = avformat_open_input(&state->format, nullptr, nullptr, nullptr);
	if (result < 0) { SetFfmpegError(state, Status::OpenFailed, result, "open media source"); }
	return result >= 0;
}
#endif

std::unique_ptr<Decoder> Decoder::Open(const char* host_path, std::string* error)
{
	return OpenInput(host_path, nullptr, error);
}

std::unique_ptr<Decoder> Decoder::OpenSource(std::unique_ptr<InputSource> source, std::string* error)
{
	return OpenInput(nullptr, std::move(source), error);
}

std::unique_ptr<Decoder> Decoder::OpenInput(const char* host_path, std::unique_ptr<InputSource> source, std::string* error)
{
	auto decoder = std::unique_ptr<Decoder>(new Decoder);
	decoder->state_->source = std::move(source);
#if !defined(KYTY_HAVE_FFMPEG)
	SetError(decoder->state_.get(), Status::Unavailable, "FFmpeg backend is not available");
#else
	const bool opened = OpenInputSource(decoder->state_.get(), host_path);
	if (opened && avformat_find_stream_info(decoder->state_->format, nullptr) < 0)
	{
		SetError(decoder->state_.get(), Status::OpenFailed, "read media stream information");
	}
	else if (opened)
	{
		auto* state = decoder->state_.get();
		const AVCodec* video_codec = nullptr;
		const AVCodec* audio_codec = nullptr;
		state->video_stream = av_find_best_stream(state->format, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec, 0);
		state->audio_stream = av_find_best_stream(state->format, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
		if (state->video_stream < 0 && state->audio_stream < 0)
		{
			SetError(state, Status::OpenFailed, "media has no audio or video stream");
		}
		else
		{
			if (state->video_stream >= 0 && !OpenCodec(state->format, state->video_stream, &state->video, &video_codec, state, true))
			{
				state->video_stream = -1;
			}
			if (state->audio_stream >= 0 && !OpenCodec(state->format, state->audio_stream, &state->audio, &audio_codec, state, false))
			{
				state->audio_stream = -1;
			}
			state->video_eof = state->video == nullptr;
			state->audio_eof = state->audio == nullptr;
			if (state->video == nullptr && state->audio == nullptr)
			{
				if (state->error.empty())
				{
					SetError(state, Status::OpenFailed, "unable to open media codecs");
				}
			}
			else if (state->audio != nullptr && !ConfigureAudio(state))
			{
				state->audio_stream = -1;
				state->audio_eof    = true;
			}
			else
			{
				state->packet      = av_packet_alloc();
				state->video_frame = state->video != nullptr ? av_frame_alloc() : nullptr;
				state->transferred_video_frame = state->video != nullptr && state->hardware_device != nullptr ? av_frame_alloc() : nullptr;
				state->audio_frame = state->audio != nullptr ? av_frame_alloc() : nullptr;
				if (state->packet == nullptr || (state->video != nullptr && state->video_frame == nullptr) ||
				    (state->hardware_device != nullptr && state->transferred_video_frame == nullptr) ||
				    (state->audio != nullptr && state->audio_frame == nullptr))
				{
					SetError(state, Status::OpenFailed, "allocate media decode buffers");
				}
				else
				{
					if (state->video != nullptr)
					{
						state->info.has_video    = true;
						state->info.video_width  = static_cast<uint32_t>(std::max(state->video->width, 0));
						state->info.video_height = static_cast<uint32_t>(std::max(state->video->height, 0));
						state->info.video_pitch  = state->info.video_width;
						AVRational rate = state->format->streams[state->video_stream]->avg_frame_rate;
						if (rate.num == 0 || rate.den == 0)
						{
							rate = state->format->streams[state->video_stream]->r_frame_rate;
						}
						state->info.video_frame_rate = rate.num != 0 && rate.den != 0 ? av_q2d(rate) : 60.0;
					}
					if (state->audio != nullptr)
					{
						state->info.has_audio        = true;
						state->info.audio_channels   = kOutputChannels;
						state->info.audio_sample_rate = kOutputSampleRate;
					}
					if (state->format->duration != AV_NOPTS_VALUE && state->format->duration > 0)
					{
						state->info.duration_ms = static_cast<uint64_t>(state->format->duration * 1000 / AV_TIME_BASE);
					}
					state->status = Status::Ok;
					state->worker = std::thread(DecodeLoop, state);
				}
			}
		}
	}
#endif
	if (error != nullptr)
	{
		*error = decoder->state_->error;
	}
	if (decoder->state_->status != Status::Ok)
	{
		return nullptr;
	}
	return decoder;
}

const StreamInfo& Decoder::GetStreamInfo() const
{
	return state_->info;
}

Status Decoder::LastStatus() const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->status;
}

const char* Decoder::LastError() const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->error.c_str();
}

bool Decoder::EndOfStream() const
{
#if defined(KYTY_HAVE_FFMPEG)
	std::lock_guard<std::mutex> lock(state_->mutex);
	if (state_->video != nullptr)
	{
		return state_->decode_done && state_->video_queue.empty();
	}
	if (state_->audio != nullptr)
	{
		return state_->decode_done && state_->audio_queue.empty();
	}
	return true;
#else
	return true;
#endif
}

bool Decoder::ReadVideoFrame(VideoFrame* frame)
{
	if (frame == nullptr)
	{
		return false;
	}
	frame->data.clear();
#if !defined(KYTY_HAVE_FFMPEG)
	return false;
#else
	std::unique_lock<std::mutex> lock(state_->mutex);
	if (state_->video == nullptr)
	{
		return false;
	}
	state_->condition.wait(lock, [this] {
		return !state_->video_queue.empty() || state_->decode_done || state_->stop_requested || state_->status != Status::Ok;
	});
	if (state_->video_queue.empty())
	{
		return false;
	}
	*frame = std::move(state_->video_queue.front());
	state_->video_queue.pop_front();
	lock.unlock();
	state_->condition.notify_all();
	return true;
#endif
}

bool Decoder::ReadAudioFrame(AudioFrame* frame)
{
	if (frame == nullptr)
	{
		return false;
	}
	frame->data.clear();
#if !defined(KYTY_HAVE_FFMPEG)
	return false;
#else
	std::unique_lock<std::mutex> lock(state_->mutex);
	if (state_->audio == nullptr)
	{
		return false;
	}
	state_->condition.wait(lock, [this] {
		return !state_->audio_queue.empty() || state_->decode_done || state_->stop_requested || state_->status != Status::Ok;
	});
	if (state_->audio_queue.empty())
	{
		return false;
	}
	*frame = std::move(state_->audio_queue.front());
	state_->audio_queue.pop_front();
	lock.unlock();
	state_->condition.notify_all();
	return true;
#endif
}

bool Decoder::TryReadVideoFrame(VideoFrame* frame)
{
	if (frame == nullptr)
	{
		return false;
	}
	frame->data.clear();
#if !defined(KYTY_HAVE_FFMPEG)
	return false;
#else
	std::lock_guard<std::mutex> lock(state_->mutex);
	if (state_->video == nullptr || state_->video_queue.empty())
	{
		return false;
	}
	*frame = std::move(state_->video_queue.front());
	state_->video_queue.pop_front();
	state_->condition.notify_all();
	return true;
#endif
}

bool Decoder::TryReadAudioFrame(AudioFrame* frame)
{
	if (frame == nullptr)
	{
		return false;
	}
	frame->data.clear();
#if !defined(KYTY_HAVE_FFMPEG)
	return false;
#else
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		if (state_->audio == nullptr || state_->audio_queue.empty())
		{
			return false;
		}
		*frame = std::move(state_->audio_queue.front());
		state_->audio_queue.pop_front();
	}
	state_->condition.notify_all();
	return true;
#endif
}

bool Decoder::Seek(uint64_t time_ms)
{
#if !defined(KYTY_HAVE_FFMPEG)
	(void)time_ms;
	return false;
#else
	State* state = state_.get();
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->status != Status::Ok)
		{
			return false;
		}
		state->stop_requested = true;
		state->decode_done    = false;
	}
	state->condition.notify_all();
	if (state->worker.joinable())
	{
		// The decode worker runs guest read callbacks, so waiting for it is a safe point.
		Kernel::PthreadInterruptibleScope any_syscall;
		state->worker.join();
	}
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->video_queue.clear();
		state->audio_queue.clear();
		if (state->status != Status::Ok)
		{
			return false;
		}
	}
	if (!RepositionInput(state, time_ms))
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->stop_requested = false;
	}
	state->worker = std::thread(DecodeLoop, state);
	return true;
#endif
}

void Decoder::Close()
{
#if defined(KYTY_HAVE_FFMPEG)
	FreeState(state_.get());
#endif
	if (state_ != nullptr)
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		state_->status = Status::Closed;
	}
}

struct ElementaryVideoDecoder::State
{
	std::string error;
	int32_t     max_frame_width  = 0;
	int32_t     max_frame_height = 0;
#if defined(KYTY_HAVE_FFMPEG)
	AVCodecContext* context  = nullptr;
	AVPacket*       packet   = nullptr;
	AVFrame*        frame    = nullptr;
	SwsContext*     scaler   = nullptr;
	bool            drained  = false;
	std::deque<std::pair<VideoFrame, int64_t>> pictures;
#endif
};

ElementaryVideoDecoder::ElementaryVideoDecoder(): state_(std::make_unique<State>()) {}

ElementaryVideoDecoder::~ElementaryVideoDecoder()
{
#if defined(KYTY_HAVE_FFMPEG)
	sws_freeContext(state_->scaler);
	av_frame_free(&state_->frame);
	av_packet_free(&state_->packet);
	avcodec_free_context(&state_->context);
#endif
}

#if defined(KYTY_HAVE_FFMPEG)
// Moves every picture the codec has finished into the queue.
static bool CollectPictures(ElementaryVideoDecoder::State* state)
{
	for (;;)
	{
		const int result = avcodec_receive_frame(state->context, state->frame);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
		{
			return true;
		}
		if (result < 0)
		{
			state->error = "receive decoded picture";
			return false;
		}
		if (state->frame->width <= 0 || state->frame->height <= 0 || state->frame->width > state->max_frame_width ||
		    state->frame->height > state->max_frame_height)
		{
			state->error = "decoded picture exceeds configured maximum dimensions";
			av_frame_unref(state->frame);
			return false;
		}
		VideoFrame  picture;
		const char* copy_error = nullptr;
		const bool  copied     = CopyNv12Picture(state->frame, static_cast<uint32_t>(state->frame->width),
		                                         static_cast<uint32_t>(state->frame->height), &state->scaler, &picture, &copy_error);
		const int64_t tag = state->frame->pts;
		av_frame_unref(state->frame);
		if (!copied)
		{
			state->error = copy_error;
			return false;
		}
		state->pictures.emplace_back(std::move(picture), tag);
	}
}
#endif

std::unique_ptr<ElementaryVideoDecoder> ElementaryVideoDecoder::Open(VideoCodec codec, int32_t max_frame_width,
	                                                                 int32_t max_frame_height, std::string* error)
{
#if !defined(KYTY_HAVE_FFMPEG)
	(void)codec;
	(void)max_frame_width;
	(void)max_frame_height;
	if (error != nullptr)
	{
		*error = "FFmpeg is not available";
	}
	return nullptr;
#else
	if (max_frame_width <= 0 || max_frame_height <= 0)
	{
		if (error != nullptr)
		{
			*error = "invalid maximum video dimensions";
		}
		return nullptr;
	}
	std::unique_ptr<ElementaryVideoDecoder> decoder(new ElementaryVideoDecoder());
	auto*                                   state = decoder->state_.get();
	state->max_frame_width                   = max_frame_width;
	state->max_frame_height                  = max_frame_height;
	const AVCodec* av_codec = avcodec_find_decoder(codec == VideoCodec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	state->context          = av_codec != nullptr ? avcodec_alloc_context3(av_codec) : nullptr;
	state->packet           = av_packet_alloc();
	state->frame            = av_frame_alloc();
	if (state->context == nullptr || state->packet == nullptr || state->frame == nullptr)
	{
		if (error != nullptr)
		{
			*error = "allocate video decoder";
		}
		return nullptr;
	}
	// Slice threads add no picture latency beyond the stream's own reordering.
	state->context->thread_type  = FF_THREAD_SLICE;
	state->context->thread_count = 0;
	if (avcodec_open2(state->context, av_codec, nullptr) < 0)
	{
		if (error != nullptr)
		{
			*error = "open video decoder";
		}
		return nullptr;
	}
	return decoder;
#endif
}

bool ElementaryVideoDecoder::Send(const uint8_t* data, size_t size, int64_t tag)
{
#if !defined(KYTY_HAVE_FFMPEG)
	(void)data;
	(void)size;
	(void)tag;
	return false;
#else
	auto* state = state_.get();
	if (data == nullptr || size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max()))
	{
		state->error = "invalid access unit";
		return false;
	}
	if (state->drained)
	{
		avcodec_flush_buffers(state->context);
		state->drained = false;
	}
	av_packet_unref(state->packet);
	if (av_new_packet(state->packet, static_cast<int>(size)) < 0)
	{
		state->error = "allocate access unit";
		return false;
	}
	std::memcpy(state->packet->data, data, size);
	state->packet->pts = tag;
	for (;;)
	{
		const int result = avcodec_send_packet(state->context, state->packet);
		if (result == AVERROR(EAGAIN))
		{
			if (!CollectPictures(state))
			{
				return false;
			}
			continue;
		}
		if (result < 0)
		{
			state->error = "decode access unit";
			return false;
		}
		return CollectPictures(state);
	}
#endif
}

bool ElementaryVideoDecoder::Drain()
{
#if !defined(KYTY_HAVE_FFMPEG)
	return false;
#else
	auto* state = state_.get();
	bool  sent  = true;
	if (!state->drained)
	{
		sent           = avcodec_send_packet(state->context, nullptr) >= 0;
		state->drained = sent;
		if (!sent)
		{
			state->error = "drain video decoder";
		}
	}
	// Pictures the codec already finished are kept even when the drain send failed.
	return CollectPictures(state) && sent;
#endif
}

bool ElementaryVideoDecoder::Receive(VideoFrame* frame, int64_t* tag)
{
#if !defined(KYTY_HAVE_FFMPEG)
	(void)frame;
	(void)tag;
	return false;
#else
	auto* state = state_.get();
	if (frame == nullptr || tag == nullptr || state->pictures.empty())
	{
		return false;
	}
	*frame = std::move(state->pictures.front().first);
	*tag   = state->pictures.front().second;
	state->pictures.pop_front();
	return true;
#endif
}

void ElementaryVideoDecoder::Reset()
{
#if defined(KYTY_HAVE_FFMPEG)
	avcodec_flush_buffers(state_->context);
	state_->drained = false;
	state_->pictures.clear();
#endif
}

const char* ElementaryVideoDecoder::LastError() const
{
	return state_->error.c_str();
}

struct ElementaryAudioDecoder::State
{
	std::string error;
	bool        is_opus                 = false;
	uint32_t    channels                = 0;
	uint32_t    sample_rate             = 0;
	uint32_t    block_align             = 0;
	uint32_t    max_samples_per_channel = 0;
	// ATRAC9 decodes through its own decoder; packet keeps zero padding after one superframe.
	void*                atrac9 = nullptr;
	uint8_t              atrac9_config[ATRAC9_CONFIG_DATA_SIZE] {};
	uint32_t             atrac9_frames        = 0;
	uint32_t             atrac9_frame_samples = 0;
	std::vector<uint8_t> atrac9_packet;
	std::vector<int16_t> atrac9_pcm;
#if defined(KYTY_HAVE_FFMPEG)
	AVCodecContext* context = nullptr;
	AVPacket*       packet  = nullptr;
	AVFrame*        frame   = nullptr;
#endif
};

namespace {

bool Atrac9SuperframeBlockAlign(const uint8_t* config, uint32_t* block_align)
{
	const uint32_t superframe_index = (config[3] >> 3u) & 3u;
	if ((superframe_index & 1u) != 0 || superframe_index > 2u)
	{
		return false;
	}

	const uint32_t frame_bytes = ((static_cast<uint32_t>(config[2]) << 3u) | (config[3] >> 5u)) + 1u;
	const uint32_t frame_count = 1u << superframe_index;
	if (frame_bytes > ElementaryAudioDecoder::kAtrac9MaxSuperframeBytes / frame_count)
	{
		return false;
	}
	*block_align = frame_bytes * frame_count;
	return true;
}

// The supported config subset uses zero in the final three low bits; their
// behavior for nonzero values is not established here. The decoder remains
// authoritative for channels and sample rate.
const char* Atrac9BlockAlignFromConfig(const uint8_t* config, size_t config_size, uint32_t* block_align)
{
	if (config == nullptr || config_size != 4)
	{
		return "ATRAC9 config must be exactly 4 bytes";
	}
	if (config[0] != 0xfeu)
	{
		return "ATRAC9 config has an invalid sync byte";
	}
	if ((config[1] & 1u) != 0)
	{
		return "ATRAC9 config verification bit must be clear";
	}
	if (((config[1] >> 1u) & 7u) > 5u)
	{
		return "ATRAC9 config has an invalid block configuration";
	}
	if ((config[3] & 7u) != 0)
	{
		return "ATRAC9 config uses unsupported nonzero low bits";
	}

	if (!Atrac9SuperframeBlockAlign(config, block_align))
	{
		return "ATRAC9 superframe index must describe 1 or 4 frames";
	}
	return nullptr;
}

// Each decoder init rewrites tables that every ATRAC9 handle reads while decoding.
std::shared_mutex g_atrac9_tables;

// The bit reader has no end: a malformed frame may read past its superframe into this zeroed tail.
constexpr size_t kAtrac9PacketPadding = ElementaryAudioDecoder::kAtrac9MaxSuperframeBytes;

void* OpenAtrac9Handle(const uint8_t* config, Atrac9CodecInfo* info)
{
	void* handle = Atrac9GetHandle();
	if (handle == nullptr)
	{
		return nullptr;
	}
	unsigned char config_copy[ATRAC9_CONFIG_DATA_SIZE];
	std::memcpy(config_copy, config, sizeof(config_copy));
	int result = 0;
	{
		std::unique_lock lock(g_atrac9_tables);
		result = Atrac9InitDecoder(handle, config_copy);
	}
	if (result != 0 || Atrac9GetCodecInfo(handle, info) != 0)
	{
		Atrac9ReleaseHandle(handle);
		return nullptr;
	}
	return handle;
}

// A failed frame leaves the stream between frames, so the next packet starts a new stream.
void RestartAtrac9(ElementaryAudioDecoder::State* state)
{
	if (state->atrac9 != nullptr)
	{
		Atrac9ReleaseHandle(state->atrac9);
	}
	Atrac9CodecInfo info {};
	state->atrac9 = OpenAtrac9Handle(state->atrac9_config, &info);
}

} // namespace

ElementaryAudioDecoder::ElementaryAudioDecoder(): state_(std::make_unique<State>()) {}

ElementaryAudioDecoder::~ElementaryAudioDecoder()
{
	if (state_->atrac9 != nullptr)
	{
		Atrac9ReleaseHandle(state_->atrac9);
	}
#if defined(KYTY_HAVE_FFMPEG)
	av_frame_free(&state_->frame);
	av_packet_free(&state_->packet);
	avcodec_free_context(&state_->context);
#endif
}

std::unique_ptr<ElementaryAudioDecoder> ElementaryAudioDecoder::Open(AudioCodec codec, const uint8_t* extradata, size_t extradata_size,
                                                                     uint32_t block_align, uint32_t channels, uint32_t sample_rate,
                                                                     std::string* error)
{
	if (codec != AudioCodec::Atrac9)
	{
		if (error != nullptr)
		{
			*error = "unsupported audio codec";
		}
		return nullptr;
	}
	const ExpectedFormat expected_format {channels, sample_rate};
	return OpenAtrac9(extradata, extradata_size, block_align, &expected_format, error);
}

std::unique_ptr<ElementaryAudioDecoder> ElementaryAudioDecoder::OpenOpus(uint32_t channels, uint32_t sample_rate, std::string* error)
{
#if !defined(KYTY_HAVE_FFMPEG)
	(void)channels;
	(void)sample_rate;
	if (error != nullptr)
	{
		*error = "FFmpeg is not available";
	}
	return nullptr;
#else
	auto fail = [error](const char* message) -> std::unique_ptr<ElementaryAudioDecoder>
	{
		if (error != nullptr)
		{
			*error = message;
		}
		return nullptr;
	};
	if ((channels != 1 && channels != 2) || sample_rate != 48000)
	{
		return fail("Opus requires mono or stereo input at 48000 Hz");
	}
	const AVCodec* av_codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (av_codec == nullptr)
	{
		return fail("Opus decoder is unavailable");
	}

	std::unique_ptr<ElementaryAudioDecoder> decoder(new ElementaryAudioDecoder());
	auto*                                   state = decoder->state_.get();
	state->context                                = avcodec_alloc_context3(av_codec);
	state->packet                                 = av_packet_alloc();
	state->frame                                  = av_frame_alloc();
	if (state->context == nullptr || state->packet == nullptr || state->frame == nullptr)
	{
		return fail("allocate Opus decoder");
	}
	state->context->sample_rate = static_cast<int>(sample_rate);
	av_channel_layout_default(&state->context->ch_layout, static_cast<int>(channels));
	if (avcodec_open2(state->context, av_codec, nullptr) < 0)
	{
		return fail("open Opus decoder");
	}
	const int actual_channels    = state->context->ch_layout.nb_channels;
	const int actual_sample_rate = state->context->sample_rate;
	if (actual_channels != static_cast<int>(channels) || actual_sample_rate != static_cast<int>(sample_rate))
	{
		return fail("Opus decoder format does not match the requested format");
	}
	if (state->context->sample_fmt != AV_SAMPLE_FMT_FLTP)
	{
		return fail("Opus decoder output is not planar float");
	}
	state->is_opus                 = true;
	state->channels                = static_cast<uint32_t>(actual_channels);
	state->sample_rate             = static_cast<uint32_t>(actual_sample_rate);
	state->max_samples_per_channel = kOpusMaxPacketSamples;
	return decoder;
#endif
}

std::unique_ptr<ElementaryAudioDecoder> ElementaryAudioDecoder::OpenAtrac9FromConfig(const uint8_t* config, size_t config_size,
                                                                                     std::string* error)
{
	uint32_t block_align = 0;
	if (const char* config_error = Atrac9BlockAlignFromConfig(config, config_size, &block_align); config_error != nullptr)
	{
		if (error != nullptr)
		{
			*error = config_error;
		}
		return nullptr;
	}

	// Wrap the config in the RIFF WAVEFORMATEXTENSIBLE tail. Version and reserved
	// words are zero for the standard ATRAC9 setup.
	uint8_t extradata[kAtrac9ExtradataSize] = {};
	std::memcpy(extradata + 4, config, config_size);
	return OpenAtrac9(extradata, sizeof(extradata), block_align, nullptr, error);
}

std::unique_ptr<ElementaryAudioDecoder> ElementaryAudioDecoder::OpenAtrac9(const uint8_t* extradata, size_t extradata_size,
                                                                          uint32_t block_align, const ExpectedFormat* expected_format,
                                                                          std::string* error)
{
	auto fail = [error](const char* message) -> std::unique_ptr<ElementaryAudioDecoder>
	{
		if (error != nullptr)
		{
			*error = message;
		}
		return nullptr;
	};
	if (extradata == nullptr || extradata_size != kAtrac9ExtradataSize)
	{
		return fail("ATRAC9 extradata must be 12 bytes");
	}
	const uint32_t version = static_cast<uint32_t>(extradata[0]) | (static_cast<uint32_t>(extradata[1]) << 8u) |
	                         (static_cast<uint32_t>(extradata[2]) << 16u) | (static_cast<uint32_t>(extradata[3]) << 24u);
	if (version > 2)
	{
		return fail("ATRAC9 extradata version must be at most 2");
	}
	if (block_align == 0 || block_align > kAtrac9MaxSuperframeBytes)
	{
		return fail("ATRAC9 block align must be 1 to 8192 bytes");
	}
	if (expected_format != nullptr &&
	    (expected_format->channels == 0 || expected_format->channels > kAtrac9MaxChannels || expected_format->sample_rate == 0 ||
	     expected_format->sample_rate > kAtrac9MaxSampleRate))
	{
		return fail("ATRAC9 channels or sample rate out of range");
	}
	const uint8_t* config = extradata + 4;
	if (config[0] != 0xfeu || (config[1] & 1u) != 0)
	{
		return fail("ATRAC9 config has an invalid sync byte or verification bit");
	}
	if (((config[1] >> 1u) & 7u) > 5u)
	{
		return fail("ATRAC9 config has an invalid block configuration");
	}
	uint32_t config_block_align = 0;
	if (!Atrac9SuperframeBlockAlign(config, &config_block_align))
	{
		return fail("ATRAC9 config has an invalid superframe size");
	}
	if (block_align != config_block_align)
	{
		return fail("ATRAC9 block align does not match one configured superframe");
	}

	std::unique_ptr<ElementaryAudioDecoder> decoder(new ElementaryAudioDecoder());
	auto*                                   state = decoder->state_.get();
	std::memcpy(state->atrac9_config, config, sizeof(state->atrac9_config));
	Atrac9CodecInfo info {};
	state->atrac9 = OpenAtrac9Handle(state->atrac9_config, &info);
	if (state->atrac9 == nullptr)
	{
		return fail("open ATRAC9 decoder: config rejected");
	}
	// The config, not the container, defines the channel layout and rate.
	if (info.channels <= 0 || info.channels > static_cast<int>(kAtrac9MaxChannels) || info.samplingRate <= 0 ||
	    info.samplingRate > static_cast<int>(kAtrac9MaxSampleRate))
	{
		return fail("ATRAC9 config derives an unsupported channel count or sample rate");
	}
	if (info.superframeSize != static_cast<int>(block_align) || info.framesInSuperframe <= 0 || info.frameSamples <= 0 ||
	    static_cast<uint32_t>(info.framesInSuperframe * info.frameSamples) > kAtrac9MaxSuperframeSamples)
	{
		return fail("ATRAC9 config derives an unsupported superframe");
	}
	if (expected_format != nullptr &&
	    (info.channels != static_cast<int>(expected_format->channels) || info.samplingRate != static_cast<int>(expected_format->sample_rate)))
	{
		return fail("ATRAC9 config does not match the waveform format");
	}
	state->channels                = static_cast<uint32_t>(info.channels);
	state->sample_rate             = static_cast<uint32_t>(info.samplingRate);
	state->block_align             = block_align;
	state->max_samples_per_channel = kAtrac9MaxSuperframeSamples;
	state->atrac9_frames           = static_cast<uint32_t>(info.framesInSuperframe);
	state->atrac9_frame_samples    = static_cast<uint32_t>(info.frameSamples);
	state->atrac9_packet.assign(block_align + kAtrac9PacketPadding, 0);
	state->atrac9_pcm.resize(static_cast<size_t>(state->atrac9_frame_samples) * state->channels);
	return decoder;
}

// Decodes the frames of one superframe into interleaved float PCM.
static bool DecodeAtrac9Superframe(ElementaryAudioDecoder::State* state, const uint8_t* packet, size_t size, std::vector<float>* interleaved)
{
	if (state->atrac9 == nullptr)
	{
		RestartAtrac9(state);
		if (state->atrac9 == nullptr)
		{
			state->error = "open ATRAC9 decoder: config rejected";
			return false;
		}
	}
	std::memcpy(state->atrac9_packet.data(), packet, size);
	interleaved->reserve(static_cast<size_t>(state->atrac9_frames) * state->atrac9_pcm.size());
	size_t consumed = 0;
	bool   decoded  = true;
	{
		std::shared_lock lock(g_atrac9_tables);
		for (uint32_t frame = 0; frame < state->atrac9_frames && decoded; frame++)
		{
			int used = 0;
			decoded  = Atrac9Decode(state->atrac9, state->atrac9_packet.data() + consumed, state->atrac9_pcm.data(), &used) == 0 &&
			          used > 0 && static_cast<size_t>(used) <= size - consumed;
			if (decoded)
			{
				consumed += static_cast<size_t>(used);
				for (const int16_t sample: state->atrac9_pcm)
				{
					interleaved->push_back(static_cast<float>(sample) / 32768.0f);
				}
			}
		}
	}
	if (!decoded)
	{
		interleaved->clear();
		RestartAtrac9(state);
		state->error = "decode ATRAC9 packet";
		return false;
	}
	return true;
}

#if defined(KYTY_HAVE_FFMPEG)
// Appends one planar float frame within the codec's per-packet sample limit.
static bool AppendInterleavedFrame(ElementaryAudioDecoder::State* state, std::vector<float>* interleaved)
{
	const AVFrame* frame = state->frame;
	if (frame->format != AV_SAMPLE_FMT_FLTP || frame->ch_layout.nb_channels != static_cast<int>(state->channels) ||
	    frame->nb_samples <= 0 || static_cast<uint32_t>(frame->nb_samples) > state->max_samples_per_channel)
	{
		state->error = "Opus frame layout does not match the decoder";
		return false;
	}
	const auto channels           = static_cast<int>(state->channels);
	const auto limit              = static_cast<size_t>(state->max_samples_per_channel) * state->channels;
	const auto frame_sample_count = static_cast<size_t>(frame->nb_samples) * state->channels;
	if (interleaved->size() > limit || frame_sample_count > limit - interleaved->size())
	{
		state->error = "Opus packet produced more than 120 ms of samples";
		return false;
	}
	for (int channel = 0; channel < channels; channel++)
	{
		if (frame->data[channel] == nullptr)
		{
			state->error = "Opus frame is missing a channel plane";
			return false;
		}
	}
	for (int sample = 0; sample < frame->nb_samples; sample++)
	{
		for (int channel = 0; channel < channels; channel++)
		{
			interleaved->push_back(reinterpret_cast<const float*>(frame->data[channel])[sample]);
		}
	}
	return true;
}

static bool CollectAvailableAudioFrames(ElementaryAudioDecoder::State* state, std::vector<float>* interleaved)
{
	for (;;)
	{
		const int result = avcodec_receive_frame(state->context, state->frame);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
		{
			return true;
		}
		if (result < 0)
		{
			state->error = "receive Opus frame";
			return false;
		}
		const bool appended = AppendInterleavedFrame(state, interleaved);
		av_frame_unref(state->frame);
		if (!appended)
		{
			return false;
		}
	}
}
#endif

bool ElementaryAudioDecoder::Decode(const uint8_t* packet, size_t size, std::vector<float>* interleaved)
{
	auto* state = state_.get();
	if (packet == nullptr || interleaved == nullptr)
	{
		state->error = "audio packet or output is null";
		return false;
	}
	if ((!state->is_opus && size != state->block_align) || (state->is_opus && (size == 0 || size > kOpusMaxPacketBytes)))
	{
		state->error = state->is_opus ? "Opus packet must be 1 to 65535 bytes" : "ATRAC9 packet must be exactly one superframe";
		return false;
	}
	interleaved->clear();
	if (!state->is_opus)
	{
		return DecodeAtrac9Superframe(state, packet, size, interleaved);
	}
#if !defined(KYTY_HAVE_FFMPEG)
	state->error = "FFmpeg is not available";
	return false;
#else
	av_packet_unref(state->packet);
	if (av_new_packet(state->packet, static_cast<int>(size)) < 0)
	{
		state->error = "allocate Opus packet";
		return false;
	}
	std::memcpy(state->packet->data, packet, size);
	int send_result = avcodec_send_packet(state->context, state->packet);
	if (send_result == AVERROR(EAGAIN))
	{
		// An Opus decoder can retain output until the caller receives it. Drain
		// that output and retry without flushing the packet history.
		if (!CollectAvailableAudioFrames(state, interleaved))
		{
			return false;
		}
		send_result = avcodec_send_packet(state->context, state->packet);
	}
	if (send_result < 0)
	{
		state->error = "send Opus packet to decoder";
		return false;
	}
	return CollectAvailableAudioFrames(state, interleaved);
#endif
}

void ElementaryAudioDecoder::Reset()
{
	if (!state_->is_opus)
	{
		RestartAtrac9(state_.get());
		return;
	}
#if defined(KYTY_HAVE_FFMPEG)
	avcodec_flush_buffers(state_->context);
#endif
}

const char* ElementaryAudioDecoder::LastError() const
{
	return state_->error.c_str();
}

uint32_t ElementaryAudioDecoder::Channels() const
{
	return state_->channels;
}

uint32_t ElementaryAudioDecoder::SampleRate() const
{
	return state_->sample_rate;
}

uint32_t ElementaryAudioDecoder::BlockAlign() const
{
	return state_->block_align;
}

} // namespace Kyty::Emulator::AudioVideoBackend
