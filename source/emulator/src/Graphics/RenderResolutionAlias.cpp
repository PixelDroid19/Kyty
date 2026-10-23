#include "Emulator/Graphics/RenderResolutionAlias.h"

namespace Kyty::Libs::Graphics {

namespace {

// Peers that may share an exact range with a scaled image. Their data reaches
// the image only through guest-extent copies, which refuse scaled images, or
// through sampled binds, which are gated on normalized coordinates. A buffer
// never reloads from an image.
bool AllowsExactPeer(GpuMemoryObjectType expected_type, GpuMemoryObjectType peer)
{
	switch (expected_type)
	{
		case GpuMemoryObjectType::VideoOutBuffer: return peer == GpuMemoryObjectType::StorageBuffer;
		case GpuMemoryObjectType::RenderTexture: return peer == GpuMemoryObjectType::StorageBuffer || peer == GpuMemoryObjectType::Texture;
		default: return false;
	}
}

} // namespace

bool RenderResolutionAliasAllowsSnapshot(const GpuMemoryOverlapSnapshot& overlaps, GpuMemoryObjectType expected_type, bool allow_empty)
{
	if (overlaps.truncated)
	{
		return false;
	}
	if (overlaps.total_count == 0)
	{
		return allow_empty && overlaps.exact_count == 0 && overlaps.entry_count == 0;
	}

	bool     expected_exact = false;
	uint32_t peer_types     = 0;
	uint32_t peer_count     = 0;
	for (uint32_t index = 0; index < overlaps.entry_count; index++)
	{
		const auto& entry = overlaps.entries[index];
		if (!entry.exact || entry.relation != GpuMemoryOverlapType::Equals || entry.count != 1)
		{
			return false;
		}
		if (entry.type == expected_type)
		{
			if (expected_exact)
			{
				return false;
			}
			expected_exact = true;
			continue;
		}
		const uint32_t peer_bit = 1u << static_cast<uint32_t>(entry.type);
		if (!AllowsExactPeer(expected_type, entry.type) || (peer_types & peer_bit) != 0)
		{
			return false;
		}
		peer_types |= peer_bit;
		peer_count++;
	}

	// allow_empty lets the owner itself be absent, as before its creation.
	const uint32_t expected_total = peer_count + (expected_exact ? 1u : 0u);
	return (expected_exact || allow_empty) && overlaps.entry_count == expected_total && overlaps.total_count == expected_total &&
	       overlaps.exact_count == expected_total;
}

bool RenderResolutionAliasAllowsDepthTargetSnapshot(const GpuMemoryOverlapSnapshot& overlaps)
{
	if (overlaps.truncated)
	{
		return false;
	}
	bool depth_exact = false;
	for (uint32_t index = 0; index < overlaps.entry_count; index++)
	{
		const auto& entry = overlaps.entries[index];
		if (entry.type == GpuMemoryObjectType::StorageBuffer)
		{
			continue;
		}
		if (entry.type != GpuMemoryObjectType::DepthStencilBuffer || depth_exact || !entry.exact ||
		    entry.relation != GpuMemoryOverlapType::Equals || entry.count != 1)
		{
			return false;
		}
		depth_exact = true;
	}
	return true;
}

bool RenderResolutionAliasAllowsRanges(const uint64_t* vaddr, const uint64_t* size, int count, GpuMemoryObjectType expected_type,
                                       bool allow_empty)
{
	GpuMemoryOverlapSnapshot overlaps;
	return GpuMemoryQueryOverlaps(vaddr, size, count, &overlaps) &&
	       RenderResolutionAliasAllowsSnapshot(overlaps, expected_type, allow_empty);
}

} // namespace Kyty::Libs::Graphics
