#include "Emulator/AudioNgs2Reverb.h"

#include <algorithm>
#include <cmath>

namespace Kyty::Libs::Audio::Ngs2Reverb {

namespace {

// Published I3DL2 ranges.
constexpr int32_t kMinLevel            = -10000;
constexpr int32_t kMaxReflectionsLevel = 1000;
constexpr int32_t kMaxReverbLevel      = 2000;
constexpr float   kMaxReflectionsDelay = 0.3f;
constexpr float   kMaxReverbDelay      = 0.1f;

// Host rendition choices, not guest contracts. Early taps are spread over reverb_delay at these fractions with
// decreasing gains; the input diffusers and the late lines have mutually unrelated lengths, the late ones scaled by
// density between kMinDensityScale and 1.
constexpr std::array<double, 4>          kEarlyTapFractions = {0.0, 0.29, 0.53, 0.79};
constexpr std::array<double, 4>          kEarlyTapGains     = {1.0, 0.71, 0.5, 0.35};
constexpr std::array<double, kDiffusers> kDiffuserMs        = {4.71, 1.63};
constexpr std::array<double, kLateLines> kLateLineMs        = {31.27, 37.91, 43.37, 49.13};
constexpr double                         kMinDensityScale   = 0.4;
constexpr double                         kMaxDiffuserGain   = 0.65;
// The damping crossover is kept below the Nyquist frequency.
constexpr double kMaxHfReferenceFraction = 0.45;
constexpr double kPi                     = 3.14159265358979323846;

bool IsFinite(const Params& params)
{
	return std::isfinite(params.wet) && std::isfinite(params.dry) && std::isfinite(params.decay_time) &&
	       std::isfinite(params.decay_hf_ratio) && std::isfinite(params.reflections_delay) && std::isfinite(params.reverb_delay) &&
	       std::isfinite(params.diffusion) && std::isfinite(params.density) && std::isfinite(params.hf_reference);
}

bool SameParams(const Params& a, const Params& b)
{
	return a.wet == b.wet && a.dry == b.dry && a.room == b.room && a.room_hf == b.room_hf &&
	       a.reflection_pattern == b.reflection_pattern && a.decay_time == b.decay_time && a.decay_hf_ratio == b.decay_hf_ratio &&
	       a.reflections == b.reflections && a.reflections_delay == b.reflections_delay && a.reverb == b.reverb &&
	       a.reverb_delay == b.reverb_delay && a.diffusion == b.diffusion && a.density == b.density && a.hf_reference == b.hf_reference;
}

// -10000 mB is silence, not -100 dB.
double LevelGain(int32_t millibels)
{
	return millibels <= kMinLevel ? 0.0 : std::pow(10.0, static_cast<double>(millibels) / 2000.0);
}

size_t Samples(double seconds, uint32_t sample_rate)
{
	return static_cast<size_t>(std::lround(seconds * static_cast<double>(sample_rate)));
}

size_t LengthSamples(double milliseconds, uint32_t sample_rate)
{
	return std::max<size_t>(1, Samples(milliseconds / 1000.0, sample_rate));
}

// Gain of one pass through a line of length samples for a 60 dB decay in decay_time seconds.
double DecayGain(size_t length, uint32_t sample_rate, double decay_time)
{
	return std::pow(10.0, -3.0 * static_cast<double>(length) / (static_cast<double>(sample_rate) * decay_time));
}

double EarlyTapEnergy()
{
	double energy = 0.0;
	for (const double gain: kEarlyTapGains)
	{
		energy += gain * gain;
	}
	return std::sqrt(energy);
}

void Advance(size_t* cursor, size_t length)
{
	*cursor = *cursor + 1 == length ? 0 : *cursor + 1;
}

} // namespace

Params DefaultParams()
{
	Params params;
	params.wet               = 1.0f;
	params.dry               = 0.0f;
	params.room              = kMinLevel;
	params.room_hf           = 0;
	params.decay_time        = 1.0f;
	params.decay_hf_ratio    = 0.5f;
	params.reflections       = kMinLevel;
	params.reflections_delay = 0.02f;
	params.reverb            = kMinLevel;
	params.reverb_delay      = 0.04f;
	params.diffusion         = 100.0f;
	params.density           = 100.0f;
	params.hf_reference      = 5000.0f;
	return params;
}

bool Normalize(Params* params)
{
	if (!IsFinite(*params))
	{
		return false;
	}
	params->wet               = std::max(params->wet, 0.0f);
	params->dry               = std::max(params->dry, 0.0f);
	params->room              = std::clamp(params->room, kMinLevel, 0);
	params->room_hf           = std::clamp(params->room_hf, kMinLevel, 0);
	params->decay_time        = std::clamp(params->decay_time, 0.1f, 20.0f);
	params->decay_hf_ratio    = std::clamp(params->decay_hf_ratio, 0.1f, 2.0f);
	params->reflections       = std::clamp(params->reflections, kMinLevel, kMaxReflectionsLevel);
	params->reflections_delay = std::clamp(params->reflections_delay, 0.0f, kMaxReflectionsDelay);
	params->reverb            = std::clamp(params->reverb, kMinLevel, kMaxReverbLevel);
	params->reverb_delay      = std::clamp(params->reverb_delay, 0.0f, kMaxReverbDelay);
	params->diffusion         = std::clamp(params->diffusion, 0.0f, 100.0f);
	params->density           = std::clamp(params->density, 0.0f, 100.0f);
	params->hf_reference      = std::clamp(params->hf_reference, 20.0f, 20000.0f);
	return true;
}

bool Reverb::Configure(uint32_t sample_rate)
{
	if (sample_rate == 0 || sample_rate > kMaxSampleRate)
	{
		return false;
	}
	m_sample_rate = sample_rate;
	m_input.assign(Samples(static_cast<double>(kMaxReflectionsDelay) + static_cast<double>(kMaxReverbDelay), sample_rate) + 1, 0.0);
	for (size_t i = 0; i < kDiffusers; i++)
	{
		m_diffusers[i].length = LengthSamples(kDiffuserMs[i], sample_rate);
		m_diffusers[i].buffer.assign(m_diffusers[i].length, 0.0);
	}
	for (size_t i = 0; i < kLateLines; i++)
	{
		m_lines[i].buffer.assign(LengthSamples(kLateLineMs[i], sample_rate), 0.0);
	}
	m_params = DefaultParams();
	UpdateCoefficients();
	Reset();
	return true;
}

void Reverb::Reset()
{
	std::fill(m_input.begin(), m_input.end(), 0.0);
	m_input_cursor = 0;
	for (auto& line: m_diffusers)
	{
		std::fill(line.buffer.begin(), line.buffer.end(), 0.0);
		line.cursor = 0;
	}
	for (auto& line: m_lines)
	{
		std::fill(line.buffer.begin(), line.buffer.end(), 0.0);
		line.cursor  = 0;
		line.lowpass = 0.0;
	}
	m_room_hf_state = {};
}

void Reverb::SetParams(const Params& params)
{
	if (SameParams(params, m_params))
	{
		return;
	}
	m_params = params;
	if (IsConfigured())
	{
		UpdateCoefficients();
	}
}

void Reverb::UpdateCoefficients()
{
	const uint32_t rate        = m_sample_rate;
	const size_t   early_start = Samples(m_params.reflections_delay, rate);
	for (size_t i = 0; i < m_early_delays.size(); i++)
	{
		m_early_delays[i] = early_start + Samples(kEarlyTapFractions[i] * m_params.reverb_delay, rate);
	}
	const double room = LevelGain(m_params.room);
	m_early_gain      = room * LevelGain(m_params.reflections) / EarlyTapEnergy();
	m_late_delay      = early_start + Samples(m_params.reverb_delay, rate);
	m_late_gain       = room * LevelGain(m_params.reverb);
	m_diffuser_gain   = kMaxDiffuserGain * static_cast<double>(m_params.diffusion) / 100.0;

	const double hf_reference = std::min(static_cast<double>(m_params.hf_reference), kMaxHfReferenceFraction * rate);
	m_damping_pole            = std::exp(-2.0 * kPi * hf_reference / rate);
	m_room_hf_gain            = LevelGain(m_params.room_hf);

	const double density_scale = kMinDensityScale + (1.0 - kMinDensityScale) * static_cast<double>(m_params.density) / 100.0;
	for (size_t i = 0; i < kLateLines; i++)
	{
		auto& line   = m_lines[i];
		line.length  = std::min(line.buffer.size(), LengthSamples(kLateLineMs[i] * density_scale, rate));
		line.cursor  = line.cursor < line.length ? line.cursor : 0;
		line.gain    = DecayGain(line.length, rate, m_params.decay_time);
		line.gain_hf = DecayGain(line.length, rate, static_cast<double>(m_params.decay_time) * m_params.decay_hf_ratio);
	}
}

double Reverb::DelayedInput(size_t delay) const
{
	const size_t size = m_input.size();
	return m_input[(m_input_cursor + size - delay) % size];
}

void Reverb::Process(const double* input, uint32_t frames, double* output)
{
	if (!IsConfigured())
	{
		std::fill(output, output + static_cast<size_t>(frames) * 2u, 0.0);
		return;
	}
	const double wet  = m_params.wet;
	const double dry  = m_params.dry;
	const double pole = m_damping_pole;
	for (uint32_t frame = 0; frame < frames; frame++)
	{
		const double left  = input[frame * 2u];
		const double right = input[frame * 2u + 1u];
		m_input[m_input_cursor] = 0.5 * (left + right);

		double early = 0.0;
		for (size_t i = 0; i < m_early_delays.size(); i++)
		{
			early += kEarlyTapGains[i] * DelayedInput(m_early_delays[i]);
		}
		early *= m_early_gain;

		// Schroeder allpasses diffuse the late input.
		double late_input = DelayedInput(m_late_delay);
		for (auto& diffuser: m_diffusers)
		{
			const double delayed            = diffuser.buffer[diffuser.cursor];
			const double stored             = late_input - m_diffuser_gain * delayed;
			diffuser.buffer[diffuser.cursor] = stored;
			late_input                      = delayed + m_diffuser_gain * stored;
			Advance(&diffuser.cursor, diffuser.length);
		}

		// Each line output passes a damping filter with gain `gain` at DC and `gain_hf` at high frequencies, then the
		// lossless Householder matrix feeds every line back.
		std::array<double, kLateLines> taps {};
		std::array<double, kLateLines> damped {};
		double                         damped_sum = 0.0;
		for (size_t i = 0; i < kLateLines; i++)
		{
			auto& line   = m_lines[i];
			taps[i]      = line.buffer[line.cursor];
			line.lowpass = (1.0 - pole) * taps[i] + pole * line.lowpass;
			damped[i]    = line.gain_hf * taps[i] + (line.gain - line.gain_hf) * line.lowpass;
			damped_sum += damped[i];
		}
		for (size_t i = 0; i < kLateLines; i++)
		{
			auto& line                   = m_lines[i];
			line.buffer[line.cursor] = damped[i] - 0.5 * damped_sum + 0.5 * late_input;
			Advance(&line.cursor, line.length);
		}

		std::array<double, 2> reverb = {
		    early + m_late_gain * 0.5 * (taps[0] - taps[1] + taps[2] - taps[3]),
		    early + m_late_gain * 0.5 * (taps[0] + taps[1] - taps[2] - taps[3]),
		};
		// room_hf: unit gain at DC, m_room_hf_gain at high frequencies.
		for (size_t channel = 0; channel < reverb.size(); channel++)
		{
			m_room_hf_state[channel] = (1.0 - pole) * reverb[channel] + pole * m_room_hf_state[channel];
			reverb[channel]          = m_room_hf_gain * reverb[channel] + (1.0 - m_room_hf_gain) * m_room_hf_state[channel];
		}
		output[frame * 2u]      = dry * left + wet * reverb[0];
		output[frame * 2u + 1u] = dry * right + wet * reverb[1];
		Advance(&m_input_cursor, m_input.size());
	}
}

} // namespace Kyty::Libs::Audio::Ngs2Reverb
