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

} // namespace Kyty::Emulator::AudioVideoBackend


#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_VIDEO_BACKEND_H_
