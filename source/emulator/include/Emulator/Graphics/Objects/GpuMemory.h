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

// Non-exact FindObjects relations for sample→RT aliasing.
// GetOverlapType(existing, query):
//   IsContainedWithin = existing RT sits inside the sample query
//   Contains          = sample sits inside an existing live RT
// Unconditional Contains matched multiple overlapping parent RTs and tripped
// FindRenderTexture's Size()>1 EXIT (loading soft-lock). Accept Contains only
// with same_base (identical start address, sample size ≤ RT size) — the
// size-mismatch Equals miss — so one GPU image aliases without parent thrash.
// Offset-into-parent cropped views remain a separate contract. Crosses rejected.
inline bool GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType relation, bool exact, bool same_base = false)
{
	if (relation == GpuMemoryOverlapType::Equals)
	{
		return true;
	}
	if (exact)
	{
		return false;
	}
	if (relation == GpuMemoryOverlapType::IsContainedWithin)
	{
		return true;
	}
	return relation == GpuMemoryOverlapType::Contains && same_base;
}

// When non-exact FindRenderTexture returns multiple overlapping GPU images for
// one sample bind, prefer the tightest cover: smallest object_size that still
// covers sample_size. If every object is smaller than the sample
// (IsContainedWithin-only set), prefer the largest object under the sample.
// sample_size == 0 means "prefer the smallest object" (tightest alias when the
// caller only has a comparable size proxy such as pixel area).
// Inventing a new GPU image or falling through to guest-memory upload here
// previously painted opaque-black props; aborting on Size()>1 killed boot.
[[nodiscard]] inline size_t PreferGpuMemoryAliasIndex(const uint64_t* object_sizes, size_t count, uint64_t sample_size)
{
	if (object_sizes == nullptr || count == 0)
	{
		return 0;
	}
	if (sample_size == 0)
	{
		size_t best = 0;
		for (size_t i = 1; i < count; i++)
		{
			if (object_sizes[i] < object_sizes[best])
			{
				best = i;
			}
		}
		return best;
	}
	size_t best_cover = 0;
	bool   have_cover = false;
	for (size_t i = 0; i < count; i++)
	{
		if (object_sizes[i] < sample_size)
		{
			continue;
		}
		if (!have_cover || object_sizes[i] < object_sizes[best_cover])
		{
			best_cover = i;
			have_cover = true;
		}
	}
	if (have_cover)
	{
		return best_cover;
	}
	size_t best = 0;
	for (size_t i = 1; i < count; i++)
	{
		if (object_sizes[i] > object_sizes[best])
		{
			best = i;
		}
	}
	return best;
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
// Also captured multi-parent create: new VB 0x12500 with parents
// SB/RT IsContainedWithin (surface sits inside the new VB range) and
// SB/RT Crosses — link all surface directions rather than EXIT.
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
	       relation == GpuMemoryOverlapType::Equals || relation == GpuMemoryOverlapType::IsContainedWithin;
}

// Incoming IndexBuffer inside an existing surface allocation. Captured post-Play
// dual-strict: Texture Contains IndexBuffer (vaddr size 0xe4 under a larger
// Texture). Link both — never reclaim the Texture for a tiny index range.
// Same surface types as VertexContainedInSurface; peer IndexBuffer reclaim stays
// on the ObjectsRelation delete_all path.
inline bool GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                                   GpuMemoryObjectType incoming_type)
{
	if (incoming_type != GpuMemoryObjectType::IndexBuffer)
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

// Peer VertexBuffer overlapping an incoming VertexBuffer: reclaim (delete) the
// older VB. Captured multi-parent set mixes surface links with VB
// IsContainedWithin + Crosses parents of the same new VertexBuffer.
inline bool GpuMemoryAllowsVertexReclaimVertex(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                              GpuMemoryObjectType incoming_type)
{
	return existing_type == GpuMemoryObjectType::VertexBuffer && incoming_type == GpuMemoryObjectType::VertexBuffer &&
	       (relation == GpuMemoryOverlapType::Crosses || relation == GpuMemoryOverlapType::IsContainedWithin ||
	        relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Equals);
}

// Incoming VertexBuffer overlapping an existing IndexBuffer. Captured after
// WaitRegMem64 advance: Texture Contains + IndexBuffer Crosses a new VB
// (0x480 under Texture 0x150000, IB 0xf0). Link the IB — do not reclaim it.
inline bool GpuMemoryAllowsVertexLinkIndexBuffer(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                                 GpuMemoryObjectType incoming_type)
{
	return existing_type == GpuMemoryObjectType::IndexBuffer && incoming_type == GpuMemoryObjectType::VertexBuffer &&
	       (relation == GpuMemoryOverlapType::Crosses || relation == GpuMemoryOverlapType::IsContainedWithin ||
	        relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Equals);
}

// Peer IndexBuffer overlapping an incoming IndexBuffer: reclaim the older IB.
// Captured multi-parent: old IB IsContainedWithin (0xe4 under new 0xfc) + VB
// Contains the new IndexBuffer.
inline bool GpuMemoryAllowsIndexReclaimIndex(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                            GpuMemoryObjectType incoming_type)
{
	return existing_type == GpuMemoryObjectType::IndexBuffer && incoming_type == GpuMemoryObjectType::IndexBuffer &&
	       (relation == GpuMemoryOverlapType::Crosses || relation == GpuMemoryOverlapType::IsContainedWithin ||
	        relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Equals);
}

// Incoming IndexBuffer covered by an existing VertexBuffer. Link the VB —
// same family as VertexLinkIndexBuffer (inverse create direction).
inline bool GpuMemoryAllowsIndexLinkVertexBuffer(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                                GpuMemoryObjectType incoming_type)
{
	return existing_type == GpuMemoryObjectType::VertexBuffer && incoming_type == GpuMemoryObjectType::IndexBuffer &&
	       (relation == GpuMemoryOverlapType::Crosses || relation == GpuMemoryOverlapType::IsContainedWithin ||
	        relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Equals);
}

// Incoming Texture overlapping existing StorageBuffer/RenderTexture/Texture.
// Used with multi-parent Texture create that also reclaims VertexBuffers.
// Captured: SB/RT Contains + SB/RT IsContainedWithin (larger texture over a
// second surface pair); Texture Crosses peer Texture.
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
	       relation == GpuMemoryOverlapType::Equals || relation == GpuMemoryOverlapType::IsContainedWithin;
}

