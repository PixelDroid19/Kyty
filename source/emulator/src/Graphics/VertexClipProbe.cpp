#include "Emulator/Graphics/VertexClipProbe.h"

#include <charconv>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

// VCPROB8 plus a reserved low byte for descriptor-set variants. Revision 8
// expands the shared host-only raw aggregate with post-sample RGBA statistics.
constexpr uint64_t kVertexClipProbeDiagnosticRevision = 0x564350524f423800ull;
// PSPR keeps sample-result variants separate from both ordinary modules and
// coordinate-input probes. Bits [39:8] hold the explicit ImageSampleB ordinal.
constexpr uint64_t kPixelSampleProbeDiagnosticRevision = 0x5053505200000000ull;
// PMR revision 2 records raster coverage in addition to final color. The upper
// 28 bits stay separate from the lower 36-bit ordinal/target/set payload.
constexpr uint64_t kPixelMrtProbeDiagnosticRevision = 0x504d526000000000ull;
constexpr uint64_t kAttachmentReadbackMaxBytes       = 64ull * 1024ull * 1024ull;

bool ParseExactHexChecksum(const char* text, uint64_t* value)
{
	if (text == nullptr || value == nullptr || std::strlen(text) != 16u)
	{
		return false;
	}
	for (const char* ptr = text; *ptr != '\0'; ++ptr)
	{
		const bool digit = *ptr >= '0' && *ptr <= '9';
		const bool lower = *ptr >= 'a' && *ptr <= 'f';
		const bool upper = *ptr >= 'A' && *ptr <= 'F';
		if (!digit && !lower && !upper)
		{
			return false;
		}
	}
	const auto [end, error] = std::from_chars(text, text + 16, *value, 16);
	return error == std::errc {} && end == text + 16;
}

struct VertexClipProbeDrawSelector
{
	bool     indexed = false;
	uint32_t count   = 0;
};

bool ParseDrawSelector(const char* text, VertexClipProbeDrawSelector* selector)
{
	if (text == nullptr || selector == nullptr)
	{
		return false;
	}
	const char* count_text = nullptr;
	if (std::strncmp(text, "indexed:", 8u) == 0)
	{
		selector->indexed = true;
		count_text        = text + 8;
	} else if (std::strncmp(text, "auto:", 5u) == 0)
	{
		selector->indexed = false;
		count_text        = text + 5;
	} else
	{
		return false;
	}
	if (*count_text == '\0')
	{
		return false;
	}
	for (const char* ptr = count_text; *ptr != '\0'; ++ptr)
	{
		if (*ptr < '0' || *ptr > '9')
		{
			return false;
		}
	}
	const auto [end, error] = std::from_chars(count_text, count_text + std::strlen(count_text), selector->count, 10);
	return error == std::errc {} && *end == '\0';
}

bool ParseOptionalMinPresent(const char* text, uint64_t* min_present)
{
	if (min_present == nullptr)
	{
		return false;
	}
	*min_present = 0;
	if (text == nullptr || *text == '\0')
	{
		return true;
	}
	for (const char* ptr = text; *ptr != '\0'; ++ptr)
	{
		if (*ptr < '0' || *ptr > '9')
		{
			return false;
		}
	}
	const auto [end, error] = std::from_chars(text, text + std::strlen(text), *min_present, 10);
	return error == std::errc {} && *end == '\0';
}

bool ParseOptionalMatchOrdinal(const char* text, uint32_t* match_ordinal)
{
	if (match_ordinal == nullptr)
	{
		return false;
	}
	*match_ordinal = 0;
	if (text == nullptr || *text == '\0')
	{
		return true;
	}
	for (const char* ptr = text; *ptr != '\0'; ++ptr)
	{
		if (*ptr < '0' || *ptr > '9')
		{
			return false;
		}
	}
	const auto [end, error] = std::from_chars(text, text + std::strlen(text), *match_ordinal, 10);
	return error == std::errc {} && *end == '\0';
}

bool ParseOptionalPositiveCount(const char* text, uint32_t* value)
{
	if (value == nullptr)
	{
		return false;
	}
	*value = 1u;
	if (text == nullptr || *text == '\0')
	{
		return true;
	}
	for (const char* ptr = text; *ptr != '\0'; ++ptr)
	{
		if (*ptr < '0' || *ptr > '9')
		{
			return false;
		}
	}
	const auto [end, error] = std::from_chars(text, text + std::strlen(text), *value, 10);
	return error == std::errc {} && *end == '\0' && *value != 0u;
}

bool ParseOptionalStrictBoolean(const char* text, bool* value)
{
	if (value == nullptr)
	{
		return false;
	}
	*value = false;
	if (text == nullptr || *text == '\0' || std::strcmp(text, "0") == 0)
	{
		return true;
	}
	if (std::strcmp(text, "1") == 0)
	{
		*value = true;
		return true;
	}
	return false;
}

bool ParsePixelSampleSelector(const char* text, uint64_t* code_id, uint32_t* ordinal)
{
	if (text == nullptr || code_id == nullptr || ordinal == nullptr || std::strlen(text) < 19u || text[16] != ':' ||
	    text[17] != '@' || text[18] == '\0')
	{
		return false;
	}
	for (uint32_t index = 0; index < 16u; ++index)
	{
		const char value = text[index];
		const bool digit = value >= '0' && value <= '9';
		const bool lower = value >= 'a' && value <= 'f';
		const bool upper = value >= 'A' && value <= 'F';
		if (!digit && !lower && !upper)
		{
			return false;
		}
	}
	for (const char* ptr = text + 18; *ptr != '\0'; ++ptr)
	{
		if (*ptr < '0' || *ptr > '9')
		{
			return false;
		}
	}
	const auto [code_end, code_error] = std::from_chars(text, text + 16, *code_id, 16);
	const auto [ordinal_end, ordinal_error] =
	    std::from_chars(text + 18, text + std::strlen(text), *ordinal, 10);
	return code_error == std::errc {} && code_end == text + 16 && ordinal_error == std::errc {} && *ordinal_end == '\0';
}

