#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUWRITEBACKPAGECACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUWRITEBACKPAGECACHE_H_

#include <cstdint>
#include <vector>

namespace Kyty::Libs::Graphics {

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

	explicit GpuWritebackPageCache(uint64_t page_size = 4096u): m_page_size(page_size) {}

	void Reset(const void* source, uint64_t size);

	[[nodiscard]] GpuWritebackResult CopyChangedPages(void* guest_dst, const void* gpu_src, uint64_t size,
	                                                 const uint64_t* hole_begin, const uint64_t* hole_end, int hole_count,
	                                                 NotifyWriteFunc notify_write, void* notify_opaque);

private:
	std::vector<uint8_t> m_snapshot;
	uint64_t             m_page_size = 4096u;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUWRITEBACKPAGECACHE_H_ */
