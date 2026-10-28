#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/StorageTexture.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Profiler.h"

#include <memory>
#include <mutex>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static void update_func(GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* /*obj*/, const uint64_t* /*vaddr*/,
                        const uint64_t* /*size*/, int /*vaddr_num*/)
{
	KYTY_PROFILER_BLOCK("DepthStencilBufferObject::update_func");
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("DepthStencilBufferObject::Create");

	EXIT_IF(size == nullptr || vaddr == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto pixel_format = static_cast<VkFormat>(params[DepthStencilBufferObject::PARAM_FORMAT]);
	auto guest_width  = params[DepthStencilBufferObject::PARAM_GUEST_WIDTH];
	auto guest_height = params[DepthStencilBufferObject::PARAM_GUEST_HEIGHT];
	auto host_width   = params[DepthStencilBufferObject::PARAM_HOST_WIDTH];
	auto host_height  = params[DepthStencilBufferObject::PARAM_HOST_HEIGHT];
	bool htile = params[DepthStencilBufferObject::PARAM_HTILE] != 0;
	const auto usage = params[DepthStencilBufferObject::PARAM_USAGE];
	bool sampled = (usage & 0x1u) != 0;
	bool sample_locations_compatible = (usage & 0x2u) != 0;
	auto samples = static_cast<VkSampleCountFlagBits>(params[DepthStencilBufferObject::PARAM_SAMPLES]);

	if (pixel_format == VK_FORMAT_UNDEFINED) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: pixel_format == VK_FORMAT_UNDEFINED condition ignored (continuing)\n"); }
	if (guest_width == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: guest_width == 0 condition ignored (continuing)\n"); }
	if (guest_height == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: guest_height == 0 condition ignored (continuing)\n"); }
	if (host_width == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: host_width == 0 condition ignored (continuing)\n"); }
	if (host_height == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: host_height == 0 condition ignored (continuing)\n"); }

	VulkanResolutionAttachmentRequest capability_request {};
	capability_request.extent       = {static_cast<uint32_t>(host_width), static_cast<uint32_t>(host_height)};
	capability_request.format       = pixel_format;
	capability_request.usage        = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
	capability_request.flags        = (sample_locations_compatible ? VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT : 0);
	capability_request.sample_count = samples;
	const auto capability            = EvaluateVulkanResolutionAttachment(ctx, capability_request);
	if (capability.status != VulkanRenderResolutionCapabilityStatus::Success ||
	                     capability.decision.status != RenderResolutionImageCapabilityStatus::Supported) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: capability.status != VulkanRenderResolutionCapabilityStatus::Success || condition ignored (continuing)\n"); }

	auto* vk_obj = new DepthStencilVulkanImage;

	vk_obj->SetNativeExtent(guest_width, guest_height);
	vk_obj->SetHostExtent(host_width, host_height);
	vk_obj->format     = pixel_format;
	vk_obj->image      = nullptr;
	vk_obj->layout     = VK_IMAGE_LAYOUT_UNDEFINED;
	vk_obj->samples    = samples;
	vk_obj->guest_size = *size;
	vk_obj->sample_locations_compatible = sample_locations_compatible;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	vk_obj->compressed = !htile;

	VulkanImageDescriptor image_descriptor {};
	image_descriptor.extent         = {vk_obj->extent.width, vk_obj->extent.height, 1};
	image_descriptor.format         = vk_obj->format;
	image_descriptor.flags          = capability_request.flags;
	image_descriptor.initial_layout = vk_obj->layout;
	image_descriptor.samples        = vk_obj->samples;
	image_descriptor.usage = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0));
	const auto image_info  = VulkanBuildImageCreateInfo(image_descriptor);
	if (!VulkanCreateDeviceImage(ctx, image_info, vk_obj, mem)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanCreateDeviceImage(ctx, image_info, vk_obj, mem) condition ignored (continuing)\n"); }

	// EXIT_NOT_IMPLEMENTED(mem->requirements.size > *size);

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);
	vk_obj->content_stamp = VulkanImageNextStamp();

	VulkanImageViewDescriptor view_descriptor {};
	view_descriptor.image       = vk_obj->image;
	view_descriptor.format      = vk_obj->format;
	view_descriptor.aspect_mask = DepthFormatAspectMask(vk_obj->format);
	if (!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]) condition ignored (continuing)\n"); }

	view_descriptor.components  = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
	view_descriptor.aspect_mask = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_DEPTH_TEXTURE])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_DEPTH_TEXTURE]) condition ignored (continuing)\n"); }
	view_descriptor.view_type   = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	view_descriptor.layer_count = 1;
	if (
	    !VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_DEPTH_TEXTURE_ARRAY])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[V condition ignored (continuing)\n"); }

	if (DepthFormatHasStencil(vk_obj->format))
	{
		view_descriptor.components  = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		                               VK_COMPONENT_SWIZZLE_IDENTITY};
		view_descriptor.aspect_mask = VK_IMAGE_ASPECT_STENCIL_BIT;
		// Stencil readers declare a non-arrayed 2D image.
		view_descriptor.view_type = VK_IMAGE_VIEW_TYPE_2D;
		if (
		    !VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_STENCIL_TEXTURE])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[V condition ignored (continuing)\n"); }
	}

	// Leave layout UNDEFINED until the first render pass. A layout-only
	// transition to ATTACHMENT does not define pixels; a later LOAD then reads
	// host garbage. ResolveDepthAttachmentLoadOps CLEARs first use, so image
	// creation must not fabricate a guest HTILE clear event.

	return vk_obj;
}

