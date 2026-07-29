#include "Emulator/Graphics/Objects/RenderTexture.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Profiler.h"

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static bool buffer_is_tiled(uint64_t vaddr, uint64_t size)
{
	if ((size & 0x7u) == 0)
	{
		const auto* ptr     = reinterpret_cast<const uint64_t*>(vaddr);
		const auto* ptr_end = reinterpret_cast<const uint64_t*>(vaddr + size / 8);
		for (uint64_t element = *ptr; ptr < ptr_end; ptr++)
		{
			if (element != *ptr)
			{
				return true;
			}
		}
		return false;
	}
	return true;
}

static void create_render_texture_image_views(GraphicContext* ctx, RenderTextureVulkanImage* vk_obj)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(vk_obj == nullptr);
	EXIT_NOT_IMPLEMENTED(!VulkanCreateStandardColorImageViews(ctx, vk_obj));
}

static void update_func(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	KYTY_PROFILER_BLOCK("RenderTextureObject::update_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = static_cast<RenderTextureVulkanImage*>(obj);

	bool tiled      = (params[RenderTextureObject::PARAM_TILED] != 0);
	bool write_back = (params[RenderTextureObject::PARAM_WRITE_BACK] != 0);
	// bool neo    = (params[RenderTextureObject::PARAM_NEO] != 0);
	auto pitch = params[RenderTextureObject::PARAM_PITCH];
	auto width = params[RenderTextureObject::PARAM_WIDTH];
	// auto height = params[RenderTextureObject::PARAM_HEIGHT];

	// GPU-owned tiled RT (no write-back): first consumer is a render pass.
	// Create leaves layout UNDEFINED once; Update must not force UNDEFINED again.
	// StorageBuffer WriteBack invalidates alias parents (hash/submit_id), which
	// re-enters Update. Marking UNDEFINED then transitioning to COLOR_ATTACHMENT
	// discards prior render-pass contents (white intermediate targets).
	if (!RenderTextureMayCpuUploadOnUpdate(tiled, write_back))
	{
		return;
	}
	EXIT_NOT_IMPLEMENTED(vk_obj->samples != VK_SAMPLE_COUNT_1_BIT);

	vk_obj->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (tiled && buffer_is_tiled(*vaddr, *size))
	{
		EXIT_NOT_IMPLEMENTED(width != pitch);
		auto* temp_buf = new uint8_t[*size];
		KYTY_NOT_IMPLEMENTED;
		// TODO()
		// TileConvertTiledToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), TileMode::VideoOutTiled, width, height, neo);
		// UtilFillImage(ctx, vk_obj, temp_buf, *size, pitch);
		delete[] temp_buf;
	} else
	{
		UtilFillImage(ctx, vk_obj, reinterpret_cast<void*>(*vaddr), *size, pitch,
		              static_cast<uint64_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	}
}

static void update2_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, void* obj, GpuMemoryScenario scenario,
                         const Vector<GpuMemoryObject>& objects)
{
	KYTY_PROFILER_BLOCK("RenderTextureObject::update_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(objects.IsEmpty());

	auto* vk_obj = static_cast<RenderTextureVulkanImage*>(obj);
	EXIT_NOT_IMPLEMENTED(vk_obj->samples != VK_SAMPLE_COUNT_1_BIT);

	// bool neo    = (params[RenderTextureObject::PARAM_NEO] != 0);
	// auto pitch  = params[RenderTextureObject::PARAM_PITCH];
	auto width  = params[RenderTextureObject::PARAM_WIDTH];
	auto height = params[RenderTextureObject::PARAM_HEIGHT];

	// GPU-owned tiled RT (write_back=0): never force UNDEFINED — that discards
	// prior render-pass contents (white / false-color intermediate targets when
	// the float lighting RT is re-entered via multi-parent Update2). GenerateMips
	// below transitions from the tracked layout via UtilFillImage/ImageToImage.
	const bool tiled     = (params[RenderTextureObject::PARAM_TILED] != 0);
	const bool gpu_owned = tiled && params[RenderTextureObject::PARAM_WRITE_BACK] == 0;
	if (!gpu_owned)
	{
		vk_obj->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	//	if (objects.Size() == 2 && objects.At(0).type == GpuMemoryObjectType::StorageBuffer &&
	//	    objects.At(1).type == GpuMemoryObjectType::StorageTexture && scenario == GpuMemoryScenario::GenerateMips)
	//	{
	//		auto* src_obj = static_cast<StorageTextureVulkanImage*>(objects.At(1).obj);
	if (objects.Size() == 3 && objects.At(0).type == GpuMemoryObjectType::StorageBuffer &&
	    objects.At(1).type == GpuMemoryObjectType::Texture && objects.At(2).type == GpuMemoryObjectType::StorageTexture &&
	    scenario == GpuMemoryScenario::GenerateMips)
	{
		auto* src_obj = static_cast<StorageTextureVulkanImage*>(objects.At(2).obj);

		const auto src_guest_extent = src_obj->GetGuestExtent();
		uint32_t   mip_width        = src_guest_extent.width;
		uint32_t   mip_height       = src_guest_extent.height;

		Vector<ImageImageCopy> regions(1);

		bool updated = false;

		for (uint32_t i = 0; i < 16 && !updated; i++)
		{
			if (mip_width == width && mip_height == height)
			{
				auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);

				regions[0].src_image = src_obj;
				regions[0].src_level = 0;
				regions[0].dst_level = 0;
				regions[0].width     = mip_width;
				regions[0].height    = mip_height;
				regions[0].src_x     = mipmap_offset.first;
				regions[0].src_y     = mipmap_offset.second;
				regions[0].dst_x     = 0;
				regions[0].dst_y     = 0;

				if (buffer == nullptr)
				{
					UtilFillImage(ctx, regions, vk_obj, static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
				} else
				{
					UtilImageToImage(buffer, regions, vk_obj, static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
				}

				updated = true;
			}

			if (mip_width > 1)
			{
				mip_width /= 2;
			}
			if (mip_height > 1)
			{
				mip_height /= 2;
			}
		}

		EXIT_NOT_IMPLEMENTED(!updated);
	} else
	{
		KYTY_NOT_IMPLEMENTED;
	}
}

static VkFormat resolve_render_texture_format(uint64_t format)
{
	switch (static_cast<RenderTextureFormat>(format))
	{
		case RenderTextureFormat::R8Unorm: return VK_FORMAT_R8_UNORM;
		case RenderTextureFormat::R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
		case RenderTextureFormat::R8G8B8A8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
		case RenderTextureFormat::B8G8R8A8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
		case RenderTextureFormat::B8G8R8A8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
		case RenderTextureFormat::R16G16B16A16Sfloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case RenderTextureFormat::Unknown: break;
	}
	return VK_FORMAT_UNDEFINED;
}

static RenderTextureVulkanImage* create_render_texture_image(GraphicContext* ctx, const uint64_t* params, VulkanMemory* mem,
                                                             const uint64_t* guest_size)
{
	EXIT_IF(ctx == nullptr || params == nullptr || mem == nullptr);

	const auto width     = params[RenderTextureObject::PARAM_WIDTH];
	const auto height    = params[RenderTextureObject::PARAM_HEIGHT];
	const auto vk_format = resolve_render_texture_format(params[RenderTextureObject::PARAM_FORMAT]);
	const auto samples    = static_cast<VkSampleCountFlagBits>(params[RenderTextureObject::PARAM_SAMPLES]);
	EXIT_NOT_IMPLEMENTED(vk_format == VK_FORMAT_UNDEFINED || width == 0 || height == 0);

	VulkanResolutionAttachmentRequest capability_request {};
	capability_request.extent       = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
	capability_request.format       = vk_format;
	capability_request.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	capability_request.sample_count = samples;
	const auto capability            = EvaluateVulkanResolutionAttachment(ctx, capability_request);
	EXIT_NOT_IMPLEMENTED(capability.status != VulkanRenderResolutionCapabilityStatus::Success ||
	                     capability.decision.status != RenderResolutionImageCapabilityStatus::Supported);

	auto* vk_obj = new RenderTextureVulkanImage;
	vk_obj->SetNativeExtent(width, height);
	vk_obj->format = vk_format;
	vk_obj->image  = nullptr;
	vk_obj->samples = samples;
	if (guest_size != nullptr)
	{
		vk_obj->guest_size = *guest_size;
	}
	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	VulkanImageDescriptor image_descriptor {};
	image_descriptor.extent = {vk_obj->extent.width, vk_obj->extent.height, 1};
	image_descriptor.format = vk_obj->format;
	image_descriptor.samples = vk_obj->samples;
	image_descriptor.usage  = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	const auto image_info   = VulkanBuildImageCreateInfo(image_descriptor);
	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImage(ctx, image_info, vk_obj, mem));
	return vk_obj;
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("RenderTextureObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr);
	auto*      vk_obj = create_render_texture_image(ctx, params, mem, size);
	const auto width  = params[RenderTextureObject::PARAM_WIDTH];
	const auto height = params[RenderTextureObject::PARAM_HEIGHT];

	printf("RenderTextureObject::Create()\n");
	printf("\t mem->requirements.size = %" PRIu64 "\n", mem->requirements.size);
	printf("\t width                  = %" PRIu64 "\n", width);
	printf("\t height                 = %" PRIu64 "\n", height);
	printf("\t size                   = %" PRIu64 "\n", *size);

	// EXIT_NOT_IMPLEMENTED(mem->requirements.size > *size);

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	create_render_texture_image_views(ctx, vk_obj);

	return vk_obj;
}

static void* create2_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, GpuMemoryScenario scenario,
                          const Vector<GpuMemoryObject>& objects, VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("RenderTextureObject::CreateFromObjects");

	EXIT_IF(objects.IsEmpty());
	auto*      vk_obj = create_render_texture_image(ctx, params, mem, nullptr);
	const auto width  = params[RenderTextureObject::PARAM_WIDTH];
	const auto height = params[RenderTextureObject::PARAM_HEIGHT];

	printf("RenderTextureObject::CreateFromObjects()\n");
	printf("\t mem->requirements.size = %" PRIu64 "\n", mem->requirements.size);
	printf("\t width                  = %" PRIu64 "\n", width);
	printf("\t height                 = %" PRIu64 "\n", height);
	// printf("\t size                   = %" PRIu64 "\n", *size);

	// EXIT_NOT_IMPLEMENTED(mem->requirements.size > *size);

	update2_func(ctx, buffer, params, vk_obj, scenario, objects);

	create_render_texture_image_views(ctx, vk_obj);

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("RenderTextureObject::delete_func");

	auto* vk_obj = reinterpret_cast<RenderTextureVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);

	DeleteDescriptor(vk_obj);

	DeleteFramebuffer(vk_obj);

	for (auto image_view: vk_obj->image_view)
	{
		if (image_view != nullptr)
		{
			vkDestroyImageView(ctx->device, image_view, nullptr);
		}
	}

	vkDestroyImage(ctx->device, vk_obj->image, nullptr);

	VulkanFree(ctx, mem);

	delete vk_obj;
}

static GpuWritebackResult write_back(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size,
                                     int vaddr_num)
{
	KYTY_PROFILER_BLOCK("RenderTextureObject::write_back");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(obj == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	bool tiled = (params[RenderTextureObject::PARAM_TILED] != 0);
	auto pitch = params[RenderTextureObject::PARAM_PITCH];
	auto width = params[RenderTextureObject::PARAM_WIDTH];

	EXIT_IF(!(params[RenderTextureObject::PARAM_WRITE_BACK] != 0));

	EXIT_NOT_IMPLEMENTED(tiled);
	EXIT_NOT_IMPLEMENTED(width != pitch);

	auto* vk_obj = reinterpret_cast<RenderTextureVulkanImage*>(obj);
	EXIT_NOT_IMPLEMENTED(vk_obj->samples != VK_SAMPLE_COUNT_1_BIT);

	UtilFillBuffer(ctx, reinterpret_cast<void*>(*vaddr), *size, pitch, vk_obj,
	               static_cast<uint64_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	const uint64_t changed_pages = *size / 4096u + ((*size % 4096u) == 0u ? 0u : 1u);
	return {.changed_pages = changed_pages, .copied_bytes = *size, .content_changed = true};
}

bool RenderTextureObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_WIDTH] == other[PARAM_WIDTH] &&
	        params[PARAM_HEIGHT] == other[PARAM_HEIGHT] && params[PARAM_TILED] == other[PARAM_TILED] &&
	        params[PARAM_NEO] == other[PARAM_NEO] && params[PARAM_PITCH] == other[PARAM_PITCH] &&
	        params[PARAM_WRITE_BACK] == other[PARAM_WRITE_BACK] && params[PARAM_SAMPLES] == other[PARAM_SAMPLES]);
}

GpuObject::create_func_t RenderTextureObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::create_from_objects_func_t RenderTextureObject::GetCreateFromObjectsFunc() const
{
	return create2_func;
}

GpuObject::delete_func_t RenderTextureObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t RenderTextureObject::GetUpdateFunc() const
{
	return update_func;
}

GpuObject::write_back_func_t RenderTextureObject::GetWriteBackFunc() const
{
	bool wb = (params[RenderTextureObject::PARAM_WRITE_BACK] != 0);
	return (wb ? write_back : nullptr);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
