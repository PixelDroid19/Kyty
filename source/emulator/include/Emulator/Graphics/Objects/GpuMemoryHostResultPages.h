#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYHOSTRESULTPAGES_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYHOSTRESULTPAGES_H_

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#include <algorithm>
#include <array>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// A buffer object is a plain copy of the guest bytes under it unless it is
// writable and still in use: its GPU writes reach guest memory when they are
// written back, which clears the use. Any other object (a surface) may hold a
// GPU result the guest memory does not show yet.
[[nodiscard]] constexpr bool GpuMemoryObjectMayHoldHostResult(GpuMemoryObjectType type, bool read_only,
                                                              bool in_use)
{
	const bool is_buffer = type == GpuMemoryObjectType::StorageBuffer ||
	                       type == GpuMemoryObjectType::VertexBuffer ||
	                       type == GpuMemoryObjectType::IndexBuffer;
	return !is_buffer || (in_use && !read_only);
}

// Counts, per guest page, the live objects that may hold a host result
// (GpuMemoryObjectMayHoldHostResult). A range without such a page reads the
// same bytes from guest memory as from every object over it, so a draw can
// read it in place.
class GpuMemoryHostResultPages
{
public:
	void Add(uint64_t vaddr, uint64_t size) { Adjust(vaddr, size, true); }
	void Remove(uint64_t vaddr, uint64_t size) { Adjust(vaddr, size, false); }

	[[nodiscard]] bool Any(uint64_t vaddr, uint64_t size) const
	{
		if (size == 0 || m_blocks.empty())
		{
			return false;
		}
		const uint64_t last = LastPage(vaddr, size);
		for (uint64_t page = vaddr >> kPageShift; page <= last;)
		{
			const uint64_t block_last = std::min(last, page | (kBlockPages - 1u));
			if (const auto block = m_blocks.find(page >> kBlockShift); block != m_blocks.end())
			{
				for (uint64_t p = page; p <= block_last; p++)
				{
					if (block->second.pages[p & (kBlockPages - 1u)] != 0u)
					{
						return true;
					}
				}
			}
			if (block_last == UINT64_MAX >> kPageShift)
			{
				break;
			}
			page = block_last + 1u;
		}
		return false;
	}

private:
	static constexpr uint32_t kPageShift  = 12u;
	static constexpr uint32_t kBlockShift = 9u;
	static constexpr uint64_t kBlockPages = 1ull << kBlockShift;

	struct Block
	{
		uint64_t                          live = 0;
		std::array<uint32_t, kBlockPages> pages {};
	};

	[[nodiscard]] static uint64_t LastPage(uint64_t vaddr, uint64_t size)
	{
		const uint64_t last_byte = size - 1u > UINT64_MAX - vaddr ? UINT64_MAX : vaddr + (size - 1u);
		return last_byte >> kPageShift;
	}

	void Adjust(uint64_t vaddr, uint64_t size, bool add)
	{
		if (size == 0)
		{
			return;
		}
		const uint64_t last = LastPage(vaddr, size);
		for (uint64_t page = vaddr >> kPageShift; page <= last;)
		{
			const uint64_t block_last = std::min(last, page | (kBlockPages - 1u));
			const uint64_t count      = block_last - page + 1u;
			const uint64_t key        = page >> kBlockShift;
			const auto     block      = add ? m_blocks.try_emplace(key).first : m_blocks.find(key);
			EXIT_IF(block == m_blocks.end() || (!add && block->second.live < count));
			for (uint64_t p = page; p <= block_last; p++)
			{
				auto& counted = block->second.pages[p & (kBlockPages - 1u)];
				EXIT_IF(counted == (add ? UINT32_MAX : 0u));
				counted += add ? 1u : UINT32_MAX;
			}
			block->second.live = add ? block->second.live + count : block->second.live - count;
			if (block->second.live == 0u)
			{
				m_blocks.erase(block);
			}
			if (block_last == UINT64_MAX >> kPageShift)
			{
				break;
			}
			page = block_last + 1u;
		}
	}

	std::unordered_map<uint64_t, Block> m_blocks;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYHOSTRESULTPAGES_H_ */
