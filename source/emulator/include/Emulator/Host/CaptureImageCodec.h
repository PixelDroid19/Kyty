#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_CAPTUREIMAGECODEC_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_CAPTUREIMAGECODEC_H_

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Kyty::Emulator::Host {

enum class HostCaptureImagePixelFormat: uint8_t
{
	Rgba8,
	Bgra8,
	Rgba16G16B16A16Sfloat,
};

struct HostCaptureImageExtent
{
	uint32_t width  = 0;
	uint32_t height = 0;
};

struct HostCaptureImageView
{
	const uint8_t*              pixels          = nullptr;
	HostCaptureImageExtent      extent          {};
	uint64_t                    row_pitch_bytes = 0;
	HostCaptureImagePixelFormat format          = HostCaptureImagePixelFormat::Rgba8;
};

enum class HostCaptureImageCodecError: uint8_t
{
	None,
	InvalidInput,
	CreateSurface,
	SaveImage,
};

struct HostCaptureImageCodecResult
{
	bool                       success            = false;
	HostCaptureImageCodecError error              = HostCaptureImageCodecError::InvalidInput;
	HostCaptureImageExtent     output_extent      {};
	bool                       downscale_fallback = false;
};

// Returns the target extent using the native capture's rounded max-edge rule.
[[nodiscard]] HostCaptureImageExtent HostCaptureImageCodecScaleToMaxEdge(HostCaptureImageExtent extent, uint32_t max_edge);

// Converts a strided source image to tightly packed RGBA8. The source format
// is explicit so callers do not need to expose platform surface types.
[[nodiscard]] bool HostCaptureImageCodecNormalizeRgba8(const HostCaptureImageView& source, std::vector<uint8_t>* rgba_out);

// Encodes an RGBA8 or BGRA8 source as PNG, applying the max-edge downscale.
// If scaling cannot complete, it writes the original extent and reports the fallback.
[[nodiscard]] HostCaptureImageCodecResult HostCaptureImageCodecWritePng(const HostCaptureImageView& source, uint32_t max_edge,
                                                                         const std::filesystem::path& path);

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_CAPTUREIMAGECODEC_H_ */
