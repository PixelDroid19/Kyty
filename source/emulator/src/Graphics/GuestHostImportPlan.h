#ifndef EMULATOR_SRC_GRAPHICS_GUESTHOSTIMPORTPLAN_H_
#define EMULATOR_SRC_GRAPHICS_GUESTHOSTIMPORTPLAN_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Kyty::Libs::Graphics::GuestHostImportPlan {

struct ImportPlan
{
	uint64_t host_capacity         = 0;
	uint64_t allocation_size      = 0;
	uint32_t memory_type_bits     = 0;
};

inline bool IsPowerOfTwo(uint64_t value)
{
	return value != 0 && (value & (value - 1u)) == 0;
}

inline bool IsPointerAligned(uintptr_t pointer, uint64_t alignment)
{
	return pointer != 0 && IsPowerOfTwo(alignment) && pointer % alignment == 0;
}

inline uint32_t IntersectMemoryTypeBits(uint32_t host_bits, uint32_t buffer_bits)
{
	return host_bits & buffer_bits;
}

inline bool IsPhysicalImportShapeValid(uintptr_t pointer, uint64_t span, uint64_t requirements_size, uint64_t pointer_alignment,
                                       bool requires_dedicated)
{
	return !requires_dedicated && span != 0 && requirements_size != 0 && requirements_size <= span &&
	       IsPointerAligned(pointer, pointer_alignment) && span % pointer_alignment == 0;
}

inline bool SelectMemoryType(uint32_t allowed_bits, const uint32_t* property_flags, uint32_t memory_type_count,
                             uint32_t required_flags, uint32_t* selected_index)
{
	if (allowed_bits == 0 || property_flags == nullptr || selected_index == nullptr || memory_type_count > 32u)
	{
		return false;
	}
	for (uint32_t i = 0; i < memory_type_count; i++)
	{
		if ((allowed_bits & (1u << i)) != 0 && (property_flags[i] & required_flags) == required_flags)
		{
			*selected_index = i;
			return true;
		}
	}
	return false;
}

inline bool AlignUp(uint64_t value, uint64_t alignment, uint64_t* result)
{
	if (result == nullptr || !IsPowerOfTwo(alignment) || value > std::numeric_limits<uint64_t>::max() - (alignment - 1u))
	{
		return false;
	}
	*result = (value + alignment - 1u) & ~(alignment - 1u);
	return true;
}

inline bool PlanPhysicalImport(uintptr_t pointer, uint64_t span, uint64_t requirements_size, uint64_t pointer_alignment,
                               uint32_t host_memory_type_bits, uint32_t buffer_memory_type_bits, bool requires_dedicated,
                               bool prefers_dedicated, ImportPlan* plan)
{
	if (plan == nullptr || !IsPhysicalImportShapeValid(pointer, span, requirements_size, pointer_alignment, requires_dedicated))
	{
		return false;
	}
	const uint32_t memory_type_bits = IntersectMemoryTypeBits(host_memory_type_bits, buffer_memory_type_bits);
	if (memory_type_bits == 0)
	{
		return false;
	}
	(void)prefers_dedicated;
	*plan = {span, span, memory_type_bits};
	return true;
}

inline bool PlanOwnedSnapshotCapacity(uint64_t span, uint64_t requirements_size, uint64_t allocation_alignment,
                                     uint64_t max_padding, uint64_t* capacity)
{
	if (capacity == nullptr || span == 0 || requirements_size == 0)
	{
		return false;
	}
	uint64_t padded_capacity = 0;
	if (!AlignUp(std::max(span, requirements_size), allocation_alignment, &padded_capacity) || padded_capacity < span ||
	    padded_capacity - span > max_padding)
	{
		return false;
	}
	*capacity = padded_capacity;
	return true;
}

inline bool PlanOwnedSnapshotImport(uintptr_t pointer, uint64_t span, uint64_t requirements_size, uint64_t pointer_alignment,
                                    uint64_t allocation_alignment, uint64_t max_padding, uint32_t host_memory_type_bits,
                                    uint32_t buffer_memory_type_bits, bool requires_dedicated, bool prefers_dedicated,
                                    ImportPlan* plan)
{
	if (plan == nullptr || requires_dedicated || !IsPointerAligned(pointer, pointer_alignment))
	{
		return false;
	}
	uint64_t capacity = 0;
	if (!PlanOwnedSnapshotCapacity(span, requirements_size, allocation_alignment, max_padding, &capacity))
	{
		return false;
	}
	const uint32_t memory_type_bits = IntersectMemoryTypeBits(host_memory_type_bits, buffer_memory_type_bits);
	if (memory_type_bits == 0)
	{
		return false;
	}
	(void)prefers_dedicated;
	*plan = {capacity, capacity, memory_type_bits};
	return true;
}

} // namespace Kyty::Libs::Graphics::GuestHostImportPlan

#endif // EMULATOR_SRC_GRAPHICS_GUESTHOSTIMPORTPLAN_H_