// The depth image takes the texels of the storage view whose bytes it reinterprets
// (StorageTextureSharesDepthTexels), instead of starting undefined.
static void* create_from_texel_bits_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params,
                                         GpuMemoryScenario /*scenario*/, const Vector<GpuMemoryObject>& objects, VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("DepthStencilBufferObject::CreateFromObjects");

	EXIT_IF(buffer == nullptr || objects.Size() != 1 || objects.At(0).type != GpuMemoryObjectType::StorageTexture);
	auto* source = static_cast<StorageTextureVulkanImage*>(objects.At(0).obj);
	EXIT_IF(source == nullptr || source->guest_size == 0u);

	const uint64_t vaddr  = 0;
	auto*          vk_obj = static_cast<DepthStencilVulkanImage*>(create_func(ctx, params, &vaddr, &source->guest_size, 1, mem));
	if (!UtilCopyTexelBits(buffer, source, vk_obj, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL))
	{
		EXIT("texel bits to depth copy failed: extent=%ux%u\n", source->extent.width, source->extent.height);
	}
	return vk_obj;
}

VkFormat DepthStencilBufferStoragePlaneFormat(VkFormat depth_format)
{
	switch (depth_format)
	{
		case VK_FORMAT_D32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_FORMAT_R32_SFLOAT;
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_D16_UNORM_S8_UINT: return VK_FORMAT_R16_UNORM;
		default: return VK_FORMAT_UNDEFINED;
	}
}

StorageTextureVulkanImage* DepthStencilBufferGetStoragePlane(GraphicContext* ctx, DepthStencilVulkanImage* depth)
{
	EXIT_IF(ctx == nullptr || depth == nullptr);
	static std::mutex          create_mutex;
	std::lock_guard<std::mutex> lock(create_mutex);
	if (depth->storage_plane != nullptr)
	{
		return depth->storage_plane;
	}

	auto* plane = new StorageTextureVulkanImage;
	plane->SetNativeExtent(depth->extent.width, depth->extent.height);
	plane->format       = DepthStencilBufferStoragePlaneFormat(depth->format);
	plane->image        = nullptr;
	plane->layout       = VK_IMAGE_LAYOUT_UNDEFINED;
	plane->array_layers = 1u;
	plane->guest_size   = depth->guest_size;
	for (auto& view: plane->image_view)
	{
		view = nullptr;
	}

	VulkanImageDescriptor image_descriptor {};
	image_descriptor.extent = {depth->extent.width, depth->extent.height, 1u};
	image_descriptor.format = plane->format;
	image_descriptor.usage  = StorageTextureGetImageUsage();
	if (!VulkanCreateDeviceImage(ctx, VulkanBuildImageCreateInfo(image_descriptor), plane, &depth->storage_plane_memory))
	{
		EXIT("depth storage plane image creation failed: extent=%ux%u\n", depth->extent.width, depth->extent.height);
	}

	VulkanImageViewDescriptor view_descriptor {};
	view_descriptor.image  = plane->image;
	view_descriptor.format = plane->format;
	EXIT_IF(!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &plane->image_view[VulkanImage::VIEW_DEFAULT]));
	view_descriptor.view_type   = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	view_descriptor.layer_count = 1u;
	EXIT_IF(!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &plane->image_view[VulkanImage::VIEW_STORAGE_ARRAY]));

	depth->storage_plane = plane;
	return plane;
}

static void DeleteStoragePlane(GraphicContext* ctx, DepthStencilVulkanImage* depth)
{
	auto* plane = depth->storage_plane;
	if (plane == nullptr)
	{
		return;
	}
	for (auto& view: plane->image_view)
	{
		if (view != nullptr)
		{
			vkDestroyImageView(ctx->device, view, nullptr);
			view = nullptr;
		}
	}
	vkDestroyImage(ctx->device, plane->image, nullptr);
	VulkanFree(ctx, &depth->storage_plane_memory);
	delete plane;
	depth->storage_plane = nullptr;
}

