#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/DbgAssert.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool ShaderIsGen5FourComponent32BitBufferFormat(uint8_t format)
{
	// GFX10+ unified formats 75, 76 and 77 are 32_32_32_32 with UINT,
	// SINT and FLOAT number interpretation respectively.
	return format >= 75 && format <= 77;
}

bool ShaderIsGen5SingleComponent32BitBufferFormat(uint8_t format)
{
	// GFX10+ unified formats 20 and 22 represent scalar 32-bit resource views.
	return format == 20 || format == 22;
}

bool ShaderRawStorageDescriptorSupported(const ShaderBufferResource& resource)
{
	// Raw BUFFER_* instructions use the descriptor stride as a byte count. The
	// address equation operates on DWORDs, so a non-zero but byte-misaligned
	// stride cannot describe a valid raw record.
	return resource.Stride() != 0 && (resource.Stride() & 3u) == 0u;
}

bool ShaderGen5RawDescriptorAlwaysOutOfBounds(const ShaderBufferResource& resource)
{
	// An empty descriptor has no addressable raw transfer. The descriptor mode
	// can choose a bounds equation, but it cannot create a record.
	return resource.NumRecords() == 0u;
}

bool ShaderGen5SBufferDescriptorAlwaysOutOfBounds(const ShaderBufferResource& resource)
{
	// Scalar-buffer loads need at least one declared record or byte. An empty
	// descriptor therefore makes every scalar dword load out of range.
	return resource.NumRecords() == 0u;
}

bool ShaderGen5SampledTextureMetadataRequiresDcc(const ShaderTextureResource& resource)
{
	const bool compression_metadata = resource.MetaPipeAligned() || resource.WriteCompress() || resource.MetaCompress() ||
	                                  resource.DccAlphaPos() || resource.DccColorTransf();
	return resource.MetaAddr() != 0u && compression_metadata;
}

bool ShaderStorageDescriptorSwizzleAllowed(bool gen5, const ShaderBufferResource& resource)
{
	// The GFX10 buffer-address swizzle is defined only for a descriptor with a
	// non-zero record stride. ADD_TID is part of the same address equation; it
	// is not a reason to discard the descriptor.
	return gen5 && resource.SwizzleEnabled() && resource.Stride() != 0u;
}

uint32_t ShaderRawBufferByteAddress(const ShaderBufferResource& resource, uint32_t index, uint32_t offset, uint32_t scalar_offset,
                                    uint32_t lane_index)
{
	uint32_t address_index = index;
	if (resource.AddTid())
	{
		// RDNA2 ADD_TID uses the lane in the executing wave. Keep the 6-bit
		// hardware width even when Vulkan exposes a larger subgroup.
		address_index += lane_index & 63u;
	}

	const uint32_t stride       = resource.Stride();
	uint32_t       byte_address = address_index * stride + offset;
	if (resource.SwizzleEnabled() && stride != 0u)
	{
		const uint32_t index_stride_log2 = resource.IndexStride();
		const uint32_t index_stride      = 8u << index_stride_log2;
		const uint32_t index_msb         = address_index >> (index_stride_log2 + 3u);
		const uint32_t index_lsb         = address_index & (index_stride - 1u);
		const uint32_t offset_msb        = offset & ~3u;
		const uint32_t offset_lsb        = offset & 3u;

		byte_address = ((index_msb * stride + offset_msb) * index_stride) + (index_lsb << 2u) + offset_lsb;
	}

	// S_OFFSET is outside the swizzled address portion (RDNA2 buffer address
	// equation), which is why it cannot be folded into the MUBUF immediate.
	return byte_address + scalar_offset;
}

bool ShaderGen5StorageDescriptorSupported(const ShaderBufferResource& resource, ShaderStorageAccess access)
{
	if (access == ShaderStorageAccess::Raw)
	{
		return ShaderRawStorageDescriptorSupported(resource);
	}

	const bool four_component = resource.Stride() == 16 && resource.DstSelXYZW() == DstSel(4, 5, 6, 7) &&
	                            ShaderIsGen5FourComponent32BitBufferFormat(resource.Format());
	const bool one_component  = resource.Stride() == 4 &&
	                            (resource.DstSelXYZW() == DstSel(4, 0, 0, 1) || resource.DstSelXYZW() == DstSel(4, 0, 0, 0)) &&
	                            ShaderIsGen5SingleComponent32BitBufferFormat(resource.Format());
	return four_component || one_component;
}

