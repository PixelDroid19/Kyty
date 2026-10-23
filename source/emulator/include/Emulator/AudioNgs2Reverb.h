#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_NGS2_REVERB_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_NGS2_REVERB_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Kyty::Libs::Audio::Ngs2Reverb {

constexpr uint32_t kMaxSampleRate = 192000;
constexpr size_t   kLateLines     = 4;
constexpr size_t   kDiffusers     = 2;

// The I3DL2 listener properties of a reverb voice, as the guest passes them: levels in millibels, times in seconds,
// diffusion and density in percent, hf_reference in hertz. wet and dry are linear gains on the reverberated and
// the unprocessed input.
struct Params
{
	float    wet                = 0.0f;
	float    dry                = 0.0f;
	int32_t  room               = 0;
	int32_t  room_hf            = 0;
	uint32_t reflection_pattern = 0;
	float    decay_time         = 0.0f;
	float    decay_hf_ratio     = 0.0f;
	int32_t  reflections        = 0;
	float    reflections_delay  = 0.0f;
	int32_t  reverb             = 0;
	float    reverb_delay       = 0.0f;
	float    diffusion          = 0.0f;
	float    density            = 0.0f;
	float    hf_reference       = 0.0f;
};

// The block the guest library fills before its first change: unit wet, no dry and every level at -10000 mB, so a
// voice that was only set up passes silence.
Params DefaultParams();

// Refuses a non-finite field and clamps the others to the published I3DL2 ranges (wet and dry to non-negative).
// reflection_pattern is kept as given; one generic early pattern is rendered for every value.
bool Normalize(Params* params);

// Host rendition of one I3DL2 reverb on the stereo bus. The input feeds a pre-delay line; early reflections are taps
// spread over reverb_delay after reflections_delay, and the late reverb is a feedback delay network fed
// reflections_delay + reverb_delay after the input, whose lines decay by 60 dB in decay_time and, above hf_reference,
// in decay_time * decay_hf_ratio. room_hf attenuates the wet path above hf_reference. Every buffer is sized for the
// I3DL2 maxima when the reverb is configured; processing never allocates.
class Reverb
{
public:
	// Sizes the buffers for sample_rate (1-kMaxSampleRate), clears the state and loads DefaultParams.
	bool Configure(uint32_t sample_rate);
	// Clears the delay lines and filters, ending any tail. Parameters are kept.
	void Reset();
	// Takes normalized parameters. Coefficients are recomputed without clearing the lines; identical parameters
	// change nothing.
	void SetParams(const Params& params);

	[[nodiscard]] bool          IsConfigured() const { return m_sample_rate != 0; }
	[[nodiscard]] const Params& GetParams() const { return m_params; }

	// Writes frames stereo frames (two doubles per frame) of dry * input + wet * reverb. input and output may be the
	// same buffer. A reverb that is not configured writes silence.
	void Process(const double* input, uint32_t frames, double* output);

private:
	struct Line
	{
		std::vector<double> buffer;
		size_t              length  = 0;
		size_t              cursor  = 0;
		double              gain    = 0.0;
		double              gain_hf = 0.0;
		double              lowpass = 0.0;
	};

	void   UpdateCoefficients();
	[[nodiscard]] double DelayedInput(size_t delay) const;

	Params   m_params {};
	uint32_t m_sample_rate = 0;

	std::vector<double> m_input;
	size_t              m_input_cursor = 0;

	std::array<Line, kDiffusers> m_diffusers;
	std::array<Line, kLateLines> m_lines;

	std::array<size_t, 4> m_early_delays {};
	double                m_early_gain    = 0.0;
	size_t                m_late_delay    = 0;
	double                m_late_gain     = 0.0;
	double                m_diffuser_gain = 0.0;
	double                m_damping_pole  = 0.0;
	double                m_room_hf_gain  = 1.0;
	std::array<double, 2> m_room_hf_state {};
};

} // namespace Kyty::Libs::Audio::Ngs2Reverb

#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_NGS2_REVERB_H_
