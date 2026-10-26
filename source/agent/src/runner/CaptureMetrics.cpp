#include "Kyty/Agent/Runner/CaptureMetrics.h"

#include "Kyty/Agent/Runner/Sha256.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#include "stb_image.h"

namespace Kyty::Agent::Runner {
namespace {

struct Crop
{
	double x0, y0, x1, y1;
};

constexpr Crop kWorldCrop {0.18, 0.08, 0.92, 0.78};
constexpr Crop kHudCrop {0.02, 0.82, 0.35, 0.96};

constexpr uint64_t kMaxScoredPixels = 8192ull * 8192ull;
constexpr size_t   kMaxPngBytes     = 256u * 1024u * 1024u;

struct Bounds
{
	uint32_t x0, y0, x1, y1;
};

uint32_t Clamp(int64_t value, int64_t low, int64_t high)
{
	return static_cast<uint32_t>(std::max(low, std::min(high, value)));
}

Bounds CropBounds(const RgbImage& image, const Crop& crop)
{
	const auto width  = static_cast<int64_t>(image.width);
	const auto height = static_cast<int64_t>(image.height);
	return {Clamp(static_cast<int64_t>(static_cast<double>(width) * crop.x0), 0, width - 1),
	        Clamp(static_cast<int64_t>(static_cast<double>(height) * crop.y0), 0, height - 1),
	        Clamp(static_cast<int64_t>(static_cast<double>(width) * crop.x1), 1, width),
	        Clamp(static_cast<int64_t>(static_cast<double>(height) * crop.y1), 1, height)};
}

uint32_t PixelDistance(const uint8_t* a, const uint8_t* b)
{
	uint32_t sum = 0;
	for (int i = 0; i < 3; ++i)
	{
		sum += static_cast<uint32_t>(std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
	}
	return sum;
}

double Ratio(uint64_t count, uint64_t total)
{
	return static_cast<double>(count) / static_cast<double>(std::max<uint64_t>(total, 1));
}

bool MaterialHealthy(double white_ratio, double entropy, int64_t colors, bool stripey)
{
	return white_ratio < 0.35 && entropy >= 2.5 && colors >= 80 && !stripey;
}

double GreenHudRatio(const RgbImage& image)
{
	const Bounds hud   = CropBounds(image, kHudCrop);
	uint64_t     green = 0;
	uint64_t     total = 0;
	for (uint32_t y = hud.y0; y < hud.y1; ++y)
	{
		for (uint32_t x = hud.x0; x < hud.x1; x += 2)
		{
			const uint8_t* p = image.Pixel(x, y);
			++total;
			green += static_cast<uint64_t>(p[1] >= 140 && p[1] > p[0] + 40 && p[1] > p[2] + 40);
		}
	}
	return Ratio(green, total);
}

Json WorldMetrics(const RgbImage& image)
{
	const Bounds world = CropBounds(image, kWorldCrop);
	uint64_t     total = 0;
	uint64_t     white = 0;
	uint64_t     saturated = 0;
	// 4-bit-per-channel histogram; first-seen order keeps the entropy sum identical to the reference runner.
	std::vector<uint64_t> histogram(4096, 0);
	std::vector<uint32_t> seen_order;
	for (uint32_t y = world.y0; y < world.y1; ++y)
	{
		for (uint32_t x = world.x0; x < world.x1; ++x)
		{
			const uint8_t* p = image.Pixel(x, y);
			++total;
			white += static_cast<uint64_t>(p[0] >= 245 && p[1] >= 245 && p[2] >= 245);
			const uint8_t high = std::max({p[0], p[1], p[2]});
			const uint8_t low  = std::min({p[0], p[1], p[2]});
			saturated += static_cast<uint64_t>(high >= 245 && low <= 90);
			const uint32_t bin = (static_cast<uint32_t>(p[0] >> 4u) << 8u) | (static_cast<uint32_t>(p[1] >> 4u) << 4u) | (p[2] >> 4u);
			if (histogram[bin]++ == 0)
			{
				seen_order.push_back(bin);
			}
		}
	}

	constexpr uint32_t step        = 4;
	const uint32_t     mid_y       = (world.y0 + world.y1) / 2;
	const uint32_t     mid_x       = (world.x0 + world.x1) / 2;
	double             col_diff    = 0.0;
	double             row_diff    = 0.0;
	uint64_t           col_samples = 0;
	uint64_t           row_samples = 0;
	for (uint32_t x = world.x0; x + step < std::max(world.x0 + step, world.x1); x += step)
	{
		col_diff += PixelDistance(image.Pixel(x, mid_y), image.Pixel(x + step, mid_y));
		++col_samples;
	}
	for (uint32_t y = world.y0; y + step < std::max(world.y0 + step, world.y1); y += step)
	{
		row_diff += PixelDistance(image.Pixel(mid_x, y), image.Pixel(mid_x, y + step));
		++row_samples;
	}

	double entropy = 0.0;
	for (const uint32_t bin: seen_order)
	{
		const double probability = static_cast<double>(histogram[bin]) / static_cast<double>(std::max<uint64_t>(total, 1));
		entropy -= probability * std::log2(probability);
	}
	const double avg_col = col_diff / static_cast<double>(std::max<uint64_t>(col_samples, 1));
	const double avg_row = row_diff / static_cast<double>(std::max<uint64_t>(row_samples, 1));
	const bool   stripey = avg_col > 40 && avg_row < 8 && avg_col > avg_row * 6;
	const double white_ratio = Ratio(white, total);
	const auto   colors      = static_cast<int64_t>(seen_order.size());

	Json out = Json::MakeObject();
	out.Set("white_ratio", RoundDecimal(white_ratio, 6));
	out.Set("saturated_ratio", RoundDecimal(Ratio(saturated, total), 6));
	out.Set("entropy", RoundDecimal(entropy, 4));
	out.Set("unique_quantized_colors", colors);
	out.Set("avg_column_diff", RoundDecimal(avg_col, 4));
	out.Set("avg_row_diff", RoundDecimal(avg_row, 4));
	out.Set("stripey", stripey);
	out.Set("green_hud_ratio", RoundDecimal(GreenHudRatio(image), 6));
	out.Set("material_healthy", MaterialHealthy(white_ratio, entropy, colors, stripey));
	return out;
}

} // namespace

bool ReadFileBytes(const std::string& path, size_t max_bytes, std::vector<uint8_t>* out, std::string* error)
{
	std::FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
	{
		*error = "cannot open " + path;
		return false;
	}
	out->clear();
	std::array<uint8_t, 65536> chunk {};
	for (;;)
	{
		const size_t read = std::fread(chunk.data(), 1, chunk.size(), file);
		if (out->size() + read > max_bytes)
		{
			std::fclose(file);
			*error = "file too large: " + path;
			return false;
		}
		out->insert(out->end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read));
		if (read < chunk.size())
		{
			break;
		}
	}
	const bool failed = std::ferror(file) != 0;
	std::fclose(file);
	if (failed)
	{
		*error = "read failed: " + path;
		return false;
	}
	return true;
}

bool DecodePng(const std::vector<uint8_t>& bytes, uint64_t max_pixels, RgbImage* out, std::string* error)
{
	static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	if (bytes.size() < 24 || std::memcmp(bytes.data(), kSignature, 8) != 0 || std::memcmp(bytes.data() + 12, "IHDR", 4) != 0)
	{
		*error = "not a PNG image";
		return false;
	}
	auto big_endian = [&](size_t offset)
	{ return (uint32_t {bytes[offset]} << 24u) | (uint32_t {bytes[offset + 1]} << 16u) | (uint32_t {bytes[offset + 2]} << 8u) | bytes[offset + 3]; };
	const uint32_t width  = big_endian(16);
	const uint32_t height = big_endian(20);
	if (width == 0 || height == 0 || static_cast<uint64_t>(width) * height > max_pixels)
	{
		*error = "PNG extent out of bounds";
		return false;
	}
	int            decoded_width  = 0;
	int            decoded_height = 0;
	int            channels       = 0;
	unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &decoded_width, &decoded_height, &channels, 3);
	if (pixels == nullptr || static_cast<uint32_t>(decoded_width) != width || static_cast<uint32_t>(decoded_height) != height)
	{
		stbi_image_free(pixels);
		*error = "PNG decode failed";
		return false;
	}
	out->width  = width;
	out->height = height;
	out->rgb.assign(pixels, pixels + static_cast<size_t>(width) * height * 3u);
	stbi_image_free(pixels);
	return true;
}

