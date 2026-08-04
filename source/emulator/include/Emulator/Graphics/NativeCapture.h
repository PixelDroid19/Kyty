#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_NATIVECAPTURE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_NATIVECAPTURE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct NativeCaptureMetadata
{
	const char* milestone           = "none";
	int         frame               = 0;
	uint64_t    present             = 0;
	const char* title_id            = "unknown-title";
	const char* app_version         = "unknown-version";
	const char* build_revision      = "unknown-revision";
	bool        build_dirty         = false;
	const char* format              = "VK_FORMAT_UNSUPPORTED";
	uint32_t    width               = 0;
	uint32_t    height              = 0;
	uint32_t    source_width        = 0;
	uint32_t    source_height       = 0;
	const char* image_filename      = "";
	uint64_t    host_peak_rss_bytes = 0;
};

[[nodiscard]] bool        NativeCaptureEnvEnabled(const char* name);
[[nodiscard]] uint32_t    NativeCaptureEnvPositive(const char* name);
[[nodiscard]] std::string NativeCaptureSanitizeName(std::string value, const char* default_name);
[[nodiscard]] std::string NativeCaptureUtcStamp();
[[nodiscard]] const char* NativeCaptureFormatName(VkFormat format);
[[nodiscard]] uint64_t    NativeCaptureHostPeakRssBytes();

[[nodiscard]] bool NativeCaptureWriteMetadata(const std::filesystem::path& image_path, const NativeCaptureMetadata& metadata);
void               NativeCapturePruneDirectory(const std::filesystem::path& directory, uint32_t keep_files);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_NATIVECAPTURE_H_ */
