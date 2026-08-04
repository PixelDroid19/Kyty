#include "Emulator/Graphics/NativeCapture.h"

#include "Emulator/Graphics/Utils.h"
#include "Emulator/Host/Platform.h"

#include "SDL_surface.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool NativeCaptureEnvEnabled(const char* name)
{
	const char* value = std::getenv(name);
	return value != nullptr && value[0] == '1' && value[1] == '\0';
}

uint32_t NativeCaptureEnvPositive(const char* name)
{
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0')
	{
		return 0;
	}

	char*      end    = nullptr;
	const auto parsed = std::strtoul(value, &end, 10);
	if (end == value || *end != '\0' || parsed == 0 || parsed > UINT32_MAX)
	{
		return 0;
	}
	return static_cast<uint32_t>(parsed);
}

std::string NativeCaptureSanitizeName(std::string value, const char* default_name)
{
	for (auto& character: value)
	{
		const auto c = static_cast<unsigned char>(character);
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
		{
			character = '_';
		}
	}
	if (value.empty())
	{
		value = default_name;
	}
	if (value.size() > 64)
	{
		value.resize(64);
	}
	return value;
}

std::string NativeCaptureUtcStamp()
{
	return Kyty::Emulator::Host::UtcTimestamp();
}

const char* NativeCaptureFormatName(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
		case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
		default: return "VK_FORMAT_UNSUPPORTED";
	}
}

uint64_t NativeCaptureHostPeakRssBytes()
{
	return Kyty::Emulator::Host::PeakRssBytes();
}

bool NativeCaptureSaveSdlSurfacePng(SDL_Surface* surface, const std::filesystem::path& path)
{
	if (surface == nullptr || surface->format == nullptr || surface->w <= 0 || surface->h <= 0 || surface->format->BytesPerPixel != 4)
	{
		return false;
	}

	const bool must_lock = SDL_MUSTLOCK(surface);
	if (must_lock && SDL_LockSurface(surface) != 0)
	{
		return false;
	}

	const uint32_t       width  = static_cast<uint32_t>(surface->w);
	const uint32_t       height = static_cast<uint32_t>(surface->h);
	std::vector<uint8_t> rgba(static_cast<uint64_t>(width) * height * 4u);
	for (uint32_t y = 0; y < height; y++)
	{
		const auto* src_row = static_cast<const uint8_t*>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
		auto*       dst_row = rgba.data() + static_cast<uint64_t>(y) * width * 4u;
		for (uint32_t x = 0; x < width; x++)
		{
			uint32_t pixel = 0;
			std::memcpy(&pixel, src_row + static_cast<size_t>(x) * 4u, sizeof(pixel));
			SDL_GetRGBA(pixel, surface->format, dst_row + x * 4u, dst_row + x * 4u + 1u, dst_row + x * 4u + 2u, dst_row + x * 4u + 3u);
		}
	}

	if (must_lock)
	{
		SDL_UnlockSurface(surface);
	}
	return UtilWriteRgba8Png(path.string().c_str(), rgba.data(), width, height, width);
}

bool NativeCaptureWriteMetadata(const std::filesystem::path& image_path, const NativeCaptureMetadata& metadata)
{
	const auto metadata_path = image_path.string() + ".json";
	auto*      file          = std::fopen(metadata_path.c_str(), "wb");
	if (file == nullptr)
	{
		return false;
	}

	std::fprintf(file,
	             "{\n  \"schema_version\": 1,\n  \"milestone\": \"%s\",\n  \"frame\": %d,\n  "
	             "\"present\": %llu,\n  \"title_id\": \"%s\",\n  \"app_version\": \"%s\",\n  "
	             "\"build_revision\": \"%s\",\n  \"build_dirty\": %s,\n  \"backend\": \"Vulkan\",\n  "
	             "\"format\": \"%s\",\n  \"image_format\": \"PNG\",\n  \"mime_type\": \"image/png\",\n  "
	             "\"width\": %u,\n  \"height\": %u,\n  "
	             "\"source_width\": %u,\n  \"source_height\": %u,\n  "
	             "\"image\": \"%s\",\n  \"host_peak_rss_bytes\": %llu\n}\n",
	             metadata.milestone, metadata.frame, static_cast<unsigned long long>(metadata.present), metadata.title_id,
	             metadata.app_version, metadata.build_revision, metadata.build_dirty ? "true" : "false", metadata.format, metadata.width,
	             metadata.height, metadata.source_width, metadata.source_height, metadata.image_filename,
	             static_cast<unsigned long long>(metadata.host_peak_rss_bytes));

	return std::fclose(file) == 0;
}

void NativeCapturePruneDirectory(const std::filesystem::path& directory, uint32_t keep_files)
{
	if (keep_files == 0 || directory.empty())
	{
		return;
	}

	std::error_code                               list_error;
	std::vector<std::filesystem::directory_entry> pngs;
	for (const auto& entry: std::filesystem::directory_iterator(directory, list_error))
	{
		if (!list_error && entry.is_regular_file() && entry.path().extension() == ".png")
		{
			pngs.push_back(entry);
		}
	}
	std::sort(pngs.begin(), pngs.end(),
	          [](const std::filesystem::directory_entry& a, const std::filesystem::directory_entry& b)
	          {
		          std::error_code ea;
		          std::error_code eb;
		          return a.last_write_time(ea) > b.last_write_time(eb);
	          });
	const size_t prune = NativeCapturePruneCount(pngs.size(), keep_files);
	for (size_t i = pngs.size() - prune; i < pngs.size(); ++i)
	{
		std::error_code remove_error;
		std::filesystem::remove(pngs[i].path(), remove_error);
		std::filesystem::remove(pngs[i].path().string() + ".json", remove_error);
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
