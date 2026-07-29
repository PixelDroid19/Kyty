#include "Emulator/Graphics/GuestTextureLayout.h"

#include <map>
#include <mutex>

namespace Kyty::Libs::Graphics {

namespace {

struct GuestLinearTextureLayout
{
	uint64_t end             = 0;
	uint32_t row_pitch_bytes = 0;
};

std::mutex                                      g_guest_texture_layout_mutex;
std::map<uint64_t, GuestLinearTextureLayout>    g_guest_texture_layouts;

} // namespace

void GuestTextureLayoutRegisterLinear(uint64_t base, size_t size, uint32_t row_pitch_bytes)
{
	if (base == 0 || size == 0 || row_pitch_bytes == 0 || size > UINT64_MAX - base)
	{
		return;
	}

	std::lock_guard lock(g_guest_texture_layout_mutex);
	g_guest_texture_layouts[base] = GuestLinearTextureLayout {base + static_cast<uint64_t>(size), row_pitch_bytes};
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
	auto it = g_guest_texture_layouts.upper_bound(address);
	if (it == g_guest_texture_layouts.begin())
	{
		return 0;
	}
	--it;
	const auto& layout = it->second;
	return (address < layout.end && layout.row_pitch_bytes == visible_row_bytes ? layout.row_pitch_bytes : 0);
}

} // namespace Kyty::Libs::Graphics
