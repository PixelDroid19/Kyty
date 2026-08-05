#include "Emulator/Host/ImageSurface.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/SafeDelete.h"

#include "Emulator/Log.h"

#include "SDL_blendmode.h"
#include "SDL_error.h"
#include "SDL_pixels.h"
#include "SDL_rect.h"
#include "SDL_rwops.h"
#include "SDL_surface.h"

#include <climits>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ASSERT ASSERT
#define STBI_NO_SIMD
#include "stb_image.h"

#define KYTY_SDL_BITSPERPIXEL(X) (((X) >> 8u) & 0xFFu)
#define KYTY_SDL_PIXELTYPE(X)    (((X) >> 24u) & 0x0Fu)
#define KYTY_SDL_PIXELLAYOUT(X)  (((X) >> 16u) & 0x0Fu)
#define KYTY_SDL_PIXELORDER(X)   (((X) >> 20u) & 0x0Fu)

namespace Kyty::Emulator::Host {

using Core::File;
using Core::String;

class HostImageSurfacePrivate
{
public:
	KYTY_CLASS_NO_COPY(HostImageSurfacePrivate);

	HostImageSurfacePrivate(SDL_Surface* surface, void* stbi_pixels): sdl(surface), decoded_pixels(stbi_pixels) {}

	virtual ~HostImageSurfacePrivate()
	{
		SDL_FreeSurface(sdl);
		stbi_image_free(decoded_pixels);
	}

