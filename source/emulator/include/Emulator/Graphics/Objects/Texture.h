#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_TEXTURE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_TEXTURE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Utils.h"

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VkFormat TextureResolveSampledVkFormat(uint8_t dfmt, uint8_t nfmt, uint16_t fmt, bool force_degamma = false);

class TextureObject: public GpuObject
{
public:
	static constexpr int PARAM_FORMAT        = 0;
	static constexpr int PARAM_PITCH         = 1;
	static constexpr int PARAM_WIDTH_HEIGHT  = 2;
	static constexpr int PARAM_LEVELS        = 3;
	static constexpr int PARAM_TILE          = 4;
	static constexpr int PARAM_NEO           = 5;
	static constexpr int PARAM_SWIZZLE       = 6;
	static constexpr int PARAM_FORCE_DEGAMMA = 7;
	// When set, update_func clears transparent black and never reads guest
	// (GPU-owned range under a live color surface that is not bindable).
	static constexpr int PARAM_SKIP_GUEST_UPLOAD = 8;
	static constexpr int PARAM_RESOURCE_INFO     = 9;

	static constexpr uint64_t PackResourceInfo(uint8_t resource_type, uint32_t depth, uint32_t base_array = 0u)
	{
		return static_cast<uint64_t>(resource_type) | (static_cast<uint64_t>(depth) << 8u) |
		       (static_cast<uint64_t>(base_array) << 24u);
	}

	static constexpr uint8_t GetResourceType(uint64_t resource_info)
	{
		return static_cast<uint8_t>(resource_info & 0xffu);
	}

	static constexpr uint32_t GetResourceDepth(uint64_t resource_info)
	{
		return static_cast<uint32_t>((resource_info >> 8u) & 0xffffu);
	}

	static constexpr uint32_t GetResourceBaseArray(uint64_t resource_info)
	{
		return static_cast<uint32_t>(resource_info >> 24u);
	}

	TextureObject(uint8_t dfmt, uint8_t nfmt, uint16_t fmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t base_level,
	              uint32_t levels, uint32_t tile, bool neo, uint32_t swizzle, bool force_degamma, bool skip_guest_upload = false,
	              uint8_t resource_type = 9u, uint32_t depth = 1u, uint32_t base_array = 0u)
	{
		params[PARAM_FORMAT]            = (static_cast<uint64_t>(fmt) << 16u) | (static_cast<uint64_t>(dfmt) << 8u) | nfmt;
		params[PARAM_PITCH]             = pitch;
		params[PARAM_WIDTH_HEIGHT]      = (static_cast<uint64_t>(width) << 32u) | height;
		params[PARAM_LEVELS]            = (static_cast<uint64_t>(base_level) << 32u) | levels;
		params[PARAM_TILE]              = tile;
		params[PARAM_NEO]               = neo ? 1 : 0;
		params[PARAM_SWIZZLE]            = swizzle;
		params[PARAM_FORCE_DEGAMMA]      = force_degamma ? 1 : 0;
		params[PARAM_SKIP_GUEST_UPLOAD] = skip_guest_upload ? 1 : 0;
		params[PARAM_RESOURCE_INFO]     = PackResourceInfo(resource_type, depth, base_array);
		check_hash                      = Gen5SampleTextureUsesHashRefresh(fmt);
		type                            = Graphics::GpuMemoryObjectType::Texture;
	}

	bool Equal(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override;
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override { return nullptr; };
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_TEXTURE_H_ */