bool ScoreImage(const std::string& path, Json* metrics, std::string* error)
{
	const std::filesystem::path file(path);
	std::string                 extension = file.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (extension != ".png")
	{
		*error = "native capture scoring requires PNG: " + path;
		return false;
	}
	std::vector<uint8_t> bytes;
	RgbImage             image;
	if (!ReadFileBytes(path, kMaxPngBytes, &bytes, error) || !DecodePng(bytes, kMaxScoredPixels, &image, error))
	{
		return false;
	}
	Json out = Json::MakeObject();
	out.Set("path", file.filename().string());
	out.Set("sha256", Sha256Hex(bytes.data(), bytes.size()));
	out.Set("width", static_cast<int64_t>(image.width));
	out.Set("height", static_cast<int64_t>(image.height));
	out.Set("backend", "stb_image");
	out.Set("assessment", "material_health_only");
	out.Set("scene_status", "unknown");
	out.Set("world", WorldMetrics(image));
	*metrics = std::move(out);
	return true;
}

bool AggregateCaptures(const Json& captures, Json* aggregate, std::string* error)
{
	if (!captures.IsArray() || captures.Size() == 0)
	{
		*error = "capture contains no samples";
		return false;
	}
	double white      = -1e300;
	double saturated  = -1e300;
	double entropy    = 1e300;
	int64_t colors    = INT64_MAX;
	double column     = -1e300;
	double row        = 1e300;
	bool   stripey    = false;
	double green      = 1e300;
	bool   healthy    = true;
	for (const Json& capture: captures.Items())
	{
		const Json& world = capture["world"];
		if (!world.IsObject())
		{
			*error = "capture sample has no world metrics";
			return false;
		}
		white     = std::max(white, world["white_ratio"].AsNumber());
		saturated = std::max(saturated, world["saturated_ratio"].AsNumber());
		entropy   = std::min(entropy, world["entropy"].AsNumber());
		colors    = std::min(colors, world["unique_quantized_colors"].AsInt());
		column    = std::max(column, world["avg_column_diff"].AsNumber());
		row       = std::min(row, world["avg_row_diff"].AsNumber());
		stripey   = stripey || world["stripey"].Truthy();
		green     = std::min(green, world["green_hud_ratio"].AsNumber());
		healthy   = healthy && MaterialHealthy(world["white_ratio"].AsNumber(), world["entropy"].AsNumber(),
		                                       world["unique_quantized_colors"].AsInt(), world["stripey"].Truthy());
	}
	Json world = Json::MakeObject();
	world.Set("white_ratio", white);
	world.Set("saturated_ratio", saturated);
	world.Set("entropy", entropy);
	world.Set("unique_quantized_colors", colors);
	world.Set("avg_column_diff", column);
	world.Set("avg_row_diff", row);
	world.Set("stripey", stripey);
	world.Set("green_hud_ratio", green);
	world.Set("material_healthy", healthy);
	Json out = Json::MakeObject();
	out.Set("assessment", "material_health_only");
	out.Set("scene_status", "unknown");
	out.Set("world", std::move(world));
	*aggregate = std::move(out);
	return true;
}