bool ParsePixelMrtSelector(const char* text, uint64_t* code_id, uint32_t* target, uint32_t* ordinal)
{
	if (text == nullptr || code_id == nullptr || target == nullptr || ordinal == nullptr || std::strlen(text) < 23u ||
	    text[16] != ':' || text[17] != 'm' || text[18] != 'r' || text[19] != 't' || text[20] < '0' || text[20] > '3' ||
	    text[21] != '@' || text[22] == '\0')
	{
		return false;
	}
	for (uint32_t index = 0; index < 16u; ++index)
	{
		const char value = text[index];
		const bool digit = value >= '0' && value <= '9';
		const bool lower = value >= 'a' && value <= 'f';
		const bool upper = value >= 'A' && value <= 'F';
		if (!digit && !lower && !upper)
		{
			return false;
		}
	}
	for (const char* ptr = text + 22; *ptr != '\0'; ++ptr)
	{
		if (*ptr < '0' || *ptr > '9')
		{
			return false;
		}
	}
	const auto [code_end, code_error] = std::from_chars(text, text + 16, *code_id, 16);
	const auto [ordinal_end, ordinal_error] = std::from_chars(text + 22, text + std::strlen(text), *ordinal, 10);
	*target = static_cast<uint32_t>(text[20] - '0');
	return code_error == std::errc {} && code_end == text + 16 && ordinal_error == std::errc {} && *ordinal_end == '\0';
}

bool EnvironmentSelectorIsSet(const char* name)
{
	const char* value = std::getenv(name);
	return value != nullptr && value[0] != '\0';
}

bool HasFiniteRange(uint32_t minimum, uint32_t maximum)
{
	return minimum != std::numeric_limits<uint32_t>::max() && maximum != 0;
}

const char* AttachmentFormatName(VertexClipProbeAttachmentFormat format)
{
	switch (format)
	{
		case VertexClipProbeAttachmentFormat::B10G11R11Ufloat: return "b10g11r11";
		case VertexClipProbeAttachmentFormat::Rgba16Sfloat: return "rgba16f";
		case VertexClipProbeAttachmentFormat::Rgba8: return "rgba8";
		case VertexClipProbeAttachmentFormat::Bgra8: return "bgra8";
		case VertexClipProbeAttachmentFormat::Unsupported: return "unsupported";
	}
	return "unsupported";
}

const char* AttachmentStatusName(VertexClipProbeAttachmentStatus status)
{
	switch (status)
	{
		case VertexClipProbeAttachmentStatus::Ok: return "ok";
		case VertexClipProbeAttachmentStatus::TargetUnavailable: return "skip_target";
		case VertexClipProbeAttachmentStatus::Multisampled: return "skip_msaa";
		case VertexClipProbeAttachmentStatus::TransferSourceUnavailable: return "skip_usage";
		case VertexClipProbeAttachmentStatus::ZeroExtent: return "skip_extent";
		case VertexClipProbeAttachmentStatus::UnsupportedFormat: return "skip_format";
		case VertexClipProbeAttachmentStatus::TooLarge: return "skip_size";
		case VertexClipProbeAttachmentStatus::BufferUnavailable: return "skip_buffer";
		case VertexClipProbeAttachmentStatus::LoadDiscarded: return "skip_load";
		case VertexClipProbeAttachmentStatus::UndefinedLayout: return "skip_layout";
		case VertexClipProbeAttachmentStatus::MapFailed: return "skip_map";
		case VertexClipProbeAttachmentStatus::InvalidData: return "skip_data";
	}
	return "skip_data";
}

bool AttachmentPixelRgbNonzero(VertexClipProbeAttachmentFormat format, const uint8_t* pixel)
{
	if (pixel == nullptr)
	{
		return false;
	}
	switch (format)
	{
		case VertexClipProbeAttachmentFormat::B10G11R11Ufloat:
			return pixel[0] != 0u || pixel[1] != 0u || pixel[2] != 0u || pixel[3] != 0u;
		case VertexClipProbeAttachmentFormat::Rgba16Sfloat:
			return pixel[0] != 0u || pixel[1] != 0u || pixel[2] != 0u || pixel[3] != 0u || pixel[4] != 0u ||
			       pixel[5] != 0u;
		case VertexClipProbeAttachmentFormat::Rgba8:
		case VertexClipProbeAttachmentFormat::Bgra8: return pixel[0] != 0u || pixel[1] != 0u || pixel[2] != 0u;
		case VertexClipProbeAttachmentFormat::Unsupported: return false;
	}
	return false;
}

bool AttachmentPixelRgbEqual(VertexClipProbeAttachmentFormat format, const uint8_t* lhs, const uint8_t* rhs)
{
	if (lhs == nullptr || rhs == nullptr)
	{
		return false;
	}
	size_t rgb_bytes = 0u;
	switch (format)
	{
		case VertexClipProbeAttachmentFormat::B10G11R11Ufloat: rgb_bytes = 4u; break;
		case VertexClipProbeAttachmentFormat::Rgba16Sfloat: rgb_bytes = 6u; break;
		case VertexClipProbeAttachmentFormat::Rgba8:
		case VertexClipProbeAttachmentFormat::Bgra8: rgb_bytes = 3u; break;
		case VertexClipProbeAttachmentFormat::Unsupported: return false;
	}
	return std::memcmp(lhs, rhs, rgb_bytes) == 0;
}

