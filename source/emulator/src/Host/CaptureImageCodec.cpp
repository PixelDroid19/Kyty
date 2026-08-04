#include "Emulator/Host/CaptureImageCodec.h"
#include "Emulator/Host/Png.h"

#include "SDL_surface.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>

namespace Kyty::Emulator::Host {

namespace {

uint64_t HostCaptureImageBytesPerPixel(HostCaptureImagePixelFormat format)
{
	switch (format)
	{
		case HostCaptureImagePixelFormat::Rgba8:
		case HostCaptureImagePixelFormat::Bgra8: return 4;
		case HostCaptureImagePixelFormat::Rgba16G16B16A16Sfloat: return 8;
	}
	return 0;
}

bool HostCaptureImageViewIsValid(const HostCaptureImageView& source, uint64_t bytes_per_pixel)
{
	if (source.pixels == nullptr || source.extent.width == 0 || source.extent.height == 0 || bytes_per_pixel == 0)
	{
		return false;
	}

	const uint64_t width = source.extent.width;
	if (width > UINT64_MAX / bytes_per_pixel)
	{
		return false;
	}
	const uint64_t minimum_row_pitch = width * bytes_per_pixel;
	if (source.row_pitch_bytes < minimum_row_pitch || source.extent.width > UINT64_MAX / source.extent.height)
	{
		return false;
	}
	if (source.extent.height > 1 && source.row_pitch_bytes > UINT64_MAX / (source.extent.height - 1u))
	{
		return false;
	}
	const uint64_t last_row_offset = source.row_pitch_bytes * (source.extent.height - 1u);
	if (last_row_offset > UINT64_MAX - minimum_row_pitch)
	{
		return false;
	}
	const uint64_t pixel_count = width * source.extent.height;
	return pixel_count <= std::numeric_limits<size_t>::max() / 4u;
}

float HostCaptureImageHalfToFloat(uint16_t h)
{
	const uint32_t sign = (h >> 15u) & 1u;
	const uint32_t exp  = (h >> 10u) & 0x1fu;
	const uint32_t mant = h & 0x3ffu;
	uint32_t       bits = 0;
	if (exp == 0u)
	{
		if (mant == 0u)
		{
			bits = sign << 31u;
		} else
		{
			uint32_t m = mant;
			int      e = -14;
			while ((m & 0x400u) == 0u)
			{
				m <<= 1u;
				e--;
			}
			bits = (sign << 31u) | (static_cast<uint32_t>(e + 127) << 23u) | ((m & 0x3ffu) << 13u);
		}
	} else if (exp == 0x1fu)
	{
		bits = (sign << 31u) | (0xffu << 23u) | (mant << 13u);
	} else
	{
		bits = (sign << 31u) | ((exp - 15u + 127u) << 23u) | (mant << 13u);
	}
	float value = 0.0f;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

void HostCaptureImagePixelMasks(HostCaptureImagePixelFormat format, uint32_t* r_mask, uint32_t* g_mask, uint32_t* b_mask,
	                             uint32_t* a_mask)
{
	*r_mask = format == HostCaptureImagePixelFormat::Bgra8 ? 0x00FF0000u : 0x000000FFu;
	*g_mask = 0x0000FF00u;
	*b_mask = format == HostCaptureImagePixelFormat::Bgra8 ? 0x000000FFu : 0x00FF0000u;
	*a_mask = 0xFF000000u;
}

bool HostCaptureImageSaveSdlSurfacePng(SDL_Surface* surface, HostCaptureImagePixelFormat format, const std::filesystem::path& path)
{
	if (surface == nullptr || surface->format == nullptr || surface->w <= 0 || surface->h <= 0 || surface->pitch <= 0 ||
	    surface->format->BytesPerPixel != 4)
	{
		return false;
	}

	const bool must_lock = SDL_MUSTLOCK(surface);
	if (must_lock && SDL_LockSurface(surface) != 0)
	{
		return false;
	}

	std::vector<uint8_t> rgba;
	const bool normalized = HostCaptureImageCodecNormalizeRgba8(
	    {static_cast<const uint8_t*>(surface->pixels), {static_cast<uint32_t>(surface->w), static_cast<uint32_t>(surface->h)},
	     static_cast<uint64_t>(surface->pitch), format},
	    &rgba);

	if (must_lock)
	{
		SDL_UnlockSurface(surface);
	}
	if (!normalized)
	{
		return false;
	}

	const auto path_string = path.string();
	return WriteRgba8Png(path_string.c_str(), rgba.data(), static_cast<uint32_t>(surface->w), static_cast<uint32_t>(surface->h),
	                     static_cast<uint32_t>(surface->w));
}

} // namespace

HostCaptureImageExtent HostCaptureImageCodecScaleToMaxEdge(HostCaptureImageExtent extent, uint32_t max_edge)
{
	if (extent.width == 0 || extent.height == 0 || max_edge == 0 || (extent.width <= max_edge && extent.height <= max_edge))
	{
		return extent;
	}

	const double scale = static_cast<double>(max_edge) / static_cast<double>(extent.width > extent.height ? extent.width : extent.height);
	extent.width        = std::max(1u, static_cast<uint32_t>(static_cast<double>(extent.width) * scale + 0.5));
	extent.height       = std::max(1u, static_cast<uint32_t>(static_cast<double>(extent.height) * scale + 0.5));
	return extent;
}

bool HostCaptureImageCodecNormalizeRgba8(const HostCaptureImageView& source, std::vector<uint8_t>* rgba_out)
{
	if (rgba_out == nullptr)
	{
		return false;
	}
	rgba_out->clear();

	const uint64_t bytes_per_pixel = HostCaptureImageBytesPerPixel(source.format);
	if (!HostCaptureImageViewIsValid(source, bytes_per_pixel))
	{
		return false;
	}

	const uint64_t pixel_count = static_cast<uint64_t>(source.extent.width) * source.extent.height;
	rgba_out->resize(static_cast<size_t>(pixel_count * 4u));
	const size_t rgba_row_bytes = static_cast<size_t>(source.extent.width) * 4u;

	for (uint32_t y = 0; y < source.extent.height; y++)
	{
		const auto* src_row = source.pixels + static_cast<uint64_t>(y) * source.row_pitch_bytes;
		auto*       dst_row = rgba_out->data() + static_cast<size_t>(y) * rgba_row_bytes;

		switch (source.format)
		{
			case HostCaptureImagePixelFormat::Rgba8:
				std::memcpy(dst_row, src_row, rgba_row_bytes);
				break;
			case HostCaptureImagePixelFormat::Bgra8:
				for (uint32_t x = 0; x < source.extent.width; x++)
				{
					const auto* src = src_row + static_cast<size_t>(x) * 4u;
					auto*       dst = dst_row + static_cast<size_t>(x) * 4u;
					dst[0]         = src[2];
					dst[1]         = src[1];
					dst[2]         = src[0];
					dst[3]         = src[3];
				}
				break;
			case HostCaptureImagePixelFormat::Rgba16G16B16A16Sfloat:
				for (uint32_t x = 0; x < source.extent.width; x++)
				{
					const auto* src = src_row + static_cast<size_t>(x) * 8u;
					auto*       dst = dst_row + static_cast<size_t>(x) * 4u;
					for (uint32_t c = 0; c < 4; c++)
					{
						uint16_t component = 0;
						std::memcpy(&component, src + static_cast<size_t>(c) * 2u, sizeof(component));
						const float value = std::clamp(HostCaptureImageHalfToFloat(component), 0.0f, 1.0f);
						dst[c]            = static_cast<uint8_t>(value * 255.0f + 0.5f);
					}
				}
				break;
		}
	}

	return true;
}

HostCaptureImageCodecResult HostCaptureImageCodecWritePng(const HostCaptureImageView& source, uint32_t max_edge,
	                                                        const std::filesystem::path& path)
{
	HostCaptureImageCodecResult result {};
	if ((source.format != HostCaptureImagePixelFormat::Rgba8 && source.format != HostCaptureImagePixelFormat::Bgra8) ||
	    !HostCaptureImageViewIsValid(source, 4u) || source.extent.width > INT_MAX || source.extent.height > INT_MAX ||
	    source.row_pitch_bytes > INT_MAX)
	{
		return result;
	}

	uint32_t r_mask = 0;
	uint32_t g_mask = 0;
	uint32_t b_mask = 0;
	uint32_t a_mask = 0;
	HostCaptureImagePixelMasks(source.format, &r_mask, &g_mask, &b_mask, &a_mask);

	auto* surface = SDL_CreateRGBSurfaceFrom(const_cast<uint8_t*>(source.pixels), static_cast<int>(source.extent.width),
	                                         static_cast<int>(source.extent.height), 32, static_cast<int>(source.row_pitch_bytes), r_mask,
	                                         g_mask, b_mask, a_mask);
	if (surface == nullptr)
	{
		result.error = HostCaptureImageCodecError::CreateSurface;
		return result;
	}

	result.output_extent = source.extent;
	SDL_Surface* save_surface = surface;
	SDL_Surface* scaled       = nullptr;
	const auto   scaled_extent = HostCaptureImageCodecScaleToMaxEdge(source.extent, max_edge);
	if (scaled_extent.width != source.extent.width || scaled_extent.height != source.extent.height)
	{
		scaled = SDL_CreateRGBSurface(0, static_cast<int>(scaled_extent.width), static_cast<int>(scaled_extent.height), 32, r_mask, g_mask,
		                              b_mask, a_mask);
		if (scaled != nullptr && SDL_BlitScaled(surface, nullptr, scaled, nullptr) == 0)
		{
			save_surface        = scaled;
			result.output_extent = scaled_extent;
		} else
		{
			if (scaled != nullptr)
			{
				SDL_FreeSurface(scaled);
				scaled = nullptr;
			}
			result.downscale_fallback = true;
		}
	}

	const bool saved = HostCaptureImageSaveSdlSurfacePng(save_surface, source.format, path);
	if (scaled != nullptr)
	{
		SDL_FreeSurface(scaled);
	}
	SDL_FreeSurface(surface);
	if (!saved)
	{
		result.error = HostCaptureImageCodecError::SaveImage;
		return result;
	}

	result.success = true;
	result.error   = HostCaptureImageCodecError::None;
	return result;
}

} // namespace Kyty::Emulator::Host
