#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/Gen5TextureArrayLayout.h"
#include "Emulator/Graphics/Gen5TextureMipLayout.h"
#include "Emulator/Graphics/Gen5TextureVolumeLayout.h"
#include "Emulator/Graphics/GuestTextureLayout.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/GpuMemoryTransientBuffer.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/StorageBuffer.h"
#include "Emulator/Graphics/Objects/StorageTexture.h"
#include "Emulator/Graphics/Objects/Texture.h"
#include "Emulator/Graphics/Objects/VertexBuffer.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/VulkanVertexInputLayout.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/GuestMemory.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

using BindingStageClock = std::chrono::steady_clock;

static uint64_t BindingStageElapsedNs(BindingStageClock::time_point start)
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(BindingStageClock::now() - start).count());
}

// vertex/storage/texture/sampler preparation and descriptor bind

VulkanBuffer* TryUploadTransientReadOnlyBuffer(CommandBuffer* buffer, uint64_t addr, uint64_t size, bool read_only,
                                               VkBufferUsageFlags usage)
{
	if (buffer == nullptr || !read_only || size == 0u || size > 0x1000u)
	{
		return nullptr;
	}

	const auto query_start = BindingStageClock::now();
	const bool eligible    = GpuMemoryCanSnapshotReadOnlyBuffer(addr, size);
	const auto query_ns    = BindingStageElapsedNs(query_start);
	if (!eligible)
	{
		DebugStatsRecordTransientBufferProbe(0, query_ns, 0, false);
		return nullptr;
	}
	const auto     upload_start = BindingStageClock::now();
	auto*          result       = buffer->UploadTransientBuffer(reinterpret_cast<const void*>(addr), size, usage);
	const uint64_t upload_ns    = BindingStageElapsedNs(upload_start);
	DebugStatsRecordTransientBufferProbe(0, query_ns, upload_ns, result != nullptr);
	return result;
}

