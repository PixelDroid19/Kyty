#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORY_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class CommandBuffer;
class CommandProcessor;
struct GraphicContext;
struct VulkanMemory;
struct VulkanBuffer;
struct VulkanImage;

enum class GpuMemoryMode
{
	NoAccess,
	Read,
	Write,
	ReadWrite
};

enum class GpuMemoryObjectType : uint64_t
{
	Invalid,
	VideoOutBuffer,
	DepthStencilBuffer,
	Label,
	IndexBuffer,
	VertexBuffer,
	StorageBuffer,
	Texture,
	RenderTexture,
	StorageTexture,

	Max
};

enum class GpuMemoryScenario
{
	Common,
	GenerateMips,
	TextureTriplet
};

enum class GpuMemoryOverlapType : uint64_t
{
	None,
	Equals,
	Crosses,
	Contains,
	IsContainedWithin,
	Max
};

inline GpuMemoryOverlapType GpuMemoryReverseOverlap(GpuMemoryOverlapType relation)
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

// A texture-backed storage view is a distinct GPU object that may share the
// same allocation. Keep this policy explicit: only the relations observed in
// the Gen5 resource stream are accepted, while other containment directions
// remain strict errors until their producer contract is captured.
//
// Observed relations (strict captures):
//   - Texture Contains StorageBuffer (full sub-allocation view)
//   - Texture Crosses StorageBuffer (partial range view; same link policy)
// Inverse StorageBuffer Equals Texture/RenderTexture/StorageTexture also allowed.
inline bool GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                               GpuMemoryObjectType incoming_type)
{
	if (existing_type == GpuMemoryObjectType::StorageBuffer && relation == GpuMemoryOverlapType::Equals &&
	    (incoming_type == GpuMemoryObjectType::RenderTexture || incoming_type == GpuMemoryObjectType::StorageTexture ||
	     incoming_type == GpuMemoryObjectType::Texture))
	{
		return true;
	}

	if (existing_type != GpuMemoryObjectType::Texture || incoming_type != GpuMemoryObjectType::StorageBuffer)
	{
		return false;
	}

	return relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Crosses;
}

// VertexBuffer parent of an incoming StorageBuffer (multi-parent link path).
// Matches CreateObject multi_vertex_storage_alias / multi_mixed_storage_alias.
inline bool GpuMemoryAllowsVertexStorageShare(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                              GpuMemoryObjectType incoming_type)
{
	if (existing_type != GpuMemoryObjectType::VertexBuffer || incoming_type != GpuMemoryObjectType::StorageBuffer)
	{
		return false;
	}
	return relation == GpuMemoryOverlapType::Crosses || relation == GpuMemoryOverlapType::IsContainedWithin ||
	       relation == GpuMemoryOverlapType::Contains;
}

// Incoming VertexBuffer fully or partially covered by an existing storage or
// render-target allocation (captured: VB Contained in StorageBuffer +
// RenderTexture that share the same guest range).
inline bool GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                                    GpuMemoryObjectType incoming_type)
{
	if (incoming_type != GpuMemoryObjectType::VertexBuffer)
	{
		return false;
	}
	if (existing_type != GpuMemoryObjectType::StorageBuffer && existing_type != GpuMemoryObjectType::RenderTexture &&
	    existing_type != GpuMemoryObjectType::Texture && existing_type != GpuMemoryObjectType::StorageTexture)
	{
		return false;
	}
	return relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Crosses ||
	       relation == GpuMemoryOverlapType::Equals;
}

// Incoming Texture covered by existing StorageBuffer/RenderTexture (or Equals).
// Used with multi-parent Texture create that also reclaims VertexBuffers.
inline bool GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                                     GpuMemoryObjectType incoming_type)
{
	if (incoming_type != GpuMemoryObjectType::Texture)
	{
		return false;
	}
	if (existing_type != GpuMemoryObjectType::StorageBuffer && existing_type != GpuMemoryObjectType::RenderTexture &&
	    existing_type != GpuMemoryObjectType::StorageTexture && existing_type != GpuMemoryObjectType::Texture)
	{
		return false;
	}
	return relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Crosses ||
	       relation == GpuMemoryOverlapType::Equals;
}

// VertexBuffer parent of an incoming Texture that should be reclaimed (delete).
inline bool GpuMemoryAllowsTextureReclaimVertex(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                                GpuMemoryObjectType incoming_type)
{
	return existing_type == GpuMemoryObjectType::VertexBuffer && incoming_type == GpuMemoryObjectType::Texture &&
	       (relation == GpuMemoryOverlapType::Crosses || relation == GpuMemoryOverlapType::IsContainedWithin);
}

