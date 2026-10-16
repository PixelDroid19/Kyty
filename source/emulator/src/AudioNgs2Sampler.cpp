#include "Emulator/AudioNgs2Sampler.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Kyty::Libs::Audio::Ngs2Sampler {

// One past the last frame the block holds. Below num_skip + num_samples only
// for a streaming block still waiting for data.
static uint64_t SuppliedEnd(const Block& block, uint32_t channels)
{
	return block.first_frame + block.frames.size() / channels;
}

// Guest address of frame index of an uncoded block; past the supplied frames of
// a streaming block it is the end of its last piece.
static uintptr_t GuestFrameAddress(const Block& block, uint32_t index)
{
	if (!block.streaming)
	{
		return block.guest_data + static_cast<uintptr_t>(index) * block.guest_frame_bytes;
	}
	for (const auto& piece: block.pieces)
	{
		if (index >= piece.first_frame && index - piece.first_frame < piece.frames)
		{
			return piece.guest_data + static_cast<uintptr_t>(index - piece.first_frame) * block.guest_frame_bytes;
		}
	}
	const Piece& last = block.pieces.back();
	return last.guest_data + static_cast<uintptr_t>(last.frames) * block.guest_frame_bytes;
}

bool Playback::Configure(uint32_t channels, uint32_t sample_rate)
{
	if (channels == 0 || channels > kMaxChannels || sample_rate == 0 || sample_rate > kMaxSampleRate)
	{
		return false;
	}
	m_channels      = channels;
	m_sample_rate   = sample_rate;
	m_pitch         = 1.0f;
	m_decoded       = 0;
	m_decoded_bytes = 0;
	m_waveform_end  = 0;
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
	m_open    = true;
}

bool Playback::ValidBlock(const Block& block) const
{
	const uint64_t frames = static_cast<uint64_t>(block.num_skip) + block.num_samples;
	if (block.num_samples == 0 || block.first_frame != 0 || !block.pieces.empty() || block.frames.size() % m_channels != 0)
	{
		return false;
	}
	const uint64_t supplied = block.frames.size() / m_channels;
	if (!block.streaming)
	{
		return supplied == frames;
	}
	// A streaming block plays its waveform once, from its first frame.
	return block.num_skip == 0 && block.num_repeats == 0 && block.guest_frame_bytes != 0 && supplied != 0 && supplied < frames;
}

bool Playback::ReplaceQueue(std::vector<Block> blocks)
{
	if (m_channels == 0 || blocks.empty() || blocks.size() > kMaxQueuedBlocks)
	{
		return false;
	}
	if (!std::all_of(blocks.begin(), blocks.end(), [this](const Block& block) { return ValidBlock(block); }))
	{
		return false;
	}
	// Validation above is complete before the live queue is touched, so a rejected
	// add changes nothing. Host allocation failure is not recoverable here: it
	// follows the project-wide out-of-memory policy (the build has no exceptions).
	m_queue.clear();
	if (!QueueBlocks(std::move(blocks)))
	{
		return false;
	}
	m_ended = false;
	return true;
}

bool Playback::QueueBlocks(std::vector<Block> blocks)
{
	if (m_channels == 0 || blocks.empty() || blocks.size() > kMaxQueuedBlocks - std::min<size_t>(m_queue.size(), kMaxQueuedBlocks))
	{
		return false;
	}
	if (!std::all_of(blocks.begin(), blocks.end(), [this](const Block& block) { return ValidBlock(block); }))
	{
		return false;
	}
	const bool was_empty = m_queue.empty();
	for (auto& block: blocks)
	{
		if (block.streaming)
		{
			block.pieces.push_back(Piece {block.guest_data, 0, static_cast<uint32_t>(block.frames.size() / m_channels)});
		}
		m_queue.push_back(std::move(block));
	}
	if (was_empty)
	{
		m_cursor = m_queue.front().num_skip;
		m_phase  = 0.0;
	}
	return true;
}

