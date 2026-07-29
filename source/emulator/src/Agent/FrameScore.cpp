#include "Emulator/Agent/FrameScore.h"
#include "Kyty/Agent/Json.h"

#include "Emulator/Agent/Protocol.h"

#include "stb_image.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {
using Kyty::Agent::JsonString;

namespace {

struct PngInfo
{
	uint32_t       width  = 0;
	uint32_t       height = 0;
	unsigned char* pixels = nullptr; // tightly packed RGBA8
};

void FreePng(PngInfo* png)
{
	if (png != nullptr && png->pixels != nullptr)
	{
		stbi_image_free(png->pixels);
		png->pixels = nullptr;
	}
}

bool LoadPngRgba8(const char* path, PngInfo* out, std::string* error)
{
	if (path == nullptr || out == nullptr)
	{
		if (error != nullptr)
		{
			*error = "null path";
		}
		return false;
	}
	int width = 0;
	int height = 0;
	int channels = 0;
	out->pixels = stbi_load(path, &width, &height, &channels, 4);
	if (out->pixels == nullptr || width <= 0 || height <= 0)
	{
		if (error != nullptr)
		{
			*error = "failed to load PNG";
		}
		return false;
	}
	if (static_cast<uint32_t>(width) > 8192u || static_cast<uint32_t>(height) > 8192u)
	{
		FreePng(out);
		if (error != nullptr)
		{
			*error = "PNG dimensions exceed capture limit";
		}
		return false;
	}
	out->width  = static_cast<uint32_t>(width);
	out->height = static_cast<uint32_t>(height);
	return true;
}

void PixelAt(const PngInfo& png, uint32_t x, uint32_t y, uint8_t* r, uint8_t* g, uint8_t* b)
{
	const size_t off = (static_cast<size_t>(y) * png.width + x) * 4u;
	*r               = png.pixels[off];
	*g               = png.pixels[off + 1u];
	*b               = png.pixels[off + 2u];
}

void Classify(FrameScoreMetrics* out)
{
	// Near-white frames also trip luminance blowout; prefer white_world when the crop is
	// mostly white and not dominated by hot yellow/red slabs.
	if (out->white_ratio >= 0.35 && out->hot_block_ratio < 0.08)
	{
		out->verdict      = FrameVerdict::WhiteWorld;
		out->verdict_name = FrameVerdictName(out->verdict);
		out->hint         = "near-white world; check RT layout/WriteBack/clear";
		return;
	}
	if (out->hot_block_ratio >= 0.08 || out->blowout_ratio >= 0.20)
	{
		out->verdict      = FrameVerdict::HotCorruption;
		out->verdict_name = FrameVerdictName(out->verdict);
		out->hint         = "hot yellow/red or bloom blowout; inspect lighting/RT sampling/format";
		return;
	}
	if (out->stripey)
	{
		out->verdict      = FrameVerdict::Stripey;
		out->verdict_name = FrameVerdictName(out->verdict);
		out->hint         = "directional stripe pattern; check tiling/pitch";
		return;
	}
	if (out->entropy < 2.5 || out->color_bins < 80)
	{
		out->verdict      = FrameVerdict::LowEntropy;
		out->verdict_name = FrameVerdictName(out->verdict);
		out->hint         = "collapsed color diversity; check textures/clears";
		return;
	}
	out->verdict      = FrameVerdict::Healthy;
	out->verdict_name = FrameVerdictName(out->verdict);
	out->hint         = "metrics within diagnostic gameplay band";
}

} // namespace

const char* FrameVerdictName(FrameVerdict verdict)
{
	switch (verdict)
	{
		case FrameVerdict::Healthy: return "healthy";
		case FrameVerdict::WhiteWorld: return "white_world";
		case FrameVerdict::HotCorruption: return "hot_corruption";
		case FrameVerdict::LowEntropy: return "low_entropy";
		case FrameVerdict::Stripey: return "stripey";
		case FrameVerdict::LoadFailed: return "load_failed";
	}
	return "load_failed";
}

