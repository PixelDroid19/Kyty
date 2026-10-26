#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_CAPTUREMETRICS_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_CAPTUREMETRICS_H_

#include "Kyty/Agent/Runner/JsonValue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Kyty::Agent::Runner {

struct RgbImage
{
	uint32_t             width  = 0;
	uint32_t             height = 0;
	std::vector<uint8_t> rgb; // tightly packed RGB8

	[[nodiscard]] const uint8_t* Pixel(uint32_t x, uint32_t y) const { return rgb.data() + (static_cast<size_t>(y) * width + x) * 3u; }
};

// Reads a whole file, refusing files larger than max_bytes.
[[nodiscard]] bool ReadFileBytes(const std::string& path, size_t max_bytes, std::vector<uint8_t>* out, std::string* error);

// Decodes PNG bytes to RGB8 (alpha dropped). Refuses non-PNG data and images
// whose width * height exceeds max_pixels before decoding.
[[nodiscard]] bool DecodePng(const std::vector<uint8_t>& bytes, uint64_t max_pixels, RgbImage* out, std::string* error);

// Material-health metrics for one native PNG capture. The world crop and HUD
// crop, thresholds and rounding define the capture manifest schema 1.
[[nodiscard]] bool ScoreImage(const std::string& path, Json* metrics, std::string* error);

// Worst-case world metrics over a capture set; fails on an empty set.
[[nodiscard]] bool AggregateCaptures(const Json& captures, Json* aggregate, std::string* error);

// Relative and absolute material checks of current world metrics against a baseline.
[[nodiscard]] Json CompareMetrics(const Json& current, const Json& baseline);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_CAPTUREMETRICS_H_ */