Json CompareMetrics(const Json& current, const Json& baseline)
{
	const Json&   now      = current["world"];
	const Json&   base     = baseline["world"];
	const double  white    = now["white_ratio"].AsNumber();
	const double  entropy  = now["entropy"].AsNumber();
	const int64_t colors   = now["unique_quantized_colors"].AsInt();
	const int64_t min_colors = std::max<int64_t>(20, static_cast<int64_t>(static_cast<double>(base["unique_quantized_colors"].AsInt()) * 0.55));

	Json checks = Json::MakeObject();
	checks.Set("white_ratio_not_worse", white <= base["white_ratio"].AsNumber() + 0.08);
	checks.Set("entropy_not_collapsed", entropy >= base["entropy"].AsNumber() - 1.0);
	checks.Set("colors_not_collapsed", colors >= min_colors);
	checks.Set("not_stripey", !now["stripey"].Truthy());
	checks.Set("absolute_world_gate", white < 0.35 && entropy >= 2.5 && colors >= 80);
	bool pass = true;
	for (const auto& [name, value]: checks.Members())
	{
		pass = pass && value.AsBool();
	}

	Json delta = Json::MakeObject();
	delta.Set("white_ratio", RoundDecimal(white - base["white_ratio"].AsNumber(), 6));
	delta.Set("entropy", RoundDecimal(entropy - base["entropy"].AsNumber(), 4));
	delta.Set("unique_quantized_colors", colors - base["unique_quantized_colors"].AsInt());

	Json out = Json::MakeObject();
	out.Set("pass", pass);
	out.Set("assessment", "material_health_only");
	out.Set("scene_status", "unknown");
	out.Set("checks", std::move(checks));
	out.Set("delta", std::move(delta));
	return out;
}

} // namespace Kyty::Agent::Runner
