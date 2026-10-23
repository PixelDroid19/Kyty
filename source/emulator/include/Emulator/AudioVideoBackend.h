#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_VIDEO_BACKEND_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_VIDEO_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Kyty::Emulator::AudioVideoBackend {

enum class Status : uint8_t
{
	Ok = 0,
	InvalidArgument,
	Unavailable,
	OpenFailed,
	DecodeFailed,
	EndOfStream,
	Closed,
};

struct VideoFrame
{
	std::vector<uint8_t> data;
	uint32_t             width       = 0;
	uint32_t             height      = 0;
	uint32_t             pitch       = 0;
	uint64_t             timestamp_ms = 0;
};

struct AudioFrame
{
	std::vector<int16_t> data;
	uint32_t             channels    = 0;
	uint32_t             sample_rate = 0;
	uint64_t             timestamp_ms = 0;
};

struct StreamInfo
{
	bool     has_video   = false;
	bool     has_audio   = false;
	uint32_t video_width = 0;
	uint32_t video_height = 0;
	uint32_t video_pitch = 0;
	double   video_frame_rate = 0.0;
	uint32_t audio_channels = 0;
	uint32_t audio_sample_rate = 0;
	uint64_t duration_ms = 0;
};

// ReadAt returns the number of bytes read, zero at EOF, or a negative error.
// The decoder owns the source until its worker has stopped. Reads are serial
// and may run on the decoding thread after Open returns.
class InputSource
{
public:
	virtual ~InputSource() = default;
	[[nodiscard]] virtual uint64_t Size() const = 0;
	virtual int ReadAt(uint64_t offset, uint8_t* destination, uint32_t size) = 0;
};

// Owns one demux/decode session. Reads return copied frames, so callers may
// retain a frame until the next read or until the session is closed.
class Decoder final
{
public:
	struct State;

	static bool IsAvailable();
	static const char* BackendName();
	static std::unique_ptr<Decoder> Open(const char* host_path, std::string* error = nullptr);
	static std::unique_ptr<Decoder> OpenSource(std::unique_ptr<InputSource> source, std::string* error = nullptr);

	~Decoder();

	Decoder(const Decoder&) = delete;
	Decoder& operator=(const Decoder&) = delete;

	const StreamInfo& GetStreamInfo() const;
	Status             LastStatus() const;
	const char*        LastError() const;
	bool               EndOfStream() const;

	bool ReadVideoFrame(VideoFrame* frame);
	bool ReadAudioFrame(AudioFrame* frame);
	// Non-blocking pulls used by the guest-facing AvPlayer ABI. A false result
	// means that no decoded frame is ready yet; it is not an end-of-stream
	// indication. EndOfStream() remains the authoritative completion query.
	bool TryReadVideoFrame(VideoFrame* frame);
	bool TryReadAudioFrame(AudioFrame* frame);
	// Repositions decoding at time_ms: queued frames are discarded and the next
	// frames start at the requested position. EndOfStream() stays false while
	// the decoder is repositioned. A failed seek ends the stream.
	bool Seek(uint64_t time_ms);
	void Close();

private:
	Decoder();
	static std::unique_ptr<Decoder> OpenInput(const char* host_path, std::unique_ptr<InputSource> source, std::string* error);

	std::unique_ptr<State> state_;
};

enum class VideoCodec : uint8_t
{
	Avc,
	Hevc,
};

// Decodes a video elementary stream one access unit (Annex B) at a time and
// returns its pictures in display order as tightly packed NV12. The value
// sent with an access unit comes back with the picture it produced.
class ElementaryVideoDecoder final
{
public:
	struct State;

	static std::unique_ptr<ElementaryVideoDecoder> Open(VideoCodec codec, int32_t max_frame_width, int32_t max_frame_height,
	                                                   std::string* error = nullptr);

	~ElementaryVideoDecoder();

	ElementaryVideoDecoder(const ElementaryVideoDecoder&)            = delete;
	ElementaryVideoDecoder& operator=(const ElementaryVideoDecoder&) = delete;