// Incoming RenderTexture sharing guest memory with existing surfaces or partial
// VertexBuffers. Captured multi-parent sets after Param5:
//   - StorageBuffer Equals + StorageBuffer Contains + RenderTexture Contains
//   - RenderTexture Contains + StorageBuffer Contains + StorageBuffer Equals
inline bool GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                                    GpuMemoryObjectType incoming_type)
{
	if (incoming_type != GpuMemoryObjectType::RenderTexture)
	{
		return false;
	}
	if (existing_type == GpuMemoryObjectType::StorageBuffer || existing_type == GpuMemoryObjectType::RenderTexture ||
	    existing_type == GpuMemoryObjectType::Texture || existing_type == GpuMemoryObjectType::StorageTexture)
	{
		return relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Crosses ||
		       relation == GpuMemoryOverlapType::Equals || relation == GpuMemoryOverlapType::IsContainedWithin;
	}
	if (existing_type == GpuMemoryObjectType::VertexBuffer)
	{
		return relation == GpuMemoryOverlapType::Crosses || relation == GpuMemoryOverlapType::IsContainedWithin;
	}
	return false;
}

// Incoming StorageBuffer overlapping existing color/depth surfaces (not only
// Texture alias / Vertex share). Captured: RT+SB Crosses and IsContainedWithin
// multi-parent set during the same load path as RenderTexture surface alias.
inline bool GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                              GpuMemoryObjectType incoming_type)
{
	if (incoming_type != GpuMemoryObjectType::StorageBuffer)
	{
		return false;
	}
	if (existing_type != GpuMemoryObjectType::StorageBuffer && existing_type != GpuMemoryObjectType::RenderTexture &&
	    existing_type != GpuMemoryObjectType::Texture && existing_type != GpuMemoryObjectType::StorageTexture)
	{
		return false;
	}
	return relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Crosses ||
	       relation == GpuMemoryOverlapType::Equals || relation == GpuMemoryOverlapType::IsContainedWithin;
}

// WriteBack (GPU -> CPU) hash bookkeeping for aliased objects.
//
// Observed Gen5 topology (private title after post-menu load): a RW
// StorageBuffer write-back target shares guest memory with several
// VertexBuffers (Crosses / IsContainedWithin) and often one Equals peer
// (RenderTexture / StorageTexture / Texture). WriteBack still maps GPU memory
// once; hash policy is:
//   - Equals parents: invalidate + re-hash from CPU (same as single-parent path)
//   - Crosses / Contains / IsContainedWithin: invalidate only (partial overlap)
//   - no parents or only non-Equals: recompute self hash from CPU after write-back
// Any other relation is unsupported and must fail structurally.
enum class GpuMemoryWriteBackParentAction : uint8_t
{
	PropagateEquals, // full hash recompute via Update (Equals alias)
	InvalidateOnly,  // zero hash; next use reloads from CPU
	Unsupported
};

inline GpuMemoryWriteBackParentAction GpuMemoryWriteBackParentActionFor(GpuMemoryOverlapType relation)
{
	switch (relation)
	{
		case GpuMemoryOverlapType::Equals: return GpuMemoryWriteBackParentAction::PropagateEquals;
		case GpuMemoryOverlapType::Crosses:
		case GpuMemoryOverlapType::Contains:
		case GpuMemoryOverlapType::IsContainedWithin: return GpuMemoryWriteBackParentAction::InvalidateOnly;
		case GpuMemoryOverlapType::None:
		case GpuMemoryOverlapType::Max: return GpuMemoryWriteBackParentAction::Unsupported;
	}
	return GpuMemoryWriteBackParentAction::Unsupported;
}

// Returns false if any parent relation is unsupported for WriteBack.
// out_recompute_self is true when no Equals parent exists (self hash after write-back).
// out_equals_count / out_invalidate_count classify parents for callers that walk the list.
inline bool GpuMemoryWriteBackClassifyParents(const GpuMemoryOverlapType* relations, uint32_t count, bool* out_recompute_self,
                                              uint32_t* out_equals_count, uint32_t* out_invalidate_count)
{
	if (out_recompute_self == nullptr || out_equals_count == nullptr || out_invalidate_count == nullptr)
	{
		return false;
	}
	*out_recompute_self  = true;
	*out_equals_count    = 0;
	*out_invalidate_count = 0;
	if (relations == nullptr || count == 0)
	{
		return true;
	}
	for (uint32_t i = 0; i < count; i++)
	{
		switch (GpuMemoryWriteBackParentActionFor(relations[i]))
		{
			case GpuMemoryWriteBackParentAction::PropagateEquals:
				(*out_equals_count)++;
				*out_recompute_self = false;
				break;
			case GpuMemoryWriteBackParentAction::InvalidateOnly: (*out_invalidate_count)++; break;
			case GpuMemoryWriteBackParentAction::Unsupported: return false;
		}
	}
	// Only non-Equals parents: still recompute self after GPU->CPU write-back.
	if (*out_equals_count == 0)
	{
		*out_recompute_self = true;
	}
	return true;
}

