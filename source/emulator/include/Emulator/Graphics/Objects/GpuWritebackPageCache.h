#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUWRITEBACKPAGECACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUWRITEBACKPAGECACHE_H_

#include <array>
#include <cstdint>
#include <vector>

namespace Kyty::Libs::Graphics {

// Bytes [offset, offset + bytes) of a GPU object.
struct GpuByteRun
{
	uint64_t offset = 0;
	uint64_t bytes  = 0;
};

struct GpuWritebackResult
{
	uint64_t changed_pages   = 0;
	uint64_t copied_bytes    = 0;
	bool     content_changed = false;
};

class GpuWritebackPageCache final
{
public:
	using NotifyWriteFunc = void (*)(void* opaque, uint64_t address, uint64_t size);

	static constexpr uint64_t kDefaultPageSize = 4096u;

	explicit GpuWritebackPageCache(uint64_t page_size = kDefaultPageSize): m_page_size(page_size) {}

	using UniformWords = std::array<uint32_t, 4>;
	static constexpr uint64_t kUniformRecordBytes = sizeof(UniformWords);

	// Flags, one per page, that the GPU sets when it writes the page. A copy or
	// adoption compares only flagged pages and clears every flag it reads.
	void TrackWrittenPages(uint32_t* flags) { m_written_pages = flags; }

	void Reset(const void* source, uint64_t size);
	// Replaces the snapshot bytes of one run of a same-sized source.
	void Refresh(const void* source, const GpuByteRun& run);

	[[nodiscard]] GpuWritebackResult CopyChangedPages(void* guest_dst, const void* gpu_src, uint64_t size,
	                                                 const uint64_t* hole_begin, const uint64_t* hole_end, int hole_count,
	                                                 NotifyWriteFunc notify_write, void* notify_opaque);

	// Records that GPU and guest memory both hold `words` repeated over `size`
	// bytes (a multiple of kUniformRecordBytes) without reading either, and
	// clears the written-page flags; the bytes are materialized only when a
	// later page copy needs them. Returns whether the snapshot changed.
	[[nodiscard]] bool AdoptUniform(const UniformWords& words, uint64_t size);

	// Records that the device already copied `gpu_src` into guest memory at
	// `guest_va`: changed pages enter the snapshot and their writes are notified,
	// but guest memory is not written.
	[[nodiscard]] GpuWritebackResult AdoptChangedPages(uint64_t guest_va, const void* gpu_src, uint64_t size,
	                                                  NotifyWriteFunc notify_write, void* notify_opaque);

private:
	void                 Materialize();
	std::vector<uint8_t> TakeChangedPages(const uint8_t* source);

	std::vector<uint8_t> m_snapshot;
	uint32_t*            m_written_pages = nullptr;
	uint64_t             m_page_size     = kDefaultPageSize;
	UniformWords         m_uniform_words {};
	bool                 m_uniform = false;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUWRITEBACKPAGECACHE_H_ */
