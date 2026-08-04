#include "Emulator/Graphics/Image.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/SafeDelete.h"

#include "Emulator/Host/ImageSurface.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

using Core::File;

namespace {

ImageOrder ToImageOrder(::Kyty::Emulator::Host::HostImageSurfaceOrder order)
{
	using ::Kyty::Emulator::Host::HostImageSurfaceOrder;

	switch (order)
	{
		case HostImageSurfaceOrder::Unknown: return ImageOrder::Unknown;
		case HostImageSurfaceOrder::Rgb: return ImageOrder::Rgb;
		case HostImageSurfaceOrder::Rgba: return ImageOrder::Rgba;
		case HostImageSurfaceOrder::Argb: return ImageOrder::Argb;
		case HostImageSurfaceOrder::Bgr: return ImageOrder::Bgr;
		case HostImageSurfaceOrder::Bgra: return ImageOrder::Bgra;
		case HostImageSurfaceOrder::Abgr: return ImageOrder::Abgr;
		case HostImageSurfaceOrder::Alpha: return ImageOrder::Alpha;
	}

	EXIT("invalid host image order\n");
	return ImageOrder::Unknown;
}

::Kyty::Emulator::Host::HostImageSurfaceRect ToHostRect(const Math::Rect& rect)
{
	return {rect.x, rect.y, rect.width, rect.height};
}

bool RectIsInside(uint32_t width, uint32_t height, const Math::Rect& rect)
{
	return rect.width != 0 && rect.height != 0 && rect.x < width && rect.y < height && rect.width <= width - rect.x &&
	       rect.height <= height - rect.y;
}

} // namespace

Image::Image(const String& name): m_name(name) {}

Image::~Image()
{
	if (m_image != nullptr)
	{
		Delete(m_image);
	}
}

void Image::LoadHostSurface(::Kyty::Emulator::Host::HostImageSurface* surface)
{
	EXIT_IF(surface == nullptr);

	m_image = surface;

	const auto& metadata = m_image->GetMetadata();
	m_width               = metadata.width;
	m_height              = metadata.height;
	m_pitch               = metadata.pitch;
	m_bits_per_pixel      = metadata.bits_per_pixel;
	m_order               = ToImageOrder(metadata.order);
	m_pixels              = m_image->GetPixels();
}

void Image::Load(const String& file_name)
{
	m_name = file_name;
	Load();
}

void Image::Load()
{
	if (m_image != nullptr)
	{
		Delete(m_image);
	}

	if (m_name.EndsWith(U".bmp", String::Case::Insensitive))
	{
		/* sdl = IMG_LoadBMP_RW(ops); */
		KYTY_NOT_IMPLEMENTED;
	} else if (m_name.EndsWith(U".tga", String::Case::Insensitive))
	{
		/* sdl = IMG_LoadTGA_RW(ops); */
		KYTY_NOT_IMPLEMENTED;
	} else if (m_name.EndsWith(U".png", String::Case::Insensitive))
	{
		LoadHostSurface(::Kyty::Emulator::Host::HostImageSurface::LoadPng(m_name));
	} else if (m_name.EndsWith(U".webp", String::Case::Insensitive))
	{
		/* sdl = IMG_LoadWEBP_RW(ops); */
		KYTY_NOT_IMPLEMENTED;
	} else
	{
		EXIT("Unknown image type %s\n", m_name.utf8_str().GetData());
	}
}

void Image::LoadSdl(void* surface)
{
	LoadHostSurface(::Kyty::Emulator::Host::HostImageSurface::FromNative(surface));
}

void Image::DbgPrint() const
{
	EXIT_IF(m_image == nullptr);
	m_image->DbgPrint(m_name);
}

bool Image::DbgEqual(const Image* img) const
{
	if (m_width != img->m_width || m_height != img->m_height || m_pitch != img->m_pitch || m_bits_per_pixel != img->m_bits_per_pixel ||
	    m_order != img->m_order)
	{
		return false;
	}

	for (uint32_t yi = 0; yi < m_height; yi++)
	{
		for (uint32_t xi = 0; xi < m_width; xi++)
		{
			if (GetPixel(xi, yi) != img->GetPixel(xi, yi))
			{
				return false;
			}
		}
	}

	return true;
}

void Image::Save(const String& file_name) const
{
	EXIT_IF(m_image == nullptr);

	if (file_name.EndsWith(U".png", String::Case::Insensitive))
	{
		/* result = IMG_SavePNG_RW(m_image->sdl, ops, 1); */
		KYTY_NOT_IMPLEMENTED;
	} else if (file_name.EndsWith(U".bmp", String::Case::Insensitive))
	{
		m_image->SaveBmp(file_name);
	} else
	{
		EXIT("Unknown image type %s\n", file_name.utf8_str().GetData());
	}
}

