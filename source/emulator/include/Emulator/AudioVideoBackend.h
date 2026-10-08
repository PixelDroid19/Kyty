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

	static std::unique_ptr<ElementaryVideoDecoder> Open(VideoCodec codec, std::string* error = nullptr);

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

// Decodes one superframe at a time into interleaved float PCM at the source
// rate. The extradata is the 12-byte tail of the RIFF WAVEFORMATEXTENSIBLE
// format: a version dword (at most 2), the FE-prefixed config dword, and a
// reserved dword. Open fails unless the decoder derives the channel count and
// sample rate that the caller expects from the config.
class ElementaryAudioDecoder final
{
public:
	struct State;

	static constexpr size_t kAtrac9ExtradataSize = 12;
	// Limits fixed by the ATRAC9 config: up to 8 channels (7.1), 192 kHz, frames
	// of at most 256 samples and 2048 bytes, and 1 or 4 frames per superframe.
	static constexpr uint32_t kAtrac9MaxChannels          = 8;
	static constexpr uint32_t kAtrac9MaxSampleRate        = 192000;
	static constexpr uint32_t kAtrac9MaxSuperframeBytes   = 8192;
	static constexpr uint32_t kAtrac9MaxSuperframeSamples = 1024;

	static std::unique_ptr<ElementaryAudioDecoder> Open(AudioCodec codec, const uint8_t* extradata, size_t extradata_size,
	                                                    uint32_t block_align, uint32_t channels, uint32_t sample_rate,
	                                                    std::string* error = nullptr);

	~ElementaryAudioDecoder();

	ElementaryAudioDecoder(const ElementaryAudioDecoder&)            = delete;
	ElementaryAudioDecoder& operator=(const ElementaryAudioDecoder&) = delete;

	// Decodes exactly one packet of block_align bytes. The interleaved samples
	// replace the contents of interleaved; a packet may produce no samples.
	bool Decode(const uint8_t* packet, size_t size, std::vector<float>* interleaved);
	// Drops the overlap state so that the next packet starts a new stream.
	void Reset();
	[[nodiscard]] const char* LastError() const;

private:
	ElementaryAudioDecoder();

	std::unique_ptr<State> state_;
};

} // namespace Kyty::Emulator::AudioVideoBackend


#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_VIDEO_BACKEND_H_
