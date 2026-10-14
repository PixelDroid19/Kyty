#include "Kyty/UnitTest.h"

#include "Emulator/AudioNgs2Reverb.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

UT_BEGIN(EmulatorAudioNgs2Reverb);

namespace {

namespace Ngs2Reverb = ::Kyty::Libs::Audio::Ngs2Reverb;

constexpr uint32_t kRate  = 48000;
constexpr uint32_t kGrain = 256;

// A late-reverb-only room: full room and reverb levels, no early reflections and no frequency-dependent decay.
Ngs2Reverb::Params LateOnlyParams(float decay_time)
{
	Ngs2Reverb::Params params;
	params.wet               = 1.0f;
	params.dry               = 0.0f;
	params.room              = 0;
	params.room_hf           = 0;
	params.decay_time        = decay_time;
	params.decay_hf_ratio    = 1.0f;
	params.reflections       = -10000;
	params.reflections_delay = 0.0f;
	params.reverb            = 0;
	params.reverb_delay      = 0.0f;
	params.diffusion         = 100.0f;
	params.density           = 100.0f;
	params.hf_reference      = 5000.0f;
	return params;
}

// Runs frames stereo frames, grain by grain, with a unit impulse on both channels at frame 0.
std::vector<double> ImpulseResponse(Ngs2Reverb::Reverb* reverb, uint32_t frames)
{
	std::vector<double> output(static_cast<size_t>(frames) * 2u, 0.0);
	output[0] = 1.0;
	output[1] = 1.0;
	for (uint32_t frame = 0; frame < frames; frame += kGrain)
	{
		double* grain = output.data() + static_cast<size_t>(frame) * 2u;
		reverb->Process(grain, kGrain, grain);
	}
	return output;
}

double MeanSquare(const std::vector<double>& stereo, double from_seconds, double to_seconds)
{
	const auto from = static_cast<size_t>(from_seconds * kRate) * 2u;
	const auto to   = static_cast<size_t>(to_seconds * kRate) * 2u;
	double     sum  = 0.0;
	for (size_t i = from; i < to; i++)
	{
		sum += stereo[i] * stereo[i];
	}
	return sum / static_cast<double>(to - from);
}

} // namespace

TEST(EmulatorAudioNgs2Reverb, NormalizeRefusesNonFiniteFieldsAndClampsToTheI3dl2Ranges)
{
	auto params = Ngs2Reverb::DefaultParams();
	params.wet  = std::numeric_limits<float>::quiet_NaN();
	EXPECT_FALSE(Ngs2Reverb::Normalize(&params));
	params            = Ngs2Reverb::DefaultParams();
	params.decay_time = std::numeric_limits<float>::infinity();
	EXPECT_FALSE(Ngs2Reverb::Normalize(&params));

	params                    = Ngs2Reverb::DefaultParams();
	params.dry                = -1.0f;
	params.room               = 500;
	params.reflections        = 5000;
	params.reflection_pattern = 99;
	params.decay_time         = 100.0f;
	params.decay_hf_ratio     = 0.0f;
	params.reflections_delay  = 1.0f;
	params.reverb_delay       = 1.0f;
	params.hf_reference       = 1.0f;
	ASSERT_TRUE(Ngs2Reverb::Normalize(&params));
	EXPECT_EQ(params.dry, 0.0f);
	EXPECT_EQ(params.room, 0);
	EXPECT_EQ(params.reflections, 1000);
	EXPECT_EQ(params.reflection_pattern, 99u);
	EXPECT_EQ(params.decay_time, 20.0f);
	EXPECT_EQ(params.decay_hf_ratio, 0.1f);
	EXPECT_EQ(params.reflections_delay, 0.3f);
	EXPECT_EQ(params.reverb_delay, 0.1f);
	EXPECT_EQ(params.hf_reference, 20.0f);
}