bool Playback::AppendPiece(std::vector<float> frames, uintptr_t guest_data)
{
	if (m_queue.empty() || !m_queue.back().streaming || frames.empty() || frames.size() % m_channels != 0)
	{
		return false;
	}
	Block&         block    = m_queue.back();
	const uint64_t supplied = SuppliedEnd(block, m_channels);
	const uint64_t added    = frames.size() / m_channels;
	if (added > block.num_samples - supplied)
	{
		return false;
	}
	DropPlayedFrames(&block);
	if (block.pieces.size() >= kMaxStreamPieces)
	{
		return false;
	}
	block.pieces.push_back(Piece {guest_data, static_cast<uint32_t>(supplied), static_cast<uint32_t>(added)});
	block.frames.insert(block.frames.end(), frames.begin(), frames.end());
	return true;
}

// Frees the frames and pieces of the front streaming block that are behind the
// cursor. The frame under the cursor stays, since the resampler still reads it.
void Playback::DropPlayedFrames(Block* block)
{
	if (block != &m_queue.front() || m_cursor <= block->first_frame)
	{
		return;
	}
	const uint32_t played = std::min<uint32_t>(m_cursor, static_cast<uint32_t>(SuppliedEnd(*block, m_channels))) - block->first_frame;
	block->frames.erase(block->frames.begin(), block->frames.begin() + static_cast<ptrdiff_t>(played) * m_channels);
	block->first_frame += played;
	while (block->pieces.size() > 1 && block->pieces.front().first_frame + block->pieces.front().frames <= m_cursor)
	{
		block->pieces.pop_front();
	}
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

uintptr_t Playback::WaveformData() const
{
	if (m_queue.empty())
	{
		return m_waveform_end;
	}
	const Block& block = m_queue.front();
	return block.guest_frame_bytes == 0 ? 0 : GuestFrameAddress(block, m_cursor);
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
		// Only a streaming block runs out of frames before its end. An open
		// waveform waits silently for the next piece; a closed one ends there.
		while (!m_open && !m_queue.empty() && m_cursor >= SuppliedEnd(m_queue.front(), m_channels))
		{
			FinishBlock(events);
		}
		if (m_queue.empty() || m_cursor >= SuppliedEnd(m_queue.front(), m_channels))
		{
			break;
		}
		const float* current  = Frame(m_queue.front(), m_cursor);
		const float* next     = NextFrame();
		const double fraction = m_phase;
		for (uint32_t channel = 0; channel < m_channels; channel++)
		{
			// Not clipped: the system mix sums in double and clamps once, so
			// opposite-sign voices still cancel before the final clamp.
			const double value = static_cast<double>(current[channel]) +
			                     (static_cast<double>(next[channel]) - static_cast<double>(current[channel])) * fraction;
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
		// The cursor never passes the supplied frames of a streaming block.
		m_phase += step;
		while (m_phase >= 1.0 && !m_queue.empty() && m_cursor < SuppliedEnd(m_queue.front(), m_channels))
		{
			m_phase -= 1.0;
			Advance(events);
		}
	}
	return true;
}

const float* Playback::Frame(const Block& block, uint32_t index) const
{
	return block.frames.data() + static_cast<size_t>(index - block.first_frame) * m_channels;
}

// The frame after the cursor. Across a block end it is the loop start when the
// block repeats, otherwise the first frame of the next queued block. A
// streaming block whose next frame is not supplied yet holds the current one.
const float* Playback::NextFrame() const
{
	const Block& block = m_queue.front();
	if (m_cursor + 1 < SuppliedEnd(block, m_channels))
	{
		return Frame(block, m_cursor + 1);
	}
	if (block.streaming && m_cursor + 1 < block.num_samples)
	{
		return Frame(block, m_cursor);
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
	m_decoded_bytes += block.guest_frame_bytes;
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
	FinishBlock(events);
}

void Playback::FinishBlock(std::vector<Event>* events)
{
	const Block& block = m_queue.front();
	m_waveform_end     = block.guest_frame_bytes == 0 ? 0 : GuestFrameAddress(block, m_cursor);
	events->push_back(Event {block.num_repeated, block.user_data, block.guest_data, block.guest_size});
	m_queue.pop_front();
	if (!m_queue.empty())
	{
		m_cursor = m_queue.front().num_skip;
		return;
	}
	// A waveform is complete once its queue drains.
	m_cursor  = 0;
	m_playing = false;
	m_ended   = true;
}

} // namespace Kyty::Libs::Audio::Ngs2Sampler