struct GpuMemoryObject
{
	GpuMemoryObjectType type = GpuMemoryObjectType::Invalid;
	void*               obj  = nullptr;
};

class GpuObject
{
public:
	using create_func_t = void* (*)(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
	                                VulkanMemory* mem);
	using create_from_objects_func_t = void* (*)(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params,
	                                             GpuMemoryScenario scenario, const Vector<GpuMemoryObject>& objects, VulkanMemory* mem);
	using write_back_func_t = void (*)(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size,
	                                   int vaddr_num);
	using delete_func_t     = void (*)(GraphicContext* ctx, void* obj, VulkanMemory* mem);
	using update_func_t     = void (*)(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size,
                                   int vaddr_num);

	static constexpr int PARAMS_MAX = 8;

	GpuObject()          = default;
	virtual ~GpuObject() = default;

	KYTY_CLASS_DEFAULT_COPY(GpuObject);

	virtual bool Equal(const uint64_t* other) const = 0;
	// virtual bool Reuse(const uint64_t* /*other*/) const { return false; };

	[[nodiscard]] virtual create_func_t              GetCreateFunc() const            = 0;
	[[nodiscard]] virtual create_from_objects_func_t GetCreateFromObjectsFunc() const = 0;
	[[nodiscard]] virtual write_back_func_t          GetWriteBackFunc() const         = 0;
	[[nodiscard]] virtual delete_func_t              GetDeleteFunc() const            = 0;
	[[nodiscard]] virtual update_func_t              GetUpdateFunc() const            = 0;

	uint64_t            params[PARAMS_MAX] = {};
	bool                check_hash         = false;
	bool                read_only          = false;
	GpuMemoryObjectType type               = GpuMemoryObjectType::Invalid;
};

void GpuMemoryInit();

void  GpuMemorySetAllocatedRange(uint64_t vaddr, uint64_t size);
void  GpuMemoryFree(GraphicContext* ctx, uint64_t vaddr, uint64_t size, bool unmap);
void* GpuMemoryCreateObject(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, uint64_t vaddr, uint64_t size,
                            const GpuObject& info);
void* GpuMemoryCreateObject(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
                            int vaddr_num, const GpuObject& info);
void  GpuMemoryResetHash(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type);
void  GpuMemoryDbgDump();
void  GpuMemoryFlush(GraphicContext* ctx, uint64_t vaddr, uint64_t size);
void  GpuMemoryFlushAll(GraphicContext* ctx);
void  GpuMemoryFrameDone();
void  GpuMemoryWriteBack(GraphicContext* ctx, CommandProcessor* cp);
bool  GpuMemoryCheckAccessViolation(uint64_t vaddr, uint64_t size);
bool  GpuMemoryWatcherEnabled();

Vector<GpuMemoryObject> GpuMemoryFindObjects(uint64_t vaddr, uint64_t size, GpuMemoryObjectType type, bool exact, bool only_first);

inline bool GpuMemoryCanShareReadOnlyStorageViews(uint64_t existing_addr, uint64_t existing_size, bool existing_read_only,
                                                   uint64_t incoming_addr, uint64_t incoming_size, bool incoming_read_only)
{
	if (!existing_read_only || !incoming_read_only || existing_size == 0 || incoming_size == 0 ||
	    (existing_addr == incoming_addr && existing_size == incoming_size))
	{
		return false;
	}
	return incoming_addr >= existing_addr ? incoming_addr - existing_addr < existing_size
	                                      : existing_addr - incoming_addr < incoming_size;
}

bool VulkanAllocate(GraphicContext* ctx, VulkanMemory* mem);
void VulkanFree(GraphicContext* ctx, VulkanMemory* mem);
void VulkanMapMemory(GraphicContext* ctx, VulkanMemory* mem, void** data);
void VulkanUnmapMemory(GraphicContext* ctx, VulkanMemory* mem);
void VulkanBindImageMemory(GraphicContext* ctx, VulkanImage* image, VulkanMemory* mem);
void VulkanBindBufferMemory(GraphicContext* ctx, VulkanBuffer* buffer, VulkanMemory* mem);

void GpuMemoryRegisterOwner(uint32_t* owner_handle, const char* name);
void GpuMemoryRegisterResource(uint32_t* resource_handle, uint32_t owner_handle, const void* memory, size_t size, const char* name,
                               uint32_t type, uint64_t user_data);
void GpuMemoryUnregisterAllResourcesForOwner(uint32_t owner_handle);
void GpuMemoryUnregisterOwnerAndResources(uint32_t owner_handle);
void GpuMemoryUnregisterResource(uint32_t resource_handle);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORY_H_ */