DepthSliceArray* DepthStencilBufferGetSliceArray(GraphicContext* ctx, DepthStencilVulkanImage* owner, uint32_t width, uint32_t height,
                                                 uint64_t layer_size, uint32_t layers)
{
	EXIT_IF(ctx == nullptr || owner == nullptr || layers < 2u || width == 0u || height == 0u);
	static std::mutex           create_mutex;
	std::lock_guard<std::mutex> lock(create_mutex);
	// In-flight submissions may still read an array, so none is replaced before its owner is destroyed.
	for (const auto& existing: owner->slice_arrays)
	{
		if (existing->image->extent.width == width && existing->image->extent.height == height && existing->image->array_layers == layers)
		{
			return existing.get();
		}
	}
	// Each distinct view over one depth plane costs a full array image.
	static constexpr size_t k_max_arrays_per_owner = 4u;
	if (owner->slice_arrays.size() >= k_max_arrays_per_owner)
	{
		EXIT("depth slice arrays exceed %zu views of one depth image: extent=%ux%u layers=%u\n", k_max_arrays_per_owner, width, height,
		     layers);
	}

	auto  slice_array = std::make_unique<DepthSliceArray>();
	auto* array       = new DepthStencilVulkanImage;
	array->SetNativeExtent(width, height);
	array->format       = owner->format;
	array->image        = nullptr;
	array->layout       = VK_IMAGE_LAYOUT_UNDEFINED;
	array->array_layers = layers;
	array->guest_size   = layer_size * layers;
	for (auto& view: array->image_view)
	{
		view = nullptr;
	}

	VulkanImageDescriptor image_descriptor {};
	image_descriptor.extent       = {width, height, 1u};
	image_descriptor.array_layers = layers;
	image_descriptor.format       = array->format;
	// Sampled in DEPTH_STENCIL_READ_ONLY_OPTIMAL, a layout valid only for depth-stencil attachment images.
	image_descriptor.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if (!VulkanCreateDeviceImage(ctx, VulkanBuildImageCreateInfo(image_descriptor), array, &slice_array->memory))
	{
		EXIT("depth slice array image creation failed: extent=%ux%u layers=%u\n", width, height, layers);
	}

	VulkanImageViewDescriptor view_descriptor {};
	view_descriptor.image       = array->image;
	view_descriptor.format      = array->format;
	view_descriptor.components  = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
	view_descriptor.aspect_mask = VK_IMAGE_ASPECT_DEPTH_BIT;
	view_descriptor.view_type   = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	view_descriptor.layer_count = layers;
	EXIT_IF(!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &array->image_view[VulkanImage::VIEW_DEPTH_TEXTURE_ARRAY]));

	slice_array->image = array;
	slice_array->layers.assign(layers, {});
	owner->slice_arrays.push_back(std::move(slice_array));
	return owner->slice_arrays.back().get();
}

static void DeleteSliceArrays(GraphicContext* ctx, DepthStencilVulkanImage* depth)
{
	for (auto& slice_array: depth->slice_arrays)
	{
		auto* array = slice_array->image;
		for (auto& view: array->image_view)
		{
			if (view != nullptr)
			{
				vkDestroyImageView(ctx->device, view, nullptr);
				view = nullptr;
			}
		}
		vkDestroyImage(ctx->device, array->image, nullptr);
		VulkanFree(ctx, &slice_array->memory);
		delete array;
		for (const auto& layer: slice_array->layers)
		{
			if (layer.guest_size != 0u)
			{
				(void)GpuDirtyPageTracker::Instance().UnregisterRange(layer.guest_addr, layer.guest_size);
			}
		}
	}
	depth->slice_arrays.clear();
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("DepthStencilBufferObject::delete_func");

	auto* vk_obj = reinterpret_cast<DepthStencilVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);

	DeleteFramebuffer(vk_obj);
	DeleteStoragePlane(ctx, vk_obj);
	DeleteSliceArrays(ctx, vk_obj);

	for (auto& view: vk_obj->image_view)
	{
		if (view != nullptr)
		{
			vkDestroyImageView(ctx->device, view, nullptr);
			view = nullptr;
		}
	}

	vkDestroyImage(ctx->device, vk_obj->image, nullptr);

	VulkanFree(ctx, mem);

	delete vk_obj;
}

bool DepthStencilBufferObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_GUEST_WIDTH] == other[PARAM_GUEST_WIDTH] &&
	        params[PARAM_GUEST_HEIGHT] == other[PARAM_GUEST_HEIGHT] && params[PARAM_HTILE] == other[PARAM_HTILE] &&
	        params[PARAM_NEO] == other[PARAM_NEO] && (params[PARAM_USAGE] & 0x1u) == (other[PARAM_USAGE] & 0x1u) &&
	        params[PARAM_HTILE_ADDR] == other[PARAM_HTILE_ADDR] && params[PARAM_HTILE_SIZE] == other[PARAM_HTILE_SIZE] &&
	        params[PARAM_HOST_WIDTH] == other[PARAM_HOST_WIDTH] && params[PARAM_HOST_HEIGHT] == other[PARAM_HOST_HEIGHT] &&
	        params[PARAM_SAMPLES] == other[PARAM_SAMPLES]);
}

GpuObject::create_func_t DepthStencilBufferObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::create_from_objects_func_t DepthStencilBufferObject::GetCreateFromObjectsFunc() const
{
	return create_from_texel_bits_func;
}

GpuObject::delete_func_t DepthStencilBufferObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t DepthStencilBufferObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
