#include "Emulator/Graphics/Objects/GpuWritebackPageCache.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/Utils.h"

#include <algorithm>
#include <cstring>

namespace Kyty::Libs::Graphics {

void GpuWritebackPageCache::Reset(const void* source, uint64_t size)
{
	EXIT_IF(source == nullptr || size == 0u || m_page_size == 0u);
	const auto* bytes = static_cast<const uint8_t*>(source);
	m_snapshot.assign(bytes, bytes + size);
}

GpuWritebackResult GpuWritebackPageCache::CopyChangedPages(void* guest_dst, const void* gpu_src, uint64_t size,
                                                          const uint64_t* hole_begin, const uint64_t* hole_end, int hole_count,
                                                          NotifyWriteFunc notify_write, void* notify_opaque)
{
	EXIT_IF(guest_dst == nullptr || gpu_src == nullptr || size == 0u || notify_write == nullptr);
	EXIT_IF(m_snapshot.size() != size || m_page_size == 0u);

	auto*       guest    = static_cast<uint8_t*>(guest_dst);
	const auto* gpu      = static_cast<const uint8_t*>(gpu_src);
	const auto  guest_va = reinterpret_cast<uint64_t>(guest_dst);

	GpuWritebackResult result;
	for (uint64_t offset = 0; offset < size; offset += m_page_size)
	{
		const uint64_t page_bytes = std::min(m_page_size, size - offset);
		if (std::memcmp(m_snapshot.data() + offset, gpu + offset, static_cast<size_t>(page_bytes)) == 0)
		{
			continue;
		}

		notify_write(notify_opaque, guest_va + offset, page_bytes);
		MemcpySkipAbsoluteRanges(guest + offset, gpu + offset, page_bytes, hole_begin, hole_end, hole_count);
		std::memcpy(m_snapshot.data() + offset, gpu + offset, static_cast<size_t>(page_bytes));
		result.changed_pages++;
		result.copied_bytes += page_bytes;
	}
	result.content_changed = result.changed_pages != 0u;
	return result;
}

} // namespace Kyty::Libs::Graphics
