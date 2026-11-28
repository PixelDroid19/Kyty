#include "Emulator/AudioAcm.h"

#include "Kyty/Core/DbgAssert.h"

#include <cmath>
#include <complex>
#include <cstring>
#include <utility>
#include <vector>

namespace Kyty::Libs::Audio::Acm {

namespace {

using Complex = std::complex<float>;

// exp(-2 pi i j / kMaxTransformSize); a transform of n samples reads every (kMaxTransformSize / n)-th entry.
const std::vector<Complex>& Twiddles()
{
	static const std::vector<Complex> table = []
	{
		std::vector<Complex> values(kMaxTransformSize / 2u);
		for (uint32_t j = 0; j < values.size(); j++)
		{
			const double angle = -2.0 * 3.14159265358979323846 * static_cast<double>(j) / kMaxTransformSize;
			values[j]          = Complex(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
		}
		return values;
	}();
	return table;
}

void ReorderBitReversed(std::vector<Complex>* x)
{
	const auto n = static_cast<uint32_t>(x->size());
	for (uint32_t i = 1, j = 0; i < n; i++)
	{
		uint32_t bit = n >> 1u;
		for (; (j & bit) != 0; bit >>= 1u)
		{
			j ^= bit;
		}
		j ^= bit;
		if (i < j)
		{
			std::swap((*x)[i], (*x)[j]);
		}
	}
}

// Unscaled in-place radix-2 DFT; the inverse direction conjugates the twiddles.
void Transform(std::vector<Complex>* x, bool inverse)
{
	const auto n = static_cast<uint32_t>(x->size());
	EXIT_IF(n < 2u || n > kMaxTransformSize || (n & (n - 1u)) != 0);
	ReorderBitReversed(x);
	const auto& twiddles = Twiddles();
	for (uint32_t length = 2; length <= n; length <<= 1u)
	{
		const uint32_t half   = length / 2u;
		const uint32_t stride = kMaxTransformSize / length;
		for (uint32_t start = 0; start < n; start += length)
		{
			for (uint32_t k = 0; k < half; k++)
			{
				const Complex w = inverse ? std::conj(twiddles[k * stride]) : twiddles[k * stride];
				const Complex u = (*x)[start + k];
				const Complex v = (*x)[start + k + half] * w;
				(*x)[start + k]        = u + v;
				(*x)[start + k + half] = u - v;
			}
		}
	}
}

} // namespace

void ForwardTransform(const float* samples, uint32_t n, float* spectrum)
{
	std::vector<Complex> x(samples, samples + n);
	Transform(&x, false);
	spectrum[0] = x[0].real();
	spectrum[1] = x[n / 2u].real();
	for (uint32_t k = 1; k < n / 2u; k++)
	{
		spectrum[2u * k]      = x[k].real();
		spectrum[2u * k + 1u] = x[k].imag();
	}
}

void InverseTransform(const float* spectrum, uint32_t n, float* samples)
{
	std::vector<Complex> x(n);
	x[0]      = spectrum[0];
	x[n / 2u] = spectrum[1];
	for (uint32_t k = 1; k < n / 2u; k++)
	{
		x[k]     = Complex(spectrum[2u * k], spectrum[2u * k + 1u]);
		x[n - k] = std::conj(x[k]);
	}
	Transform(&x, true);
	const float scale = 1.0f / static_cast<float>(n);
	for (uint32_t i = 0; i < n; i++)
	{
		samples[i] = x[i].real() * scale;
	}
}

void MultiplyAccumulate(const float* a, const float* b, float gain, uint32_t n, float* sum)
{
	sum[0] += gain * a[0] * b[0];
	sum[1] += gain * a[1] * b[1];
	for (uint32_t i = 2; i < n; i += 2)
	{
		sum[i] += gain * (a[i] * b[i] - a[i + 1] * b[i + 1]);
		sum[i + 1] += gain * (a[i] * b[i + 1] + a[i + 1] * b[i]);
	}
}

float HalfToFloat(uint16_t value)
{
	const uint32_t sign     = static_cast<uint32_t>(value & 0x8000u) << 16u;
	const uint32_t exponent = (value >> 10u) & 0x1fu;
	const uint32_t mantissa = value & 0x3ffu;
	if (exponent == 0)
	{
		const float magnitude = std::ldexp(static_cast<float>(mantissa), -24);
		return sign != 0 ? -magnitude : magnitude;
	}
	const uint32_t bits = exponent == 0x1fu ? sign | 0x7f800000u | (mantissa << 13u) : sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
	float          result = 0.0f;
	std::memcpy(&result, &bits, sizeof(result));
	return result;
}

uint16_t FloatToHalf(float value)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	const auto     sign     = static_cast<uint16_t>((bits >> 16u) & 0x8000u);
	const uint32_t exponent = (bits >> 23u) & 0xffu;
	const uint32_t mantissa = bits & 0x7fffffu;
	if (exponent == 0xffu)
	{
		return static_cast<uint16_t>(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));
	}
	const int32_t half_exponent = static_cast<int32_t>(exponent) - 112;
	if (half_exponent >= 0x1f)
	{
		return static_cast<uint16_t>(sign | 0x7c00u);
	}
	if (half_exponent <= 0)
	{
		// Subnormal or zero: shift the significand, with its implicit bit, into the 10-bit field.
		if (half_exponent < -10)
		{
			return sign;
		}
		const uint32_t significand = mantissa | 0x800000u;
		const uint32_t shift       = static_cast<uint32_t>(14 - half_exponent);
		const uint32_t kept        = significand >> shift;
		const uint32_t rest        = significand & ((1u << shift) - 1u);
		const uint32_t halfway     = 1u << (shift - 1u);
		const uint32_t rounded     = kept + ((rest > halfway || (rest == halfway && (kept & 1u) != 0)) ? 1u : 0u);
		return static_cast<uint16_t>(sign | rounded);
	}
	const uint32_t kept    = (static_cast<uint32_t>(half_exponent) << 10u) | (mantissa >> 13u);
	const uint32_t rest    = mantissa & 0x1fffu;
	const uint32_t rounded = kept + ((rest > 0x1000u || (rest == 0x1000u && (kept & 1u) != 0)) ? 1u : 0u);
	return static_cast<uint16_t>(sign | rounded);
}

} // namespace Kyty::Libs::Audio::Acm