bool ResolveAttachmentCoverageBounds(const VertexClipProbeRawStats& coverage, uint32_t width, uint32_t height,
	                                 uint32_t* min_x, uint32_t* max_x, uint32_t* min_y, uint32_t* max_y)
{
	if (min_x == nullptr || max_x == nullptr || min_y == nullptr || max_y == nullptr || width == 0u || height == 0u ||
	    !VertexClipProbeHasFinitePixelMrtCoverageExtrema(coverage))
	{
		return false;
	}
	const double raw_min_x = VertexClipProbeDecodeOrderedFloat(coverage.min_pixel_frag_x);
	const double raw_max_x = VertexClipProbeDecodeOrderedFloat(coverage.max_pixel_frag_x);
	const double raw_min_y = VertexClipProbeDecodeOrderedFloat(coverage.min_pixel_frag_y);
	const double raw_max_y = VertexClipProbeDecodeOrderedFloat(coverage.max_pixel_frag_y);
	if (!std::isfinite(raw_min_x) || !std::isfinite(raw_max_x) || !std::isfinite(raw_min_y) || !std::isfinite(raw_max_y))
	{
		return false;
	}

	const double first_x = std::ceil(raw_min_x - 0.5);
	const double last_x  = std::floor(raw_max_x - 0.5);
	const double first_y = std::ceil(raw_min_y - 0.5);
	const double last_y  = std::floor(raw_max_y - 0.5);
	if (first_x > last_x || first_y > last_y || last_x < 0.0 || last_y < 0.0 ||
	    first_x > static_cast<double>(width - 1u) || first_y > static_cast<double>(height - 1u))
	{
		return false;
	}

	*min_x = first_x <= 0.0 ? 0u : static_cast<uint32_t>(first_x);
	*max_x = last_x >= static_cast<double>(width - 1u) ? width - 1u : static_cast<uint32_t>(last_x);
	*min_y = first_y <= 0.0 ? 0u : static_cast<uint32_t>(first_y);
	*max_y = last_y >= static_cast<double>(height - 1u) ? height - 1u : static_cast<uint32_t>(last_y);
	return *min_x <= *max_x && *min_y <= *max_y;
}

} // namespace

bool ShaderVertexClipProbeEligible(bool next_gen, bool vs_embedded)
{
	return next_gen && !vs_embedded;
}

bool ShaderPixelInput0ProbeEligible(bool next_gen, bool ps_embedded)
{
	return next_gen && !ps_embedded;
}

bool VertexClipProbeCanReserveAtPresent(const ShaderVertexClipProbeConfig& config, uint64_t present)
{
	return config.enabled && present >= config.min_present;
}

bool PixelInput0ProbeCanReserveAtPresent(const ShaderPixelInput0ProbeConfig& config, uint64_t present)
{
	return config.enabled && present >= config.min_present;
}

bool VertexClipProbeStagesCanReserveAtPresent(const ShaderVertexClipProbeConfig& vertex_config,
	                                           const ShaderPixelInput0ProbeConfig& pixel_config, uint64_t present)
{
	return (!vertex_config.enabled || VertexClipProbeCanReserveAtPresent(vertex_config, present)) &&
	       (!pixel_config.enabled || PixelInput0ProbeCanReserveAtPresent(pixel_config, present));
}

bool VertexClipProbeValidatePairedPixelInstruction(bool instruction_valid, ShaderVertexClipProbeConfig* vertex_config,
	                                                ShaderPixelInput0ProbeConfig* pixel_config)
{
	if (vertex_config == nullptr || pixel_config == nullptr)
	{
		return false;
	}
	if (instruction_valid)
	{
		return true;
	}
	*vertex_config = {};
	*pixel_config  = {};
	return false;
}

bool VertexClipProbeCanReserveAutoDraw(const ShaderVertexClipProbeConfig& config, bool clear_only)
{
	return config.enabled && !clear_only;
}

uint64_t VertexClipProbeDiagnosticIdentity(uint32_t descriptor_set)
{
	if (descriptor_set > 2u)
	{
		return 0;
	}
	return kVertexClipProbeDiagnosticRevision + descriptor_set;
}

uint64_t PixelSampleProbeDiagnosticIdentity(uint32_t descriptor_set, uint32_t ordinal, bool sparse_subgroup)
{
	if (descriptor_set > 2u)
	{
		return 0;
	}
	return kPixelSampleProbeDiagnosticRevision | (static_cast<uint64_t>(ordinal) << 8u) |
	       (sparse_subgroup ? (1ull << 2u) : 0ull) | descriptor_set;
}

uint64_t PixelMrtProbeDiagnosticIdentity(uint32_t descriptor_set, uint32_t target, uint32_t ordinal)
{
	if (descriptor_set > 2u || target > 3u)
	{
		return 0;
	}
	return kPixelMrtProbeDiagnosticRevision | (static_cast<uint64_t>(ordinal) << 4u) |
	       (static_cast<uint64_t>(target) << 2u) | descriptor_set;
}

VertexClipProbeRawStats VertexClipProbeInitialRawStats()
{
	return {};
}

uint32_t VertexClipProbeAttachmentFormatBytesPerPixel(VertexClipProbeAttachmentFormat format)
{
	switch (format)
	{
		case VertexClipProbeAttachmentFormat::B10G11R11Ufloat:
		case VertexClipProbeAttachmentFormat::Rgba8:
		case VertexClipProbeAttachmentFormat::Bgra8: return 4u;
		case VertexClipProbeAttachmentFormat::Rgba16Sfloat: return 8u;
		case VertexClipProbeAttachmentFormat::Unsupported: return 0u;
	}
	return 0u;
}