	SDL_Surface*              sdl            = nullptr;
	void*                     decoded_pixels = nullptr;
	HostImageSurfaceMetadata metadata       {};
};

namespace {

void BgrToRgb(SDL_Surface* surface)
{
	auto* rowptr  = static_cast<uint8_t*>(surface->pixels);
	int   row_num = surface->h;
	int   col_num = surface->w;
	int   pitch   = surface->pitch;
	for (int row = 0; row < row_num; row++)
	{
		uint8_t* colptr = rowptr;

		for (int col = 0; col < col_num; col++)
		{
			uint8_t t = colptr[0];
			colptr[0] = colptr[2];
			colptr[2] = t;
			colptr += 3;
		}

		rowptr += pitch;
	}
}

void BgraToRgba(SDL_Surface* surface)
{
	auto* rowptr  = static_cast<uint8_t*>(surface->pixels);
	int   row_num = surface->h;
	int   col_num = surface->w;
	int   pitch   = surface->pitch;
	for (int row = 0; row < row_num; row++)
	{
		uint8_t* colptr = rowptr;

		for (int col = 0; col < col_num; col++)
		{
			uint8_t t = colptr[0];
			colptr[0] = colptr[2];
			colptr[2] = t;
			colptr += 4;
		}

		rowptr += pitch;
	}
}

bool RectIsInside(const HostImageSurfaceMetadata& metadata, const HostImageSurfaceRect& rect)
{
	return rect.width != 0 && rect.height != 0 && rect.x < metadata.width && rect.y < metadata.height &&
	       rect.width <= metadata.width - rect.x && rect.height <= metadata.height - rect.y;
}

int ReadPng(void* user, char* data, int size)
{
	auto* source = static_cast<SDL_RWops*>(user);
	return static_cast<int>(source->read(source, data, 1, size));
}

void SkipPng(void* user, int size)
{
	auto* source = static_cast<SDL_RWops*>(user);
	source->seek(source, size, RW_SEEK_CUR);
}

int EofPng(void* user)
{
	auto* source = static_cast<SDL_RWops*>(user);
	return source->seek(source, 0, RW_SEEK_CUR) >= source->size(source) ? 1 : 0;
}

} // namespace

HostImageSurface* HostImageSurface::CreateFromNative(void* native_surface, void* decoded_pixels)
{
	auto* sdl = static_cast<SDL_Surface*>(native_surface);
	EXIT_IF(sdl == nullptr || sdl->format == nullptr || sdl->pixels == nullptr || sdl->w <= 0 || sdl->h <= 0 || sdl->pitch <= 0);

	auto* surface = new HostImageSurfacePrivate(sdl, decoded_pixels);
	auto* result  = new HostImageSurface(surface);

	auto& metadata         = surface->metadata;
	metadata.bits_per_pixel = static_cast<int>(KYTY_SDL_BITSPERPIXEL(sdl->format->format));

	EXIT_IF(sdl->format->palette != nullptr && metadata.bits_per_pixel != 8);

	metadata.width  = static_cast<uint32_t>(sdl->w);
	metadata.height = static_cast<uint32_t>(sdl->h);
	metadata.pitch  = static_cast<uint32_t>(sdl->pitch);

	EXIT_IF(KYTY_SDL_PIXELTYPE(sdl->format->format) != SDL_PIXELTYPE_ARRAYU8 &&
	        KYTY_SDL_PIXELTYPE(sdl->format->format) != SDL_PIXELTYPE_INDEX8 &&
	        KYTY_SDL_PIXELTYPE(sdl->format->format) != SDL_PIXELTYPE_PACKED32);

	EXIT_IF(KYTY_SDL_PIXELLAYOUT(sdl->format->format) != 0 && KYTY_SDL_PIXELLAYOUT(sdl->format->format) != SDL_PACKEDLAYOUT_8888);

	if (metadata.bits_per_pixel == 8)
	{
		metadata.order = HostImageSurfaceOrder::Alpha;
	} else if (SDL_ISPIXELFORMAT_PACKED(sdl->format->format)) // NOLINT(hicpp-signed-bitwise)
	{
		switch (KYTY_SDL_PIXELORDER(sdl->format->format))
		{
			case SDL_PACKEDORDER_NONE: EXIT("SDL_PACKEDORDER_NONE\n"); break;
			case SDL_PACKEDORDER_XRGB: EXIT("SDL_PACKEDORDER_XRGB\n"); break;
			case SDL_PACKEDORDER_RGBX: EXIT("SDL_PACKEDORDER_RGBX\n"); break;

			// TGA32, BMP32
			case SDL_PACKEDORDER_ARGB:
				BgraToRgba(sdl);
				metadata.order = HostImageSurfaceOrder::Rgba;
				break;

			case SDL_PACKEDORDER_RGBA: EXIT("SDL_PACKEDORDER_RGBA\n"); break;
			case SDL_PACKEDORDER_XBGR: EXIT("SDL_PACKEDORDER_XBGR\n"); break;
			case SDL_PACKEDORDER_BGRX: EXIT("SDL_PACKEDORDER_BGRX\n"); break;

			// WEBP32, PNG32
			case SDL_PACKEDORDER_ABGR: metadata.order = HostImageSurfaceOrder::Rgba; break;

			case SDL_PACKEDORDER_BGRA: EXIT("SDL_PACKEDORDER_BGRA\n"); break;
			default: EXIT("unknown packed format %d\n", int(KYTY_SDL_PIXELORDER(sdl->format->format)));
		}
	} else if (SDL_ISPIXELFORMAT_ARRAY(sdl->format->format)) // NOLINT(hicpp-signed-bitwise)
	{
		switch (KYTY_SDL_PIXELORDER(sdl->format->format))
		{
			case SDL_ARRAYORDER_NONE: EXIT("SDL_ARRAYORDER_NONE\n"); break;

			// WEBP24, PNG24
			case SDL_ARRAYORDER_RGB: metadata.order = HostImageSurfaceOrder::Rgb; break;

			case SDL_ARRAYORDER_RGBA: EXIT("SDL_ARRAYORDER_RGBA\n"); break;
			case SDL_ARRAYORDER_ARGB: EXIT("SDL_ARRAYORDER_ARGB\n"); break;

			// TGA24, BMP24
			case SDL_ARRAYORDER_BGR:
				BgrToRgb(sdl);
				metadata.order = HostImageSurfaceOrder::Rgb;
				break;

			case SDL_ARRAYORDER_BGRA: EXIT("SDL_ARRAYORDER_BGRA\n"); break;
			case SDL_ARRAYORDER_ABGR: EXIT("SDL_ARRAYORDER_ABGR\n"); break;
			default: EXIT("unknown array format %d\n", int(KYTY_SDL_PIXELORDER(sdl->format->format)));
		}
	} else
	{
		EXIT("invalid format\n");
	}

	EXIT_IF(static_cast<int>(KYTY_SDL_BITSPERPIXEL(sdl->format->format)) != sdl->format->BitsPerPixel);
	EXIT_IF(metadata.bits_per_pixel != 24 && metadata.bits_per_pixel != 32 && metadata.bits_per_pixel != 8);

	const uint64_t tight_pitch = static_cast<uint64_t>(metadata.width) * static_cast<uint32_t>(metadata.bits_per_pixel >> 3u);
	EXIT_IF(metadata.pitch < tight_pitch || static_cast<uint64_t>(metadata.pitch) - tight_pitch >= 4u);

	metadata.bytes_per_pixel = sdl->format->BytesPerPixel;

	return result;
}

HostImageSurface::HostImageSurface(HostImageSurfacePrivate* surface): m_surface(surface) {}

HostImageSurface::~HostImageSurface()
{
	if (m_surface != nullptr)
	{
		Delete(m_surface);
	}
}

HostImageSurface* HostImageSurface::LoadPng(const String& file_name)
{
	File file;
	if (!file.Open(file_name, File::Mode::Read))
	{
		EXIT("Can't open file %s\n", file_name.C_Str());
	}

	SDL_RWops* ops = file.CreateSdlRWops();
	EXIT_IF(ops == nullptr);

	constexpr int decoded_channels = 4;
	int           width            = 0;
	int           height           = 0;
	int           source_channels  = 0;

	stbi_io_callbacks callbacks {};
	callbacks.read = ReadPng;
	callbacks.skip = SkipPng;
	callbacks.eof  = EofPng;

	void* pixels = stbi_load_from_callbacks(&callbacks, ops, &width, &height, &source_channels, decoded_channels);
	SDL_RWclose(ops);

	if (pixels == nullptr)
	{
		EXIT("Can't load png file %s\n", file_name.C_Str());
	}

	if (width <= 0 || height <= 0 || source_channels <= 0 || source_channels > decoded_channels || width > INT_MAX || height > INT_MAX ||
	    width > INT_MAX / decoded_channels)
	{
		stbi_image_free(pixels);
		EXIT("Invalid png dimensions %s\n", file_name.C_Str());
	}

	const int pitch = width * decoded_channels;

	const uint32_t r_mask = 0x000000FF;
	const uint32_t g_mask = 0x0000FF00;
	const uint32_t b_mask = 0x00FF0000;
	const uint32_t a_mask = 0xFF000000;

	SDL_Surface* sdl = SDL_CreateRGBSurfaceFrom(pixels, width, height, decoded_channels * 8, pitch, r_mask, g_mask, b_mask, a_mask);
	if (sdl == nullptr)
	{
		stbi_image_free(pixels);
		EXIT("SDL error: %s\n", SDL_GetError());
	}

	return CreateFromNative(sdl, pixels);
}

HostImageSurface* HostImageSurface::Create(uint32_t width, uint32_t height, int bits_per_pixel)
{
	EXIT_IF(bits_per_pixel != 24 && bits_per_pixel != 32);
	EXIT_IF(width == 0 || height == 0 || width > static_cast<uint32_t>(INT_MAX) || height > static_cast<uint32_t>(INT_MAX));

	const uint32_t a_mask = 0xFF000000;
	const uint32_t b_mask = 0x00FF0000;
	const uint32_t g_mask = 0x0000FF00;
	const uint32_t r_mask = 0x000000FF;

	SDL_Surface* sdl =
	    SDL_CreateRGBSurface(0, static_cast<int>(width), static_cast<int>(height), bits_per_pixel, r_mask, g_mask, b_mask, a_mask);
	if (sdl == nullptr)
	{
		EXIT("SDL error: %s\n", SDL_GetError());
	}

	SDL_Rect rect = {0, 0, static_cast<int>(width), static_cast<int>(height)};
	if (SDL_FillRect(sdl, &rect, 0) != 0)
	{
		SDL_FreeSurface(sdl);
		EXIT("SDL error: %s\n", SDL_GetError());
	}

	return CreateFromNative(sdl, nullptr);
}

HostImageSurface* HostImageSurface::FromNative(void* native_surface)
{
	return CreateFromNative(native_surface, nullptr);
}

HostImageSurface* HostImageSurface::Clone() const
{
	EXIT_IF(m_surface == nullptr || m_surface->sdl == nullptr);

	SDL_Surface* copy = SDL_ConvertSurface(m_surface->sdl, m_surface->sdl->format, SDL_SWSURFACE);
	if (copy == nullptr)
	{
		EXIT("SDL error: %s\n", SDL_GetError());
	}

	return CreateFromNative(copy, nullptr);
}

void HostImageSurface::SaveBmp(const String& file_name) const
{
	EXIT_IF(m_surface == nullptr || m_surface->sdl == nullptr);

	File file;
	if (!file.Create(file_name))
	{
		EXIT("Can't create file %s\n", file_name.C_Str());
	}

	SDL_RWops* ops = file.CreateSdlRWops();
	EXIT_IF(ops == nullptr);

	const int result = SDL_SaveBMP_RW(m_surface->sdl, ops, 1);
	if (result != 0)
	{
		EXIT("SDL error: %s\n", SDL_GetError());
	}
}

bool HostImageSurface::BlitTo(HostImageSurface* destination, const HostImageSurfaceRect& source, const HostImageSurfaceRect& target) const
{
	EXIT_IF(m_surface == nullptr || m_surface->sdl == nullptr || destination == nullptr || destination->m_surface == nullptr ||
	        destination->m_surface->sdl == nullptr);
	EXIT_IF(!RectIsInside(m_surface->metadata, source) || !RectIsInside(destination->m_surface->metadata, target));

	SDL_SetSurfaceBlendMode(m_surface->sdl, SDL_BLENDMODE_NONE);

	SDL_Rect source_rect = {static_cast<int>(source.x), static_cast<int>(source.y), static_cast<int>(source.width),
	                        static_cast<int>(source.height)};
	SDL_Rect target_rect = {static_cast<int>(target.x), static_cast<int>(target.y), static_cast<int>(target.width),
	                        static_cast<int>(target.height)};

	const int result = source.width != target.width || source.height != target.height
	                       ? SDL_BlitScaled(m_surface->sdl, &source_rect, destination->m_surface->sdl, &target_rect)
	                       : SDL_BlitSurface(m_surface->sdl, &source_rect, destination->m_surface->sdl, &target_rect);

	if (result != 0)
	{
		KYTY_LOG_DEBUG("Blit failed: %s\n", SDL_GetError());
		return false;
	}

	return true;
}

const HostImageSurfaceMetadata& HostImageSurface::GetMetadata() const
{
	EXIT_IF(m_surface == nullptr);
	return m_surface->metadata;
}

const void* HostImageSurface::GetPixels() const
{
	EXIT_IF(m_surface == nullptr || m_surface->sdl == nullptr);
	return m_surface->sdl->pixels;
}

void* HostImageSurface::GetPixels()
{
	EXIT_IF(m_surface == nullptr || m_surface->sdl == nullptr);
	return m_surface->sdl->pixels;
}

void* HostImageSurface::GetNativeHandle() const
{
	EXIT_IF(m_surface == nullptr || m_surface->sdl == nullptr);
	return m_surface->sdl;
}

void HostImageSurface::DbgPrint(const String& name) const
{
	EXIT_IF(m_surface == nullptr || m_surface->sdl == nullptr || m_surface->sdl->format == nullptr);

	const SDL_Surface* sdl = m_surface->sdl;
	KYTY_LOG_DEBUG("------\n");
	KYTY_LOG_DEBUG("%s:\n", name.utf8_str().GetData());
	KYTY_LOG_DEBUG("width = %d\n", sdl->w);
	KYTY_LOG_DEBUG("height = %d\n", sdl->h);
	KYTY_LOG_DEBUG("pitch = %d\n", sdl->pitch);
	KYTY_LOG_DEBUG("type = %d\n", static_cast<int>(KYTY_SDL_PIXELTYPE(sdl->format->format)));

	if (m_surface->metadata.bits_per_pixel == 8)
	{
		KYTY_LOG_DEBUG("order = alpha\n");
	} else if (SDL_ISPIXELFORMAT_PACKED(sdl->format->format)) // NOLINT(hicpp-signed-bitwise)
	{
		switch (KYTY_SDL_PIXELORDER(sdl->format->format))
		{
			case SDL_PACKEDORDER_NONE: KYTY_LOG_DEBUG("order = SDL_PACKEDORDER_NONE\n"); break;
			case SDL_PACKEDORDER_XRGB: KYTY_LOG_DEBUG("order = SDL_PACKEDORDER_XRGB\n"); break;
			case SDL_PACKEDORDER_RGBX: KYTY_LOG_DEBUG("order = SDL_PACKEDORDER_RGBX\n"); break;
			case SDL_PACKEDORDER_ARGB: KYTY_LOG_DEBUG("order = SDL_PACKEDORDER_ARGB\n"); break;
			case SDL_PACKEDORDER_RGBA: KYTY_LOG_DEBUG("order = SDL_PACKEDORDER_RGBA\n"); break;
			case SDL_PACKEDORDER_XBGR: KYTY_LOG_DEBUG("order = SDL_PACKEDORDER_XBGR\n"); break;
			case SDL_PACKEDORDER_BGRX: KYTY_LOG_DEBUG("order = SDL_PACKEDORDER_BGRX\n"); break;
			case SDL_PACKEDORDER_ABGR: KYTY_LOG_DEBUG("order = SDL_PACKEDORDER_ABGR\n"); break;
			case SDL_PACKEDORDER_BGRA: KYTY_LOG_DEBUG("order = SDL_PACKEDORDER_BGRA\n"); break;
			default: KYTY_LOG_DEBUG("order = <packed_invalid>\n");
		}
	} else if (SDL_ISPIXELFORMAT_ARRAY(sdl->format->format)) // NOLINT(hicpp-signed-bitwise)
	{
		switch (KYTY_SDL_PIXELORDER(sdl->format->format))
		{
			case SDL_ARRAYORDER_NONE: KYTY_LOG_DEBUG("order = SDL_ARRAYORDER_NONE\n"); break;
			case SDL_ARRAYORDER_RGB: KYTY_LOG_DEBUG("order = SDL_ARRAYORDER_RGB\n"); break;
			case SDL_ARRAYORDER_RGBA: KYTY_LOG_DEBUG("order = SDL_ARRAYORDER_RGBA\n"); break;
			case SDL_ARRAYORDER_ARGB: KYTY_LOG_DEBUG("order = SDL_ARRAYORDER_ARGB\n"); break;
			case SDL_ARRAYORDER_BGR: KYTY_LOG_DEBUG("order = SDL_ARRAYORDER_BGR\n"); break;
			case SDL_ARRAYORDER_BGRA: KYTY_LOG_DEBUG("order = SDL_ARRAYORDER_BGRA\n"); break;
			case SDL_ARRAYORDER_ABGR: KYTY_LOG_DEBUG("order = SDL_ARRAYORDER_ABGR\n"); break;
			default: KYTY_LOG_DEBUG("order = <array_invalid>\n");
		}
	}

	KYTY_LOG_DEBUG("layout = %d\n", static_cast<int>(KYTY_SDL_PIXELLAYOUT(sdl->format->format)));
	KYTY_LOG_DEBUG("bits_per_pixel = %d\n", static_cast<int>(KYTY_SDL_BITSPERPIXEL(sdl->format->format)));
	KYTY_LOG_DEBUG("bytes_per_pixel = %d\n", static_cast<int>(SDL_BYTESPERPIXEL(sdl->format->format))); // NOLINT(hicpp-signed-bitwise)
	KYTY_LOG_DEBUG("bits_per_pixel = %d\n", static_cast<int>(sdl->format->BitsPerPixel));
	KYTY_LOG_DEBUG("bytes_per_pixel = %d\n", static_cast<int>(sdl->format->BytesPerPixel));
}

} // namespace Kyty::Emulator::Host
