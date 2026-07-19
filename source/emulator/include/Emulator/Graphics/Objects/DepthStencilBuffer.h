#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTHSTENCILBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTHSTENCILBUFFER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class DepthStencilBufferObject: public GpuObject
{
public:
	static constexpr int PARAM_FORMAT       = 0;
	static constexpr int PARAM_GUEST_WIDTH  = 1;
	static constexpr int PARAM_GUEST_HEIGHT = 2;
	static constexpr int PARAM_HTILE        = 3;
	static constexpr int PARAM_NEO          = 4;
	static constexpr int PARAM_USAGE        = 5;
	static constexpr int PARAM_HTILE_ADDR   = 6;
	static constexpr int PARAM_HTILE_SIZE   = 7;
	static constexpr int PARAM_HOST_WIDTH   = 8;
	static constexpr int PARAM_HOST_HEIGHT  = 9;

	DepthStencilBufferObject(uint64_t vk_format, uint32_t width, uint32_t height, bool htile, bool neo, bool sampled, uint64_t htile_addr,
	                         uint64_t htile_size)
	    : DepthStencilBufferObject(vk_format, width, height, width, height, htile, neo, sampled, htile_addr, htile_size)
	{
	}

	DepthStencilBufferObject(uint64_t vk_format, uint32_t guest_width, uint32_t guest_height, uint32_t host_width, uint32_t host_height,
	                         bool htile, bool neo, bool sampled, uint64_t htile_addr, uint64_t htile_size)
	{
		params[PARAM_FORMAT]       = vk_format;
		params[PARAM_GUEST_WIDTH]  = guest_width;
		params[PARAM_GUEST_HEIGHT] = guest_height;
		params[PARAM_HTILE]        = htile ? 1 : 0;
		params[PARAM_NEO]          = neo ? 1 : 0;
		params[PARAM_USAGE]        = sampled ? 1 : 0;
		params[PARAM_HTILE_ADDR]   = htile_addr;
		params[PARAM_HTILE_SIZE]   = htile_size;
		params[PARAM_HOST_WIDTH]   = host_width;
		params[PARAM_HOST_HEIGHT]  = host_height;
		check_hash                 = false;
		type                       = Graphics::GpuMemoryObjectType::DepthStencilBuffer;
	}

	bool Equal(const uint64_t* other) const override;
	//	bool Reuse(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; };
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override { return nullptr; };
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTHSTENCILBUFFER_H_ */
