#include "Emulator/Graphics/ShaderProgramAddress.h"
#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool ShaderWriteProgramBaseMetadata(const ShaderBindResources& bind, uint32_t* metadata, uint32_t capacity_dw)
{
	if (!bind.program_base_used) { return true; }
	const uint32_t size_dw = bind.push_constant_size / 4u;
	const uint32_t offset  = bind.program_base_offset_dw;
	if (metadata == nullptr || (bind.push_constant_size % 16u) != 0u || (offset % 4u) != 0u ||
	    size_dw > capacity_dw || size_dw < 4u || offset != size_dw - 4u)
	{
		return false;
	}
	metadata[offset]      = static_cast<uint32_t>(bind.program_base);
	metadata[offset + 1u] = static_cast<uint32_t>(bind.program_base >> 32u);
	metadata[offset + 2u] = 0;
	metadata[offset + 3u] = 0;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
