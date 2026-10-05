#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUWRITEBACKPAGECACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUWRITEBACKPAGECACHE_H_

#include <array>
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

	using UniformWords = std::array<uint32_t, 4>;
	static constexpr uint64_t kUniformRecordBytes = sizeof(UniformWords);

	void Reset(const void* source, uint64_t size);

	[[nodiscard]] GpuWritebackResult CopyChangedPages(void* guest_dst, const void* gpu_src, uint64_t size,
	                                                 const uint64_t* hole_begin, const uint64_t* hole_end, int hole_count,
	                                                 NotifyWriteFunc notify_write, void* notify_opaque);

	// Records that GPU and guest memory both hold `words` repeated over `size`
	// bytes (a multiple of kUniformRecordBytes) without reading either; the
	// bytes are materialized only when a later page copy needs them. Returns
	// whether the snapshot changed.
	[[nodiscard]] bool AdoptUniform(const UniformWords& words, uint64_t size);

private:
	void Materialize();

	std::vector<uint8_t> m_snapshot;
	uint64_t             m_page_size = 4096u;
	UniformWords         m_uniform_words {};
	bool                 m_uniform = false;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUWRITEBACKPAGECACHE_H_ */