Image* Image::Clone() const
{
	auto* image = new Image(m_name);
	if (m_image != nullptr)
	{
		image->LoadHostSurface(m_image->Clone());
	}
	return image;
}

Image* Image::Create(const String& name, uint32_t width, uint32_t height, int bits_per_pixel)
{
	auto* image = new Image(name);
	image->LoadHostSurface(::Kyty::Emulator::Host::HostImageSurface::Create(width, height, bits_per_pixel));
	return image;
}

rgba32_t Image::GetPixel(uint32_t x, uint32_t y) const
{
	if (m_order == ImageOrder::Rgb && m_bits_per_pixel == 24)
	{
		auto* pixel = static_cast<uint8_t*>(m_pixels) + m_pitch * static_cast<size_t>(y) + 3 * static_cast<size_t>(x);
		return Rgb(pixel[0], pixel[1], pixel[2]);
	}

	if (m_order == ImageOrder::Rgba && m_bits_per_pixel == 32)
	{
		auto* pixel = static_cast<uint8_t*>(m_pixels) + m_pitch * static_cast<size_t>(y) + 4 * static_cast<size_t>(x);
		return Rgb(pixel[0], pixel[1], pixel[2], pixel[3]);
	}

	EXIT("invalid format: order = %d, bits = %d\n", int(m_order), int(m_bits_per_pixel));
	return 0;
}

void Image::SetPixel(uint32_t x, uint32_t y, rgba32_t color)
{
	if (m_order == ImageOrder::Rgb && m_bits_per_pixel == 24)
	{
		auto* pixel = static_cast<uint8_t*>(m_pixels) + m_pitch * static_cast<size_t>(y) + 3 * static_cast<size_t>(x);
		pixel[0]    = RgbToRed(color);
		pixel[1]    = RgbToGreen(color);
		pixel[2]    = RgbToBlue(color);
	} else if (m_order == ImageOrder::Rgba && m_bits_per_pixel == 32)
	{
		auto* pixel = static_cast<uint8_t*>(m_pixels) + m_pitch * static_cast<size_t>(y) + 4 * static_cast<size_t>(x);
		pixel[0]    = RgbToRed(color);
		pixel[1]    = RgbToGreen(color);
		pixel[2]    = RgbToBlue(color);
		pixel[3]    = RgbToAlpha(color);
	} else
	{
		EXIT("invalid format: order = %d, bits = %d\n", int(m_order), int(m_bits_per_pixel));
	}
}

rgba32_t Image::GetAvgPixel(uint32_t x, uint32_t y, uint32_t width, uint32_t height) const
{
	EXIT_IF(x >= m_width);
	EXIT_IF(y >= m_height);
	EXIT_IF(x + width > m_width);
	EXIT_IF(y + height > m_height);

	vec4 sum(0.0f);

	for (uint32_t yi = y; yi < y + height; yi++)
	{
		for (uint32_t xi = x; xi < x + width; xi++)
		{
			Color sample(GetPixel(xi, yi));
			sum = sum + sample.Rgba();
		}
	}

	sum /= static_cast<float>(width * height);

	return Color(sum).ToRgba32();
}

bool Image::BlitTo(Image* image, const Math::Rect& source, const Math::Rect& destination) const
{
	EXIT_IF(image == nullptr || m_image == nullptr || image->m_image == nullptr);
	EXIT_IF(!RectIsInside(m_width, m_height, source) || !RectIsInside(image->m_width, image->m_height, destination));

	return m_image->BlitTo(image->m_image, ToHostRect(source), ToHostRect(destination));
}

void* Image::GetSdlSurface() const
{
	EXIT_IF(m_image == nullptr);
	return m_image->GetNativeHandle();
}

Math::Size Image::GetSize(const String& name)
{
	if (!name.EndsWith(U".png", String::Case::Insensitive))
	{
		EXIT("unsupported format: %s\n", name.C_Str());
	}

	File file;
	if (!file.Open(name, File::Mode::Read))
	{
		EXIT("Can't open file %s\n", name.C_Str());
	}

	Math::Size size(0, 0);
	uint32_t   header = 0;

	file.Seek(12);
	file.Read(&header, 4);

	if (header != 0x52444849)
	{
		EXIT("wrong png file: %s\n", name.C_Str());
	}

	file.ReadR(&size.width, 4);
	file.ReadR(&size.height, 4);

	file.Close();

	return size;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