ShaderStorageAccessEvidence ResolveShaderStorageAccessEvidence(bool code_available, ShaderStorageBindingSource source,
                                                               ShaderStorageAccess exact_match, ShaderStorageAccess unbased_match,
                                                               bool decoded_unknown, bool indirect_descriptor_use)
{
	if (!code_available)
	{
		return {ShaderStorageAccess::Unknown, ShaderStorageUnknownReason::CodeUnavailable, false, false, false};
	}
	if (exact_match != ShaderStorageAccess::Unknown)
	{
		return {exact_match, ShaderStorageUnknownReason::None, true, true, false};
	}
	if (unbased_match != ShaderStorageAccess::Unknown)
	{
		return {ShaderStorageAccess::Unknown, ShaderStorageUnknownReason::RegisterBaseMismatch, true, false, true};
	}
	if (source == ShaderStorageBindingSource::MetadataSharp && !decoded_unknown && !indirect_descriptor_use)
	{
		return {ShaderStorageAccess::UnusedMetadata, ShaderStorageUnknownReason::None, true, false, false};
	}
	if (source == ShaderStorageBindingSource::MetadataSharp)
	{
		return {ShaderStorageAccess::Unknown, ShaderStorageUnknownReason::MetadataOnlyBinding, true, false, false};
	}
	return {ShaderStorageAccess::Unknown, ShaderStorageUnknownReason::NoMatchingInstruction, true, false, false};
}

void ExcludeUnusedMetadataStorage(ShaderStorageResources* resources)
{
	EXIT_IF(resources == nullptr);

	int active_indices[ShaderStorageResources::BUFFERS_MAX] = {};
	for (int& index: active_indices)
	{
		index = -1;
	}
	int active_count = 0;
	for (int source = 0; source < resources->buffers_num; ++source)
	{
		if (resources->accesses[source] == ShaderStorageAccess::UnusedMetadata)
		{
			continue;
		}
		active_indices[source] = active_count;
		if (active_count != source)
		{
			resources->buffers[active_count]                 = resources->buffers[source];
			resources->usages[active_count]                  = resources->usages[source];
			resources->accesses[active_count]                = resources->accesses[source];
			resources->sources[active_count]                 = resources->sources[source];
			resources->unknown_reasons[active_count]         = resources->unknown_reasons[source];
			resources->code_available[active_count]          = resources->code_available[source];
			resources->exact_matches[active_count]           = resources->exact_matches[source];
			resources->unbased_matches[active_count]         = resources->unbased_matches[source];
			resources->decoded_unknown[active_count]          = resources->decoded_unknown[source];
			resources->indirect_descriptor_use[active_count] = resources->indirect_descriptor_use[source];
			resources->raw_vmem_oob_guarded[active_count]    = resources->raw_vmem_oob_guarded[source];
			resources->raw_smem_use[active_count]             = resources->raw_smem_use[source];
			resources->raw_tbuffer_use[active_count]          = resources->raw_tbuffer_use[source];
			resources->raw_smem_required_bytes[active_count]  = resources->raw_smem_required_bytes[source];
			resources->raw_smem_dynamic_offset[active_count]  = resources->raw_smem_dynamic_offset[source];
			resources->dynamic_sload[active_count]            = resources->dynamic_sload[source];
			resources->slots[active_count]                   = resources->slots[source];
			resources->start_register[active_count]           = resources->start_register[source];
			resources->extended[active_count]                 = resources->extended[source];
		}
		++active_count;
	}
	resources->buffers_num = active_count;
}

bool ShaderIsNullMrtDoneFormat(ShaderInstructionFormat::Format format)
{
	switch (format)
	{
		case ShaderInstructionFormat::Mrt0OffOffComprVmDone:
		case ShaderInstructionFormat::Mrt1OffOffComprVmDone:
		case ShaderInstructionFormat::Mrt2OffOffComprVmDone:
		case ShaderInstructionFormat::Mrt3OffOffComprVmDone: return true;
		default: return false;
	}
}

uint32_t ShaderColorExportSourceComponent(uint32_t channel_order, uint32_t output_component)
{
	if (channel_order > 3) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: channel_order > 3 condition ignored (continuing)\n"); }
	if (output_component > 3) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: output_component > 3 condition ignored (continuing)\n"); }

	// The attachment VkFormat and image view own physical component order.
	// Shader exports remain logical RGBA; applying COMP_SWAP here as well would
	// swap the same components twice in render-to-texture feedback passes.
	(void)channel_order;
	return output_component;
}

