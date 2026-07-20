#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VIDEOOUTBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VIDEOOUTBUFFER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct GraphicContext;
struct VideoOutVulkanImage;

enum class VideoOutBufferFormat : uint64_t
{
	Unknown,
	R8G8B8A8Srgb,
	B8G8R8A8Srgb,
	// Gen5 SCE_VIDEO_OUT_PIXEL_FORMAT2_* 10:10:10:2 (still 32 bpp, same pitch/size).
	R10G10B10A2Unorm,
	B10G10R10A2Unorm,
};

// Tiled display buffers are GPU-owned; CPU upload on Update would LOAD stale
// guest memory before the renderer has filled the image.
[[nodiscard]] inline bool VideoOutBufferShouldCpuUploadOnUpdate(bool tiled)
{
	return !tiled;
}

[[nodiscard]] bool VideoOutBufferNeedsMaterialization(const VideoOutVulkanImage* image);
void               VideoOutBufferEnsureMaterialized(GraphicContext* ctx, VideoOutVulkanImage* image);

enum class VideoOutHostExtentStatus
{
	Selected,
	StickyMatch,
	StickyMismatch,
	InvalidArgument,
};

struct VideoOutHostExtentState
{
	uint32_t width        = 0;
	uint32_t height       = 0;
	bool     selected     = false;
	bool     materialized = false;
};

enum class VideoOutHostExtentSetSelectionStatus
{
	Selected,
	StickyMatch,
	StickyMismatch,
	InvalidArgument,
	Empty,
};

[[nodiscard]] const char* VideoOutHostExtentSetSelectionStatusName(VideoOutHostExtentSetSelectionStatus status);

enum class VideoOutHostExtentSetInspectionStatus
{
	Uniform,
	Unselected,
	NonUniform,
	InvalidArgument,
	Empty,
};

struct VideoOutHostExtentSetState
{
	uint32_t width       = 0;
	uint32_t height      = 0;
	uint32_t image_count = 0;
};

[[nodiscard]] VideoOutHostExtentStatus VideoOutBufferSelectHostExtent(VideoOutVulkanImage* image, uint32_t width, uint32_t height,
                                                                      VideoOutHostExtentState* state);
[[nodiscard]] bool                     VideoOutBufferGetHostExtentState(VideoOutVulkanImage* image, VideoOutHostExtentState* state);

enum class VideoOutPublishedImageRefreshStatus
{
	Published,
	ExtentConflict,
	InvalidArgument,
};

// Applies the VideoOut-owned host extent before publishing a newly resolved
// GpuMemory backing. The cache is observational only and remains unchanged if
// the current backing cannot honor the registered extent.
[[nodiscard]] VideoOutPublishedImageRefreshStatus
VideoOutBufferRefreshPublishedImage(VideoOutVulkanImage* current, uint32_t host_width, uint32_t host_height,
                                    VideoOutVulkanImage** published_cache);

// Locks every unique image in a stable order and preflights the complete set before selecting any image, so concurrent set selection cannot
// publish a partial or non-uniform extent. Duplicate images are invalid input.
[[nodiscard]] VideoOutHostExtentSetSelectionStatus VideoOutBufferSelectHostExtentSet(VideoOutVulkanImage* const* images,
                                                                                     uint32_t image_count, uint32_t width, uint32_t height,
                                                                                     VideoOutHostExtentSetState* state);
[[nodiscard]] VideoOutHostExtentSetInspectionStatus
VideoOutBufferInspectHostExtentSet(VideoOutVulkanImage* const* images, uint32_t image_count, VideoOutHostExtentSetState* state);

class VideoOutBufferObject: public GpuObject
{
public:
	static constexpr int PARAM_FORMAT = 0;
	static constexpr int PARAM_WIDTH  = 1;
	static constexpr int PARAM_HEIGHT = 2;
	static constexpr int PARAM_TILED  = 3;
	static constexpr int PARAM_NEO    = 4;
	static constexpr int PARAM_PITCH  = 5;

	explicit VideoOutBufferObject(VideoOutBufferFormat pixel_format, uint32_t width, uint32_t height, bool tiled, bool neo, uint32_t pitch)
	{
		params[PARAM_FORMAT] = static_cast<uint64_t>(pixel_format);
		params[PARAM_WIDTH]  = width;
		params[PARAM_HEIGHT] = height;
		params[PARAM_TILED]  = tiled ? 1 : 0;
		params[PARAM_NEO]    = neo ? 1 : 0;
		params[PARAM_PITCH]  = pitch;
		check_hash           = VideoOutBufferShouldCpuUploadOnUpdate(tiled);
		type                 = Graphics::GpuMemoryObjectType::VideoOutBuffer;
	}

	bool Equal(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; };
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override { return nullptr; };
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VIDEOOUTBUFFER_H_ */
