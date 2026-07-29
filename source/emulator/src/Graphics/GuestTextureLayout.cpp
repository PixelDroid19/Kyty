#include "Emulator/Graphics/GuestTextureLayout.h"

#include <mutex>
#include <unordered_map>

namespace Kyty::Libs::Graphics {

namespace {

struct GuestLinearTextureLayout
{
	size_t   size            = 0;
	uint32_t row_pitch_bytes = 0;
};

std::mutex                                            g_guest_texture_layout_mutex;
std::unordered_map<uint64_t, GuestLinearTextureLayout> g_guest_texture_layouts;

} // namespace

void GuestTextureLayoutRegisterLinear(uint64_t base, size_t size, uint32_t row_pitch_bytes)
{
	if (base == 0 || size == 0 || row_pitch_bytes == 0)
	{
		return;
	}

	std::lock_guard lock(g_guest_texture_layout_mutex);
	g_guest_texture_layouts[base] = GuestLinearTextureLayout {size, row_pitch_bytes};
}

void GuestTextureLayoutUnregister(uint64_t base)
{
	std::lock_guard lock(g_guest_texture_layout_mutex);
	g_guest_texture_layouts.erase(base);
}

uint32_t GuestTextureLayoutGetLinearRowPitch(uint64_t address, uint32_t visible_row_bytes)
{
	if (address == 0 || visible_row_bytes == 0)
	{
		return 0;
	}

	std::lock_guard lock(g_guest_texture_layout_mutex);
	for (const auto& [base, layout]: g_guest_texture_layouts)
	{
		if (layout.size == 0 || layout.row_pitch_bytes < visible_row_bytes)
		{
			continue;
		}
		if (address >= base && address - base < layout.size)
		{
			return layout.row_pitch_bytes;
		}
	}
	return 0;
}

} // namespace Kyty::Libs::Graphics
