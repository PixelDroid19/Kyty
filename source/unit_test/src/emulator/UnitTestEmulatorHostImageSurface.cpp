#include "Kyty/Core/String.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Host/ImageSurface.h"
#include "Emulator/Host/Png.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

UT_BEGIN(EmulatorHostImageSurface);

namespace {

class ScopedHostImageSurfaceFile final
{
public:
	explicit ScopedHostImageSurfaceFile(const char* extension)
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		path             = std::filesystem::temp_directory_path() / ("kyty-host-image-surface-" + std::to_string(nonce) + extension);
	}

	~ScopedHostImageSurfaceFile()
	{
		std::error_code error;
		std::filesystem::remove(path, error);
	}

	std::filesystem::path path;
};

Core::String HostImageSurfacePath(const std::filesystem::path& path)
{
	return Core::String::FromUtf8(path.string().c_str());
}

bool WriteFixturePng(const std::filesystem::path& path, const uint8_t* bytes, size_t size)
{
	std::ofstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		return false;
	}
	file.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
	return file.good();
}

uint8_t* PixelAt(Emulator::Host::HostImageSurface* surface, uint32_t x, uint32_t y)
{
	const auto& metadata = surface->GetMetadata();
	return static_cast<uint8_t*>(surface->GetPixels()) + static_cast<size_t>(y) * metadata.pitch + static_cast<size_t>(x) * 4u;
}

} // namespace

TEST(EmulatorHostImageSurface, CreatesClonesAndBlitsRgbaPixels)
{
	using namespace Emulator::Host;

	std::unique_ptr<HostImageSurface> source(HostImageSurface::Create(2, 2, 32));
	ASSERT_NE(source, nullptr);
	EXPECT_NE(source->GetNativeHandle(), nullptr);
	EXPECT_EQ(source->GetMetadata().width, 2u);
	EXPECT_EQ(source->GetMetadata().height, 2u);
	EXPECT_EQ(source->GetMetadata().bits_per_pixel, 32);
	EXPECT_EQ(source->GetMetadata().order, HostImageSurfaceOrder::Rgba);

	std::unique_ptr<HostImageSurface> rgb(HostImageSurface::Create(2, 1, 24));
	ASSERT_NE(rgb, nullptr);
	EXPECT_EQ(rgb->GetMetadata().bits_per_pixel, 24);
	EXPECT_EQ(rgb->GetMetadata().bytes_per_pixel, 3);
	EXPECT_EQ(rgb->GetMetadata().order, HostImageSurfaceOrder::Rgb);
	EXPECT_GE(rgb->GetMetadata().pitch, 6u);
	EXPECT_NE(rgb->GetPixels(), nullptr);

	const uint8_t source_pixel[] = {1, 2, 3, 4};
	std::copy(source_pixel, source_pixel + sizeof(source_pixel), PixelAt(source.get(), 0, 0));

	std::unique_ptr<HostImageSurface> clone(source->Clone());
	ASSERT_NE(clone, nullptr);
	EXPECT_EQ(PixelAt(clone.get(), 0, 0)[0], 1u);
	EXPECT_EQ(PixelAt(clone.get(), 0, 0)[1], 2u);
	EXPECT_EQ(PixelAt(clone.get(), 0, 0)[2], 3u);
	EXPECT_EQ(PixelAt(clone.get(), 0, 0)[3], 4u);

	PixelAt(source.get(), 0, 0)[0] = 9;
	EXPECT_EQ(PixelAt(clone.get(), 0, 0)[0], 1u);

	std::unique_ptr<HostImageSurface> destination(HostImageSurface::Create(4, 4, 32));
	ASSERT_NE(destination, nullptr);
	EXPECT_TRUE(source->BlitTo(destination.get(), {0, 0, 2, 2}, {1, 1, 2, 2}));
	EXPECT_EQ(PixelAt(destination.get(), 1, 1)[0], 9u);
	EXPECT_EQ(PixelAt(destination.get(), 1, 1)[1], 2u);
	EXPECT_EQ(PixelAt(destination.get(), 1, 1)[2], 3u);
	EXPECT_EQ(PixelAt(destination.get(), 1, 1)[3], 4u);
}

