#include "Emulator/AudioPcm.h"

#include <algorithm>
#include <cstring>
#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

size_t AudioPcmQueueBytes(uint32_t frequency, uint32_t channels, AudioPcmFormat format, uint32_t target_milliseconds)
{
	if (frequency == 0 || channels == 0 || target_milliseconds == 0)
	{
		return 0;
	}
	const size_t bytes_per_sample = AudioPcmBytesPerSample(format);
	if (frequency > std::numeric_limits<size_t>::max() / channels ||
	    static_cast<size_t>(frequency) * channels > std::numeric_limits<size_t>::max() / bytes_per_sample)
	{
		return 0;
	}
	const size_t bytes_per_second = static_cast<size_t>(frequency) * channels * bytes_per_sample;
	if (bytes_per_second > std::numeric_limits<size_t>::max() / target_milliseconds)
	{
		return 0;
	}
	return bytes_per_second * target_milliseconds / 1000u;
}

bool AudioPcmApplyChannelVolumes(const void* source, uint32_t frames, uint32_t channels, AudioPcmFormat format, const int* volumes,
                                 std::vector<uint8_t>* output)
{
	if (source == nullptr || volumes == nullptr || output == nullptr || frames == 0 || channels == 0 || channels > 8 ||
	    frames > std::numeric_limits<size_t>::max() / channels)
	{
		return false;
	}
	const size_t sample_count = static_cast<size_t>(frames) * channels;
	const size_t sample_bytes = AudioPcmBytesPerSample(format);
	if (sample_count > std::numeric_limits<size_t>::max() / sample_bytes)
	{
		return false;
	}
	output->resize(sample_count * sample_bytes);

	if (format == AudioPcmFormat::Signed16)
	{
		const auto* input = static_cast<const int16_t*>(source);
		for (size_t index = 0; index < sample_count; ++index)
		{
			const int  gain   = std::clamp(volumes[index % channels], 0, 32768);
			const auto scaled = static_cast<int16_t>((static_cast<int32_t>(input[index]) * gain) / 32768);
			std::memcpy(output->data() + index * sizeof(scaled), &scaled, sizeof(scaled));
		}
		return true;
	}

	const auto* input = static_cast<const uint8_t*>(source);
	for (size_t index = 0; index < sample_count; ++index)
	{
		float sample = 0.0f;
		std::memcpy(&sample, input + index * sizeof(sample), sizeof(sample));
		const int gain = std::clamp(volumes[index % channels], 0, 32768);
		sample *= static_cast<float>(gain) / 32768.0f;
		std::memcpy(output->data() + index * sizeof(sample), &sample, sizeof(sample));
	}
	return true;
}

} // namespace Kyty::Libs::Audio

#endif
