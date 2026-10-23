#include "Emulator/Graphics/GuestTextureLayout.h"

#include "Kyty/Core/VirtualMemory.h"

#include <array>
#include <memory>
#include <mutex>
#include <new>

namespace Kyty::Libs::Graphics {

namespace {

struct GuestLinearTextureLayout
{
	uint64_t                                          end             = 0;
	uint32_t                                          row_pitch_bytes = 0;
	Core::VirtualMemory::GuestMappingIdentitySnapshot identity {};
};

struct GuestLinearTextureLayoutRecord
{
	uint64_t                  base = 0;
	GuestLinearTextureLayout  layout {};
};

std::mutex                                                              g_guest_texture_layout_mutex;
std::array<GuestLinearTextureLayoutRecord, kGuestTextureLayoutCapacity> g_guest_texture_layouts {};
size_t                                                                  g_guest_texture_layout_count = 0;

size_t lower_bound_base(uint64_t base)
{
	size_t first = 0;
	size_t last  = g_guest_texture_layout_count;
	while (first < last)
	{
		const size_t middle = first + (last - first) / 2;
		if (g_guest_texture_layouts[middle].base < base)
		{
			first = middle + 1;
		} else
		{
			last = middle;
		}
	}
	return first;
}

size_t upper_bound_address(uint64_t address)
{
	size_t first = 0;
	size_t last  = g_guest_texture_layout_count;
	while (first < last)
	{
		const size_t middle = first + (last - first) / 2;
		if (g_guest_texture_layouts[middle].base <= address)
		{
			first = middle + 1;
		} else
		{
			last = middle;
		}
	}
	return first;
}

void erase_layout_at(size_t index)
{
	for (size_t i = index + 1; i < g_guest_texture_layout_count; ++i)
	{
		g_guest_texture_layouts[i - 1] = g_guest_texture_layouts[i];
	}
	--g_guest_texture_layout_count;
	g_guest_texture_layouts[g_guest_texture_layout_count] = {};
}

bool identity_matches(const Core::VirtualMemory::GuestMappingIdentitySnapshot& left,
	                  const Core::VirtualMemory::GuestMappingIdentitySnapshot& right)
{
	if (left.address != right.address || left.size != right.size || left.segment_count != right.segment_count ||
	    left.segment_count > Core::VirtualMemory::GuestMappingIdentitySnapshot::kMaxSegments)
	{
		return false;
	}
	for (uint32_t i = 0; i < left.segment_count; ++i)
	{
		const auto& left_segment  = left.segments[i];
		const auto& right_segment = right.segments[i];
		if (left_segment.address != right_segment.address || left_segment.size != right_segment.size ||
		    left_segment.identity != right_segment.identity)
		{
			return false;
		}
	}
	return true;
}

bool layout_matches(const GuestLinearTextureLayout& left, const GuestLinearTextureLayout& right)
{
	return left.end == right.end && left.row_pitch_bytes == right.row_pitch_bytes && identity_matches(left.identity, right.identity);
}

void erase_layout_if_unchanged(uint64_t base, const GuestLinearTextureLayout& expected)
{
	std::lock_guard lock(g_guest_texture_layout_mutex);
	const size_t    index = lower_bound_base(base);
	if (index < g_guest_texture_layout_count && g_guest_texture_layouts[index].base == base &&
	    layout_matches(g_guest_texture_layouts[index].layout, expected))
	{
		erase_layout_at(index);
	}
}

bool find_current_layout(uint64_t address, GuestLinearTextureLayout* out)
{
	if (address == 0 || out == nullptr)
	{
		return false;
	}

	uint64_t base = 0;
	{
		std::lock_guard lock(g_guest_texture_layout_mutex);
		const size_t    next = upper_bound_address(address);
		if (next == 0)
		{
			return false;
		}
		const auto& record = g_guest_texture_layouts[next - 1];
		*out               = record.layout;
		base               = record.base;
	}

	if (address >= out->end)
	{
		return false;
	}
	// Drop the layout lock before checking guest mapping state. Callers may
	// already hold the VM lock while registering a frame.
	if (Core::VirtualMemory::GuestMappingIdentityMatches(out->identity))
	{
		return true;
	}
	erase_layout_if_unchanged(base, *out);
	return false;
}

} // namespace

bool GuestTextureLayoutRegisterLinear(uint64_t base, size_t size, uint32_t row_pitch_bytes,
	                                  const Core::VirtualMemory::GuestMappingIdentitySnapshot& identity)
{
	if (base == 0 || size == 0 || row_pitch_bytes == 0 || size > UINT64_MAX - base || identity.address != base || identity.size != size)
	{
		return false;
	}

	const GuestLinearTextureLayout layout {base + static_cast<uint64_t>(size), row_pitch_bytes, identity};
	std::lock_guard lock(g_guest_texture_layout_mutex);
	const size_t    index = lower_bound_base(base);
	if (index < g_guest_texture_layout_count && g_guest_texture_layouts[index].base == base)
	{
		g_guest_texture_layouts[index].layout = layout;
		return true;
	}
	if (g_guest_texture_layout_count >= kGuestTextureLayoutCapacity)
	{
		return false;
	}
	for (size_t i = g_guest_texture_layout_count; i > index; --i)
	{
		g_guest_texture_layouts[i] = g_guest_texture_layouts[i - 1];
	}
	g_guest_texture_layouts[index] = GuestLinearTextureLayoutRecord {base, layout};
	++g_guest_texture_layout_count;
	return true;
}

void GuestTextureLayoutPruneStale()
{
	struct PruneSnapshotRecord
	{
		GuestLinearTextureLayoutRecord record {};
		bool                           stale = false;
	};

	{
		std::lock_guard lock(g_guest_texture_layout_mutex);
		if (g_guest_texture_layout_count < kGuestTextureLayoutCapacity)
		{
			return;
		}
	}

	std::unique_ptr<PruneSnapshotRecord[]> snapshot(new (std::nothrow) PruneSnapshotRecord[kGuestTextureLayoutCapacity]);
	if (!snapshot)
	{
		return;
	}

	size_t snapshot_count = 0;
	{
		std::lock_guard lock(g_guest_texture_layout_mutex);
		if (g_guest_texture_layout_count < kGuestTextureLayoutCapacity)
		{
			return;
		}
		snapshot_count = g_guest_texture_layout_count;
		for (size_t i = 0; i < snapshot_count; ++i)
		{
			snapshot[i].record = g_guest_texture_layouts[i];
		}
	}

	for (size_t i = 0; i < snapshot_count; ++i)
	{
		snapshot[i].stale = !Core::VirtualMemory::GuestMappingIdentityMatches(snapshot[i].record.layout.identity);
	}

	std::lock_guard lock(g_guest_texture_layout_mutex);
	size_t          snapshot_index = 0;
	size_t          write_index    = 0;
	const size_t    current_count  = g_guest_texture_layout_count;
	for (size_t read_index = 0; read_index < current_count; ++read_index)
	{
		const auto& current = g_guest_texture_layouts[read_index];
		while (snapshot_index < snapshot_count && snapshot[snapshot_index].record.base < current.base)
		{
			++snapshot_index;
		}

		const bool matches_snapshot = snapshot_index < snapshot_count && snapshot[snapshot_index].record.base == current.base &&
		                              layout_matches(current.layout, snapshot[snapshot_index].record.layout);
		const bool remove = matches_snapshot && snapshot[snapshot_index].stale;
		if (!remove)
		{
			if (write_index != read_index)
			{
				g_guest_texture_layouts[write_index] = current;
			}
			++write_index;
		}
	}
	for (size_t i = write_index; i < current_count; ++i)
	{
		g_guest_texture_layouts[i] = {};
	}
	g_guest_texture_layout_count = write_index;
}

void GuestTextureLayoutUnregister(uint64_t base)
{
	std::lock_guard lock(g_guest_texture_layout_mutex);
	const size_t    index = lower_bound_base(base);
	if (index < g_guest_texture_layout_count && g_guest_texture_layouts[index].base == base)
	{
		erase_layout_at(index);
	}
}

void GuestTextureLayoutInvalidateRange(uint64_t address, uint64_t size)
{
	if (address == 0 || size == 0 || size > UINT64_MAX - address)
	{
		return;
	}

	const uint64_t range_end = address + size;
	std::lock_guard lock(g_guest_texture_layout_mutex);
	size_t          write_index   = 0;
	const size_t    current_count = g_guest_texture_layout_count;
	for (size_t read_index = 0; read_index < current_count; ++read_index)
	{
		const auto& record = g_guest_texture_layouts[read_index];
		if (record.base < range_end && address < record.layout.end)
		{
			continue;
		}
		if (write_index != read_index)
		{
			g_guest_texture_layouts[write_index] = record;
		}
		++write_index;
	}
	for (size_t i = write_index; i < current_count; ++i)
	{
		g_guest_texture_layouts[i] = {};
	}
	g_guest_texture_layout_count = write_index;
}

uint32_t GuestTextureLayoutGetLinearRowPitch(uint64_t address, uint32_t visible_row_bytes)
{
	if (address == 0 || visible_row_bytes == 0)
	{
		return 0;
	}

	GuestLinearTextureLayout layout {};
	return (find_current_layout(address, &layout) && layout.row_pitch_bytes == visible_row_bytes ? layout.row_pitch_bytes : 0);
}

uint64_t GuestTextureLayoutGetLinearFootprint(uint64_t address, uint32_t visible_row_bytes, uint32_t visible_height)
{
	if (address == 0 || visible_row_bytes == 0 || visible_height == 0)
	{
		return 0;
	}

	GuestLinearTextureLayout layout {};
	if (!find_current_layout(address, &layout) || layout.row_pitch_bytes != visible_row_bytes)
	{
		return 0;
	}

	const uint64_t footprint = static_cast<uint64_t>(layout.row_pitch_bytes) * visible_height;
	return (footprint <= layout.end - address ? footprint : 0);
}

} // namespace Kyty::Libs::Graphics