uint32_t ShaderGen5TextureBytesPerElement(uint32_t format)
{
	// Gen5 sampled image element size. Block-compressed formats use bytes per
	// compressed block after dimensions are converted to block elements.
	switch (format)
	{
		case 1: return 1;    // UFMT_8_UNORM
		case 5: return 1;    // UFMT_8_UINT
		case 7: return 2;    // UFMT_16_UNORM
		case 20: return 4;   // UFMT_32_UINT
		case 22: return 4;   // UFMT_32_FLOAT
		case 13: return 2;   // UFMT_16_FLOAT
		case 14: return 2;   // UFMT_8_8_UNORM
		case 36: return 4;   // UFMT_10_11_11_FLOAT
		case 56: return 4;   // UFMT_8_8_8_8_UNORM
		case 62: return 8;   // UFMT_32_32_UINT
		case 65:             // UFMT_16_16_16_16_UNORM
		case 66:             // UFMT_16_16_16_16_SNORM
		case 67:             // UFMT_16_16_16_16_USCALED
		case 68:             // UFMT_16_16_16_16_SSCALED
		case 69:             // UFMT_16_16_16_16_UINT
		case 70:             // UFMT_16_16_16_16_SINT
		case 71: return 8;   // UFMT_16_16_16_16_FLOAT
		case 75: return 16;  // UFMT_32_32_32_32_UINT
		case 128: return 1;  // UFMT_8_SRGB
		case 129: return 2;  // UFMT_8_8_SRGB
		case 130: return 4;  // UFMT_8_8_8_8_SRGB
		case 133: return 8;  // UFMT_BC1_UNORM, 4x4 texels per block
		case 169: return 8;  // UFMT_BC1_UNORM, 4x4 texels per block
		case 170: return 8;  // UFMT_BC1_SRGB, 4x4 texels per block
		case 171: return 16; // UFMT_BC2_UNORM, 4x4 texels per block
		case 172: return 16; // UFMT_BC2_SRGB, 4x4 texels per block
		case 173: return 16; // UFMT_BC3_UNORM, 4x4 texels per block
		case 174: return 16; // UFMT_BC3_SRGB, 4x4 texels per block
		case 175: return 8;  // UFMT_BC4_UNORM, 4x4 texels per block
		case 176: return 8;  // UFMT_BC4_SNORM, 4x4 texels per block
		case 177: return 16; // UFMT_BC5_UNORM, 4x4 texels per block
		case 178: return 16; // UFMT_BC5_SNORM, 4x4 texels per block
		case 179: return 16; // UFMT_BC6H_UFLOAT, 4x4 texels per block
		case 180: return 16; // UFMT_BC6H_SFLOAT, 4x4 texels per block
		case 181: return 16; // UFMT_BC7_UNORM, 4x4 texels per block
		case 182: return 16; // UFMT_BC7_SRGB, 4x4 texels per block
		default: return 0;
	}
}

bool ShaderGen5TextureIsBlockCompressed(uint32_t format)
{
	return format == 133u || (format >= 169u && format <= 182u);
}

uint32_t ShaderGen5LinearTexturePitch(uint32_t width, uint32_t format)
{
	// GFX linear surfaces force a 256-byte row pitch alignment.
	const uint32_t bpp = ShaderGen5TextureBytesPerElement(format);
	EXIT_IF(bpp == 0);
	const uint32_t align_px = 256u / bpp;
	EXIT_IF(align_px == 0);
	if (width == 0)
	{
		return align_px;
	}
	return ((width + align_px - 1u) / align_px) * align_px;
}

uint32_t ShaderGen5ResolveLinearPitch(uint32_t width, uint32_t format, uint8_t type, uint32_t word4)
{
	// Provenance: RDNA2 SQ_IMG_RSRC 256-bit 1D/2D/2D-MSAA word4[13:0] = pitch-1.
	// Zero word4 denotes a 128-bit resource with implicit pitch = width.
	uint32_t pitch = width;
	if ((type == 8u || type == 9u || type == 14u) && word4 != 0u)
	{
		pitch = (word4 & 0x3FFFu) + 1u;
	}
	if (pitch < width)
	{
		pitch = width;
	}
	return ShaderGen5LinearTexturePitch(pitch, format);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED