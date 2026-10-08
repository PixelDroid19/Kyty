#include "Emulator/AudioNgs2Sampler.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Kyty::Libs::Audio::Ngs2Sampler {

bool Playback::Configure(uint32_t channels, uint32_t sample_rate)
{
	if (channels == 0 || channels > kMaxChannels || sample_rate == 0 || sample_rate > kMaxSampleRate)
	{
		return false;
	}
	m_channels    = channels;
	m_sample_rate = sample_rate;
	m_pitch       = 1.0f;
	m_gain        = 1.0f;
	m_decoded     = 0;
	Reset();
	return true;
}

void Playback::Reset()
{
	m_queue.clear();
	m_cursor  = 0;
	m_phase   = 0.0;
	m_playing = false;
	m_ended   = false;
}

bool Playback::ReplaceQueue(std::vector<Block> blocks)
{
	if (m_channels == 0 || blocks.empty() || blocks.size() > kMaxQueuedBlocks)
	{
		return false;
	}
	for (const auto& block: blocks)
	{
		const uint64_t frames = static_cast<uint64_t>(block.num_skip) + block.num_samples;
		if (block.num_samples == 0 || block.frames.size() != frames * m_channels)
		{
			return false;
		}
	}
	// Validation above is complete before the live queue is touched, so a rejected
	// add changes nothing. The new queue is built aside and swapped in. Host
	// allocation failure is not recoverable here: it follows the project-wide
	// out-of-memory policy (the build has no exceptions).
	std::deque<Block> fresh;
	for (auto& block: blocks)
	{
		fresh.push_back(std::move(block));
	}
	m_queue.swap(fresh);
	m_cursor = m_queue.front().num_skip;
	m_phase  = 0.0;
	m_ended  = false;
	return true;
}

uint64_t Playback::QueuedSampleBytes() const
{
	uint64_t bytes = 0;
	for (const auto& block: m_queue)
	{
		bytes += static_cast<uint64_t>(block.frames.size()) * sizeof(float);
	}
	return bytes;
}

bool Playback::SetPitch(float pitch)
{
	if (!std::isfinite(pitch) || pitch < 0.0f || pitch > kMaxPitch)
	{
		return false;
	}
	m_pitch = pitch;
	return true;
}

bool Playback::Render(uint32_t frames, uint32_t output_rate, double* stereo, std::vector<Event>* events)
{
	if (stereo == nullptr || events == nullptr || output_rate == 0)
	{
		return false;
	}
	// Positions are in source frames; step is the source rate relative to the mixer rate.
	const double step = static_cast<double>(m_pitch) * static_cast<double>(m_sample_rate) / static_cast<double>(output_rate);
	if (!(step >= 0.0 && step <= kMaxSourceFramesPerOutputFrame))
	{
		return false;
	}
	for (uint32_t i = 0; i < frames && m_playing && !m_queue.empty(); i++)
	{
		const float* current  = Frame(m_queue.front(), m_cursor);
		const float* next     = NextFrame();
		const double fraction = m_phase;
		for (uint32_t channel = 0; channel < m_channels; channel++)
		{
			// Gained but not clipped: the system mix sums in double and clamps once,
			// so opposite-sign voices still cancel before the final clamp.
			const double sample = static_cast<double>(current[channel]) +
			                      (static_cast<double>(next[channel]) - static_cast<double>(current[channel])) * fraction;
			const double value = sample * static_cast<double>(m_gain);
			if (m_channels == 1)
			{
				stereo[2 * i]     += value;
				stereo[2 * i + 1] += value;
			} else
			{
				stereo[2 * i + channel] += value;
			}
		}
		// At most kMaxSourceFramesPerOutputFrame advances per output frame, and at
		// most one event per queued block (block repeats are counted, not emitted).
		m_phase += step;
		while (m_phase >= 1.0 && !m_queue.empty())
		{
			m_phase -= 1.0;
			Advance(events);
		}
	}
	return true;
}

const float* Playback::Frame(const Block& block, uint32_t index) const
{
	return block.frames.data() + static_cast<size_t>(index) * m_channels;
}

// The frame after the cursor. Across a block end it is the loop start when the
// block repeats, otherwise the first frame of the next queued block.
const float* Playback::NextFrame() const
{
	const Block& block = m_queue.front();
	if (m_cursor + 1 < block.num_skip + block.num_samples)
	{
		return Frame(block, m_cursor + 1);
	}
	if (block.num_repeats != 0)
	{
		return Frame(block, block.num_skip);
	}
	if (m_queue.size() > 1)
	{
		return Frame(m_queue[1], m_queue[1].num_skip);
	}
	return Frame(block, m_cursor);
}

void Playback::Advance(std::vector<Event>* events)
{
	Block& block = m_queue.front();
	m_decoded++;
	m_cursor++;
	if (m_cursor < block.num_skip + block.num_samples)
	{
		return;
	}
	if (block.num_repeats != 0)
	{
		// A repeat loops in place. It advances the repeat count only, with no
		// event and no allocation, so a short looping block cannot flood the queue.
		if (block.num_repeats != kRepeatForever)
		{
			block.num_repeats--;
		}
		block.num_repeated++;
		m_cursor = block.num_skip;
		return;
	}
	events->push_back(Event {block.num_repeated, block.user_data, block.guest_data, block.guest_size});
	m_queue.pop_front();
	if (!m_queue.empty())
	{
		m_cursor = m_queue.front().num_skip;
		return;
	}
	// A reset waveform is complete once its queue drains.
	m_cursor  = 0;
	m_playing = false;
	m_ended   = true;
}

} // namespace Kyty::Libs::Audio::Ngs2Sampler
