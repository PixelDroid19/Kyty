#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGEBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGEBUFFER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct StorageVulkanBuffer;

class StorageBufferGpuObject: public GpuObject
{
public:
	StorageBufferGpuObject(uint64_t stride, uint64_t num_records, bool ronly)
	{
		params[0]  = stride;
		params[1]  = num_records;
		check_hash = true;
		read_only  = ronly;
		type       = Graphics::GpuMemoryObjectType::StorageBuffer;
	}

	bool Equal(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; };
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override;
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
};

// Write-back of a storage object whose only pending GPU write was published to
// guest memory on the device as `words` repeated over its whole range. Guest
// memory already holds the result, so the GPU copy is not read. False: the
// caller must use the byte write-back.
[[nodiscard]] bool StorageBufferWriteBackPublishedUniform(void* obj, uint64_t vaddr, uint64_t size,
                                                          const GpuWritebackPageCache::UniformWords& words, GpuWritebackResult* result);

// Uploads only `runs` of a storage object whose other bytes already match guest
// memory. False when the object observes depth metadata, whose HTILE tracking
// reads every uploaded byte: the caller must then upload the whole object.
[[nodiscard]] bool StorageBufferUploadRuns(GraphicContext* ctx, void* obj, uint64_t vaddr, uint64_t size,
                                           const std::vector<GpuByteRun>& runs);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGEBUFFER_H_ */