TEST(EmulatorAudioNgs2Reverb, AConfiguredReverbPassesSilenceUntilItGetsParameters)
{
	Ngs2Reverb::Reverb reverb;
	EXPECT_FALSE(reverb.Configure(0));
	EXPECT_FALSE(reverb.Configure(Ngs2Reverb::kMaxSampleRate + 1));

	std::vector<double> grain(kGrain * 2u, 0.5);
	reverb.Process(grain.data(), kGrain, grain.data());
	for (const double sample: grain)
	{
		EXPECT_EQ(sample, 0.0);
	}

	ASSERT_TRUE(reverb.Configure(kRate));
	for (uint32_t i = 0; i < kRate / kGrain; i++)
	{
		std::fill(grain.begin(), grain.end(), 0.5);
		reverb.Process(grain.data(), kGrain, grain.data());
		for (const double sample: grain)
		{
			ASSERT_EQ(sample, 0.0);
		}
	}
}

TEST(EmulatorAudioNgs2Reverb, NoWetGainPassesExactlyTheScaledDryInput)
{
	Ngs2Reverb::Reverb reverb;
	ASSERT_TRUE(reverb.Configure(kRate));
	auto params = LateOnlyParams(2.0f);
	params.wet  = 0.0f;
	params.dry  = 0.4f;
	ASSERT_TRUE(Ngs2Reverb::Normalize(&params));
	reverb.SetParams(params);

	std::vector<double> input(kGrain * 2u);
	std::vector<double> output(kGrain * 2u);
	for (uint32_t round = 0; round < 64; round++)
	{
		for (size_t i = 0; i < input.size(); i++)
		{
			input[i] = std::sin(static_cast<double>(round * kGrain * 2u + i) * 0.01);
		}
		reverb.Process(input.data(), kGrain, output.data());
		for (size_t i = 0; i < input.size(); i++)
		{
			ASSERT_EQ(output[i], static_cast<double>(params.dry) * input[i]);
		}
	}
}

TEST(EmulatorAudioNgs2Reverb, TheTailOutlivesItsInputAndDecaysBy60DbInTheDecayTime)
{
	Ngs2Reverb::Reverb reverb;
	ASSERT_TRUE(reverb.Configure(kRate));
	reverb.SetParams(LateOnlyParams(1.0f));

	const auto   response = ImpulseResponse(&reverb, kRate * 2u);
	const double early    = MeanSquare(response, 0.2, 0.4);
	const double late     = MeanSquare(response, 1.2, 1.4);
	ASSERT_GT(early, 0.0);
	ASSERT_GT(late, 0.0);
	EXPECT_NEAR(10.0 * std::log10(early / late), 60.0, 3.0);
}

TEST(EmulatorAudioNgs2Reverb, ResetEndsTheTailAndIdenticalParametersLeaveItUntouched)
{
	Ngs2Reverb::Reverb resent;
	Ngs2Reverb::Reverb untouched;
	ASSERT_TRUE(resent.Configure(kRate));
	ASSERT_TRUE(untouched.Configure(kRate));
	const auto params = LateOnlyParams(1.5f);
	resent.SetParams(params);
	untouched.SetParams(params);

	std::vector<double> a(kGrain * 2u, 0.0);
	std::vector<double> b(kGrain * 2u, 0.0);
	a[0] = b[0] = 1.0;
	for (uint32_t i = 0; i < 40; i++)
	{
		// A title re-sends its unchanged parameter block every update.
		resent.SetParams(params);
		resent.Process(a.data(), kGrain, a.data());
		untouched.Process(b.data(), kGrain, b.data());
		ASSERT_EQ(a, b);
		std::fill(a.begin(), a.end(), 0.0);
		std::fill(b.begin(), b.end(), 0.0);
	}

	resent.Process(a.data(), kGrain, a.data());
	double tail = 0.0;
	for (const double sample: a)
	{
		tail = std::max(tail, std::abs(sample));
	}
	ASSERT_GT(tail, 0.0);

	resent.Reset();
	for (uint32_t i = 0; i < 40; i++)
	{
		std::fill(a.begin(), a.end(), 0.0);
		resent.Process(a.data(), kGrain, a.data());
		for (const double sample: a)
		{
			ASSERT_EQ(sample, 0.0);
		}
	}
}

UT_END();
