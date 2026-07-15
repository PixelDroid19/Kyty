#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_FRAMESCORE_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_FRAMESCORE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {

enum class FrameVerdict: uint8_t
{
	Healthy        = 0,
	WhiteWorld     = 1,
	HotCorruption  = 2, // blown yellow/red blocks like broken bloom/lighting
	LowEntropy     = 3,
	Stripey        = 4,
	LoadFailed     = 5,
};

struct FrameScoreMetrics
{
	uint32_t width  = 0;
	uint32_t height = 0;
	double   white_ratio      = 0.0;
	double   saturated_ratio  = 0.0;
	double   blowout_ratio    = 0.0; // near-max luminance (bloom/clamp)
	double   hot_block_ratio  = 0.0; // dominant hot R/G with cold B (yellow/red slabs)
	double   sparkle_ratio    = 0.0; // high local contrast outliers
	double   entropy          = 0.0;
	uint32_t color_bins       = 0;
	bool     stripey          = false;
	FrameVerdict verdict      = FrameVerdict::LoadFailed;
	const char*  verdict_name = "load_failed";
	const char*  hint         = "";
};

const char* FrameVerdictName(FrameVerdict verdict);

// Scores a native VideoOut BMP (SDL 32-bpp). Path must be absolute or cwd-relative.
// Returns false only on I/O/parse failure (metrics.verdict = LoadFailed).
bool ScoreNativeBmp(const char* path, FrameScoreMetrics* out);

std::string FrameScoreToJson(const FrameScoreMetrics& metrics, const char* path);

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_FRAMESCORE_H_ */
