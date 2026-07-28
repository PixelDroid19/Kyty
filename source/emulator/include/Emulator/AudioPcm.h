#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIOPCM_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIOPCM_H_

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

enum class AudioPcmFormat
{
	Signed16,
	Float32,
};

[[nodiscard]] constexpr size_t AudioPcmBytesPerSample(AudioPcmFormat format)
{
	return format == AudioPcmFormat::Float32 ? sizeof(float) : sizeof(int16_t);
}

// Compute a time-based host queue capacity. This is independent of guest grain
// size, so ports with different grain contracts keep the same audible latency.
[[nodiscard]] size_t AudioPcmQueueBytes(uint32_t frequency, uint32_t channels, AudioPcmFormat format, uint32_t target_milliseconds);

// Copy interleaved PCM while applying the exact per-channel guest gains. The
// channel layout is preserved for SDL's host-device conversion stage.
[[nodiscard]] bool AudioPcmApplyChannelVolumes(const void* source, uint32_t frames, uint32_t channels, AudioPcmFormat format,
                                               const int* volumes, std::vector<uint8_t>* output);

} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AUDIOPCM_H_ */
