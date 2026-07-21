#include "Emulator/Graphics/ResolutionAliasPolicy.h"

namespace Kyty::Libs::Graphics {

bool ResolutionAliasPolicyAllowsSnapshot(const GpuMemoryOverlapSnapshot& overlaps, GpuMemoryObjectType expected_type, bool allow_empty)
{
	if (overlaps.truncated)
	{
		return false;
	}
	if (overlaps.total_count == 0)
	{
		return allow_empty && overlaps.exact_count == 0 && overlaps.entry_count == 0;
	}

	bool expected_exact = false;
	bool storage_exact  = false;
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
		if (expected_type != GpuMemoryObjectType::VideoOutBuffer || entry.type != GpuMemoryObjectType::StorageBuffer || storage_exact)
		{
			return false;
		}
		storage_exact = true;
	}

	const uint32_t expected_total = storage_exact ? 2u : 1u;
	return expected_exact && overlaps.entry_count == expected_total && overlaps.total_count == expected_total &&
	       overlaps.exact_count == expected_total;
}

bool ResolutionAliasPolicyAllowsRanges(const uint64_t* vaddr, const uint64_t* size, int count, GpuMemoryObjectType expected_type,
                                       bool allow_empty)
{
	GpuMemoryOverlapSnapshot overlaps;
	return GpuMemoryQueryOverlaps(vaddr, size, count, &overlaps) &&
	       ResolutionAliasPolicyAllowsSnapshot(overlaps, expected_type, allow_empty);
}

} // namespace Kyty::Libs::Graphics