bool VertexClipProbeAggregateAttachmentReadback(VertexClipProbeAttachmentFormat format, const uint8_t* data,
	                                             uint64_t data_size, uint32_t width, uint32_t height,
	                                             const VertexClipProbeRawStats& coverage,
	                                             VertexClipProbeAttachmentReadbackStats* stats)
{
	if (stats == nullptr)
	{
		return false;
	}
	*stats = {};
	const uint32_t bytes_per_pixel = VertexClipProbeAttachmentFormatBytesPerPixel(format);
	if (data == nullptr || bytes_per_pixel == 0u || width == 0u || height == 0u)
	{
		return false;
	}
	const uint64_t width_u64  = width;
	const uint64_t height_u64 = height;
	if (width_u64 > std::numeric_limits<uint64_t>::max() / height_u64)
	{
		return false;
	}
	const uint64_t pixel_count = width_u64 * height_u64;
	if (pixel_count > kAttachmentReadbackMaxBytes / bytes_per_pixel)
	{
		return false;
	}
	const uint64_t bytes = pixel_count * bytes_per_pixel;
	if (data_size != bytes)
	{
		return false;
	}

	uint32_t coverage_min_x = 0;
	uint32_t coverage_max_x = 0;
	uint32_t coverage_min_y = 0;
	uint32_t coverage_max_y = 0;
	const bool coverage_bounds =
	    ResolveAttachmentCoverageBounds(coverage, width, height, &coverage_min_x, &coverage_max_x, &coverage_min_y, &coverage_max_y);

	uint64_t hash = 14695981039346656037ull;
	for (uint64_t byte = 0; byte < bytes; ++byte)
	{
		hash ^= data[byte];
		hash *= 1099511628211ull;
	}

	uint64_t rgb_nonzero          = 0;
	uint64_t rgb_nonzero_coverage = 0;
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const uint64_t pixel = static_cast<uint64_t>(y) * width + x;
			const auto* pixel_data = data + pixel * bytes_per_pixel;
			if (!AttachmentPixelRgbNonzero(format, pixel_data))
			{
				continue;
			}
			++rgb_nonzero;
			if (coverage_bounds && x >= coverage_min_x && x <= coverage_max_x && y >= coverage_min_y && y <= coverage_max_y)
			{
				++rgb_nonzero_coverage;
			}
		}
	}

	stats->format                      = format;
	stats->width                       = width;
	stats->height                      = height;
	stats->bytes                       = bytes;
	stats->rgb_nonzero_pixels          = rgb_nonzero;
	stats->rgb_nonzero_coverage_pixels = rgb_nonzero_coverage;
	stats->fnv1a64                     = hash;
	stats->coverage_bounds_available   = coverage_bounds;
	return true;
}

bool VertexClipProbeAggregateAttachmentDelta(VertexClipProbeAttachmentFormat format, const uint8_t* before,
	                                            const uint8_t* after, uint64_t data_size, uint32_t width,
	                                            uint32_t height, const VertexClipProbeRawStats& coverage,
	                                            VertexClipProbeAttachmentDeltaStats* stats)
{
	if (before == nullptr || after == nullptr || stats == nullptr || width == 0u || height == 0u)
	{
		return false;
	}
	*stats = {};
	const uint32_t bytes_per_pixel = VertexClipProbeAttachmentFormatBytesPerPixel(format);
	const uint64_t pixel_count     = static_cast<uint64_t>(width) * height;
	if (bytes_per_pixel == 0u || pixel_count > UINT64_MAX / bytes_per_pixel ||
	    data_size != pixel_count * bytes_per_pixel)
	{
		return false;
	}

	uint32_t coverage_min_x = 0u;
	uint32_t coverage_max_x = 0u;
	uint32_t coverage_min_y = 0u;
	uint32_t coverage_max_y = 0u;
	const bool coverage_bounds = ResolveAttachmentCoverageBounds(
	    coverage, width, height, &coverage_min_x, &coverage_max_x, &coverage_min_y, &coverage_max_y);

	for (uint32_t y = 0u; y < height; ++y)
	{
		for (uint32_t x = 0u; x < width; ++x)
		{
			const uint64_t pixel_offset = (static_cast<uint64_t>(y) * width + x) * bytes_per_pixel;
			const auto* before_pixel = before + pixel_offset;
			const auto* after_pixel  = after + pixel_offset;
			if (AttachmentPixelRgbEqual(format, before_pixel, after_pixel))
			{
				continue;
			}
			++stats->rgb_changed_pixels;
			const bool in_coverage = coverage_bounds && x >= coverage_min_x && x <= coverage_max_x &&
			                         y >= coverage_min_y && y <= coverage_max_y;
			if (!in_coverage)
			{
				continue;
			}
			++stats->rgb_changed_coverage_pixels;
			const bool before_nonzero = AttachmentPixelRgbNonzero(format, before_pixel);
			const bool after_nonzero  = AttachmentPixelRgbNonzero(format, after_pixel);
			if (!before_nonzero && after_nonzero)
			{
				++stats->rgb_zero_to_nonzero_coverage_pixels;
			} else if (before_nonzero && !after_nonzero)
			{
				++stats->rgb_nonzero_to_zero_coverage_pixels;
			}
		}
	}
	stats->coverage_bounds_available = coverage_bounds;
	return true;
}

