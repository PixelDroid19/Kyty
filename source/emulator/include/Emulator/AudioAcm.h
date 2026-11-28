#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_ACM_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_ACM_H_

#include <cstdint>

namespace Kyty::Libs::Audio::Acm {

// Transforms of n real samples, n a power of two from 2 to kMaxTransformSize, in the audio compute library's packed
// spectrum of n floats: bin k (0 < k < n/2) as real and imaginary parts at floats 2k and 2k + 1, and the purely real
// DC and Nyquist values at floats 0 and 1. A forward then an inverse transform returns the input, so the product of
// two spectra transforms back to the linear convolution of their zero-padded inputs.
constexpr uint32_t kMaxTransformSize = 2048;

void ForwardTransform(const float* samples, uint32_t n, float* spectrum);
void InverseTransform(const float* spectrum, uint32_t n, float* samples);

// Adds gain * a * b, bin by bin, to the packed spectrum sum of n floats.
void MultiplyAccumulate(const float* a, const float* b, float gain, uint32_t n, float* sum);

// IEEE binary16 values, rounded to nearest even on narrowing.
float    HalfToFloat(uint16_t value);
uint16_t FloatToHalf(float value);

} // namespace Kyty::Libs::Audio::Acm

#endif /* EMULATOR_INCLUDE_EMULATOR_AUDIO_ACM_H_ */
