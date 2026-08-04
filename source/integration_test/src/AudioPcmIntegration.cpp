#include "Emulator/AudioPcm.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using Kyty::Libs::Audio::AudioPcmApplyChannelVolumes;
using Kyty::Libs::Audio::AudioPcmFormat;
using Kyty::Libs::Audio::AudioPcmQueueBytes;

namespace {

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "audio pcm integration failed: %s\n", message);
	}
	return condition;
}

template <typename T>
T ReadSample(const std::vector<uint8_t>& bytes, size_t index)
{
	T sample {};
	std::memcpy(&sample, bytes.data() + index * sizeof(sample), sizeof(sample));
	return sample;
}

bool ScalesSigned16Channels()
{
	const int16_t source[]  = {10000, -20000, -16384, 32767};
	const int     volumes[] = {16384, 8192};
	std::vector<uint8_t> output;
	if (!Check(AudioPcmApplyChannelVolumes(source, 2, 2, AudioPcmFormat::Signed16, volumes, &output), "S16 scaling failed") ||
	    !Check(output.size() == sizeof(source), "S16 output size changed"))
	{
		return false;
	}
	return Check(ReadSample<int16_t>(output, 0) == 5000, "S16 left channel frame 0") &&
	       Check(ReadSample<int16_t>(output, 1) == -5000, "S16 right channel frame 0") &&
	       Check(ReadSample<int16_t>(output, 2) == -8192, "S16 left channel frame 1") &&
	       Check(ReadSample<int16_t>(output, 3) == 8191, "S16 right channel frame 1");
}

bool ScalesFloat32Channels()
{
	const float source[]  = {0.75f, -0.50f, -0.25f, 1.00f};
	const int   volumes[] = {32768, 16384};
	std::vector<uint8_t> output;
	if (!Check(AudioPcmApplyChannelVolumes(source, 2, 2, AudioPcmFormat::Float32, volumes, &output), "F32 scaling failed") ||
	    !Check(output.size() == sizeof(source), "F32 output size changed"))
	{
		return false;
	}
	return Check(std::fabs(ReadSample<float>(output, 0) - 0.75f) < 0.00001f, "F32 left channel frame 0") &&
	       Check(std::fabs(ReadSample<float>(output, 1) + 0.25f) < 0.00001f, "F32 right channel frame 0") &&
	       Check(std::fabs(ReadSample<float>(output, 2) + 0.25f) < 0.00001f, "F32 left channel frame 1") &&
	       Check(std::fabs(ReadSample<float>(output, 3) - 0.50f) < 0.00001f, "F32 right channel frame 1");
}

bool RejectsZeroAndOverflowInputs()
{
	const int16_t sample = 1;
	const int     volume = 32768;
	std::vector<uint8_t> output;
	return Check(!AudioPcmApplyChannelVolumes(&sample, 0, 1, AudioPcmFormat::Signed16, &volume, &output),
	             "zero PCM frames were accepted") &&
	       Check(!AudioPcmApplyChannelVolumes(&sample, 1, std::numeric_limits<uint32_t>::max(), AudioPcmFormat::Signed16,
	                                          &volume, &output),
	             "overflow channel count was accepted") &&
	       Check(AudioPcmQueueBytes(0, 2, AudioPcmFormat::Signed16, 60) == 0, "zero frequency queue was accepted") &&
	       Check(AudioPcmQueueBytes(std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(),
	                                AudioPcmFormat::Float32, 1) == 0,
	             "overflow queue calculation was accepted");
}

bool CalculatesStereoQueueBytes()
{
	return Check(AudioPcmQueueBytes(48000, 2, AudioPcmFormat::Signed16, 60) == 11520, "48 kHz stereo S16 queue bytes") &&
	       Check(AudioPcmQueueBytes(48000, 2, AudioPcmFormat::Float32, 60) == 23040, "48 kHz stereo F32 queue bytes");
}

} // namespace

int main()
{
	if (!ScalesSigned16Channels() || !ScalesFloat32Channels() || !RejectsZeroAndOverflowInputs() || !CalculatesStereoQueueBytes())
	{
		return 1;
	}
	std::puts("audio pcm integration passed");
	return 0;
}