TEST(EmulatorHostImageSurface, LoadsPngThroughHostFileSurfaceAndWritesBmp)
{
	using namespace Emulator::Host;

	ScopedHostImageSurfaceFile png_file(".png");
	ScopedHostImageSurfaceFile bmp_file(".bmp");
	const uint8_t               expected_pixel[] = {11, 22, 33, 44};
	ASSERT_TRUE(WriteRgba8Png(png_file.path.string().c_str(), expected_pixel, 1, 1, 1));

	std::unique_ptr<HostImageSurface> image(HostImageSurface::LoadPng(HostImageSurfacePath(png_file.path)));
	ASSERT_NE(image, nullptr);
	EXPECT_EQ(image->GetMetadata().width, 1u);
	EXPECT_EQ(image->GetMetadata().height, 1u);
	EXPECT_EQ(image->GetMetadata().bits_per_pixel, 32);
	EXPECT_EQ(image->GetMetadata().order, HostImageSurfaceOrder::Rgba);
	EXPECT_EQ(PixelAt(image.get(), 0, 0)[0], 11u);
	EXPECT_EQ(PixelAt(image.get(), 0, 0)[1], 22u);
	EXPECT_EQ(PixelAt(image.get(), 0, 0)[2], 33u);
	EXPECT_EQ(PixelAt(image.get(), 0, 0)[3], 44u);

	image->SaveBmp(HostImageSurfacePath(bmp_file.path));
	std::ifstream bmp(bmp_file.path, std::ios::binary);
	uint8_t       signature[2] {};
	if (bmp.is_open())
	{
		bmp.read(reinterpret_cast<char*>(signature), sizeof(signature));
	}
	EXPECT_EQ(signature[0], 'B');
	EXPECT_EQ(signature[1], 'M');
	EXPECT_GT(std::filesystem::file_size(bmp_file.path), 2u);
}

TEST(EmulatorHostImageSurface, NormalizesRgbPngToRgba8WithoutRowPadding)
{
	using namespace Emulator::Host;

	static constexpr uint8_t png[] = {
	    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
	    0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0x16, 0xe3, 0x21, 0x70, 0x00, 0x00, 0x00,
	    0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xe0, 0x12, 0x91, 0x63, 0xd0, 0x30, 0xb2, 0x01, 0x00, 0x02,
	    0x74, 0x00, 0xd3, 0x96, 0x4d, 0xcc, 0x95, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
	    0x82,
	};

	ScopedHostImageSurfaceFile png_file(".png");
	ASSERT_TRUE(WriteFixturePng(png_file.path, png, sizeof(png)));

	std::unique_ptr<HostImageSurface> image(HostImageSurface::LoadPng(HostImageSurfacePath(png_file.path)));
	ASSERT_NE(image, nullptr);
	const auto& metadata = image->GetMetadata();
	ASSERT_EQ(metadata.width, 1u);
	ASSERT_EQ(metadata.height, 2u);
	ASSERT_EQ(metadata.bits_per_pixel, 32);
	ASSERT_EQ(metadata.bytes_per_pixel, 4);
	ASSERT_EQ(metadata.pitch, 4u);
	ASSERT_EQ(metadata.order, HostImageSurfaceOrder::Rgba);

	const auto* pixels = static_cast<const uint8_t*>(image->GetPixels());
	ASSERT_NE(pixels, nullptr);
	EXPECT_EQ(pixels[0], 10u);
	EXPECT_EQ(pixels[1], 20u);
	EXPECT_EQ(pixels[2], 30u);
	EXPECT_EQ(pixels[3], 255u);
	EXPECT_EQ(pixels[metadata.pitch + 0u], 40u);
	EXPECT_EQ(pixels[metadata.pitch + 1u], 50u);
	EXPECT_EQ(pixels[metadata.pitch + 2u], 60u);
	EXPECT_EQ(pixels[metadata.pitch + 3u], 255u);
}

TEST(EmulatorHostImageSurface, NormalizesGrayscalePngToRgba8)
{
	using namespace Emulator::Host;

	static constexpr uint8_t png[] = {
	    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
	    0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00, 0xbc, 0xea, 0xe9, 0xfb, 0x00, 0x00, 0x00,
	    0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf0, 0x65, 0x98, 0x0d, 0x00, 0x01, 0x86, 0x00, 0xe9, 0x55,
	    0x4c, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
	};

	ScopedHostImageSurfaceFile png_file(".png");
	ASSERT_TRUE(WriteFixturePng(png_file.path, png, sizeof(png)));

	std::unique_ptr<HostImageSurface> image(HostImageSurface::LoadPng(HostImageSurfacePath(png_file.path)));
	ASSERT_NE(image, nullptr);
	const auto& metadata = image->GetMetadata();
	ASSERT_EQ(metadata.width, 1u);
	ASSERT_EQ(metadata.height, 2u);
	ASSERT_EQ(metadata.bits_per_pixel, 32);
	ASSERT_EQ(metadata.bytes_per_pixel, 4);
	ASSERT_EQ(metadata.pitch, 4u);
	ASSERT_EQ(metadata.order, HostImageSurfaceOrder::Rgba);

	const auto* pixels = static_cast<const uint8_t*>(image->GetPixels());
	ASSERT_NE(pixels, nullptr);
	EXPECT_EQ(pixels[0], 77u);
	EXPECT_EQ(pixels[1], 77u);
	EXPECT_EQ(pixels[2], 77u);
	EXPECT_EQ(pixels[3], 255u);
	EXPECT_EQ(pixels[metadata.pitch + 0u], 155u);
	EXPECT_EQ(pixels[metadata.pitch + 1u], 155u);
	EXPECT_EQ(pixels[metadata.pitch + 2u], 155u);
	EXPECT_EQ(pixels[metadata.pitch + 3u], 255u);
}

UT_END();
