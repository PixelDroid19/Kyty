#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYOVERLAP_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYOVERLAP_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

constexpr int GPU_MEMORY_RANGE_SET_MAX = 3;

enum class GpuMemoryOverlapType : uint64_t
{
	None,
	Equals,
	Crosses,
	Contains,
	IsContainedWithin,
	Max
};

// Classify existing relative to query. Both ranges are half-open [address, address + size).
// Invalid or overflowing ranges violate the caller contract instead of being silently ignored.
[[nodiscard]] inline GpuMemoryOverlapType GpuMemoryClassifyRange(uint64_t existing_address, uint64_t existing_size, uint64_t query_address,
                                                                 uint64_t query_size)
{
	EXIT_IF(existing_size == 0 || query_size == 0);
	EXIT_IF(existing_address > UINT64_MAX - (existing_size - 1u));
	EXIT_IF(query_address > UINT64_MAX - (query_size - 1u));

	if (existing_address == query_address && existing_size == query_size)
	{
		return GpuMemoryOverlapType::Equals;
	}

	const uint64_t existing_last = existing_address + existing_size - 1u;
	const uint64_t query_last    = query_address + query_size - 1u;
	if (existing_address <= query_address && existing_last >= query_last)
	{
		return GpuMemoryOverlapType::Contains;
	}
	if (query_address <= existing_address && query_last >= existing_last)
	{
		return GpuMemoryOverlapType::IsContainedWithin;
	}
	if (existing_address <= query_last && query_address <= existing_last)
	{
		return GpuMemoryOverlapType::Crosses;
	}
	return GpuMemoryOverlapType::None;
}

// Canonical relation for GPU objects that can own one or more disjoint ranges.
// Multi-range objects are Equals only when every positional range matches; any
// other contact is Crosses. only_first intentionally compares only range zero
// and is valid solely for a one-range query.
[[nodiscard]] inline GpuMemoryOverlapType GpuMemoryClassifyRangeSets(const uint64_t* existing_address, const uint64_t* existing_size,
                                                                     int existing_count, const uint64_t* query_address,
                                                                     const uint64_t* query_size, int query_count, bool only_first)
{
	EXIT_IF(existing_address == nullptr || existing_size == nullptr || query_address == nullptr || query_size == nullptr);
	EXIT_IF(existing_count <= 0 || existing_count > GPU_MEMORY_RANGE_SET_MAX);
	EXIT_IF(query_count <= 0 || query_count > GPU_MEMORY_RANGE_SET_MAX);
	EXIT_IF(only_first && query_count != 1);
	for (int index = 0; index < existing_count; index++)
	{
		EXIT_IF(existing_size[index] == 0 || existing_address[index] > UINT64_MAX - (existing_size[index] - 1u));
	}
	for (int index = 0; index < query_count; index++)
	{
		EXIT_IF(query_size[index] == 0 || query_address[index] > UINT64_MAX - (query_size[index] - 1u));
	}

	if (query_count == 1 && (existing_count == 1 || only_first))
	{
		return GpuMemoryClassifyRange(existing_address[0], existing_size[0], query_address[0], query_size[0]);
	}

	if (existing_count == query_count)
	{
		bool equal = true;
		for (int index = 0; index < query_count; index++)
		{
			if (GpuMemoryClassifyRange(existing_address[index], existing_size[index], query_address[index], query_size[index]) !=
			    GpuMemoryOverlapType::Equals)
			{
				equal = false;
				break;
			}
		}
		if (equal)
		{
			return GpuMemoryOverlapType::Equals;
		}
	}

	for (int query_index = 0; query_index < query_count; query_index++)
	{
		for (int existing_index = 0; existing_index < existing_count; existing_index++)
		{
			if (GpuMemoryClassifyRange(existing_address[existing_index], existing_size[existing_index], query_address[query_index],
			                           query_size[query_index]) != GpuMemoryOverlapType::None)
			{
				return GpuMemoryOverlapType::Crosses;
			}
		}
	}
	return GpuMemoryOverlapType::None;
}

[[nodiscard]] inline GpuMemoryOverlapType GpuMemoryReverseOverlap(GpuMemoryOverlapType relation)
{
	switch (relation)
	{
		case GpuMemoryOverlapType::Equals: return GpuMemoryOverlapType::Equals;
		case GpuMemoryOverlapType::Crosses: return GpuMemoryOverlapType::Crosses;
		case GpuMemoryOverlapType::Contains: return GpuMemoryOverlapType::IsContainedWithin;
		case GpuMemoryOverlapType::IsContainedWithin: return GpuMemoryOverlapType::Contains;
		case GpuMemoryOverlapType::None:
		case GpuMemoryOverlapType::Max: return GpuMemoryOverlapType::None;
	}
	return GpuMemoryOverlapType::None;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYOVERLAP_H_
