#include "Emulator/Graphics/Objects/GpuWritebackPageCache.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/Utils.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace Kyty::Libs::Graphics {

namespace {

void ComparePages(const uint8_t* a, const uint8_t* b, uint64_t size, uint64_t page_size, uint64_t first, uint64_t last,
                  uint8_t* changed)
{
	for (uint64_t page = first; page < last; page++)
	{
		const uint64_t offset = page * page_size;
		const uint64_t bytes  = std::min(page_size, size - offset);
		changed[page]         = std::memcmp(a + offset, b + offset, static_cast<size_t>(bytes)) != 0 ? 1u : 0u;
	}
}

// Compares the pages of large spans on a few helper threads. A completion
// writes its storage back before it publishes the label the guest waits on,
// so one thread scanning a multi-megabyte range sits on that wait.
class PageCompareScanner final
{
public:
	static constexpr uint64_t kChunkPages       = 64u;
	static constexpr uint64_t kMinParallelPages = 4u * kChunkPages;
	static constexpr uint32_t kMaxHelpers       = 3u;

	static PageCompareScanner& Instance()
	{
		static PageCompareScanner scanner;
		return scanner;
	}

	PageCompareScanner(const PageCompareScanner&)            = delete;
	PageCompareScanner& operator=(const PageCompareScanner&) = delete;

	~PageCompareScanner()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stopping = true;
		}
		m_ready.notify_all();
		for (auto& worker: m_workers)
		{
			worker.join();
		}
	}

	// Sets changed[i] to whether page i of a and b differ.
	void Scan(const uint8_t* a, const uint8_t* b, uint64_t size, uint64_t page_size, uint8_t* changed)
	{
		const uint64_t pages = (size + page_size - 1u) / page_size;
		if (m_workers.empty() || pages < kMinParallelPages)
		{
			ComparePages(a, b, size, page_size, 0, pages, changed);
			return;
		}

		std::lock_guard<std::mutex> scan_lock(m_scan_mutex);
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_job         = Job {a, b, size, page_size, changed, pages, (pages + kChunkPages - 1u) / kChunkPages};
			m_done_chunks = 0;
			m_next_chunk.store(0, std::memory_order_relaxed);
			m_open = true;
			m_serial++;
		}
		m_ready.notify_all();
		const uint64_t done = RunChunks();

		std::unique_lock<std::mutex> lock(m_mutex);
		m_done_chunks += done;
		// Helpers that joined must leave before the job can be replaced.
		m_finished.wait(lock, [this] { return m_done_chunks == m_job.chunks && m_active == 0; });
		m_open = false;
	}

private:
	struct Job
	{
		const uint8_t* a         = nullptr;
		const uint8_t* b         = nullptr;
		uint64_t       size      = 0;
		uint64_t       page_size = 0;
		uint8_t*       changed   = nullptr;
		uint64_t       pages     = 0;
		uint64_t       chunks    = 0;
	};

	PageCompareScanner()
	{
		const uint32_t helpers = std::min(kMaxHelpers, std::thread::hardware_concurrency() / 4u);
		for (uint32_t i = 0; i < helpers; i++)
		{
			m_workers.emplace_back([this] { WorkerMain(); });
		}
	}

	uint64_t RunChunks()
	{
		uint64_t done = 0;
		for (uint64_t chunk = m_next_chunk.fetch_add(1, std::memory_order_relaxed); chunk < m_job.chunks;
		     chunk          = m_next_chunk.fetch_add(1, std::memory_order_relaxed))
		{
			const uint64_t first = chunk * kChunkPages;
			ComparePages(m_job.a, m_job.b, m_job.size, m_job.page_size, first, std::min(first + kChunkPages, m_job.pages), m_job.changed);
			done++;
		}
		return done;
	}

	void WorkerMain()
	{
		uint64_t                     seen = 0;
		std::unique_lock<std::mutex> lock(m_mutex);
		for (;;)
		{
			m_ready.wait(lock, [this, &seen] { return m_stopping || m_serial != seen; });
			if (m_stopping)
			{
				return;
			}
			seen = m_serial;
			if (!m_open)
			{
				continue;
			}
			m_active++;
			lock.unlock();
			const uint64_t done = RunChunks();
			lock.lock();
			m_done_chunks += done;
			m_active--;
			m_finished.notify_one();
		}
	}

	std::mutex               m_scan_mutex;
	std::mutex               m_mutex;
	std::condition_variable  m_ready;
	std::condition_variable  m_finished;
	std::vector<std::thread> m_workers;
	Job                      m_job;
	std::atomic<uint64_t>    m_next_chunk {0};
	uint64_t                 m_done_chunks = 0;
	uint64_t                 m_serial      = 0;
	uint32_t                 m_active      = 0;
	bool                     m_open        = false;
	bool                     m_stopping    = false;
};

std::vector<uint8_t> ChangedPages(const std::vector<uint8_t>& snapshot, const uint8_t* source, uint64_t page_size)
{
	const uint64_t       size = snapshot.size();
	std::vector<uint8_t> changed(static_cast<size_t>((size + page_size - 1u) / page_size));
	PageCompareScanner::Instance().Scan(snapshot.data(), source, size, page_size, changed.data());
	return changed;
}

} // namespace

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
	EXIT_IF(size == 0u || size % kUniformRecordBytes != 0u);
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
	const auto         changed = ChangedPages(m_snapshot, gpu, m_page_size);
	GpuWritebackResult result;
	for (uint64_t offset = 0; offset < size; offset += m_page_size)
	{
		const uint64_t page_bytes = std::min(m_page_size, size - offset);
		if (changed[offset / m_page_size] == 0u)
		{
			continue;
		}
		notify_write(notify_opaque, guest_va + offset, page_bytes);
		std::memcpy(m_snapshot.data() + offset, gpu + offset, static_cast<size_t>(page_bytes));
		result.changed_pages++;
	}
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
	const auto  changed  = ChangedPages(m_snapshot, gpu, m_page_size);

	GpuWritebackResult result;
	for (uint64_t offset = 0; offset < size; offset += m_page_size)
	{
		const uint64_t page_bytes = std::min(m_page_size, size - offset);
		if (changed[offset / m_page_size] == 0u)
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