bool VertexClipProbeFormatResultMessage(const VertexClipProbeResultInfo& result, const VertexClipProbeRawStats& stats, char* dst,
	                                      size_t dst_size)
{
	if (dst == nullptr || dst_size == 0 || result.descriptor_set > 2u)
	{
		return false;
	}

	const bool finite = VertexClipProbeHasFiniteExtrema(stats);
	int        written = 0;
	if (!finite)
	{
		written = std::snprintf(dst, dst_size, "cs=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32
		                        " inv=%" PRIu32 " nf=%" PRIu32 " fin=0",
		                        result.checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set,
		                        stats.invocations, stats.nonfinite);
	} else
	{
		written = std::snprintf(
		    dst, dst_size,
		    "cs=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " inv=%" PRIu32 " nf=%" PRIu32
		    " fin=1 w=%.6g:%.6g x=%.6g:%.6g y=%.6g:%.6g z=%.6g:%.6g",
		    result.checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set, stats.invocations,
		    stats.nonfinite, VertexClipProbeDecodeOrderedFloat(stats.min_w), VertexClipProbeDecodeOrderedFloat(stats.max_w),
		    VertexClipProbeDecodeOrderedFloat(stats.min_x_w), VertexClipProbeDecodeOrderedFloat(stats.max_x_w),
		    VertexClipProbeDecodeOrderedFloat(stats.min_y_w), VertexClipProbeDecodeOrderedFloat(stats.max_y_w),
		    VertexClipProbeDecodeOrderedFloat(stats.min_z_w), VertexClipProbeDecodeOrderedFloat(stats.max_z_w));
	}
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeFormatPopulationResultMessage(const VertexClipProbeResultInfo& result,
                                                  const VertexClipProbeRawStats& stats, char* dst, size_t dst_size)
{
	if (dst == nullptr || dst_size == 0 || result.descriptor_set > 2u)
	{
		return false;
	}
	const int written = std::snprintf(
	    dst, dst_size,
	    "cs=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " wnp=%" PRIu32 " oxy=%" PRIu32
	    " oz01=%" PRIu32 " in01=%" PRIu32 " ozn=%" PRIu32 " inn=%" PRIu32,
	    result.checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set,
	    stats.clip_w_nonpositive, stats.clip_xy_outside, stats.clip_z_outside_zero_to_one,
	    stats.clip_inside_zero_to_one, stats.clip_z_outside_negative_one_to_one,
	    stats.clip_inside_negative_one_to_one);
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeFormatParam0ResultMessage(const VertexClipProbeResultInfo& result, const VertexClipProbeRawStats& stats, char* dst,
	                                            size_t dst_size)
{
	if (dst == nullptr || dst_size == 0 || result.descriptor_set > 2u)
	{
		return false;
	}

	const bool finite = VertexClipProbeHasFiniteParam0Extrema(stats);
	int        written = 0;
	if (!finite)
	{
		written = std::snprintf(dst, dst_size, "cs=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32
		                        " p0n=%" PRIu32 " p0nf=%" PRIu32 " p0fin=0",
		                        result.checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set,
		                        stats.param0_exports, stats.param0_nonfinite);
	} else
	{
		written = std::snprintf(dst, dst_size,
		                        "cs=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " p0n=%" PRIu32
		                        " p0nf=%" PRIu32 " p0fin=1 p0x=%.6g:%.6g p0y=%.6g:%.6g",
		                        result.checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set,
		                        stats.param0_exports, stats.param0_nonfinite,
		                        VertexClipProbeDecodeOrderedFloat(stats.min_param0_x),
		                        VertexClipProbeDecodeOrderedFloat(stats.max_param0_x),
		                        VertexClipProbeDecodeOrderedFloat(stats.min_param0_y),
		                        VertexClipProbeDecodeOrderedFloat(stats.max_param0_y));
	}
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeFormatPixelInput0ResultMessage(const VertexClipProbeResultInfo& result,
	                                                 const VertexClipProbeRawStats& stats, char* dst, size_t dst_size)
{
	if (dst == nullptr || dst_size == 0 || result.descriptor_set > 2u)
	{
		return false;
	}

	const bool finite = VertexClipProbeHasFinitePixelInput0Extrema(stats);
	int        written = 0;
	if (!finite)
	{
		written = std::snprintf(dst, dst_size, "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32
		                        " i0n=%" PRIu32 " i0nf=%" PRIu32 " i0fin=0",
		                        result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set,
		                        stats.pixel_input0_samples, stats.pixel_input0_nonfinite);
	} else
	{
		written = std::snprintf(dst, dst_size,
		                        "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " i0n=%" PRIu32
		                        " i0nf=%" PRIu32 " i0fin=1 i0x=%.6g:%.6g i0y=%.6g:%.6g",
		                        result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set,
		                        stats.pixel_input0_samples, stats.pixel_input0_nonfinite,
		                        VertexClipProbeDecodeOrderedFloat(stats.min_pixel_input0_x),
		                        VertexClipProbeDecodeOrderedFloat(stats.max_pixel_input0_x),
		                        VertexClipProbeDecodeOrderedFloat(stats.min_pixel_input0_y),
		                        VertexClipProbeDecodeOrderedFloat(stats.max_pixel_input0_y));
	}
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeFormatPixelSampleResultMessage(const VertexClipProbeResultInfo& result, uint32_t ordinal,
	                                                 const VertexClipProbeRawStats& stats, char* dst, size_t dst_size)
{
	if (dst == nullptr || dst_size == 0 || result.descriptor_set > 2u)
	{
		return false;
	}

	const bool finite = VertexClipProbeHasFinitePixelSampleExtrema(stats);
	int        written = 0;
	if (!finite)
	{
		written = std::snprintf(dst, dst_size,
		                        "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " ord=%" PRIu32
		                        " sn=%" PRIu32 " snf=%" PRIu32 " sfin=0",
		                        result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set,
		                        ordinal, stats.pixel_sample_invocations, stats.pixel_sample_nonfinite);
	} else
	{
		written = std::snprintf(
		    dst, dst_size,
		    "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " ord=%" PRIu32
		    " sn=%" PRIu32 " snf=%" PRIu32 " sfin=1 r=%.6g:%.6g g=%.6g:%.6g b=%.6g:%.6g a=%.6g:%.6g",
		    result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set, ordinal,
		    stats.pixel_sample_invocations, stats.pixel_sample_nonfinite,
		    VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_r),
		    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_r),
		    VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_g),
		    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_g),
		    VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_b),
		    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_b),
		    VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_a),
		    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_a));
	}
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeFormatPixelMrtResultMessage(const VertexClipProbeResultInfo& result, uint32_t target, uint32_t ordinal,
	                                              const VertexClipProbeRawStats& stats, char* dst, size_t dst_size)
{
	if (dst == nullptr || dst_size == 0 || result.descriptor_set > 2u || target > 3u)
	{
		return false;
	}
	const bool finite = VertexClipProbeHasFinitePixelSampleExtrema(stats);
	const int written = finite
	                        ? std::snprintf(
	                              dst, dst_size,
	                              "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " mrt=%" PRIu32 " ord=%" PRIu32
	                              " on=%" PRIu32 " onf=%" PRIu32
	                              " ofin=1 r=%.6g:%.6g g=%.6g:%.6g b=%.6g:%.6g a=%.6g:%.6g",
	                              result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count,
	                              result.descriptor_set, target, ordinal, stats.pixel_sample_invocations,
	                              stats.pixel_sample_nonfinite,
	                              VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_r),
	                              VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_r),
	                              VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_g),
	                              VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_g),
	                              VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_b),
	                              VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_b),
	                              VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_a),
	                              VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_a))
	                        : std::snprintf(dst, dst_size,
	                                        "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " mrt=%" PRIu32
	                                        " ord=%" PRIu32 " on=%" PRIu32 " onf=%" PRIu32 " ofin=0",
	                                        result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count,
	                                        result.descriptor_set, target, ordinal, stats.pixel_sample_invocations,
	                                        stats.pixel_sample_nonfinite);
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeFormatPixelMrtCoverageResultMessage(const VertexClipProbeResultInfo& result, uint32_t target,
	                                                       uint32_t ordinal, const VertexClipProbeRawStats& stats, char* dst,
	                                                       size_t dst_size)
{
	if (dst == nullptr || dst_size == 0 || result.descriptor_set > 2u || target > 3u)
	{
		return false;
	}
	const bool finite = VertexClipProbeHasFinitePixelMrtCoverageExtrema(stats);
	const int written = finite
	                        ? std::snprintf(dst, dst_size,
	                                        "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " mrt=%" PRIu32
	                                        " ord=%" PRIu32 " cfin=1 x=%.6g:%.6g y=%.6g:%.6g",
	                                        result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count,
	                                        result.descriptor_set, target, ordinal,
	                                        VertexClipProbeDecodeOrderedFloat(stats.min_pixel_frag_x),
	                                        VertexClipProbeDecodeOrderedFloat(stats.max_pixel_frag_x),
	                                        VertexClipProbeDecodeOrderedFloat(stats.min_pixel_frag_y),
	                                        VertexClipProbeDecodeOrderedFloat(stats.max_pixel_frag_y))
	                        : std::snprintf(dst, dst_size,
	                                        "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " mrt=%" PRIu32
	                                        " ord=%" PRIu32 " cfin=0",
	                                        result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count,
	                                        result.descriptor_set, target, ordinal);
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeFormatPixelMrtAttachmentResultMessage(
	const VertexClipProbeResultInfo& result, uint32_t target, uint32_t ordinal, uint32_t match_ordinal,
	uint32_t empty_retries, VertexClipProbeAttachmentStatus status,
	const VertexClipProbeAttachmentReadbackStats* stats, char* dst, size_t dst_size)
{
	if (dst == nullptr || dst_size == 0u || result.descriptor_set > 2u || target > 3u ||
	    empty_retries > kVertexClipProbeAttachmentMaxEmptyRetries ||
	    (status == VertexClipProbeAttachmentStatus::Ok && stats == nullptr))
	{
		return false;
	}

	int written = 0;
	if (status == VertexClipProbeAttachmentStatus::Ok)
	{
		if (stats->format == VertexClipProbeAttachmentFormat::Unsupported || stats->width == 0u || stats->height == 0u ||
		    stats->bytes == 0u)
		{
			return false;
		}
		written = std::snprintf(
		    dst, dst_size,
		    "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " mrt=%" PRIu32 " ord=%" PRIu32
		    " f=%s e=%" PRIu32 "x%" PRIu32 " nz=%" PRIu64 " b=%u in=%" PRIu64 " h=%016" PRIx64
		    " ok=1 m=%" PRIu32 " r=%" PRIu32,
		    result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set, target, ordinal,
		    AttachmentFormatName(stats->format), stats->width, stats->height, stats->rgb_nonzero_pixels,
		    stats->coverage_bounds_available ? 1u : 0u, stats->rgb_nonzero_coverage_pixels, stats->fnv1a64, match_ordinal,
		    empty_retries);
	} else
	{
		written = std::snprintf(dst, dst_size,
		                        "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " mrt=%" PRIu32 " ord=%" PRIu32
		                        " status=%s m=%" PRIu32 " r=%" PRIu32,
		                        result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set,
		                        target, ordinal, AttachmentStatusName(status), match_ordinal, empty_retries);
	}
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeFormatPixelMrtAttachmentDeltaMessage(
	const VertexClipProbeResultInfo& result, uint32_t target, uint32_t ordinal, uint64_t guest_addr,
	uint32_t match_ordinal,
	uint32_t empty_retries, uint32_t load_op, VertexClipProbeAttachmentStatus status,
	const VertexClipProbeAttachmentDeltaStats* stats, char* dst, size_t dst_size)
{
	if (dst == nullptr || dst_size == 0u || result.descriptor_set > 2u || target > 3u ||
	    empty_retries > kVertexClipProbeAttachmentMaxEmptyRetries ||
	    (load_op != 0u && load_op != 1u && load_op != 2u) ||
	    (status == VertexClipProbeAttachmentStatus::Ok && stats == nullptr))
	{
		return false;
	}

	int written = 0;
	if (status == VertexClipProbeAttachmentStatus::Ok)
	{
		written = std::snprintf(
		    dst, dst_size,
		    "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " mrt=%" PRIu32 " ord=%" PRIu32
		    " ga=%012" PRIx64 " l=%u d=%" PRIu64 " b=%u in=%" PRIu64 " up=%" PRIu64 " dn=%" PRIu64
		    " m=%" PRIu32 " r=%" PRIu32,
		    result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set, target, ordinal,
		    guest_addr, load_op, stats->rgb_changed_pixels,
		    stats->coverage_bounds_available ? 1u : 0u, stats->rgb_changed_coverage_pixels,
		    stats->rgb_zero_to_nonzero_coverage_pixels, stats->rgb_nonzero_to_zero_coverage_pixels, match_ordinal,
		    empty_retries);
	} else
	{
		written = std::snprintf(dst, dst_size,
		                        "ps=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " mrt=%" PRIu32 " ord=%" PRIu32
		                        " ga=%012" PRIx64 " l=%u status=%s m=%" PRIu32 " r=%" PRIu32,
		                        result.pixel_checksum, result.indexed ? 'i' : 'a', result.guest_count,
		                        result.descriptor_set, target, ordinal, guest_addr, load_op,
		                        AttachmentStatusName(status), match_ordinal, empty_retries);
	}
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeFormatResolverResultMessage(const VertexClipProbeResultInfo& result,
	                                              const VertexClipProbeRawStats& stats, char* dst, size_t dst_size)
{
	const bool empty_payload = stats.resolver_instruction_pc == 0u && stats.resolver_access_width == 0u &&
	                           stats.resolver_desc0 == 0u && stats.resolver_desc1 == 0u &&
	                           stats.resolver_raw_byte_offset == 0u && stats.resolver_valid == 0u &&
	                           stats.resolver_final_slot == 0u && stats.resolver_final_byte_offset == 0u;
	const bool claimed_payload_valid = stats.resolver_access_width != 0u && stats.resolver_valid <= 1u &&
	                                   (stats.resolver_valid == 1u ||
	                                    (stats.resolver_final_slot == 0u && stats.resolver_final_byte_offset == 0u));
	if (dst == nullptr || dst_size == 0 || result.descriptor_set > 2u || stats.resolver_claimed > 1u ||
	    (stats.resolver_claimed == 0u && !empty_payload) ||
	    (stats.resolver_claimed == 1u && !claimed_payload_valid))
	{
		return false;
	}
	const int written = stats.resolver_claimed == 0u
	                        ? std::snprintf(dst, dst_size, "cs=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " c=0",
	                                        result.checksum, result.indexed ? 'i' : 'a', result.guest_count,
	                                        result.descriptor_set)
	                        : std::snprintf(
	                              dst, dst_size,
	                              "cs=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32 " c=1 p=%08" PRIx32 " w=%" PRIu32
	                              " d0=%08" PRIx32 " d1=%08" PRIx32 " ro=%" PRIu32 " v=%" PRIu32 " fs=%" PRIu32
	                              " fo=%" PRIu32,
	                              result.checksum, result.indexed ? 'i' : 'a', result.guest_count, result.descriptor_set,
	                              stats.resolver_instruction_pc, stats.resolver_access_width, stats.resolver_desc0,
	                              stats.resolver_desc1, stats.resolver_raw_byte_offset, stats.resolver_valid,
	                              stats.resolver_final_slot, stats.resolver_final_byte_offset);
	if (written < 0 || static_cast<size_t>(written) >= dst_size)
	{
		dst[0] = '\0';
		return false;
	}
	return true;
}

bool VertexClipProbeLifecycle::Reserve(uint32_t match_ordinal)
{
	if (m_state != VertexClipProbeState::Idle)
	{
		return false;
	}
	if (m_matching_occurrences < match_ordinal)
	{
		++m_matching_occurrences;
		return false;
	}
	m_state = VertexClipProbeState::Reserved;
	return true;
}

bool VertexClipProbeLifecycle::BeginRecording()
{
	if (m_state != VertexClipProbeState::Reserved)
	{
		return false;
	}
	m_state = VertexClipProbeState::Recording;
	return true;
}

bool VertexClipProbeLifecycle::MarkPendingFence()
{
	if (m_state != VertexClipProbeState::Recording)
	{
		return false;
	}
	m_state = VertexClipProbeState::PendingFence;
	return true;
}

bool VertexClipProbeLifecycle::RetryAfterFence()
{
	if (m_state != VertexClipProbeState::PendingFence)
	{
		return false;
	}
	m_state = VertexClipProbeState::Idle;
	return true;
}

bool VertexClipProbeLifecycle::Complete()
{
	if (m_state != VertexClipProbeState::PendingFence)
	{
		return false;
	}
	m_state = VertexClipProbeState::Completed;
	return true;
}

ShaderVertexClipProbeConfig ShaderResolveVertexClipProbeConfig(uint64_t code_id, bool indexed, uint32_t guest_count)
{
	ShaderVertexClipProbeConfig config {};
	uint64_t                     selected_code_id = 0;
	if (!ParseExactHexChecksum(std::getenv("KYTY_VS_CLIP_PROBE"), &selected_code_id) || selected_code_id != code_id)
	{
		return config;
	}

	VertexClipProbeDrawSelector selector {};
	if (!ParseDrawSelector(std::getenv("KYTY_VS_CLIP_PROBE_DRAW"), &selector))
	{
		return config;
	}
	config.draw_scoped = true;
	if (selector.indexed != indexed || selector.count != guest_count)
	{
		return config;
	}
	if (!ParseOptionalMinPresent(std::getenv("KYTY_VS_CLIP_PROBE_MIN_PRESENT"), &config.min_present))
	{
		return config;
	}

	config.enabled             = true;
	config.diagnostic_identity = VertexClipProbeDiagnosticIdentity(0u);
	return config;
}

ShaderPixelInput0ProbeConfig ShaderResolvePixelInput0ProbeConfig(uint64_t code_id, bool indexed, uint32_t guest_count,
	                                                             bool stage_enabled)
{
	return ShaderResolvePixelProbeConfig(code_id, indexed, guest_count, stage_enabled);
}

ShaderPixelInput0ProbeConfig ShaderResolvePixelProbeConfig(uint64_t code_id, bool indexed, uint32_t guest_count,
	                                                        bool stage_enabled)
{
	ShaderPixelInput0ProbeConfig config {};
	if (!stage_enabled)
	{
		return config;
	}
	const bool input0_selector = EnvironmentSelectorIsSet("KYTY_PS_INPUT0_PROBE");
	const bool sample_selector = EnvironmentSelectorIsSet("KYTY_PS_SAMPLE_PROBE");
	const bool mrt_selector    = EnvironmentSelectorIsSet("KYTY_PS_MRT_PROBE");
	const uint32_t selector_count = static_cast<uint32_t>(input0_selector) + static_cast<uint32_t>(sample_selector) +
	                                static_cast<uint32_t>(mrt_selector);
	if (selector_count > 1u || (mrt_selector && EnvironmentSelectorIsSet("KYTY_FS_TAP")))
	{
		return config;
	}
	if (mrt_selector)
	{
		uint64_t selected_code_id = 0;
		uint32_t target            = 0;
		uint32_t ordinal           = 0;
		if (!ParsePixelMrtSelector(std::getenv("KYTY_PS_MRT_PROBE"), &selected_code_id, &target, &ordinal) ||
		    selected_code_id != code_id)
		{
			return config;
		}
		VertexClipProbeDrawSelector selector {};
		if (!ParseDrawSelector(std::getenv("KYTY_PS_MRT_PROBE_DRAW"), &selector))
		{
			return config;
		}
		config.draw_scoped = true;
		const bool min_invocations_requested =
		    EnvironmentSelectorIsSet("KYTY_PS_MRT_ATTACHMENT_MIN_INVOCATIONS");
		if (selector.indexed != indexed || selector.count != guest_count ||
		    !ParseOptionalMinPresent(std::getenv("KYTY_PS_MRT_PROBE_MIN_PRESENT"), &config.min_present) ||
		    !ParseOptionalMatchOrdinal(std::getenv("KYTY_PS_MRT_PROBE_MATCH_ORDINAL"), &config.match_ordinal) ||
		    !ParseOptionalStrictBoolean(std::getenv("KYTY_PS_MRT_ATTACHMENT_PROBE"), &config.attachment_readback) ||
		    !ParseOptionalPositiveCount(std::getenv("KYTY_PS_MRT_ATTACHMENT_MIN_INVOCATIONS"),
		                                &config.attachment_min_invocations) ||
		    (min_invocations_requested && !config.attachment_readback))
		{
			return config;
		}
		config.enabled             = true;
		config.kind                = ShaderPixelProbeKind::FinalMrtResult;
		config.export_ordinal      = ordinal;
		config.mrt_target          = target;
		config.diagnostic_identity = PixelMrtProbeDiagnosticIdentity(0u, target, ordinal);
		return config;
	}
	if (sample_selector)
	{
		uint64_t selected_code_id = 0;
		uint32_t ordinal          = 0;
		if (!ParsePixelSampleSelector(std::getenv("KYTY_PS_SAMPLE_PROBE"), &selected_code_id, &ordinal) ||
		    selected_code_id != code_id)
		{
			return config;
		}

		VertexClipProbeDrawSelector selector {};
		if (!ParseDrawSelector(std::getenv("KYTY_PS_SAMPLE_PROBE_DRAW"), &selector))
		{
			return config;
		}
		config.draw_scoped = true;
		if (selector.indexed != indexed || selector.count != guest_count ||
		    !ParseOptionalMinPresent(std::getenv("KYTY_PS_SAMPLE_PROBE_MIN_PRESENT"), &config.min_present) ||
		    !ParseOptionalMatchOrdinal(std::getenv("KYTY_PS_SAMPLE_PROBE_MATCH_ORDINAL"), &config.match_ordinal) ||
		    !ParseOptionalStrictBoolean(std::getenv("KYTY_PS_SAMPLE_PROBE_SPARSE"), &config.sparse_subgroup))
		{
			return config;
		}

		config.enabled             = true;
		config.kind                = ShaderPixelProbeKind::SampleResult;
		config.sample_ordinal      = ordinal;
		config.diagnostic_identity = PixelSampleProbeDiagnosticIdentity(0u, ordinal, config.sparse_subgroup);
		return config;
	}
	if (!input0_selector)
	{
		return config;
	}
	uint64_t                     selected_code_id = 0;
	if (!ParseExactHexChecksum(std::getenv("KYTY_PS_INPUT0_PROBE"), &selected_code_id) || selected_code_id != code_id)
	{
		return config;
	}

	VertexClipProbeDrawSelector selector {};
	if (!ParseDrawSelector(std::getenv("KYTY_PS_INPUT0_PROBE_DRAW"), &selector))
	{
		return config;
	}
	config.draw_scoped = true;
	if (selector.indexed != indexed || selector.count != guest_count)
	{
		return config;
	}
	if (!ParseOptionalMinPresent(std::getenv("KYTY_PS_INPUT0_PROBE_MIN_PRESENT"), &config.min_present))
	{
		return config;
	}

	config.enabled             = true;
	config.kind                = ShaderPixelProbeKind::Input0;
	config.diagnostic_identity = VertexClipProbeDiagnosticIdentity(0u);
	return config;
}

bool VertexClipProbeAttachmentShouldRetry(const VertexClipProbeRawStats& stats, uint32_t min_invocations,
	                                       uint32_t retries)
{
	return min_invocations != 0u && stats.pixel_sample_invocations < min_invocations &&
	       retries < kVertexClipProbeAttachmentMaxEmptyRetries;
}

uint32_t VertexClipProbeEncodeOrderedFloat(float value)
{
	uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	return (bits & 0x80000000u) != 0 ? ~bits : (bits ^ 0x80000000u);
}

float VertexClipProbeDecodeOrderedFloat(uint32_t value)
{
	const uint32_t bits = (value & 0x80000000u) != 0 ? (value ^ 0x80000000u) : ~value;
	float          result = 0.0f;
	static_assert(sizeof(bits) == sizeof(result));
	std::memcpy(&result, &bits, sizeof(result));
	return result;
}

bool VertexClipProbeHasFiniteExtrema(const VertexClipProbeRawStats& stats)
{
	return HasFiniteRange(stats.min_w, stats.max_w) && HasFiniteRange(stats.min_x_w, stats.max_x_w) &&
	       HasFiniteRange(stats.min_y_w, stats.max_y_w) && HasFiniteRange(stats.min_z_w, stats.max_z_w);
}

bool VertexClipProbeHasFiniteParam0Extrema(const VertexClipProbeRawStats& stats)
{
	return HasFiniteRange(stats.min_param0_x, stats.max_param0_x) && HasFiniteRange(stats.min_param0_y, stats.max_param0_y);
}

bool VertexClipProbeHasFinitePixelInput0Extrema(const VertexClipProbeRawStats& stats)
{
	return HasFiniteRange(stats.min_pixel_input0_x, stats.max_pixel_input0_x) &&
	       HasFiniteRange(stats.min_pixel_input0_y, stats.max_pixel_input0_y);
}

bool VertexClipProbeHasFinitePixelSampleExtrema(const VertexClipProbeRawStats& stats)
{
	return HasFiniteRange(stats.min_pixel_sample_r, stats.max_pixel_sample_r) &&
	       HasFiniteRange(stats.min_pixel_sample_g, stats.max_pixel_sample_g) &&
	       HasFiniteRange(stats.min_pixel_sample_b, stats.max_pixel_sample_b) &&
	       HasFiniteRange(stats.min_pixel_sample_a, stats.max_pixel_sample_a);
}

bool VertexClipProbeHasFinitePixelMrtCoverageExtrema(const VertexClipProbeRawStats& stats)
{
	return HasFiniteRange(stats.min_pixel_frag_x, stats.max_pixel_frag_x) &&
	       HasFiniteRange(stats.min_pixel_frag_y, stats.max_pixel_frag_y);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
