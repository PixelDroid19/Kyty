#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUCLOCKMARK_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUCLOCKMARK_H_

#include <cmath>
#include <cstdint>

namespace Kyty::Libs::Graphics {

// One device timestamp and the host counter value read at the same instant.
struct GpuClockCalibration
{
	uint64_t device_ticks = 0;
	uint64_t host_counter = 0;
};

// Host counter value at device timestamp `ticks`. Timestamps keep `valid_bits` bits and wrap, so the distance to the
// calibration, taken near the mark, is sign-extended from them. A result before the counter's start is zero.
inline uint64_t GpuClockMarkHostCounter(const GpuClockCalibration& calibration, uint64_t ticks, uint32_t valid_bits,
                                        double period_ns, uint64_t host_frequency)
{
	const uint64_t mask    = valid_bits >= 64 ? ~uint64_t {0} : (uint64_t {1} << valid_bits) - 1;
	uint64_t       delta   = (ticks - calibration.device_ticks) & mask;
	const uint64_t sign    = valid_bits >= 64 ? uint64_t {1} << 63 : uint64_t {1} << (valid_bits - 1);
	const bool     before  = (delta & sign) != 0;
	delta                  = before ? ((~delta + 1) & mask) : delta;
	const double   host    = static_cast<double>(delta) * period_ns * static_cast<double>(host_frequency) / 1e9;
	const uint64_t counter = static_cast<uint64_t>(std::llround(host));
	if (!before)
	{
		return calibration.host_counter + counter;
	}
	return counter > calibration.host_counter ? 0 : calibration.host_counter - counter;
}

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUCLOCKMARK_H_ */