// VertexBuffer parent of an incoming Texture that should be reclaimed (delete).
inline bool GpuMemoryAllowsTextureReclaimVertex(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                                GpuMemoryObjectType incoming_type)
{
	return existing_type == GpuMemoryObjectType::VertexBuffer && incoming_type == GpuMemoryObjectType::Texture &&
	       (relation == GpuMemoryOverlapType::Crosses || relation == GpuMemoryOverlapType::IsContainedWithin);
}

// Larger VertexBuffer fully covering an incoming Texture: keep both linked
// (single-parent path already allows Contains/Equals). Captured multi-parent
// Texture 0x1000 with VB Contains + SB Contains + RT Contains.
inline bool GpuMemoryAllowsTextureLinkVertex(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                             GpuMemoryObjectType incoming_type)
{
	return existing_type == GpuMemoryObjectType::VertexBuffer && incoming_type == GpuMemoryObjectType::Texture &&
	       (relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Equals);
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
// Also captured dual-strict after VOP1 SDWA: StorageBuffer 0x8000 Crosses an
// active DepthStencilBuffer (htile/depth plane view) — link, do not reclaim DS.
inline bool GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                              GpuMemoryObjectType incoming_type)
{
	if (incoming_type != GpuMemoryObjectType::StorageBuffer)
	{
		return false;
	}
	if (existing_type != GpuMemoryObjectType::StorageBuffer && existing_type != GpuMemoryObjectType::RenderTexture &&
	    existing_type != GpuMemoryObjectType::Texture && existing_type != GpuMemoryObjectType::StorageTexture &&
	    existing_type != GpuMemoryObjectType::DepthStencilBuffer)
	{
		return false;
	}
	return relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Crosses ||
	       relation == GpuMemoryOverlapType::Equals || relation == GpuMemoryOverlapType::IsContainedWithin;
}

// Keep GpuMemory Label objects linked when a StorageBuffer aliases their fence
// words. Deleting Labels removes them from LabelWriteBackCopy's hole set, so a
// later StorageBuffer WriteBack can zero guest EOP fences (immediate store /
// FireCallbacks publish) and leave CPU code spinning with val=0 — guest then
// never reaches KernelSetEventFlag(ThreadFlag). Captured Dead Cells soft-lock:
// EVENTFLAG_SET=0 while OnlyFlip still presents.
inline bool GpuMemoryKeepLabelWriteBackHole(GpuMemoryObjectType existing_type, GpuMemoryOverlapType relation,
                                           GpuMemoryObjectType incoming_type)
{
	if (existing_type == GpuMemoryObjectType::Label && incoming_type == GpuMemoryObjectType::StorageBuffer)
	{
		return relation == GpuMemoryOverlapType::IsContainedWithin || relation == GpuMemoryOverlapType::Equals ||
		       relation == GpuMemoryOverlapType::Crosses;
	}
	// Creating a Label inside an existing StorageBuffer: link both so the Label
	// stays registered for WriteBack holes (do not reclaim the StorageBuffer).
	if (existing_type == GpuMemoryObjectType::StorageBuffer && incoming_type == GpuMemoryObjectType::Label)
	{
		return relation == GpuMemoryOverlapType::Contains || relation == GpuMemoryOverlapType::Equals ||
		       relation == GpuMemoryOverlapType::Crosses;
	}
	return false;
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
//
// GPU-owned tiled RenderTextures (PARAM_TILED set, PARAM_WRITE_BACK clear) have
// no CPU upload path. Captured multi-parent write-back links always pair a RW
// StorageBuffer with RT Contains/Crosses parents on the post-Play path. Zeroing
// those parents' hash/submit_id only forces a no-op Update (after layout fix) or
// historically discarded contents via UNDEFINED. Skip parent invalidate entirely
// for that class — GPU image remains source of truth.
//
// RenderTextureObject param slots (must stay in sync with RenderTexture.h):
//   PARAM_TILED = 3, PARAM_WRITE_BACK = 6
inline bool GpuMemoryIsGpuOwnedRenderTextureParams(const uint64_t* params)
{
	if (params == nullptr)
	{
		return false;
	}
	return params[3] != 0 && params[6] == 0;
}

inline bool GpuMemorySkipWriteBackParentInvalidate(GpuMemoryObjectType parent_type, const uint64_t* parent_params)
{
	return parent_type == GpuMemoryObjectType::RenderTexture && GpuMemoryIsGpuOwnedRenderTextureParams(parent_params);
}

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
