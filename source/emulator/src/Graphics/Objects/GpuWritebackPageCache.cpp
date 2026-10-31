#include "Emulator/Graphics/Objects/GpuWritebackPageCache.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/Utils.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Kyty::Libs::Graphics {

// Calls visit(offset, bytes, pages) for each run of consecutive changed pages,
// so a run's write is notified once and its protection changes once.
template <typename Visit>
static void VisitChangedRuns(const std::vector<uint8_t>& changed, uint64_t page_size, uint64_t size, const Visit& visit)
{
	const uint64_t pages = changed.size();
	for (uint64_t page = 0; page < pages;)
	{
		if (changed[page] == 0u)
		{
			page++;
			continue;
		}
		uint64_t end = page + 1u;
		while (end < pages && changed[end] != 0u)
		{
			end++;
		}
		const uint64_t offset = page * page_size;
		visit(offset, std::min(end * page_size, size) - offset, end - page);
		page = end;
	}
}

void GpuWritebackPageCache::Reset(const void* source, uint64_t size)
{
	EXIT_IF(source == nullptr || size == 0u || m_page_size == 0u);
	const auto* bytes = static_cast<const uint8_t*>(source);
	m_snapshot.assign(bytes, bytes + size);
	m_uniform = false;
}

void GpuWritebackPageCache::Refresh(const void* source, const GpuByteRun& run)
{
	EXIT_IF(source == nullptr || run.bytes == 0u || run.offset > m_snapshot.size() || run.bytes > m_snapshot.size() - run.offset);
	Materialize();
	std::memcpy(m_snapshot.data() + run.offset, static_cast<const uint8_t*>(source) + run.offset, static_cast<size_t>(run.bytes));
}

std::vector<uint8_t> GpuWritebackPageCache::TakeChangedPages(const uint8_t* source)
{
	EXIT_IF(m_written_pages == nullptr);
	const uint64_t       size  = m_snapshot.size();
	const uint64_t       pages = (size + m_page_size - 1u) / m_page_size;
	std::vector<uint8_t> changed(static_cast<size_t>(pages));
	for (uint64_t page = 0; page < pages; page++)
	{
		if (m_written_pages[page] == 0u)
		{
			continue;
		}
		m_written_pages[page] = 0u;
		const uint64_t offset = page * m_page_size;
		const uint64_t bytes  = std::min(m_page_size, size - offset);
		changed[page]         = std::memcmp(m_snapshot.data() + offset, source + offset, static_cast<size_t>(bytes)) != 0 ? 1u : 0u;
	}
	return changed;
}

void GpuWritebackPageCache::Materialize()
{
	if (!m_uniform)
	{
		return;
	}
	for (uint64_t offset = 0; offset + kUniformRecordBytes <= m_snapshot.size(); offset += kUniformRecordBytes)
	{
		std::memcpy(m_snapshot.data() + offset, m_uniform_words.data(), kUniformRecordBytes);
	}
	m_uniform = false;
}

bool GpuWritebackPageCache::AdoptUniform(const UniformWords& words, uint64_t size)
{
	EXIT_IF(size == 0u || size % kUniformRecordBytes != 0u || m_written_pages == nullptr);
	std::fill_n(m_written_pages, (size + m_page_size - 1u) / m_page_size, 0u);
	bool changed = true;
	if (m_snapshot.size() != size)
	{
		m_snapshot.resize(size);
	} else if (m_uniform)
	{
		changed = m_uniform_words != words;
	} else
	{
		changed = false;
		for (uint64_t offset = 0; offset < size && !changed; offset += kUniformRecordBytes)
		{
			changed = std::memcmp(m_snapshot.data() + offset, words.data(), kUniformRecordBytes) != 0;
		}
	}
	m_uniform_words = words;
	m_uniform       = true;
	return changed;
}

GpuWritebackResult GpuWritebackPageCache::AdoptChangedPages(uint64_t guest_va, const void* gpu_src, uint64_t size,
                                                           NotifyWriteFunc notify_write, void* notify_opaque)
{
	EXIT_IF(gpu_src == nullptr || size == 0u || notify_write == nullptr);
	EXIT_IF(m_snapshot.size() != size || m_page_size == 0u);
	Materialize();

	const auto*        gpu     = static_cast<const uint8_t*>(gpu_src);
	const auto         changed = TakeChangedPages(gpu);
	GpuWritebackResult result;
	VisitChangedRuns(changed, m_page_size, size,
	                 [&](uint64_t offset, uint64_t bytes, uint64_t pages)
	                 {
		                 notify_write(notify_opaque, guest_va + offset, bytes);
		                 std::memcpy(m_snapshot.data() + offset, gpu + offset, static_cast<size_t>(bytes));
		                 result.changed_pages += pages;
	                 });
	result.content_changed = result.changed_pages != 0u;
	return result;
}

GpuWritebackResult GpuWritebackPageCache::CopyChangedPages(void* guest_dst, const void* gpu_src, uint64_t size,
                                                          const uint64_t* hole_begin, const uint64_t* hole_end, int hole_count,
                                                          NotifyWriteFunc notify_write, void* notify_opaque)
{
	EXIT_IF(guest_dst == nullptr || gpu_src == nullptr || size == 0u || notify_write == nullptr);
	EXIT_IF(m_snapshot.size() != size || m_page_size == 0u);
	Materialize();

	auto*       guest    = static_cast<uint8_t*>(guest_dst);
	const auto* gpu      = static_cast<const uint8_t*>(gpu_src);
	const auto  guest_va = reinterpret_cast<uint64_t>(guest_dst);
	const auto  changed  = TakeChangedPages(gpu);

	GpuWritebackResult result;
	VisitChangedRuns(changed, m_page_size, size,
	                 [&](uint64_t offset, uint64_t bytes, uint64_t pages)
	                 {
		                 notify_write(notify_opaque, guest_va + offset, bytes);
		                 // An in-place object's device writes are already guest bytes.
		                 if (guest != gpu)
		                 {
			                 MemcpySkipAbsoluteRanges(guest + offset, gpu + offset, bytes, hole_begin, hole_end, hole_count);
		                 }
		                 std::memcpy(m_snapshot.data() + offset, gpu + offset, static_cast<size_t>(bytes));
		                 result.changed_pages += pages;
		                 result.copied_bytes += bytes;
	                 });
	result.content_changed = result.changed_pages != 0u;
	return result;
}

} // namespace Kyty::Libs::Graphics
