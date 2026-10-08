#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_NGS2_SAMPLER_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_NGS2_SAMPLER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace Kyty::Libs::Audio::Ngs2Sampler {

constexpr uint32_t kMaxSampleRate   = 192000;
constexpr uint32_t kMaxChannels     = 2;
constexpr uint32_t kMaxQueuedBlocks = 64;
constexpr uint32_t kRepeatForever   = 0xffffffffu;
// Host limits, not guest contracts. A pitch above kMaxPitch is refused, so a
// voice never advances more than kMaxSourceFramesPerOutputFrame source frames per
// output frame at the 48 kHz system rate.
constexpr float  kMaxPitch                      = 16.0f;
constexpr double kMaxSourceFramesPerOutputFrame = 64.0;

// A queued waveform block as finite float frames. frames holds num_skip +
// num_samples interleaved frames; playback starts at frame num_skip.
struct Block
{
	std::vector<float> frames;
	uint32_t           num_skip     = 0;
	uint32_t           num_samples  = 0;
	uint32_t           num_repeats  = 0;
	uint32_t           num_repeated = 0;
	uintptr_t          user_data    = 0;
	uintptr_t          guest_data   = 0;
	uint64_t           guest_size   = 0;
};

// Reported once when a block finishes all its passes. Block repeats are counted
// in num_repeated but never reported on their own: the guest repeat callback
// contract is unconfirmed, and emitting one event per repeat would be unbounded.
struct Event
{
	uint32_t  num_repeated;
	uintptr_t user_data;
	uintptr_t guest_data;
	uint64_t  guest_size;
};

// Block queue, cursor and resampler of one standard sampler voice. Source
// positions advance by sample_rate * pitch / output_rate per output frame and
// carry over between Render calls and across block ends.
class Playback
{
public:
	// Sets the source format and clears the queue. Channels 1-2, rate 1-kMaxSampleRate.
	bool Configure(uint32_t channels, uint32_t sample_rate);
	// Drops the queued blocks, cursor, phase and ended state.
	void Reset();
	// Replaces the queue with blocks, as a waveform reset does. On failure
	// nothing changes. Fails on an empty list, too many blocks, or a block whose
	// frame count does not match its layout.
	bool ReplaceQueue(std::vector<Block> blocks);

	[[nodiscard]] size_t   QueuedBlocks() const { return m_queue.size(); }
	[[nodiscard]] bool     HasBlocks() const { return !m_queue.empty(); }
	[[nodiscard]] uint64_t QueuedSampleBytes() const;

	// Refuses non-finite, negative and above-kMaxPitch ratios.
	bool SetPitch(float pitch);
	void SetGain(float gain) { m_gain = gain; }
	// A paused or stopped voice keeps its cursor and queue but renders nothing.
	void SetPlaying(bool playing) { m_playing = playing; }

	// Accumulates up to frames stereo frames into stereo (two doubles per frame,
	// gained but not clipped, so the caller clamps the whole system mix once) and
	// appends one event per finished block. Mono sources are written to both
	// channels. Returns false, accumulating nothing, when the step for output_rate
	// exceeds kMaxSourceFramesPerOutputFrame.
	bool Render(uint32_t frames, uint32_t output_rate, double* stereo, std::vector<Event>* events);

	// True once the queue drained during playback.
	[[nodiscard]] bool     Ended() const { return m_ended; }
	[[nodiscard]] uint64_t DecodedFrames() const { return m_decoded; }
	[[nodiscard]] uint32_t Channels() const { return m_channels; }

private:
	[[nodiscard]] const float* Frame(const Block& block, uint32_t index) const;
	[[nodiscard]] const float* NextFrame() const;
	void                       Advance(std::vector<Event>* events);

	std::deque<Block> m_queue;
	uint32_t          m_channels    = 0;
	uint32_t          m_sample_rate = 0;
	uint32_t          m_cursor      = 0;
	double            m_phase       = 0.0;
	float             m_pitch       = 1.0f;
	float             m_gain        = 1.0f;
	uint64_t          m_decoded     = 0;
	bool              m_playing     = false;
	bool              m_ended       = false;
};

} // namespace Kyty::Libs::Audio::Ngs2Sampler

#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_NGS2_SAMPLER_H_
