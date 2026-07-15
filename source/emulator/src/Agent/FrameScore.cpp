#include "Emulator/Agent/FrameScore.h"

#include "Emulator/Agent/Protocol.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {
namespace {

struct BmpInfo
{
	uint32_t width      = 0;
	uint32_t height     = 0;
	uint32_t row_bytes  = 0;
	bool     top_down   = false;
	uint32_t r_mask     = 0x00ff0000u;
	uint32_t g_mask     = 0x0000ff00u;
	uint32_t b_mask     = 0x000000ffu;
	std::vector<uint8_t> pixels; // tightly packed BGRA-ish via masks, 4 bpp
};

uint8_t ChannelFromMask(uint32_t pixel, uint32_t mask)
{
	if (mask == 0)
	{
		return 0;
	}
	uint32_t shift = 0;
	uint32_t m     = mask;
	while ((m & 1u) == 0u)
	{
		m >>= 1u;
		++shift;
	}
	return static_cast<uint8_t>((pixel & mask) >> shift);
}

bool LoadBmp32(const char* path, BmpInfo* out, std::string* error)
{
	if (path == nullptr || out == nullptr)
	{
		if (error != nullptr)
		{
			*error = "null path";
		}
		return false;
	}
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		if (error != nullptr)
		{
			*error = "open failed";
		}
		return false;
	}
	uint8_t header[54] {};
	in.read(reinterpret_cast<char*>(header), 54);
	if (!in || header[0] != 'B' || header[1] != 'M')
	{
		if (error != nullptr)
		{
			*error = "not a BMP";
		}
		return false;
	}
	const uint32_t data_offset = static_cast<uint32_t>(header[10]) | (static_cast<uint32_t>(header[11]) << 8) |
	                             (static_cast<uint32_t>(header[12]) << 16) | (static_cast<uint32_t>(header[13]) << 24);
	const int32_t width_i = static_cast<int32_t>(static_cast<uint32_t>(header[18]) | (static_cast<uint32_t>(header[19]) << 8) |
	                                             (static_cast<uint32_t>(header[20]) << 16) | (static_cast<uint32_t>(header[21]) << 24));
	const int32_t height_i = static_cast<int32_t>(static_cast<uint32_t>(header[22]) | (static_cast<uint32_t>(header[23]) << 8) |
	                                              (static_cast<uint32_t>(header[24]) << 16) | (static_cast<uint32_t>(header[25]) << 24));
	const uint16_t planes = static_cast<uint16_t>(header[26] | (header[27] << 8));
	const uint16_t bpp    = static_cast<uint16_t>(header[28] | (header[29] << 8));
	const uint32_t compression = static_cast<uint32_t>(header[30]) | (static_cast<uint32_t>(header[31]) << 8) |
	                             (static_cast<uint32_t>(header[32]) << 16) | (static_cast<uint32_t>(header[33]) << 24);
	if (planes != 1 || bpp != 32 || width_i == 0 || height_i == 0)
	{
		if (error != nullptr)
		{
			*error = "unsupported BMP (need 32-bpp)";
		}
		return false;
	}

	out->width    = static_cast<uint32_t>(width_i < 0 ? -width_i : width_i);
	out->height   = static_cast<uint32_t>(height_i < 0 ? -height_i : height_i);
	out->top_down = height_i < 0;
	out->row_bytes = ((out->width * 4u + 3u) / 4u) * 4u;

	// BI_BITFIELDS may store masks after the 40-byte DIB header.
	out->r_mask = 0x00ff0000u;
	out->g_mask = 0x0000ff00u;
	out->b_mask = 0x000000ffu;
	if (compression == 3u)
	{
		uint8_t masks[12] {};
		in.read(reinterpret_cast<char*>(masks), 12);
		if (in)
		{
			out->r_mask = static_cast<uint32_t>(masks[0]) | (static_cast<uint32_t>(masks[1]) << 8) |
			              (static_cast<uint32_t>(masks[2]) << 16) | (static_cast<uint32_t>(masks[3]) << 24);
			out->g_mask = static_cast<uint32_t>(masks[4]) | (static_cast<uint32_t>(masks[5]) << 8) |
			              (static_cast<uint32_t>(masks[6]) << 16) | (static_cast<uint32_t>(masks[7]) << 24);
			out->b_mask = static_cast<uint32_t>(masks[8]) | (static_cast<uint32_t>(masks[9]) << 8) |
			              (static_cast<uint32_t>(masks[10]) << 16) | (static_cast<uint32_t>(masks[11]) << 24);
		}
	}

	in.seekg(data_offset, std::ios::beg);
	out->pixels.resize(static_cast<size_t>(out->row_bytes) * out->height);
	in.read(reinterpret_cast<char*>(out->pixels.data()), static_cast<std::streamsize>(out->pixels.size()));
	if (!in)
	{
		if (error != nullptr)
		{
			*error = "pixel read failed";
		}
		return false;
	}
	return true;
}

void PixelAt(const BmpInfo& bmp, uint32_t x, uint32_t y, uint8_t* r, uint8_t* g, uint8_t* b)
{
	const uint32_t row = bmp.top_down ? y : (bmp.height - 1u - y);
	const size_t   off = static_cast<size_t>(row) * bmp.row_bytes + static_cast<size_t>(x) * 4u;
	const uint32_t px  = static_cast<uint32_t>(bmp.pixels[off]) | (static_cast<uint32_t>(bmp.pixels[off + 1]) << 8) |
	                    (static_cast<uint32_t>(bmp.pixels[off + 2]) << 16) | (static_cast<uint32_t>(bmp.pixels[off + 3]) << 24);
	*r = ChannelFromMask(px, bmp.r_mask);
	*g = ChannelFromMask(px, bmp.g_mask);
	*b = ChannelFromMask(px, bmp.b_mask);
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

bool ScoreNativeBmp(const char* path, FrameScoreMetrics* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = FrameScoreMetrics {};
	BmpInfo     bmp {};
	std::string error;
	if (!LoadBmp32(path, &bmp, &error))
	{
		out->verdict      = FrameVerdict::LoadFailed;
		out->verdict_name = FrameVerdictName(out->verdict);
		out->hint         = "failed to load native BMP";
		return false;
	}

	out->width  = bmp.width;
	out->height = bmp.height;

	// World crop ~full frame minus HUD strip (matches kyty_capture DEFAULT_WORLD_CROP intent).
	const uint32_t x0 = bmp.width / 20u;
	const uint32_t x1 = bmp.width - bmp.width / 20u;
	const uint32_t y0 = bmp.height / 10u;
	const uint32_t y1 = (bmp.height * 9u) / 10u;

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
			PixelAt(bmp, x, y, &r, &g, &b);
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
				PixelAt(bmp, x + 4, y, &r2, &g2, &b2);
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
		PixelAt(bmp, x, mid_y, &a[0], &a[1], &a[2]);
		PixelAt(bmp, x + step, mid_y, &b[0], &b[1], &b[2]);
		col_diff += std::abs(static_cast<int>(a[0]) - b[0]) + std::abs(static_cast<int>(a[1]) - b[1]) +
		            std::abs(static_cast<int>(a[2]) - b[2]);
		++col_samples;
	}
	for (uint32_t y = y0; y + step < y1; y += step)
	{
		uint8_t a[3] {};
		uint8_t b[3] {};
		PixelAt(bmp, mid_x, y, &a[0], &a[1], &a[2]);
		PixelAt(bmp, mid_x, y + step, &b[0], &b[1], &b[2]);
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
