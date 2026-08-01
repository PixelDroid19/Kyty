#include "Emulator/AudioVideoBackend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <utility>

#if defined(KYTY_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace Kyty::Libs::Audio::AudioVideoBackend {

#if defined(KYTY_HAVE_FFMPEG)

namespace {

constexpr uint32_t kOutputSampleRate = 48000;
constexpr uint32_t kOutputChannels   = 2;
constexpr size_t   kMaxQueuedFrames  = 8;

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

struct Decoder::State
{
	Status     status = Status::Unavailable;
	std::string error;
	StreamInfo info;

	AVFormatContext* format = nullptr;
	AVCodecContext*  video  = nullptr;
	AVCodecContext*  audio  = nullptr;
	AVPacket*        packet = nullptr;
	AVFrame*         video_frame = nullptr;
	AVFrame*         audio_frame = nullptr;
	SwsContext*      scaler = nullptr;
	SwrContext*      resampler = nullptr;
	AVChannelLayout  output_layout {};

	int video_stream = -1;
	int audio_stream = -1;
	bool demux_eof   = false;
	bool video_eof   = false;
	bool audio_eof   = false;
	bool flushed     = false;

	uint64_t next_video_timestamp = 0;
	uint64_t next_audio_timestamp = 0;
	std::deque<VideoFrame> video_queue;
	std::deque<AudioFrame> audio_queue;
};

static void SetError(Decoder::State* state, Status status, const char* message)
{
	if (state == nullptr)
	{
		return;
	}
	state->status = status;
	state->error  = message != nullptr ? message : "FFmpeg operation failed";
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
	if (state->format != nullptr)
	{
		avformat_close_input(&state->format);
	}
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
                      Decoder::State* state)
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
	*context = avcodec_alloc_context3(*codec);
	if (*context == nullptr)
	{
		SetError(state, Status::OpenFailed, "allocate codec context");
		return false;
	}
	if (avcodec_parameters_to_context(*context, stream->codecpar) < 0 || avcodec_open2(*context, *codec, nullptr) < 0)
	{
		SetError(state, Status::OpenFailed, "open media codec");
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
	const uint32_t width  = static_cast<uint32_t>(state->info.video_width);
	const uint32_t height = static_cast<uint32_t>(state->info.video_height);
	const size_t chroma_height = (static_cast<size_t>(height) + 1u) / 2u;
	if (width == 0 || height == 0 || static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / height)
	{
		SetError(state, Status::DecodeFailed, "decoded video dimensions overflow");
		return false;
	}
	const size_t luma_bytes = static_cast<size_t>(width) * height;
	if (static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / chroma_height ||
	    luma_bytes > std::numeric_limits<size_t>::max() - static_cast<size_t>(width) * chroma_height)
	{
		SetError(state, Status::DecodeFailed, "decoded video buffer size overflow");
		return false;
	}

	VideoFrame frame;
	frame.width       = width;
	frame.height      = height;
	frame.pitch       = width;
	frame.timestamp_ms = TimestampMilliseconds(source->best_effort_timestamp,
	                                            state->format->streams[state->video_stream]->time_base,
	                                            state->next_video_timestamp);
	frame.data.resize(luma_bytes + static_cast<size_t>(width) * chroma_height);
	state->scaler = sws_getCachedContext(state->scaler,
	                                      source->width,
	                                      source->height,
	                                      static_cast<AVPixelFormat>(source->format),
	                                      static_cast<int>(width),
	                                      static_cast<int>(height),
	                                      AV_PIX_FMT_NV12,
	                                      SWS_BILINEAR,
	                                      nullptr,
	                                      nullptr,
	                                      nullptr);
	if (state->scaler == nullptr)
	{
		SetError(state, Status::DecodeFailed, "initialize video scaler");
		return false;
	}
	uint8_t* destination[4] = {frame.data.data(), frame.data.data() + luma_bytes, nullptr, nullptr};
	int destination_pitch[4] = {static_cast<int>(width), static_cast<int>(width), 0, 0};
	if (sws_scale(state->scaler, source->data, source->linesize, 0, source->height, destination, destination_pitch) <= 0)
	{
		SetError(state, Status::DecodeFailed, "convert video frame to NV12");
		return false;
	}
	state->next_video_timestamp = frame.timestamp_ms +
	                              (state->info.video_frame_rate > 0.0
	                                   ? static_cast<uint64_t>(1000.0 / state->info.video_frame_rate + 0.5)

                                   : 0);
	if (state->video_queue.size() >= kMaxQueuedFrames)
	{
		state->video_queue.pop_front();
	}
	state->video_queue.push_back(std::move(frame));
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
	if (!frame.data.empty())
	{
		if (state->audio_queue.size() >= kMaxQueuedFrames)
		{
			state->audio_queue.pop_front();
		}
		state->audio_queue.push_back(std::move(frame));
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

std::unique_ptr<Decoder> Decoder::Open(const char* host_path, std::string* error)
{
	auto decoder = std::unique_ptr<Decoder>(new Decoder);
#if !defined(KYTY_HAVE_FFMPEG)
	SetError(decoder->state_.get(), Status::Unavailable, "FFmpeg backend is not available");
#else
	if (host_path == nullptr || host_path[0] == '\0')
	{
		SetError(decoder->state_.get(), Status::InvalidArgument, "empty media path");
	}
	else if (avformat_open_input(&decoder->state_->format, host_path, nullptr, nullptr) < 0)
	{
		SetError(decoder->state_.get(), Status::OpenFailed, "open media file");
	}
	else if (avformat_find_stream_info(decoder->state_->format, nullptr) < 0)
	{
		SetError(decoder->state_.get(), Status::OpenFailed, "read media stream information");
	}
	else
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
			if (state->video_stream >= 0 && !OpenCodec(state->format, state->video_stream, &state->video, &video_codec, state))
			{
				state->video_stream = -1;
			}
			if (state->audio_stream >= 0 && !OpenCodec(state->format, state->audio_stream, &state->audio, &audio_codec, state))
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
				state->audio_frame = state->audio != nullptr ? av_frame_alloc() : nullptr;
				if (state->packet == nullptr || (state->video != nullptr && state->video_frame == nullptr) ||
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
	return state_->status;
}

const char* Decoder::LastError() const
{
	return state_->error.c_str();
}

bool Decoder::EndOfStream() const
{
#if defined(KYTY_HAVE_FFMPEG)
	return state_->demux_eof && state_->video_queue.empty() && state_->audio_queue.empty() && state_->video_eof && state_->audio_eof;
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
	if (state_->status != Status::Ok || state_->video == nullptr)
	{
		return false;
	}
	while (state_->video_queue.empty() && !state_->video_eof && state_->status == Status::Ok)
	{
		if (!Pump(state_.get()))
		{
			break;
		}
	}
	if (state_->video_queue.empty())
	{
		if (state_->demux_eof)
		{
			state_->video_eof = true;
		}
		return false;
	}
	*frame = std::move(state_->video_queue.front());
	state_->video_queue.pop_front();
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
	if (state_->status != Status::Ok || state_->audio == nullptr)
	{
		return false;
	}
	while (state_->audio_queue.empty() && !state_->audio_eof && state_->status == Status::Ok)
	{
		if (!Pump(state_.get()))
		{
			break;
		}
	}
	if (state_->audio_queue.empty())
	{
		if (state_->demux_eof)
		{
			state_->audio_eof = true;
		}
		return false;
	}
	*frame = std::move(state_->audio_queue.front());
	state_->audio_queue.pop_front();
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
		state_->status = Status::Closed;
	}
}

} // namespace Kyty::Libs::Audio::AudioVideoBackend