void BindVertexBuffers(uint64_t submit_id, CommandBuffer* buffer, VkCommandBuffer vk_buffer, const ShaderVertexInputInfo& input)
{
	EXIT_IF(buffer == nullptr || vk_buffer == nullptr || g_render_ctx == nullptr);

	for (int i = 0; i < input.buffers_num; i++)
	{
		const auto& buffer_info = input.buffers[i];
		const uint64_t address  = buffer_info.addr;
		const uint64_t size     = ShaderBufferByteSize(buffer_info.stride, buffer_info.num_records);

		auto* vertices = TryUploadTransientReadOnlyBuffer(buffer, address, size, true, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
		if (vertices == nullptr)
		{
			vertices = static_cast<VulkanBuffer*>(
			    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, address, size, VertexBufferGpuObject()));
		}
		if (vertices == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vertices == nullptr condition ignored (continuing)\n"); }

		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(vk_buffer, static_cast<uint32_t>(i), 1, &vertices->buffer, &offset);
	}
}

static Emulator::Agent::Lifecycle::StorageBindingProvenance DescribeStorageBinding(const ShaderStorageResources& storage_buffers,
                                                                                      int index)
{
	Emulator::Agent::Lifecycle::StorageBindingProvenance binding {};
	switch (storage_buffers.accesses[index])
	{
		case ShaderStorageAccess::Unknown: binding.access = Emulator::Agent::Lifecycle::StorageAccessClass::Unknown; break;
		case ShaderStorageAccess::Raw: binding.access = Emulator::Agent::Lifecycle::StorageAccessClass::Raw; break;
		case ShaderStorageAccess::Typed: binding.access = Emulator::Agent::Lifecycle::StorageAccessClass::Typed; break;
		case ShaderStorageAccess::Mixed: binding.access = Emulator::Agent::Lifecycle::StorageAccessClass::Mixed; break;
		case ShaderStorageAccess::UnusedMetadata: binding.access = Emulator::Agent::Lifecycle::StorageAccessClass::Unknown; break;
	}
	switch (storage_buffers.sources[index])
	{
		case ShaderStorageBindingSource::DirectResource:
			binding.source = Emulator::Agent::Lifecycle::StorageBindingSource::Direct;
			break;
		case ShaderStorageBindingSource::MetadataSharp:
			binding.source = Emulator::Agent::Lifecycle::StorageBindingSource::Metadata;
			break;
		case ShaderStorageBindingSource::DynamicScalarLoad:
			binding.source = Emulator::Agent::Lifecycle::StorageBindingSource::Dynamic;
			break;
	}
	switch (storage_buffers.unknown_reasons[index])
	{
		case ShaderStorageUnknownReason::None: binding.unknown_reason = Emulator::Agent::Lifecycle::StorageUnknownReason::None; break;
		case ShaderStorageUnknownReason::CodeUnavailable:
			binding.unknown_reason = Emulator::Agent::Lifecycle::StorageUnknownReason::CodeUnavailable;
			break;
		case ShaderStorageUnknownReason::NoMatchingInstruction:
			binding.unknown_reason = Emulator::Agent::Lifecycle::StorageUnknownReason::NoMatchingInstruction;
			break;
		case ShaderStorageUnknownReason::RegisterBaseMismatch:
			binding.unknown_reason = Emulator::Agent::Lifecycle::StorageUnknownReason::RegisterBaseMismatch;
			break;
		case ShaderStorageUnknownReason::MetadataOnlyBinding:
			binding.unknown_reason = Emulator::Agent::Lifecycle::StorageUnknownReason::MetadataOnlyBinding;
			break;
	}
	binding.code_available = storage_buffers.code_available[index];
	binding.exact_match    = storage_buffers.exact_matches[index];
	binding.indirect_use   = storage_buffers.indirect_descriptor_use[index];
	binding.raw_vmem_oob_guarded = storage_buffers.raw_vmem_oob_guarded[index];
	binding.raw_smem_use         = storage_buffers.raw_smem_use[index];
	binding.raw_tbuffer_use      = storage_buffers.raw_tbuffer_use[index];
	return binding;
}

static Emulator::Agent::Lifecycle::StorageRangeBacking DescribeStorageRangeBacking(
	Emulator::GuestMemory::MappedRangeKind kind)
{
	switch (kind)
	{
		case Emulator::GuestMemory::MappedRangeKind::None: return Emulator::Agent::Lifecycle::StorageRangeBacking::None;
		case Emulator::GuestMemory::MappedRangeKind::Physical: return Emulator::Agent::Lifecycle::StorageRangeBacking::Physical;
		case Emulator::GuestMemory::MappedRangeKind::Flexible: return Emulator::Agent::Lifecycle::StorageRangeBacking::Flexible;
	}
	return Emulator::Agent::Lifecycle::StorageRangeBacking::None;
}

static void ReportStorageRange(const ShaderStorageResources& storage_buffers, int index, const ShaderBufferResource& resource,
	                           uint64_t base, uint64_t declared_size, uint64_t materialized_size)
{
	Emulator::GuestMemory::MappedRange mapped_range {};
	const bool has_mapped_range = declared_size != 0 && Emulator::GuestMemory::GetPort().QueryMappedRange(base, declared_size, &mapped_range);

	Emulator::Agent::Lifecycle::StorageRangeContext context {};
	context.binding           = DescribeStorageBinding(storage_buffers, index);
	context.backing           = has_mapped_range ? DescribeStorageRangeBacking(mapped_range.kind) :
	                                              Emulator::Agent::Lifecycle::StorageRangeBacking::None;
	context.backing_size      = has_mapped_range ? mapped_range.size : 0;
	context.resource_index    = index;
	context.sgpr              = storage_buffers.start_register[index];
	context.slot              = storage_buffers.slots[index];
	context.usage             = static_cast<uint32_t>(storage_buffers.usages[index]);
	context.stride            = resource.Stride();
	context.base              = base;
	context.declared_size     = declared_size;
	context.materialized_size = materialized_size;
	context.descriptor_words[0] = resource.fields[0];
	context.descriptor_words[1] = resource.fields[1];
	context.descriptor_words[2] = resource.fields[2];
	context.descriptor_words[3] = resource.fields[3];
	if (declared_size == 0 || materialized_size == 0)
	{
		Emulator::Agent::Lifecycle::EmitStorageRangeFatal(context);
	} else
	{
		Emulator::Agent::Lifecycle::EmitStorageRange(context);
	}
}

static void PrepareStorageBuffers(uint64_t submit_id, CommandBuffer* buffer, const ShaderStorageResources& storage_buffers,
                                  VulkanBuffer** buffers, uint32_t** sgprs)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffers == nullptr);
	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);
	EXIT_IF(buffer == nullptr);

	bool gen5 = Config::IsNextGen();

	for (int i = 0; i < storage_buffers.buffers_num; i++)
	{
		EXIT_IF(storage_buffers.accesses[i] == ShaderStorageAccess::UnusedMetadata);
		auto r = storage_buffers.buffers[i];

		// Gen5 MUBUF/MTBUF lowering consumes the descriptor's ADD_TID, swizzle
		// and index-stride bits in the shared SPIR-V byte-address equation. Keep
		// the descriptor intact here for both raw and typed resource users.

		if (gen5)
		{
			// OutOfBounds modes (0..3) are descriptor policy; Vulkan SSBO
			// robust access covers the common case. Do not hard-fail.
			if (!ShaderGen5StorageDescriptorSupported(r, storage_buffers.accesses[i]))
			{
				Emulator::Agent::Lifecycle::StorageFrontierContext context {};
				context.binding         = DescribeStorageBinding(storage_buffers, i);
				context.unbased_match   = storage_buffers.unbased_matches[i];
				context.decoded_unknown = storage_buffers.decoded_unknown[i];
				context.resource_index  = i;
				context.sgpr            = storage_buffers.start_register[i];
				context.slot            = storage_buffers.slots[i];
				context.usage           = static_cast<uint32_t>(storage_buffers.usages[i]);
				context.stride          = r.Stride();
				context.format          = r.Format();
				context.dst_sel         = r.DstSelXYZW();
				context.add_tid         = r.AddTid();
				context.swizzle         = r.SwizzleEnabled();
				Emulator::Agent::Lifecycle::EmitStorageFrontierFatal(context);
				/* [gen5-nonfatal] EXIT("unsupported Gen5 storage buffer format: index=%d start=%d usage=%u stride=%u dstsel=0x%03" PRIx32
				 */
				KYTY_LOG_DEBUG("WARNING: unsupported Gen5 storage buffer format: index=%d start=%d usage=%u stride=%u dstsel=0x%03" PRIx32
				       " (continuing)\n",
				       i, storage_buffers.start_register[i], static_cast<uint32_t>(storage_buffers.usages[i]),
				       static_cast<uint32_t>(r.Stride()), r.DstSelXYZW());
			}
		} else
		{
			const bool address_only_descriptor =
			    storage_buffers.accesses[i] == ShaderStorageAccess::Raw ||
			    (storage_buffers.accesses[i] == ShaderStorageAccess::Unknown && !storage_buffers.code_available[i]);
			if (address_only_descriptor)
			{
				if (!ShaderRawStorageDescriptorSupported(r)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderRawStorageDescriptorSupported(r) condition ignored (continuing)\n"); }
			} else
			{
				if (!((r.Stride() == 4 && r.DstSelXYZW() == DstSel(4, 0, 0, 0) && r.Dfmt() == 4 && r.Nfmt() == 4) ||
				                       (r.Stride() == 4 && r.DstSelXYZW() == DstSel(4, 0, 0, 1) && r.Dfmt() == 4 && r.Nfmt() == 7) ||
				                       (r.Stride() == 8 && r.DstSelXYZW() == DstSel(4, 5, 0, 0) && r.Dfmt() == 11 && r.Nfmt() == 4) ||
				                       (r.Stride() == 16 && r.DstSelXYZW() == DstSel(4, 5, 6, 7) && r.Dfmt() == 14 && r.Nfmt() == 7))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !((r.Stride() == 4 && r.DstSelXYZW() == DstSel(4, 0, 0, 0) && r.Dfmt() == 4 && r condition ignored (continuing)\n"); }
			}
			if (!(r.MemoryType() == 0x00 || r.MemoryType() == 0x10 || r.MemoryType() == 0x6d)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !(r.MemoryType() == 0x00 || r.MemoryType() == 0x10 || r.MemoryType() == 0x6d) condition ignored (continuing)\n"); }
		}

		auto           addr        = (gen5 ? r.Base48() : r.Base44());
		auto           stride      = r.Stride();
		auto           num_records = r.NumRecords();
		const bool raw_vmem_empty_oob = storage_buffers.raw_vmem_oob_guarded[i] && ShaderGen5RawDescriptorAlwaysOutOfBounds(r);
		const bool raw_smem_empty_oob = storage_buffers.raw_smem_use[i] && ShaderGen5SBufferDescriptorAlwaysOutOfBounds(r);
		const bool raw_empty_oob = gen5 && storage_buffers.accesses[i] == ShaderStorageAccess::Raw &&
		                           !storage_buffers.decoded_unknown[i] && !storage_buffers.raw_tbuffer_use[i] &&
		                           (storage_buffers.raw_vmem_oob_guarded[i] || storage_buffers.raw_smem_use[i]) &&
		                           (!storage_buffers.raw_vmem_oob_guarded[i] || raw_vmem_empty_oob) &&
		                           (!storage_buffers.raw_smem_use[i] || raw_smem_empty_oob);

		VulkanBuffer* buf = nullptr;
		if (raw_empty_oob)
		{
			// Every known raw consumer is proven out of range: MUBUF is guarded in
			// SPIR-V and S_BUFFER_LOAD is lowered to zero. Vulkan still requires a
			// valid SSBO array element, so bind a minimal carrier with no guest-memory
			// ownership or observable data path.
			static constexpr uint32_t kEmptyRawStorageDescriptorCarrier = 0;
			buf = buffer->UploadTransientBuffer(&kEmptyRawStorageDescriptorCarrier, sizeof(kEmptyRawStorageDescriptorCarrier),
			                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
		} else
		{
			const uint64_t declared_size = ShaderBufferByteSize(stride, num_records);
			if (declared_size == 0)
			{
				ReportStorageRange(storage_buffers, i, r, addr, declared_size, 0);
				if (true) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: true condition ignored (continuing)\n"); }
			}

			const bool read_only = ShaderStorageUsageIsReadOnly(storage_buffers.usages[i]);
			if (read_only && !(storage_buffers.usages[i] == ShaderStorageUsage::ReadOnly ||
			                                    storage_buffers.usages[i] == ShaderStorageUsage::Constant)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: read_only && !(storage_buffers.usages[i] == ShaderStorageUsage::ReadOnly || condition ignored (continuing)\n"); }
			const bool exact_static_smem = gen5 && storage_buffers.accesses[i] == ShaderStorageAccess::Raw &&
			                               storage_buffers.exact_matches[i] && !storage_buffers.decoded_unknown[i] &&
			                               !storage_buffers.indirect_descriptor_use[i] && storage_buffers.raw_smem_use[i] &&
			                               !storage_buffers.raw_vmem_oob_guarded[i] && !storage_buffers.raw_tbuffer_use[i] &&
			                               !storage_buffers.raw_smem_dynamic_offset[i] && storage_buffers.raw_smem_required_bytes[i] != 0;
			const uint64_t requested_size =
			    exact_static_smem ? std::min(declared_size, storage_buffers.raw_smem_required_bytes[i]) : declared_size;
			const uint64_t materialized_size = GpuMemoryGetAllocatedRangePrefix(addr, requested_size);

			// Executable images are mapped by the loader rather than the GPU heap.
			// A statically addressed scalar load can safely use a per-submit copy of
			// only the dwords proven reachable by the shader.
			if (materialized_size == 0 && exact_static_smem && read_only && requested_size <= 0x1000u &&
			    Core::VirtualMemory::IsRangeReadable(addr, requested_size))
			{
				buf =
				    buffer->UploadTransientBuffer(reinterpret_cast<const void*>(addr), requested_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			} else if (materialized_size == 0)
			{
				ReportStorageRange(storage_buffers, i, r, addr, declared_size, materialized_size);
				if (materialized_size == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: materialized_size == 0 condition ignored (continuing)\n"); }

				EXIT("storage buffer range is not materialized: index=%d addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", i,
				     addr, requested_size);
			} else
			{
				if (materialized_size != requested_size)
				{
					ReportStorageRange(storage_buffers, i, r, addr, declared_size, materialized_size);
				}
				if (materialized_size == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: materialized_size == 0 condition ignored (continuing)\n"); }

				StorageBufferGpuObject buf_info(stride, num_records, read_only);
				buf = TryUploadTransientReadOnlyBuffer(buffer, addr, materialized_size, read_only, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
				if (buf == nullptr)
				{
					buf = static_cast<StorageVulkanBuffer*>(
					    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, materialized_size, buf_info));
				}
			}
		}

		// Descriptor writes require a real VkBuffer. Only proven empty/OOB
		// descriptors receive a zero carrier; all materialization failures stay strict.
		EXIT_IF(buf == nullptr || buf->buffer == nullptr);

		buffers[i] = buf;

		if (gen5)
		{
			r.UpdateAddress48(i);
		} else
		{
			r.UpdateAddress44(i);
		}

		if (((gen5 ? r.Base48() : r.Base44()) >> 32u) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ((gen5 ? r.Base48() : r.Base44()) >> 32u) != 0 condition ignored (continuing)\n"); }

		(*sgprs)[0] = r.fields[0];
		(*sgprs)[1] = r.fields[1];
		(*sgprs)[2] = r.fields[2];
		(*sgprs)[3] = r.fields[3];

		(*sgprs) += 4;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static bool ShouldForceGen5Degamma(const ShaderSamplerResources& samplers, int sampled_index)
{
	if (sampled_index < 0 || sampled_index >= samplers.samplers_num)
	{
		return false;
	}

	const auto& sampler = samplers.samplers[sampled_index];
	return sampler.ForceDegamma() && !sampler.SkipDegamma();
}

static void PrepareTextures(uint64_t submit_id, CommandBuffer* buffer, const ShaderTextureResources& textures,
                            const ShaderSamplerResources& samplers, VulkanImage** images_sampled, VulkanImage** images_storage,
                            int* images_sampled_view, VulkanImage** images_sampled_array, int* images_sampled_array_view,
                            VulkanImage** images_sampled_3d, int* images_sampled_3d_view, VulkanImage** images_sampled_uint,
                            int* images_sampled_uint_view, VulkanImage** images_sampled_array_uint,
                            int* images_sampled_array_uint_view, VulkanImage** images_sampled_3d_uint,
                            int* images_sampled_3d_uint_view, int* images_storage_view, uint32_t storage_seed_skip_mask,
                            uint32_t** sgprs)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(images_sampled == nullptr);
	EXIT_IF(images_sampled_array == nullptr);
	EXIT_IF(images_sampled_3d == nullptr);
	EXIT_IF(images_sampled_uint == nullptr);
	EXIT_IF(images_sampled_array_uint == nullptr);
	EXIT_IF(images_sampled_3d_uint == nullptr);
	EXIT_IF(images_storage == nullptr);
	EXIT_IF(images_sampled_view == nullptr);
	EXIT_IF(images_sampled_array_view == nullptr);
	EXIT_IF(images_sampled_3d_view == nullptr);
	EXIT_IF(images_sampled_uint_view == nullptr);
	EXIT_IF(images_sampled_array_uint_view == nullptr);
	EXIT_IF(images_sampled_3d_uint_view == nullptr);
	EXIT_IF(images_storage_view == nullptr);
	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	int          index_sampled                  = 0;
	int          index_sampled_array            = 0;
	int          index_sampled_3d               = 0;
	int          index_sampled_uint             = 0;
	int          index_sampled_array_uint       = 0;
	int          index_sampled_3d_uint          = 0;
	int          index_storage                  = 0;
	VulkanImage* sampled_2d_padding_image       = nullptr;
	VulkanImage* sampled_2d_array_padding_image = nullptr;
	VulkanImage* sampled_3d_padding_image       = nullptr;
	int          sampled_2d_padding_view        = VulkanImage::VIEW_DEFAULT;
	int          sampled_2d_array_padding_view  = VulkanImage::VIEW_ARRAY;
	int          sampled_3d_padding_view        = VulkanImage::VIEW_3D;
	VulkanImage* sampled_2d_uint_padding_image       = nullptr;
	VulkanImage* sampled_2d_array_uint_padding_image = nullptr;
	VulkanImage* sampled_3d_uint_padding_image       = nullptr;
	int          sampled_2d_uint_padding_view        = VulkanImage::VIEW_DEFAULT;
	int          sampled_2d_array_uint_padding_view  = VulkanImage::VIEW_ARRAY;
	int          sampled_3d_uint_padding_view        = VulkanImage::VIEW_3D;

	bool gen5 = Config::IsNextGen();
	const int sampled_total = textures.textures2d_sampled_num + textures.textures2d_array_sampled_num + textures.textures3d_sampled_num;
	const int sampled_uint_total = textures.textures2d_sampled_uint_num + textures.textures2d_array_sampled_uint_num +
	                               textures.textures3d_sampled_uint_num;
	const bool split_numeric_types = gen5 && sampled_uint_total > 0 && sampled_uint_total < sampled_total;

	for (int i = 0; i < textures.textures_num; i++)
	{
		auto       r                 = textures.desc[i].texture;
		const auto sampled_shape = ShaderGen5SampledTextureShapeForType(r.Type());
		const bool arrayed_2d = gen5 && sampled_shape == ShaderGen5SampledTextureShape::TwoDimensionalArray;
		const bool three_dimensional = gen5 && sampled_shape == ShaderGen5SampledTextureShape::ThreeDimensional;

		if (gen5)
		{
			const auto tile_mode = static_cast<uint32_t>(r.TileMode());
			if (tile_mode != 0u && tile_mode != 5u && tile_mode != 9u && tile_mode != 24u && tile_mode != 27u)
			{
				EXIT("unsupported Gen5 sampled texture tile mode: tile=%u format=%u width=%u height=%u base=0x%012" PRIx64
				     " type=%u\n",
				     tile_mode, static_cast<uint32_t>(r.Format()), r.Width5() + 1u, r.Height5() + 1u, r.Base40(),
				     static_cast<uint32_t>(r.Type()));
			}
			if (!VulkanSupportsGen5ImageFormat(GuestImageUsage::Sampled, r.Format()))
			{
				EXIT("unsupported Gen5 sampled texture format: format=%u tile=%u width=%u height=%u base=0x%012" PRIx64
				     " type=%u\n",
				     static_cast<uint32_t>(r.Format()), tile_mode, r.Width5() + 1u, r.Height5() + 1u, r.Base40(),
				     static_cast<uint32_t>(r.Type()));
			}
			if (r.PerfMod5() != 7 && r.PerfMod5() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.PerfMod5() != 7 && r.PerfMod5() != 0 condition ignored (continuing)\n"); }
			if (r.BCSwizzle() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.BCSwizzle() != 0 condition ignored (continuing)\n"); }
			// BaseArray5 and ArrayPitch are layer-addressing fields. Their bit
			// positions are not array state for Color3D descriptors.
			if (!three_dimensional && !arrayed_2d && r.BaseArray5() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !three_dimensional && !arrayed_2d && r.BaseArray5() != 0 condition ignored (continuing)\n"); }
			if (!three_dimensional && !arrayed_2d && r.ArrayPitch() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !three_dimensional && !arrayed_2d && r.ArrayPitch() != 0 condition ignored (continuing)\n"); }
			// MAX_MIP describes the backing allocation; BASE_LEVEL/LAST_LEVEL are
			// only the view range. The upload below creates the full allocation so
			// the physical offsets of its mip chain (including its tail) remain
			// correct for every view.
			if (r.MaxMip() != 0 && (r.BaseLevel() > r.LastLevel() || r.LastLevel() > r.MaxMip())) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MaxMip() != 0 && (r.BaseLevel() > r.LastLevel() || r.LastLevel() > r.MaxMip()) condition ignored (continuing)\n"); }
			if (r.MinLodWarn5() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MinLodWarn5() != 0 condition ignored (continuing)\n"); }
			if (r.MipStatsCntId() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MipStatsCntId() != 0 condition ignored (continuing)\n"); }
			if (r.MipStatsCntEn() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MipStatsCntEn() != false condition ignored (continuing)\n"); }
			if (r.CornerSample() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.CornerSample() != false condition ignored (continuing)\n"); }
			if (r.PrtDefColor() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.PrtDefColor() != false condition ignored (continuing)\n"); }
			if (r.MsaaDepth() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MsaaDepth() != false condition ignored (continuing)\n"); }
			if (r.MaxUncompBlkSize() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MaxUncompBlkSize() != 0 condition ignored (continuing)\n"); }
			if (r.MaxCompBlkSize() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MaxCompBlkSize() != 0 condition ignored (continuing)\n"); }
			if (r.MetaPipeAligned() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MetaPipeAligned() != false condition ignored (continuing)\n"); }
			if (r.WriteCompress() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.WriteCompress() != false condition ignored (continuing)\n"); }
			if (r.MetaCompress() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MetaCompress() != false condition ignored (continuing)\n"); }
			if (r.DccAlphaPos() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.DccAlphaPos() != false condition ignored (continuing)\n"); }
			if (r.DccColorTransf() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.DccColorTransf() != false condition ignored (continuing)\n"); }
			if (std::getenv("KYTY_SAMPLE_BIND_METADATA_LOG") != nullptr)
			{
				static std::atomic_uint metadata_log_count {0};
				const unsigned ordinal = metadata_log_count.fetch_add(1, std::memory_order_relaxed);
				if (ordinal < 32u)
				{
					KYTY_LOG_DEBUG(
					             "KYTY_SAMPLE_BIND_METADATA format=%u tile=%u extent=%ux%u base=0x%012" PRIx64
					             " meta=0x%012" PRIx64 " pipe=%u write=%u compress=%u alpha=%u color=%u\n",
					             static_cast<unsigned>(r.Format()), static_cast<unsigned>(r.TileMode()),
					             static_cast<unsigned>(r.Width5() + 1u), static_cast<unsigned>(r.Height5() + 1u), r.Base40(),
					             r.MetaAddr(), r.MetaPipeAligned() ? 1u : 0u, r.WriteCompress() ? 1u : 0u,
					             r.MetaCompress() ? 1u : 0u, r.DccAlphaPos() ? 1u : 0u, r.DccColorTransf() ? 1u : 0u);
				}
				else if (ordinal == 32u)
				{
					KYTY_LOG_DEBUG( "KYTY_SAMPLE_BIND_METADATA further entries suppressed\n");
				}
			}
			if (ShaderGen5SampledTextureMetadataRequiresDcc(r)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ShaderGen5SampledTextureMetadataRequiresDcc(r) condition ignored (continuing)\n"); }
		} else
		{
			if (r.Dfmt() != 1 && r.Dfmt() != 10 && r.Dfmt() != 37 && r.Dfmt() != 4 && r.Dfmt() != 35 && r.Dfmt() != 3 &&
			                     r.Dfmt() != 36) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.Dfmt() != 1 && r.Dfmt() != 10 && r.Dfmt() != 37 && r.Dfmt() != 4 && r.Dfmt() ! condition ignored (continuing)\n"); }
			if (r.Nfmt() != 9 && r.Nfmt() != 0 && r.Nfmt() != 7) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.Nfmt() != 9 && r.Nfmt() != 0 && r.Nfmt() != 7 condition ignored (continuing)\n"); }
			if (r.PerfMod() != 7 && r.PerfMod() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.PerfMod() != 7 && r.PerfMod() != 0 condition ignored (continuing)\n"); }
			if (r.Interlaced() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.Interlaced() != false condition ignored (continuing)\n"); }
			if (!(r.TileMode() == 8 || r.TileMode() == 13 || r.TileMode() == 14 || r.TileMode() == 2 ||
			                       r.TileMode() == 10 || r.TileMode() == 31)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !(r.TileMode() == 8 || r.TileMode() == 13 || r.TileMode() == 14 || r.TileMode()  condition ignored (continuing)\n"); }
			if (r.BaseArray() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.BaseArray() != 0 condition ignored (continuing)\n"); }
			if (r.LastArray() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.LastArray() != 0 condition ignored (continuing)\n"); }
			if (r.MinLodWarn() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MinLodWarn() != 0 condition ignored (continuing)\n"); }
			if (r.CounterBankId() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.CounterBankId() != 0 condition ignored (continuing)\n"); }
			if (r.LodHdwCntEn() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.LodHdwCntEn() != false condition ignored (continuing)\n"); }
			if (r.MemoryType() != 0x10 && r.MemoryType() != 0x6d) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MemoryType() != 0x10 && r.MemoryType() != 0x6d condition ignored (continuing)\n"); }
		}
		if ((gen5 ? r.Base40() : r.Base38()) == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (gen5 ? r.Base40() : r.Base38()) == 0 condition ignored (continuing)\n"); }
		if (r.MinLod() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.MinLod() != 0 condition ignored (continuing)\n"); }
		if (r.Type() != 8 && r.Type() != 9 && !arrayed_2d && !three_dimensional) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.Type() != 8 && r.Type() != 9 && !arrayed_2d && !three_dimensional condition ignored (continuing)\n"); }
		if (arrayed_2d && (r.ArrayPitch() != 0 || r.BaseArray5() > r.Depth())) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: arrayed_2d && (r.ArrayPitch() != 0 || r.BaseArray5() > r.Depth()) condition ignored (continuing)\n"); }
		// Gen5 2D resources encode pitch in word4[13:0]; Depth() overlaps those bits.
		if (!gen5)
		{
			if (r.Depth() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.Depth() != 0 condition ignored (continuing)\n"); }
		}

		bool read_only = (gen5 ? false : (r.MemoryType() == 0x10));

		if (read_only && !(textures.desc[i].usage == ShaderTextureUsage::ReadOnly)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: read_only && !(textures.desc[i].usage == ShaderTextureUsage::ReadOnly) condition ignored (continuing)\n"); }

		TileSizeAlign  size {};
		auto           addr       = (gen5 ? r.Base40() : r.Base38());
		bool           neo        = Config::IsNeo();
		auto           width      = (gen5 ? r.Width5() : r.Width4()) + 1;
		auto           height     = (gen5 ? r.Height5() : r.Height4()) + 1;
		const uint32_t depth      = ((three_dimensional || arrayed_2d) ? static_cast<uint32_t>(r.Depth()) + 1u : 1u);
		const uint32_t base_array = (arrayed_2d ? static_cast<uint32_t>(r.BaseArray5()) : 0u);
		auto           tile       = r.TileMode();
		// Gen5 linear rows are 256-byte aligned (e.g. RGBA8 pitch = align(width, 64)).
		// Using width alone mis-unpacks non-pow2 logos into horizontal bands.
		// Mode 27 (kRenderTarget): element pitch aligns to the 64 KiB block width
		// for the sample BPE so size/alias matches the CB path.
		// Mode 9 (kStandard64KB) pitches to the 128-element block width for 4 BPE.
		uint32_t pitch = 0;
		if (!gen5)
		{
			pitch = r.Pitch() + 1;
		} else if (tile == 27)
		{
			const uint32_t bpp = ShaderGen5TextureBytesPerElement(r.Format());
			pitch              = TileAlign64KBPitch(width, bpp);
			if (pitch == 0u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: pitch == 0u condition ignored (continuing)\n"); }
		} else if (tile == 9 || tile == 24)
		{
			pitch = TileAlign64KBPitch(width, ShaderGen5TextureBytesPerElement(r.Format()));
			if (pitch == 0u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: pitch == 0u condition ignored (continuing)\n"); }
		} else if (tile == 5 && !three_dimensional && !arrayed_2d)
		{
			// Standard4KB resources use a canonical tiled pitch. Word4 is a
			// linear-resource field and must not expand the mip layout.
			pitch = width;
		} else
		{
			// Prefer descriptor pitch (256-bit RSRC word4) over width alone.
			pitch = ShaderGen5ResolveLinearPitch(width, r.Format(), r.Type(), r.fields[4]);
		}
		auto       base_level    = (gen5 && r.MaxMip() == 0u ? 0u : r.BaseLevel());
		auto       levels        = (gen5 ? static_cast<uint32_t>(r.MaxMip()) + 1u : r.LastLevel() + 1u);
		auto       dfmt          = (gen5 ? 0 : r.Dfmt());
		auto       nfmt          = (gen5 ? 0 : r.Nfmt());
		auto       fmt           = (gen5 ? r.Format() : 0);
		uint32_t   swizzle       = r.DstSelXYZW();
		uint32_t   view_swizzle  = swizzle;
		const bool force_degamma = gen5 && !textures.desc[i].textures2d_without_sampler && ShouldForceGen5Degamma(samplers, index_sampled);

		const bool check_depth_texture = (!gen5 && tile == 2u) || (gen5 && tile == 24u);

		if (gen5 && check_depth_texture)
		{
			if (fmt != 22u || (r.Type() != 9u && r.Type() != 13u) || r.Depth() != 0u || r.BaseArray5() != 0u ||
			                     r.BaseLevel() != 0u || r.LastLevel() != 0u || r.MaxMip() != 0u || r.BCSwizzle() != 0u || r.MsaaDepth()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: fmt != 22u || (r.Type() != 9u && r.Type() != 13u) || r.Depth() != 0u || r.BaseAr condition ignored (continuing)\n"); }
			if (swizzle != DstSel(4, 4, 4, 4) && swizzle != DstSel(4, 0, 0, 0) &&
			                     swizzle != DstSel(4, 0, 0, 1)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: swizzle != DstSel(4, 4, 4, 4) && swizzle != DstSel(4, 0, 0, 0) && condition ignored (continuing)\n"); }
		}

		if (gen5 && !three_dimensional && !arrayed_2d && tile == 5u && levels > 1u)
		{
			Gen5TextureMipLayout mip_layout {};
			if (!Gen5GetStandard4KBTextureMipLayout(fmt, width, height, pitch, levels, &mip_layout))
			{
				EXIT("Unsupported Gen5 Standard4KB mip texture layout: format=%u width=%u height=%u pitch=%u levels=%u\n", fmt, width,
				     height, pitch, levels);
			}
			size = mip_layout.tiled;
		} else if (three_dimensional)
		{
			Gen5TextureVolumeLayout volume_layout {};
			if (!Gen5GetStandard4KBVolumeTextureLayout(fmt, width, height, depth, pitch, levels, tile, &volume_layout))
			{
				const uint32_t bpe = std::max(1u, ShaderGen5TextureBytesPerElement(fmt));
				size.size          = std::max(4096u, pitch * height * depth * bpe * std::max(1u, levels));
				size.align         = 4096;
			} else
			{
				size = volume_layout.tiled;
			}
		} else if (arrayed_2d && !check_depth_texture)
		{
			Gen5TextureArrayLayout array_layout {};
			if (!Gen5GetTextureArrayLayout(fmt, width, height, pitch, levels, tile, depth, &array_layout))
			{
				EXIT("Unsupported Gen5 2D-array layout: format=%u width=%u height=%u pitch=%u levels=%u tile=%u layers=%u base_array=%u\\n",
				     fmt, width, height, pitch, levels, tile, depth, base_array);
			}
			size.size  = static_cast<uint32_t>(array_layout.tiled_size);
			size.align = array_layout.tiled_slice.align;
		} else if (gen5)
		{
			TileGetTextureSize2(fmt, width, height, pitch, levels, tile, &size, nullptr, nullptr);
		} else
		{
			TileGetTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, &size, nullptr, nullptr);
		}

		if (gen5 && tile == 0u && levels == 1u && !three_dimensional && !arrayed_2d)
		{
			const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(fmt);
			const uint64_t visible_row_bytes = static_cast<uint64_t>(width) * bytes_per_element;
			if (visible_row_bytes <= UINT32_MAX)
			{
				const uint64_t footprint = GuestTextureLayoutGetLinearFootprint(addr, static_cast<uint32_t>(visible_row_bytes), height);
				if (footprint != 0u && footprint <= UINT32_MAX)
				{
					size.size = static_cast<uint32_t>(footprint);
					// A registered tight single-channel linear surface is a native R8
					// image. Broadcast its red plane through the view so shaders see
					// the same value in every component, matching the hardware T#
					// result for narrow sampled resources. The AvPlayer descriptor can
					// carry any legal narrow DST_SEL; the registered layout is the
					// provenance that distinguishes this path from ordinary textures.
					if (fmt == 1u && bytes_per_element == 1u)
					{
						view_swizzle = DstSel(4, 4, 4, 4);
					}
				}
			}
		}

		if (size.size == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: size.size == 0 condition ignored (continuing)\n"); }
		if ((addr & (static_cast<uint64_t>(size.align) - 1u)) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (addr & (static_cast<uint64_t>(size.align) - 1u)) != 0 condition ignored (continuing)\n"); }

		// Opt-in catalog (KYTY_SAMPLE_BIND_CATALOG=/abs/path): unique sample binds
		// for residual investigation. No guest-visible side effects when unset.
		const auto catalog_sample = [gen5, fmt, tile, width, height, pitch, addr, swizzle, view_swizzle, force_degamma, &r](const char* path)
		{
			static const char* catalog_path = std::getenv("KYTY_SAMPLE_BIND_CATALOG");
			if (catalog_path == nullptr || catalog_path[0] == '\0' || !gen5)
			{
				return;
			}
			static std::mutex            catalog_mu;
			static std::set<std::string> catalog_seen;
			char                         line[320];
			std::snprintf(line, sizeof(line),
			              "fmt=%u tile=%u %ux%u pitch=%u swizzle=0x%03x view_swizzle=0x%03x degamma=%u word4=0x%08x type=%u depth=%u base_array=%u path=%s addr=0x%012" PRIx64 "\n",
			              fmt, tile, width, height, pitch, swizzle, view_swizzle, force_degamma ? 1u : 0u, r.fields[4],
			              static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Depth()) + 1u,
			              static_cast<uint32_t>(r.BaseArray5()), path, static_cast<uint64_t>(addr));
			std::lock_guard<std::mutex> lock(catalog_mu);
			if (!catalog_seen.insert(line).second)
			{
				return;
			}
			if (FILE* f = std::fopen(catalog_path, "a"))
			{
				std::fputs(line, f);
				std::fclose(f);
			}
		};

		VulkanImage* tex            = nullptr;
		bool         render_texture = false;
		bool         depth_texture  = false;
		int          view_type      = VulkanImage::VIEW_DEFAULT;

		if (check_depth_texture)
		{
			auto dtex     = FindDepthStencil(buffer, addr, size.size, true);
			if (dtex.IsEmpty() && gen5)
			{
				dtex = FindDepthStencil(buffer, addr, size.size, false);
			}
			depth_texture = !dtex.IsEmpty();
			if (depth_texture)
			{
				if (swizzle != DstSel(4, 4, 4, 4) && swizzle != DstSel(4, 0, 0, 0) &&
				                     swizzle != DstSel(4, 0, 0, 1)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: swizzle != DstSel(4, 4, 4, 4) && swizzle != DstSel(4, 0, 0, 0) && condition ignored (continuing)\n"); }
				if (dtex.At(0)->compressed) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dtex.At(0)->compressed condition ignored (continuing)\n"); }
				size_t alias_index = 0;
				if (dtex.Size() > 1)
				{
					uint64_t   sizes[16]       = {};
					const auto n               = static_cast<size_t>(dtex.Size() < 16 ? dtex.Size() : 16);
					bool       use_guest_bytes = true;
					for (size_t i = 0; i < n; i++)
					{
						if (dtex.At(static_cast<int>(i))->guest_size == 0)
						{
							use_guest_bytes = false;
							break;
						}
					}
					if (use_guest_bytes)
					{
						for (size_t i = 0; i < n; i++)
						{
							sizes[i] = dtex.At(static_cast<int>(i))->guest_size;
						}
						alias_index = PreferGpuMemoryAliasIndex(sizes, n, size.size);
					} else
					{
						for (size_t i = 0; i < n; i++)
						{
							const auto e = dtex.At(static_cast<int>(i))->GetGuestExtent();
							sizes[i]     = static_cast<uint64_t>(e.width) * static_cast<uint64_t>(e.height);
						}
						const uint64_t sample_area = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
						alias_index                = PreferGpuMemoryAliasIndex(sizes, n, sample_area);
					}
				}
				tex = dtex.At(static_cast<int>(alias_index));
			}
		} else
		{
			auto rtex      = FindRenderTexture(buffer, addr, size.size, true);
			render_texture = !rtex.IsEmpty();
			// Exact miss: sample range can sit inside a live RT (Contains) or cover a
			// smaller RT (IsContainedWithin). Falling through to guest-memory tile-27
			// upload then reads empty GPU-owned backing and paints opaque-black props.
			if (!render_texture && gen5)
			{
				rtex           = FindRenderTexture(buffer, addr, size.size, false);
				render_texture = !rtex.IsEmpty();
			}
			// Gen4 Display_2dThin (tile 10) UI often samples a CB whose guest size is
			// width*height*4 while the sample descriptor sizes the Display Thin pad
			// (pitch×align128(height)×4). Exact miss then created an empty RT; allow
			// non-exact alias before guest detile upload.
			if (!render_texture && !gen5 && tile == 10)
			{
				rtex           = FindRenderTexture(buffer, addr, size.size, false);
				render_texture = !rtex.IsEmpty();
			}
			if (render_texture)
			{
				if (swizzle != DstSel(4, 5, 6, 7) && swizzle != DstSel(6, 5, 4, 7) && swizzle != DstSel(7, 6, 5, 4))
				{
					/* [gen5-nonfatal] EXIT("unsupported render texture sampled swizzle: swizzle=0x%03" PRIx32 */
					KYTY_LOG_DEBUG("WARNING: unsupported render texture sampled swizzle (continuing)\n");
				}
				// Multiple non-exact RT aliases are expected under Gen5 nested /
				// same-base parents. Prefer the tightest cover using guest allocation
				// bytes when every match recorded guest_size at create; otherwise
				// fall back to sample/RT pixel area (same units). sample_size=0
				// always picked the smallest RT — including tiny IsContainedWithin
				// children under a large sample — which bound a partial image and
				// left opaque-black prop/character boxes.
				//
				// Format family: ufmt 56 → 8bpc, ufmt 71 → float16. Binding a
				// float lighting RT as an RGBA8 sample produces residual world
				// false-color (cyan props / hot slabs). When every overlapping
				// RT is the wrong family for a known ufmt, reject the alias and
				// fall through to guest-memory / storage upload instead.
				//
				// Extent: among format-compatible RTs, prefer exact sample
				// width×height. Binding a full-screen parent without a crop view
				// leaves horizontal bands and selects the wrong atlas tiles.
				const size_t cand_n       = static_cast<size_t>(rtex.Size() < 16 ? rtex.Size() : 16);
				VkFormat     cand_fmt[16] = {};
				uint32_t     cand_w[16]   = {};
				uint32_t     cand_h[16]   = {};
				for (size_t i = 0; i < cand_n; i++)
				{
					cand_fmt[i]             = rtex.At(static_cast<int>(i))->format;
					const auto guest_extent = rtex.At(static_cast<int>(i))->GetGuestExtent();
					cand_w[i]               = guest_extent.width;
					cand_h[i]               = guest_extent.height;
				}
				int    filtered[16] = {};
				size_t filtered_n   = 0;
				bool   reject_alias = false;
				EXIT_IF(!Gen5PickSampleSurfaceAliases(fmt, width, height, cand_n, cand_fmt, cand_w, cand_h, filtered, &filtered_n,
				                                      &reject_alias));
				if (reject_alias)
				{
					render_texture = false;
				} else
				{
					const bool   use_filter  = filtered_n > 0;
					const size_t n           = use_filter ? filtered_n : static_cast<size_t>(rtex.Size() < 16 ? rtex.Size() : 16);
					size_t       alias_index = 0;
					if (n > 1)
					{
						uint64_t sizes[16]       = {};
						bool     use_guest_bytes = true;
						for (size_t i = 0; i < n; i++)
						{
							const int ri = use_filter ? filtered[i] : static_cast<int>(i);
							if (rtex.At(ri)->guest_size == 0)
							{
								use_guest_bytes = false;
								break;
							}
						}
						if (use_guest_bytes)
						{
							for (size_t i = 0; i < n; i++)
							{
								const int ri = use_filter ? filtered[i] : static_cast<int>(i);
								sizes[i]     = rtex.At(ri)->guest_size;
							}
							alias_index = PreferGpuMemoryAliasIndex(sizes, n, size.size);
						} else
						{
							for (size_t i = 0; i < n; i++)
							{
								const int  ri = use_filter ? filtered[i] : static_cast<int>(i);
								const auto e  = rtex.At(ri)->GetGuestExtent();
								sizes[i]      = static_cast<uint64_t>(e.width) * static_cast<uint64_t>(e.height);
							}
							const uint64_t sample_area = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
							alias_index                = PreferGpuMemoryAliasIndex(sizes, n, sample_area);
						}
					}
					if (use_filter)
					{
						alias_index = static_cast<size_t>(filtered[alias_index]);
					}
					tex = rtex.At(static_cast<int>(alias_index));
					if (swizzle == DstSel(6, 5, 4, 7))
					{
						view_type = VulkanImage::VIEW_BGRA;
					}
				}
				if (swizzle == DstSel(7, 6, 5, 4))
				{
					view_type = VulkanImage::VIEW_ABGR;
				}
			}
			// Live StorageTexture (compute/UAV) can own GPU pixels without ever
			// being a color RT. Prefer that image over tile-27 CPU upload of empty
			// guest memory (opaque-black wall/prop quads). Same format+extent
			// ranking as the RT path so float UAV parents do not paint cyan sprites.
			bool storage_texture = false;
			if (!render_texture && gen5)
			{
				auto stex = FindStorageTexture(buffer, addr, size.size, true);
				if (stex.IsEmpty())
				{
					stex = FindStorageTexture(buffer, addr, size.size, false);
				}
				if (!stex.IsEmpty())
				{
					const size_t cand_n       = static_cast<size_t>(stex.Size() < 16 ? stex.Size() : 16);
					VkFormat     cand_fmt[16] = {};
					uint32_t     cand_w[16]   = {};
					uint32_t     cand_h[16]   = {};
					for (size_t i = 0; i < cand_n; i++)
					{
						cand_fmt[i]             = stex.At(static_cast<int>(i))->format;
						const auto guest_extent = stex.At(static_cast<int>(i))->GetGuestExtent();
						cand_w[i]               = guest_extent.width;
						cand_h[i]               = guest_extent.height;
					}
					int    filtered[16] = {};
					size_t filtered_n   = 0;
					bool   reject_st    = false;
					EXIT_IF(!Gen5PickSampleSurfaceAliases(fmt, width, height, cand_n, cand_fmt, cand_w, cand_h, filtered, &filtered_n,
					                                      &reject_st));
					if (!reject_st)
					{
						storage_texture          = true;
						const bool   use_filter  = filtered_n > 0;
						const size_t n           = use_filter ? filtered_n : cand_n;
						size_t       alias_index = 0;
						if (n > 1)
						{
							uint64_t sizes[16]       = {};
							bool     use_guest_bytes = true;
							for (size_t i = 0; i < n; i++)
							{
								const int ri = use_filter ? filtered[i] : static_cast<int>(i);
								if (stex.At(ri)->guest_size == 0)
								{
									use_guest_bytes = false;
									break;
								}
							}
							if (use_guest_bytes)
							{
								for (size_t i = 0; i < n; i++)
								{
									const int ri = use_filter ? filtered[i] : static_cast<int>(i);
									sizes[i]     = stex.At(ri)->guest_size;
								}
								alias_index = PreferGpuMemoryAliasIndex(sizes, n, size.size);
							} else
							{
								for (size_t i = 0; i < n; i++)
								{
									const int  ri = use_filter ? filtered[i] : static_cast<int>(i);
									const auto e  = stex.At(ri)->GetGuestExtent();
									sizes[i]      = static_cast<uint64_t>(e.width) * static_cast<uint64_t>(e.height);
								}
								const uint64_t sample_area = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
								alias_index                = PreferGpuMemoryAliasIndex(sizes, n, sample_area);
							}
						}
						if (use_filter)
						{
							alias_index = static_cast<size_t>(filtered[alias_index]);
						}
						tex = stex.At(static_cast<int>(alias_index));
					}
				}
			}
			if (gen5)
			{
				const auto backing = State::ResolveGen5SampleBacking(fmt, tile, render_texture || storage_texture);
				if (backing == State::Gen5SampleBacking::Unsupported)
				{
					/* [gen5-nonfatal] EXIT("Gen5 sampled texture has no exact render-target backing and no guest-memory upload: " */
					KYTY_LOG_DEBUG("WARNING: Gen5 sampled texture has no exact render-target backing and no guest-memory upload:  (continuing)\n");
				}
			}
			if (!render_texture && !depth_texture && tex == nullptr && !textures.desc[i].textures2d_without_sampler)
			{
				// VideoOut surfaces are also valid sampled images. Reuse the registered
				// image instead of creating a TextureObject over the same guest range;
				// the latter adds an alias parent that detiles the display buffer during
				// every GPU write-back.
				const auto video_image = VideoOut::VideoOutGetImageForSubmission(addr, buffer);
				if (video_image.image != nullptr && video_image.buffer_size == size.size && video_image.buffer_pitch == pitch &&
				    video_image.image->MatchesGuestExtent(width, height) &&
				    (!gen5 || VulkanGen5SampleFormatMatches(static_cast<uint16_t>(fmt), video_image.image->format)))
				{
					tex = video_image.image;
					if (swizzle == DstSel(6, 5, 4, 7))
					{
						view_type = VulkanImage::VIEW_BGRA;
					} else if (swizzle == DstSel(7, 6, 5, 4))
					{
						view_type = VulkanImage::VIEW_ABGR;
					}
				}
			}
		}

		if (!render_texture && !depth_texture && tex == nullptr)
		{
			if (textures.desc[i].textures2d_without_sampler)
			{
				if (textures.desc[i].usage != ShaderTextureUsage::ReadWrite) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: textures.desc[i].usage != ShaderTextureUsage::ReadWrite condition ignored (continuing)\n"); }

				StorageTextureObject vulkan_texture_info(dfmt, nfmt, fmt, width, height, pitch, base_level, levels, tile, neo, swizzle,
				                                         r.Type(), depth, base_array,
				                                         (storage_seed_skip_mask & (1u << static_cast<uint32_t>(i))) != 0);
				tex = static_cast<StorageTextureVulkanImage*>(
				    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, size.size, vulkan_texture_info));
			} else
			{
				if (textures.desc[i].usage != ShaderTextureUsage::ReadOnly) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: textures.desc[i].usage != ShaderTextureUsage::ReadOnly condition ignored (continuing)\n"); }

				// Tile 10 used to create a GPU-owned RenderTexture with no CPU upload.
				// Display-thin atlases are guest surfaces; that path never detiled
				// them and sampled garbage or smeared glyphs.
				// Fall through to TextureObject, which detiles Display Thin BGRA8.
				EXIT_IF(Gen5SampleMayWriteBackStorageBeforeGuestUpload());
				// Tiled samples under a live RT/ST that was not bound as alias
				// must not detile GPU-owned guest (catalog: 642x362 tile27 guest).
				bool live_cover = false;
				if (gen5 && (tile == 27u || tile == 9u))
				{
					const auto r_cover = FindRenderTexture(buffer, addr, size.size, false);
					const auto s_cover = FindStorageTexture(buffer, addr, size.size, false);
					live_cover         = !r_cover.IsEmpty() || !s_cover.IsEmpty();
				}
				const bool skip_guest = !Gen5SampleMayGuestUploadTiled(tile, fmt, live_cover);
				TextureObject vulkan_texture_info(dfmt, nfmt, fmt, width, height, pitch, base_level, levels, tile, neo, view_swizzle,
				                                  force_degamma, skip_guest, r.Type(), depth, base_array);
				tex = static_cast<TextureVulkanImage*>(
				    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, size.size, vulkan_texture_info));
			}
		}

		if (tex == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: tex == nullptr condition ignored (continuing)\n"); }
		if (const char* dump_texture_bind = std::getenv("KYTY_DUMP_TEXTURE_BIND"); dump_texture_bind != nullptr)
		{
			uint32_t selected_width  = 0;
			uint32_t selected_height = 0;
			if (std::sscanf(dump_texture_bind, "%ux%u", &selected_width, &selected_height) == 2 &&
			    selected_width == static_cast<uint32_t>(width) && selected_height == static_cast<uint32_t>(height))
			{
				static std::atomic_uint texture_bind_logs {0};
				const unsigned         ordinal = texture_bind_logs.fetch_add(1, std::memory_order_relaxed);
				if (ordinal < 256u)
				{
					KYTY_LOG_DEBUG(
					             "KYTY_DUMP_TEXTURE_BIND ordinal=%u index=%d guest_addr=0x%012" PRIx64
					             " guest_format=%u tile=%u size=%ux%u usage=%u host_id=%" PRIu64
					             " host_type=%u vk_format=%u vk_usage=0x%08x layout=%u\n",
					             ordinal, i, static_cast<uint64_t>(addr), fmt, tile, width, height,
					             static_cast<unsigned>(textures.desc[i].usage), tex->memory.unique_id,
					             static_cast<uint32_t>(tex->type), static_cast<uint32_t>(tex->format),
					             static_cast<uint32_t>(tex->usage), static_cast<uint32_t>(tex->layout));
				}
			}
		}
		if (fmt == 75u && width == 29u && height == 30u && tex->format == VK_FORMAT_R32G32B32A32_UINT)
		{
			if (textures.desc[i].textures2d_without_sampler)
			{
				g_dump_bc3_compute_destination = tex;
			} else
			{
				g_dump_bc3_compute_source = tex;
			}
		}
		if (fmt == 173u && width == 116u && height == 120u && tex->format == VK_FORMAT_BC3_UNORM_BLOCK)
		{
			g_dump_bc3_image = tex;
		}
		static const char* bound_dump_spec   = std::getenv("KYTY_DUMP_BOUND_SAMPLE");
		uint32_t           bound_dump_width  = 0;
		uint32_t           bound_dump_height = 0;
		if (bound_dump_spec != nullptr && std::sscanf(bound_dump_spec, "%ux%u", &bound_dump_width, &bound_dump_height) == 2 &&
		    bound_dump_width == static_cast<uint32_t>(width) && bound_dump_height == static_cast<uint32_t>(height))
		{
			char dump_tag[96];
			std::snprintf(dump_tag, sizeof(dump_tag), "bound-addr%012" PRIx64 "-guestfmt%u-hostfmt%u-type%u", static_cast<uint64_t>(addr),
			              fmt, static_cast<uint32_t>(tex->format), static_cast<uint32_t>(tex->type));
			UtilDumpVulkanImageRgba8Png(g_render_ctx->GetGraphicCtx(), tex, "/tmp/kyty-dump-bound-sample", dump_tag);
		}

		if (render_texture)
		{
			catalog_sample("rt");
		} else if (depth_texture)
		{
			catalog_sample("depth");
		} else if (tex != nullptr && tex->type == VulkanImageType::StorageTexture)
		{
			catalog_sample("st");
		} else if (tex != nullptr && tex->type == VulkanImageType::VideoOut)
		{
			catalog_sample("video");
		} else
		{
			catalog_sample("guest");
		}

		if (textures.desc[i].textures2d_without_sampler)
		{
			images_storage[index_storage] = tex;
			images_storage_view[index_storage] =
			    (three_dimensional ? VulkanImage::VIEW_3D : (arrayed_2d ? VulkanImage::VIEW_ARRAY : VulkanImage::VIEW_DEFAULT));
			if (gen5)
			{
				r.UpdateAddress40(index_storage);
			} else
			{
				r.UpdateAddress38(index_storage);
			}
			index_storage++;
		} else
		{
			VulkanImage** sampled_images = images_sampled;
			int*          sampled_views  = images_sampled_view;
			int*          sampled_index  = &index_sampled;
			uint32_t      descriptor_tag = 0u;
			const auto numeric_type = VulkanGen5ImageNumericType(fmt);
			if (numeric_type == GuestImageNumericType::Unsupported || numeric_type == GuestImageNumericType::SignedInteger) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: numeric_type == GuestImageNumericType::Unsupported || numeric_type == GuestImageNumericType::SignedInteger condition ignored (continuing)\n"); }
			if (numeric_type == GuestImageNumericType::UnsignedInteger)
			{
				descriptor_tag |= ShaderTextureResources::UNSIGNED_INTEGER_INDEX_TAG;
				if (split_numeric_types)
				{
					sampled_images = images_sampled_uint;
					sampled_views  = images_sampled_uint_view;
					sampled_index  = &index_sampled_uint;
				}
			}
			if (three_dimensional)
			{
				const bool uint_binding = numeric_type == GuestImageNumericType::UnsignedInteger && split_numeric_types;
				sampled_images          = uint_binding ? images_sampled_3d_uint : images_sampled_3d;
				sampled_views           = uint_binding ? images_sampled_3d_uint_view : images_sampled_3d_view;
				sampled_index           = uint_binding ? &index_sampled_3d_uint : &index_sampled_3d;
				descriptor_tag |= ShaderTextureResources::THREE_DIMENSIONAL_INDEX_TAG;
			} else if (arrayed_2d)
			{
				const bool uint_binding = numeric_type == GuestImageNumericType::UnsignedInteger && split_numeric_types;
				sampled_images          = uint_binding ? images_sampled_array_uint : images_sampled_array;
				sampled_views           = uint_binding ? images_sampled_array_uint_view : images_sampled_array_view;
				sampled_index           = uint_binding ? &index_sampled_array_uint : &index_sampled_array;
				descriptor_tag |= ShaderTextureResources::TWO_DIMENSIONAL_ARRAY_INDEX_TAG;
			}
			sampled_images[*sampled_index] = tex;
			if (three_dimensional && (depth_texture || view_type != VulkanImage::VIEW_DEFAULT)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: three_dimensional && (depth_texture || view_type != VulkanImage::VIEW_DEFAULT) condition ignored (continuing)\n"); }
			if (arrayed_2d && !depth_texture && view_type != VulkanImage::VIEW_DEFAULT) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: arrayed_2d && !depth_texture && view_type != VulkanImage::VIEW_DEFAULT condition ignored (continuing)\n"); }
			sampled_views[*sampled_index] =
			    (three_dimensional
			         ? VulkanImage::VIEW_3D
			         : (depth_texture ? (arrayed_2d ? VulkanImage::VIEW_DEPTH_TEXTURE_ARRAY : VulkanImage::VIEW_DEPTH_TEXTURE)
			                          : (arrayed_2d ? VulkanImage::VIEW_ARRAY : view_type)));
			if (std::getenv("KYTY_DUMP_VIDEO_BIND") != nullptr && gen5 && (fmt == 1u || fmt == 14u))
			{
				const uint64_t bind_addr = static_cast<uint64_t>(addr);
				const bool     video_va  = bind_addr >= 0x6c000000ull && bind_addr <= 0x6e000000ull;
				if (video_va)
				{
					static std::atomic_uint bind_dump_count[2] {{0}, {0}};
					const unsigned         fmt_index = fmt == 14u ? 1u : 0u;
					const auto             dump_index = bind_dump_count[fmt_index].fetch_add(1, std::memory_order_relaxed);
					if (dump_index < 1024u)
					{
						const uint32_t descriptor_index = static_cast<uint32_t>(*sampled_index) | descriptor_tag;
						KYTY_LOG_DEBUG( "KYTY_DUMP_VIDEO_BIND index=%u fmt=%u addr=0x%012" PRIx64 " sampled_index=%d descriptor=0x%08x image=%p id=%" PRIu64 " format=%d view=%d swizzle=0x%03x extent=%ux%u type=%d\n",
						             dump_index, fmt, bind_addr, *sampled_index, descriptor_index, static_cast<void*>(tex), tex->memory.unique_id,
						             static_cast<int>(tex->format), sampled_views[*sampled_index], view_swizzle, width, height, static_cast<int>(tex->type));
					}
				}
			}
			if (*sampled_index == 0)
			{
				if (three_dimensional)
				{
					if (numeric_type == GuestImageNumericType::UnsignedInteger && split_numeric_types)
					{
						sampled_3d_uint_padding_image = tex;
						sampled_3d_uint_padding_view  = sampled_views[*sampled_index];
					} else
					{
						sampled_3d_padding_image = tex;
						sampled_3d_padding_view  = sampled_views[*sampled_index];
					}
				} else if (arrayed_2d)
				{
					if (numeric_type == GuestImageNumericType::UnsignedInteger && split_numeric_types)
					{
						sampled_2d_array_uint_padding_image = tex;
						sampled_2d_array_uint_padding_view  = sampled_views[*sampled_index];
					} else
					{
						sampled_2d_array_padding_image = tex;
						sampled_2d_array_padding_view  = sampled_views[*sampled_index];
					}
				} else
				{
					if (numeric_type == GuestImageNumericType::UnsignedInteger && split_numeric_types)
					{
						sampled_2d_uint_padding_image = tex;
						sampled_2d_uint_padding_view  = sampled_views[*sampled_index];
					} else
					{
						sampled_2d_padding_image = tex;
						sampled_2d_padding_view  = sampled_views[*sampled_index];
					}
				}
			}
			if (gen5)
			{
				const uint32_t descriptor_index = static_cast<uint32_t>(*sampled_index) | descriptor_tag;
				r.UpdateAddress40(descriptor_index);
			} else
			{
				r.UpdateAddress38(*sampled_index);
			}
			(*sampled_index)++;
		}

		if (gen5 && !textures.desc[i].textures2d_without_sampler)
		{
			const uint32_t descriptor_shape = r.fields[0] & ~ShaderTextureResources::DESCRIPTOR_INDEX_MASK;
			if ((r.fields[1] & 0xffu) != 0u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (r.fields[1] & 0xffu) != 0u condition ignored (continuing)\n"); }
			if (descriptor_shape != 0u &&
			                     descriptor_shape != ShaderTextureResources::UNSIGNED_INTEGER_INDEX_TAG &&
			                     descriptor_shape != ShaderTextureResources::TWO_DIMENSIONAL_ARRAY_INDEX_TAG &&
			                     descriptor_shape != ShaderTextureResources::THREE_DIMENSIONAL_INDEX_TAG &&
			                     descriptor_shape != (ShaderTextureResources::UNSIGNED_INTEGER_INDEX_TAG |
			                                          ShaderTextureResources::TWO_DIMENSIONAL_ARRAY_INDEX_TAG) &&
			                     descriptor_shape != (ShaderTextureResources::UNSIGNED_INTEGER_INDEX_TAG |
			                                          ShaderTextureResources::THREE_DIMENSIONAL_INDEX_TAG)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: descriptor_shape != 0u && condition ignored (continuing)\n"); }
		} else
		{
			if (((gen5 ? r.Base40() : r.Base38()) >> 32u) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ((gen5 ? r.Base40() : r.Base38()) >> 32u) != 0 condition ignored (continuing)\n"); }
		}
		if (bound_dump_spec != nullptr && std::sscanf(bound_dump_spec, "%ux%u", &bound_dump_width, &bound_dump_height) == 2 &&
		    bound_dump_width == static_cast<uint32_t>(width) && bound_dump_height == static_cast<uint32_t>(height))
		{
			KYTY_LOG_DEBUG(
			             "KYTY_DUMP_BOUND_SAMPLE addr=0x%012" PRIx64 " id=%" PRIu64 " type=%u format=%u layout=%u "
			             "index=%d descriptor=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
			             static_cast<uint64_t>(addr), tex->memory.unique_id, static_cast<unsigned>(tex->type),
			             static_cast<unsigned>(tex->format), static_cast<unsigned>(tex->layout),
			             (three_dimensional ? index_sampled_3d : (arrayed_2d ? index_sampled_array : index_sampled)) - 1, r.fields[0], r.fields[1], r.fields[2], r.fields[3],
			             r.fields[4], r.fields[5], r.fields[6], r.fields[7]);
		}

		(*sgprs)[0] = r.fields[0];
		(*sgprs)[1] = r.fields[1];
		(*sgprs)[2] = r.fields[2];
		(*sgprs)[3] = r.fields[3];
		(*sgprs)[4] = r.fields[4];
		(*sgprs)[5] = r.fields[5];
		(*sgprs)[6] = r.fields[6];
		(*sgprs)[7] = r.fields[7];

		(*sgprs) += 8;
	}

	const auto pad_descriptors = [](VulkanImage** images, int* views, int populated, int required, VulkanImage* padding_image,
	                                int padding_view)
	{
		EXIT_IF(populated < 0 || populated > required);
		if (populated == 0)
		{
			return;
		}
		EXIT_IF(padding_image == nullptr);
		for (int i = populated; i < required; ++i)
		{
			images[i] = padding_image;
			views[i]  = padding_view;
		}
	};
	pad_descriptors(images_sampled, images_sampled_view, index_sampled, textures.textures2d_sampled_num, sampled_2d_padding_image,
	                sampled_2d_padding_view);
	pad_descriptors(images_sampled_array, images_sampled_array_view, index_sampled_array, textures.textures2d_array_sampled_num,
	                sampled_2d_array_padding_image, sampled_2d_array_padding_view);
	pad_descriptors(images_sampled_3d, images_sampled_3d_view, index_sampled_3d, textures.textures3d_sampled_num,
	                sampled_3d_padding_image, sampled_3d_padding_view);
	pad_descriptors(images_sampled_uint, images_sampled_uint_view, index_sampled_uint, textures.textures2d_sampled_num,
	                sampled_2d_uint_padding_image, sampled_2d_uint_padding_view);
	pad_descriptors(images_sampled_array_uint, images_sampled_array_uint_view, index_sampled_array_uint,
	                textures.textures2d_array_sampled_num, sampled_2d_array_uint_padding_image, sampled_2d_array_uint_padding_view);
	pad_descriptors(images_sampled_3d_uint, images_sampled_3d_uint_view, index_sampled_3d_uint, textures.textures3d_sampled_num,
	                sampled_3d_uint_padding_image, sampled_3d_uint_padding_view);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void PrepareSamplers(const ShaderSamplerResources& samplers, uint64_t* sampler_ids, uint32_t** sgprs)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(sampler_ids == nullptr);
	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	bool gen5 = Config::IsNextGen();

	for (int i = 0; i < samplers.samplers_num; i++)
	{
		auto r = samplers.samplers[i];

		// EXIT_NOT_IMPLEMENTED(r.ClampX() != 0);
		// EXIT_NOT_IMPLEMENTED(r.ClampY() != 0);
		// EXIT_NOT_IMPLEMENTED(r.ClampZ() != 0);
		// EXIT_NOT_IMPLEMENTED(r.MaxAnisoRatio() != 0);
		// Regular image sampling uses a non-comparison Vulkan sampler even when
		// the descriptor retains a depth comparison function. The pipeline's
		// image instruction selects comparison semantics.
		// ForceUnormCoords is materialized in SamplerCache with Vulkan's
		// unnormalized-coordinate restrictions.
		// Vulkan exposes no anisotropic threshold; preserve filter mapping and MaxAnisoRatio.
		if (!gen5 && r.McCoordTrunc() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !gen5 && r.McCoordTrunc() != false condition ignored (continuing)\n"); }
		// ForceDegamma / SkipDegamma are resolved in ShouldForceGen5Degamma and
		// VulkanResolveGuestImageFormat (RGBA8 → sRGB only when force && !skip).
		// Both flags are legal guest sampler state; do not EXIT on them.
		if (gen5 && r.PointPreclamp() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: gen5 && r.PointPreclamp() != false condition ignored (continuing)\n"); }
		if (gen5 && r.AnisoOverride() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: gen5 && r.AnisoOverride() != false condition ignored (continuing)\n"); }
		if (gen5 && r.BlendZeroPrt() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: gen5 && r.BlendZeroPrt() != false condition ignored (continuing)\n"); }
		if (r.AnisoBias() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.AnisoBias() != 0 condition ignored (continuing)\n"); }
		if (r.TruncCoord() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.TruncCoord() != false condition ignored (continuing)\n"); }
		if (r.DisableCubeWrap() != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.DisableCubeWrap() != false condition ignored (continuing)\n"); }
		if (r.FilterMode() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.FilterMode() != 0 condition ignored (continuing)\n"); }
		// EXIT_NOT_IMPLEMENTED(r.MinLod() != 0);
		// EXIT_NOT_IMPLEMENTED(r.MaxLod() != 4095);
		// PERF_MIP and PERF_Z are guest texture-unit performance hints. Vulkan
		// exposes no corresponding sampler state; sampling semantics are carried
		// by the filter, LOD and address fields handled below.
		// EXIT_NOT_IMPLEMENTED(r.LodBias() != 0);
		if (r.LodBiasSec() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.LodBiasSec() != 0 condition ignored (continuing)\n"); }
		// EXIT_NOT_IMPLEMENTED(r.XyMagFilter() != 1);
		// EXIT_NOT_IMPLEMENTED(r.XyMinFilter() != 1);
		// Vulkan has no separate Z texture filter in VkSampler; 2D sampled images
		// are controlled by XY min/mag and mip filtering below.
		// EXIT_NOT_IMPLEMENTED(r.MipFilter() != 0 && r.MipFilter() != 2);
		if (r.BorderColorPtr() != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.BorderColorPtr() != 0 condition ignored (continuing)\n"); }
		// Types 0 through 2 are fixed transparent-black, opaque-black, and
		// opaque-white values. SamplerCache translates those values directly to
		// Vulkan. Type 3 requires a guest border-color table, which is a distinct
		// resource contract and cannot be represented by a fixed VkBorderColor.
		if (r.BorderColorType() == 3) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.BorderColorType() == 3 condition ignored (continuing)\n"); }

		sampler_ids[i] = g_render_ctx->GetSamplerCache()->GetSamplerId(r);

		r.UpdateIndex(i);

		(*sgprs)[0] = r.fields[0];
		(*sgprs)[1] = r.fields[1];
		(*sgprs)[2] = r.fields[2];
		(*sgprs)[3] = r.fields[3];

		(*sgprs) += 4;
	}
}

static void PrepareGdsPointers(const ShaderGdsResources& gds_pointers, uint32_t** sgprs)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	for (int i = 0; i < gds_pointers.pointers_num; i++)
	{
		auto r = gds_pointers.pointers[i];

		if (r.Size() != 4) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r.Size() != 4 condition ignored (continuing)\n"); }

		(*sgprs)[i] = r.field;
	}

	if (gds_pointers.pointers_num > 0)
	{
		(*sgprs) += static_cast<ptrdiff_t>(4 * ((gds_pointers.pointers_num - 1) / 4 + 1));
	}
}

static void PrepareDirectSgprs(const ShaderDirectSgprsResources& direct_sgprs, uint32_t** sgprs)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	for (int i = 0; i < direct_sgprs.sgprs_num; i++)
	{
		auto r = direct_sgprs.sgprs[i];

		(*sgprs)[i] = r.field;
	}

	if (direct_sgprs.sgprs_num > 0)
	{
		(*sgprs) += static_cast<ptrdiff_t>(4 * ((direct_sgprs.sgprs_num - 1) / 4 + 1));
	}
}

void BindDescriptors(uint64_t submit_id, CommandBuffer* buffer, VkPipelineBindPoint pipeline_bind_point, VkPipelineLayout layout,
                     const ShaderBindResources& bind, VkShaderStageFlags vk_stage, DescriptorCache::Stage stage,
                     uint32_t storage_seed_skip_mask)
{
	KYTY_PROFILER_FUNCTION();

	if (bind.push_constant_size > 0)
	{
		const bool record_draw_timing = pipeline_bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
		if (!bind.vsharp_uniform_buffer && bind.push_constant_size > DescriptorCache::PUSH_CONSTANTS_MAX * 4) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !bind.vsharp_uniform_buffer && bind.push_constant_size > DescriptorCache::PUSH_CONSTANTS_MAX * 4 condition ignored (continuing)\n"); }
		if (bind.push_constant_size > DescriptorCache::METADATA_DWORDS_MAX * 4) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: bind.push_constant_size > DescriptorCache::METADATA_DWORDS_MAX * 4 condition ignored (continuing)\n"); }
		if (bind.storage_buffers.buffers_num > DescriptorCache::BUFFERS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: bind.storage_buffers.buffers_num > DescriptorCache::BUFFERS_MAX condition ignored (continuing)\n"); }
		if (
		    (bind.textures2D.textures2d_storage_num > DescriptorCache::TEXTURES_STORAGE_MAX) ||
		    (bind.textures2D.textures2d_sampled_num + bind.textures2D.textures2d_array_sampled_num +
		     bind.textures2D.textures3d_sampled_num > DescriptorCache::TEXTURES_SAMPLED_MAX)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (bind.textures2D.textures2d_storage_num > DescriptorCache::TEXTURES_STORAGE_MAX) condition ignored (continuing)\n"); }
		if (bind.textures2D.textures2d_storage_num + bind.textures2D.textures2d_sampled_num +
		                         bind.textures2D.textures2d_array_sampled_num + bind.textures2D.textures3d_sampled_num !=
		                     bind.textures2D.textures_num) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: bind.textures2D.textures2d_storage_num + bind.textures2D.textures2d_sampled_num  condition ignored (continuing)\n"); }
		if (bind.samplers.samplers_num > DescriptorCache::SAMPLERS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: bind.samplers.samplers_num > DescriptorCache::SAMPLERS_MAX condition ignored (continuing)\n"); }

		bool need_descriptor = false;

		VulkanBuffer* storage_buffers[DescriptorCache::BUFFERS_MAX] = {};
		VulkanImage*  textures2d_sampled[DescriptorCache::TEXTURES_SAMPLED_MAX] = {};
		int           textures2d_sampled_view[DescriptorCache::TEXTURES_SAMPLED_MAX];
		VulkanImage*  textures2d_array_sampled[DescriptorCache::TEXTURES_SAMPLED_MAX] = {};
		int           textures2d_array_sampled_view[DescriptorCache::TEXTURES_SAMPLED_MAX];
		VulkanImage*  textures3d_sampled[DescriptorCache::TEXTURES_SAMPLED_MAX] = {};
		int           textures3d_sampled_view[DescriptorCache::TEXTURES_SAMPLED_MAX];
		VulkanImage*  textures2d_sampled_uint[DescriptorCache::TEXTURES_SAMPLED_MAX] = {};
		int           textures2d_sampled_uint_view[DescriptorCache::TEXTURES_SAMPLED_MAX];
		VulkanImage*  textures2d_array_sampled_uint[DescriptorCache::TEXTURES_SAMPLED_MAX] = {};
		int           textures2d_array_sampled_uint_view[DescriptorCache::TEXTURES_SAMPLED_MAX];
		VulkanImage*  textures3d_sampled_uint[DescriptorCache::TEXTURES_SAMPLED_MAX] = {};
		int           textures3d_sampled_uint_view[DescriptorCache::TEXTURES_SAMPLED_MAX];
		VulkanImage*  textures2d_storage[DescriptorCache::TEXTURES_STORAGE_MAX] = {};
		int           textures2d_storage_view[DescriptorCache::TEXTURES_STORAGE_MAX];
		uint64_t      samplers[DescriptorCache::SAMPLERS_MAX];
		uint32_t      sgprs[DescriptorCache::METADATA_DWORDS_MAX] = {};

		uint32_t* sgprs_ptr = sgprs;

		VulkanBuffer* gds_buffer    = nullptr;
		VulkanBuffer* vsharp_buffer = nullptr;

		if (bind.storage_buffers.buffers_num > 0)
		{
			const auto stage_start = BindingStageClock::now();
			PrepareStorageBuffers(submit_id, buffer, bind.storage_buffers, storage_buffers, &sgprs_ptr);
			if (record_draw_timing) { DebugStatsRecordDrawDescriptorStorage(BindingStageElapsedNs(stage_start)); }
			need_descriptor = true;
		}
		if (bind.textures2D.textures_num > 0)
		{
			const auto stage_start = BindingStageClock::now();
			PrepareTextures(submit_id, buffer, bind.textures2D, bind.samplers, textures2d_sampled, textures2d_storage,
			                textures2d_sampled_view, textures2d_array_sampled, textures2d_array_sampled_view, textures3d_sampled,
			                textures3d_sampled_view, textures2d_sampled_uint, textures2d_sampled_uint_view,
			                textures2d_array_sampled_uint, textures2d_array_sampled_uint_view, textures3d_sampled_uint,
			                textures3d_sampled_uint_view, textures2d_storage_view, storage_seed_skip_mask, &sgprs_ptr);
			if (record_draw_timing) { DebugStatsRecordDrawDescriptorTexture(BindingStageElapsedNs(stage_start)); }
			need_descriptor = true;
		}
		if (bind.samplers.samplers_num > 0)
		{
			const auto stage_start = BindingStageClock::now();
			PrepareSamplers(bind.samplers, samplers, &sgprs_ptr);
			if (record_draw_timing) { DebugStatsRecordDrawDescriptorSampler(BindingStageElapsedNs(stage_start)); }
			need_descriptor = true;
		}
		const auto finalize_start = BindingStageClock::now();
		if (bind.gds_pointers.pointers_num > 0)
		{
			PrepareGdsPointers(bind.gds_pointers, &sgprs_ptr);
			gds_buffer      = g_render_ctx->GetGdsBuffer()->GetBuffer(g_render_ctx->GetGraphicCtx());
			need_descriptor = true;
		}
		if (bind.direct_sgprs.sgprs_num > 0)
		{
			PrepareDirectSgprs(bind.direct_sgprs, &sgprs_ptr);
		}

		EXIT_IF(bind.push_constant_size != (sgprs_ptr - sgprs) * 4);
		if (bind.vsharp_uniform_buffer)
		{
			// Spill path for push constants > PORTABLE_PUSH_CONSTANT_BYTES. Must
			// produce a real VkBuffer; DescriptorCache EXIT_IFs on null vsharp.
			vsharp_buffer = buffer->UploadTransientBuffer(sgprs, bind.push_constant_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
			if (vsharp_buffer == nullptr)
			{
				// Transient pool exhausted even after larger-entry reuse. Prefer a
				// second attempt with STORAGE|UNIFORM so a free multi-usage slab can
				// serve (still host-visible). If that also fails, hard-fail with size
				// so the next session can raise MaxEntries rather than null-deref.
				vsharp_buffer = buffer->UploadTransientBuffer(sgprs, bind.push_constant_size,
				                                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			}
			if (vsharp_buffer == nullptr)
			{
				EXIT("vsharp uniform spill failed: push_constant_size=%u (transient pool exhausted)\n", bind.push_constant_size);
			}
			need_descriptor = true;
		}

		auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

		if (bind.textures2D.textures_num > 0)
		{
			for (int i = 0; i < bind.textures2D.textures2d_storage_num; i++)
			{
				const auto* storage_image = textures2d_storage[i];
				EXIT_IF(storage_image == nullptr);
				if ((storage_image->usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0)
				{
					EXIT("storage image binding requires VK_IMAGE_USAGE_STORAGE_BIT: index=%d image_id=%" PRIu64
					     " type=%u format=%u usage=0x%08x layout=%u\n",
					     i, storage_image->memory.unique_id, static_cast<uint32_t>(storage_image->type),
					     static_cast<uint32_t>(storage_image->format), static_cast<uint32_t>(storage_image->usage),
					     static_cast<uint32_t>(storage_image->layout));
				}
			}

			const auto validate_sampled_storage_aliases = [](VulkanImage* const* sampled_images, int sampled_count,
			                                                 VulkanImage* const* storage_images, int storage_count,
			                                                 const char* sampled_kind)
			{
				for (int storage_index = 0; storage_index < storage_count; storage_index++)
				{
					const auto* storage_image = storage_images[storage_index];
					for (int sampled_index = 0; sampled_index < sampled_count; sampled_index++)
					{
						const auto* sampled_image = sampled_images[sampled_index];
						if (sampled_image == nullptr)
						{
							continue;
						}
						if (sampled_image == storage_image || sampled_image->image == storage_image->image)
						{
							EXIT("image cannot be sampled in SHADER_READ_ONLY and bound as storage in GENERAL in the same descriptor bind: "
							     "storage_index=%d sampled_kind=%s sampled_index=%d image_id=%" PRIu64 "\n",
							     storage_index, sampled_kind, sampled_index, storage_image->memory.unique_id);
						}
					}
				}
			};
			validate_sampled_storage_aliases(textures2d_sampled, DescriptorCache::TEXTURES_SAMPLED_MAX, textures2d_storage,
			                                    bind.textures2D.textures2d_storage_num, "2d");
			validate_sampled_storage_aliases(textures2d_array_sampled, DescriptorCache::TEXTURES_SAMPLED_MAX, textures2d_storage,
			                                    bind.textures2D.textures2d_storage_num, "2d_array");
			validate_sampled_storage_aliases(textures3d_sampled, DescriptorCache::TEXTURES_SAMPLED_MAX, textures2d_storage,
			                                    bind.textures2D.textures2d_storage_num, "3d");
			validate_sampled_storage_aliases(textures2d_sampled_uint, DescriptorCache::TEXTURES_SAMPLED_MAX,
			                                    textures2d_storage, bind.textures2D.textures2d_storage_num, "2d_uint");
			validate_sampled_storage_aliases(textures2d_array_sampled_uint, DescriptorCache::TEXTURES_SAMPLED_MAX,
			                                    textures2d_storage, bind.textures2D.textures2d_storage_num, "2d_array_uint");
			validate_sampled_storage_aliases(textures3d_sampled_uint, DescriptorCache::TEXTURES_SAMPLED_MAX,
			                                    textures2d_storage, bind.textures2D.textures2d_storage_num, "3d_uint");

			const auto transition_sampled_images = [&vk_buffer](VulkanImage** images, int count)
			{
				for (int i = 0; i < count; i++)
				{
					if (images[i] == nullptr)
					{
						continue;
					}
					if (images[i]->type == VulkanImageType::DepthStencil)
					{
						GraphicsRenderDepthStencilBarrier(vk_buffer, images[i]);
					} else if (images[i]->type == VulkanImageType::RenderTexture || images[i]->type == VulkanImageType::VideoOut ||
					           images[i]->type == VulkanImageType::StorageTexture)
					{
						// Storage images written by compute also need SHADER_READ before sampling.
						GraphicsRenderRenderTextureBarrier(vk_buffer, images[i]);
					}
				}
			};
			transition_sampled_images(textures2d_sampled, bind.textures2D.textures2d_sampled_num);
			transition_sampled_images(textures2d_array_sampled, bind.textures2D.textures2d_array_sampled_num);
			transition_sampled_images(textures3d_sampled, bind.textures2D.textures3d_sampled_num);
			transition_sampled_images(textures2d_sampled_uint, bind.textures2D.textures2d_sampled_uint_num);
			transition_sampled_images(textures2d_array_sampled_uint, bind.textures2D.textures2d_array_sampled_uint_num);
			transition_sampled_images(textures3d_sampled_uint, bind.textures2D.textures3d_sampled_uint_num);
			for (int i = 0; i < bind.textures2D.textures2d_storage_num; i++)
			{
				auto* storage_image = textures2d_storage[i];
				const auto old_layout = storage_image->layout;
				GraphicsRenderStorageImageBarrier(vk_buffer, storage_image);
				if (std::getenv("KYTY_DUMP_STORAGE_IMAGE") != nullptr)
				{
					static std::atomic_uint storage_image_logs {0};
					const unsigned         ordinal = storage_image_logs.fetch_add(1, std::memory_order_relaxed);
					if (ordinal < 256u)
					{
						KYTY_LOG_DEBUG(
						             "KYTY_DUMP_STORAGE_IMAGE ordinal=%u stage=%d index=%d host_id=%" PRIu64
						             " host_type=%u vk_format=%u vk_usage=0x%08x old_layout=%u new_layout=%u\n",
						             ordinal, static_cast<int>(stage), i, storage_image->memory.unique_id,
						             static_cast<uint32_t>(storage_image->type), static_cast<uint32_t>(storage_image->format),
						             static_cast<uint32_t>(storage_image->usage), static_cast<uint32_t>(old_layout),
						             static_cast<uint32_t>(storage_image->layout));
					}
				}
			}
		}

		if (need_descriptor)
		{
			auto* descriptor_set = g_render_ctx->GetDescriptorCache()->GetDescriptor(
			    stage, storage_buffers, textures2d_sampled, textures2d_sampled_view, textures2d_array_sampled,
			    textures2d_array_sampled_view, textures3d_sampled, textures3d_sampled_view, textures2d_sampled_uint,
			    textures2d_sampled_uint_view, textures2d_array_sampled_uint, textures2d_array_sampled_uint_view,
			    textures3d_sampled_uint, textures3d_sampled_uint_view, textures2d_storage,
			    textures2d_storage_view, samplers, &gds_buffer, vsharp_buffer, bind);

			EXIT_IF(descriptor_set == nullptr);

			vkCmdBindDescriptorSets(vk_buffer, pipeline_bind_point, layout, bind.descriptor_set_slot, 1, &descriptor_set->set, 0, nullptr);
		}
		if (!bind.vsharp_uniform_buffer)
		{
			vkCmdPushConstants(vk_buffer, layout, vk_stage, bind.push_constant_offset, bind.push_constant_size, sgprs);
		}
		if (record_draw_timing) { DebugStatsRecordDrawDescriptorFinalize(BindingStageElapsedNs(finalize_start)); }
	}
}

static void VulkanCmdSetColorWriteEnableEXT(GraphicContext* ctx, VkCommandBuffer command_buffer, uint32_t attachment_count,
                                            const VkBool32* p_color_write_enables)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->instance == nullptr);

	static auto func =
	    reinterpret_cast<PFN_vkCmdSetColorWriteEnableEXT>(vkGetInstanceProcAddr(ctx->instance, "vkCmdSetColorWriteEnableEXT"));

	if (func != nullptr)
	{
		func(command_buffer, attachment_count, p_color_write_enables);
	} else
	{
		KYTY_LOG_DEBUG("WARNING: vkCmdSetColorWriteEnableEXT not present, skipping\n");
	}
}

void SetDynamicParams(VkCommandBuffer vk_buffer, VulkanPipeline* pipeline)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(pipeline == nullptr);
	EXIT_IF(pipeline->static_params == nullptr);
	EXIT_IF(pipeline->dynamic_params == nullptr);

	if (pipeline->dynamic_params->vk_dynamic_state_line_width)
	{
		vkCmdSetLineWidth(vk_buffer, pipeline->dynamic_params->line_width);
	}

	if (pipeline->static_params->stencil_test_enable)
	{
		if (pipeline->dynamic_params->vk_dynamic_state_stencil_compare_mask)
		{
			vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT, pipeline->dynamic_params->stencil_front.compareMask);
			vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT, pipeline->dynamic_params->stencil_back.compareMask);
		}
		if (pipeline->dynamic_params->vk_dynamic_state_stencil_write_mask)
		{
			vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT, pipeline->dynamic_params->stencil_front.writeMask);
			vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT, pipeline->dynamic_params->stencil_back.writeMask);
		}
		if (pipeline->dynamic_params->vk_dynamic_state_stencil_reference)
		{
			vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_FRONT_BIT, pipeline->dynamic_params->stencil_front.reference);
			vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_BACK_BIT, pipeline->dynamic_params->stencil_back.reference);
		}
	}

	if (pipeline->dynamic_params->vk_dynamic_state_color_write_enable_ext && g_render_ctx->GetGraphicCtx()->color_write_enable_supported)
	{
		VkBool32 enable = (pipeline->dynamic_params->color_write_enable ? VK_TRUE : VK_FALSE);
		VulkanCmdSetColorWriteEnableEXT(g_render_ctx->GetGraphicCtx(), vk_buffer, 1, &enable);
	}

	if (pipeline->dynamic_params->vk_dynamic_state_viewport)
	{
		const bool unrestricted = g_render_ctx->GetGraphicCtx()->depth_range_unrestricted_supported;
		const auto depth_range =
		    State::ResolveViewportDepth(pipeline->dynamic_params->viewport_scale[2], pipeline->dynamic_params->viewport_offset[2],
		                                pipeline->static_params->dx_clip_space, unrestricted,
		                                pipeline->dynamic_params->viewport_depth_clamp[0],
		                                pipeline->dynamic_params->viewport_depth_clamp[1]);
		const auto xy = State::ResolveViewportXy(pipeline->dynamic_params->viewport_scale[0], pipeline->dynamic_params->viewport_offset[0],
		                                         pipeline->dynamic_params->viewport_scale[1], pipeline->dynamic_params->viewport_offset[1]);
		VkViewport viewport {};
		viewport.x        = xy.x;
		viewport.y        = xy.y;
		viewport.width    = xy.width;
		viewport.height   = xy.height;
		viewport.minDepth = depth_range.min_depth;
		viewport.maxDepth = depth_range.max_depth;
		vkCmdSetViewport(vk_buffer, 0, 1, &viewport);
	}

	if (pipeline->dynamic_params->vk_dynamic_state_scissor)
	{
		// Scissor must stay inside the framebuffer. Guest scissor often covers the
		// full color target even when the FB was shrunk by a mismatched depth size.
		int32_t left   = pipeline->dynamic_params->scissor_ltrb[0];
		int32_t top    = pipeline->dynamic_params->scissor_ltrb[1];
		int32_t right  = pipeline->dynamic_params->scissor_ltrb[2];
		int32_t bottom = pipeline->dynamic_params->scissor_ltrb[3];
		if (right < left)
		{
			std::swap(left, right);
		}
		if (bottom < top)
		{
			std::swap(top, bottom);
		}
		const int32_t fb_w = static_cast<int32_t>(pipeline->framebuffer_extent.width);
		const int32_t fb_h = static_cast<int32_t>(pipeline->framebuffer_extent.height);
		if (fb_w > 0 && fb_h > 0)
		{
			left   = std::max(0, std::min(left, fb_w));
			top    = std::max(0, std::min(top, fb_h));
			right  = std::max(left, std::min(right, fb_w));
			bottom = std::max(top, std::min(bottom, fb_h));
		}
		VkRect2D scissor {};
		scissor.offset = {left, top};
		scissor.extent = {static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)};
		vkCmdSetScissor(vk_buffer, 0, 1, &scissor);
	}

	if (pipeline->dynamic_params->vk_dynamic_state_blend_constants)
	{
		const float blend_constants[4] = {pipeline->dynamic_params->blend_color_red, pipeline->dynamic_params->blend_color_green,
		                                  pipeline->dynamic_params->blend_color_blue, pipeline->dynamic_params->blend_color_alpha};
		vkCmdSetBlendConstants(vk_buffer, blend_constants);
	}

	if (pipeline->static_params->depth_bias_enable && pipeline->dynamic_params->vk_dynamic_state_depth_bias)
	{
		vkCmdSetDepthBias(vk_buffer, pipeline->dynamic_params->depth_bias_constant_factor, pipeline->dynamic_params->depth_bias_clamp,
		                  pipeline->dynamic_params->depth_bias_slope_factor);
	}
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