	bool Send(const uint8_t* data, size_t size, int64_t tag);
	// Ends the stream: the pictures the decoder still holds become available.
	bool Drain();
	// The next picture and its tag, or false when none is ready.
	bool Receive(VideoFrame* frame, int64_t* tag);
	// Drops every held picture and starts a new stream.
	void Reset();
	[[nodiscard]] const char* LastError() const;

private:
	ElementaryVideoDecoder();

	std::unique_ptr<State> state_;
};

enum class AudioCodec : uint8_t
{
	Atrac9,
};

// Decodes ATRAC9 superframes or individual Opus packets into interleaved float
// PCM at the codec's source rate. ATRAC9 extradata is the 12-byte tail of the
// RIFF WAVEFORMATEXTENSIBLE format: a version dword (at most 2), the FE-prefixed
// config dword, and a reserved dword. Open requires the decoder's channel count
// and sample rate to match the caller's expectations.
class ElementaryAudioDecoder final
{
public:
	struct State;

	static constexpr size_t kAtrac9ExtradataSize = 12;
	static constexpr size_t kOpusMaxPacketBytes  = 65535;
	// Opus packets may encode up to 120 ms: 5760 samples per channel at 48 kHz.
	static constexpr uint32_t kOpusMaxPacketSamples = 5760;
	// Limits fixed by the ATRAC9 config: up to 8 channels (7.1), 192 kHz, frames
	// of at most 256 samples and 2048 bytes, and 1 or 4 frames per superframe.
	static constexpr uint32_t kAtrac9MaxChannels          = 8;
	static constexpr uint32_t kAtrac9MaxSampleRate        = 192000;
	static constexpr uint32_t kAtrac9MaxSuperframeBytes   = 8192;
	static constexpr uint32_t kAtrac9MaxSuperframeSamples = 1024;

	static std::unique_ptr<ElementaryAudioDecoder> Open(AudioCodec codec, const uint8_t* extradata, size_t extradata_size,
	                                                    uint32_t block_align, uint32_t channels, uint32_t sample_rate,
	                                                    std::string* error = nullptr);
	// Opens the demonstrated four-byte ATRAC9 config subset. Nonzero low config
	// bits with no established contract are refused. The decoder determines the
	// channel count and sample rate; block alignment is derived from the config.
	static std::unique_ptr<ElementaryAudioDecoder> OpenAtrac9FromConfig(const uint8_t* config, size_t config_size,
	                                                                    std::string* error = nullptr);
	// Opens a raw mono or stereo Opus stream at 48 kHz with FFmpeg's standard
	// channel mapping. No extradata or AJM configuration is passed to FFmpeg.
	static std::unique_ptr<ElementaryAudioDecoder> OpenOpus(uint32_t channels, uint32_t sample_rate, std::string* error = nullptr);

	~ElementaryAudioDecoder();

	ElementaryAudioDecoder(const ElementaryAudioDecoder&)            = delete;
	ElementaryAudioDecoder& operator=(const ElementaryAudioDecoder&) = delete;

	// Decodes one ATRAC9 superframe or one Opus packet. Opus packets must contain
	// 1 to kOpusMaxPacketBytes bytes. Interleaved samples replace the contents of
	// interleaved; a packet may produce no samples while the decoder is delayed.
	bool Decode(const uint8_t* packet, size_t size, std::vector<float>* interleaved);
	// Drops the overlap state so that the next packet starts a new stream.
	void                      Reset();
	[[nodiscard]] const char* LastError() const;
	[[nodiscard]] uint32_t    Channels() const;
	[[nodiscard]] uint32_t    SampleRate() const;
	// Returns zero for Opus, whose packets have variable length.
	[[nodiscard]] uint32_t BlockAlign() const;

private:
	struct ExpectedFormat
	{
		uint32_t channels    = 0;
		uint32_t sample_rate = 0;
	};

	ElementaryAudioDecoder();
	static std::unique_ptr<ElementaryAudioDecoder> OpenAtrac9(const uint8_t* extradata, size_t extradata_size,
	                                                        uint32_t block_align, const ExpectedFormat* expected_format,
	                                                        std::string* error);

	std::unique_ptr<State> state_;
};

} // namespace Kyty::Emulator::AudioVideoBackend


#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_VIDEO_BACKEND_H_