bool ScoreNativePng(const char* path, FrameScoreMetrics* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = FrameScoreMetrics {};
	PngInfo     png {};
	std::string error;
	if (!LoadPngRgba8(path, &png, &error))
	{
		out->verdict      = FrameVerdict::LoadFailed;
		out->verdict_name = FrameVerdictName(out->verdict);
		out->hint         = "failed to load native PNG";
		return false;
	}

	out->width  = png.width;
	out->height = png.height;

	// World crop ~full frame minus HUD strip (matches kyty_capture DEFAULT_WORLD_CROP intent).
	const uint32_t x0 = png.width / 20u;
	const uint32_t x1 = png.width - png.width / 20u;
	const uint32_t y0 = png.height / 10u;
	const uint32_t y1 = (png.height * 9u) / 10u;

	uint64_t total = 0;
	uint64_t white = 0;
	uint64_t saturated = 0;
	uint64_t blowout = 0;
	uint64_t hot     = 0;
	uint64_t sparkle = 0;
	uint32_t hist[4096] {};
	uint32_t bins_used = 0;

	double col_diff = 0.0;
	double row_diff = 0.0;
	uint32_t col_samples = 0;
	uint32_t row_samples = 0;

	for (uint32_t y = y0; y < y1; ++y)
	{
		for (uint32_t x = x0; x < x1; ++x)
		{
			uint8_t r = 0;
			uint8_t g = 0;
			uint8_t b = 0;
			PixelAt(png, x, y, &r, &g, &b);
			++total;
			white += (r >= 245 && g >= 245 && b >= 245) ? 1u : 0u;
			const uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
			const uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
			saturated += (mx >= 245 && mn <= 90) ? 1u : 0u;
			const uint32_t lum = static_cast<uint32_t>(r) + g + b;
			blowout += (mx >= 250 && lum >= 600) ? 1u : 0u;
			// Hot yellow/red slab: strong R or G, weak B, high peak.
			hot += (mx >= 220 && (r >= 200 || g >= 200) && b <= static_cast<uint8_t>(mx * 55 / 100)) ? 1u : 0u;

			const uint32_t key = ((static_cast<uint32_t>(r) >> 4) << 8) | ((static_cast<uint32_t>(g) >> 4) << 4) | (static_cast<uint32_t>(b) >> 4);
			if (hist[key] == 0)
			{
				++bins_used;
			}
			++hist[key];

			// Sparse sparkle probe: compare to a neighbor 4px away.
			if (((x + y) & 7u) == 0u && x + 4 < x1)
			{
				uint8_t r2 = 0;
				uint8_t g2 = 0;
				uint8_t b2 = 0;
				PixelAt(png, x + 4, y, &r2, &g2, &b2);
				const int dr = std::abs(static_cast<int>(r) - r2);
				const int dg = std::abs(static_cast<int>(g) - g2);
				const int db = std::abs(static_cast<int>(b) - b2);
				if (dr + dg + db >= 180)
				{
					++sparkle;
				}
			}
		}
	}

	const uint32_t step = 4;
	const uint32_t mid_y = (y0 + y1) / 2u;
	const uint32_t mid_x = (x0 + x1) / 2u;
	for (uint32_t x = x0; x + step < x1; x += step)
	{
		uint8_t a[3] {};
		uint8_t b[3] {};
		PixelAt(png, x, mid_y, &a[0], &a[1], &a[2]);
		PixelAt(png, x + step, mid_y, &b[0], &b[1], &b[2]);
		col_diff += std::abs(static_cast<int>(a[0]) - b[0]) + std::abs(static_cast<int>(a[1]) - b[1]) +
		            std::abs(static_cast<int>(a[2]) - b[2]);
		++col_samples;
	}
	for (uint32_t y = y0; y + step < y1; y += step)
	{
		uint8_t a[3] {};
		uint8_t b[3] {};
		PixelAt(png, mid_x, y, &a[0], &a[1], &a[2]);
		PixelAt(png, mid_x, y + step, &b[0], &b[1], &b[2]);
		row_diff += std::abs(static_cast<int>(a[0]) - b[0]) + std::abs(static_cast<int>(a[1]) - b[1]) +
		            std::abs(static_cast<int>(a[2]) - b[2]);
		++row_samples;
	}

	double entropy = 0.0;
	for (uint32_t c : hist)
	{
		if (c == 0)
		{
			continue;
		}
		const double p = static_cast<double>(c) / static_cast<double>(total == 0 ? 1 : total);
		entropy -= p * std::log2(p);
	}

	const double avg_col = col_diff / static_cast<double>(col_samples == 0 ? 1 : col_samples);
	const double avg_row = row_diff / static_cast<double>(row_samples == 0 ? 1 : row_samples);
	out->white_ratio     = static_cast<double>(white) / static_cast<double>(total == 0 ? 1 : total);
	out->saturated_ratio = static_cast<double>(saturated) / static_cast<double>(total == 0 ? 1 : total);
	out->blowout_ratio   = static_cast<double>(blowout) / static_cast<double>(total == 0 ? 1 : total);
	out->hot_block_ratio = static_cast<double>(hot) / static_cast<double>(total == 0 ? 1 : total);
	const uint64_t sparkle_den = total / 8u == 0 ? 1u : total / 8u;
	out->sparkle_ratio   = static_cast<double>(sparkle) / static_cast<double>(sparkle_den);
	out->entropy         = entropy;
	out->color_bins      = bins_used;
	out->stripey         = avg_col > 40.0 && avg_row < 8.0 && avg_col > avg_row * 6.0;
	Classify(out);
	FreePng(&png);
	return true;
}

std::string FrameScoreToJson(const FrameScoreMetrics& metrics, const char* path)
{
	char buf[1024];
	std::snprintf(
	    buf, sizeof(buf),
	    "{\"path\":%s,\"width\":%u,\"height\":%u,\"verdict\":%s,\"hint\":%s,"
	    "\"white_ratio\":%.6f,\"saturated_ratio\":%.6f,\"blowout_ratio\":%.6f,\"hot_block_ratio\":%.6f,"
	    "\"sparkle_ratio\":%.6f,\"entropy\":%.4f,\"color_bins\":%u,\"stripey\":%s,\"healthy\":%s}",
	    JsonString(path != nullptr ? path : "").c_str(), metrics.width, metrics.height, JsonString(metrics.verdict_name).c_str(),
	    JsonString(metrics.hint).c_str(), metrics.white_ratio, metrics.saturated_ratio, metrics.blowout_ratio, metrics.hot_block_ratio,
	    metrics.sparkle_ratio, metrics.entropy, metrics.color_bins, metrics.stripey ? "true" : "false",
	    metrics.verdict == FrameVerdict::Healthy ? "true" : "false");
	return std::string(buf);
}

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED
