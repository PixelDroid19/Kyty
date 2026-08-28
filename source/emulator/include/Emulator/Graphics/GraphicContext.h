#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuWritebackPageCache.h"
#include "Emulator/Graphics/SampleLocations.h"

#include <vulkan/vulkan_core.h> // IWYU pragma: export

// Vendored vulkan_core.h may predate VK_EXT_depth_clip_control; define the ABI
// locally when the header lacks it so capability-driven hosts can enable it.
#ifndef VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME
#define VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME "VK_EXT_depth_clip_control"
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT static_cast<VkStructureType>(1000355000)
#define VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT static_cast<VkStructureType>(1000355001)
typedef struct VkPhysicalDeviceDepthClipControlFeaturesEXT
{
	VkStructureType sType;
	void*           pNext;
	VkBool32        depthClipControl;
} VkPhysicalDeviceDepthClipControlFeaturesEXT;
typedef struct VkPipelineViewportDepthClipControlCreateInfoEXT
{
	VkStructureType sType;
	const void*     pNext;
	VkBool32        negativeOneToOne;
} VkPipelineViewportDepthClipControlCreateInfoEXT;
#endif

#ifndef VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME
#define VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME "VK_EXT_depth_range_unrestricted"
#endif

#ifndef VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME
#define VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME "VK_EXT_load_store_op_none"
#endif

#ifndef VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME
#define VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME "VK_KHR_load_store_op_none"
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

[[nodiscard]] constexpr VkAttachmentStoreOp VulkanAttachmentStoreOpNone()
{
	// The vendored header exposes the original alias; KHR and EXT use the same
	// registry value when their corresponding device extension is enabled.
	return VK_ATTACHMENT_STORE_OP_NONE_QCOM;
}

struct VulkanSwapchain
{
	VkSwapchainKHR swapchain                  = nullptr;
	VkFormat       swapchain_format           = VK_FORMAT_UNDEFINED;
	VkExtent2D     swapchain_extent           = {};
	VkImage*       swapchain_images           = nullptr;
	VkImageView*   swapchain_image_views      = nullptr;
	uint32_t       swapchain_images_count     = 0;
	VkSemaphore*   render_finished_semaphores = nullptr;
	VkFence        present_complete_fence     = nullptr;
	uint32_t       current_index              = 0;
};

struct VulkanCommandPool
{
	Core::Mutex      mutex;
	VkCommandPool    pool          = nullptr;
	VkCommandBuffer* buffers       = nullptr;
	VkFence*         fences        = nullptr;
	VkSemaphore*     semaphores    = nullptr;
	bool*            busy          = nullptr;
	uint32_t         buffers_count = 0;
};

struct VulkanQueueInfo
{
	Core::Mutex* mutex    = nullptr;
	uint32_t     family   = static_cast<uint32_t>(-1);
	uint32_t     index    = static_cast<uint32_t>(-1);
	VkQueue      vk_queue = nullptr;
};

struct GpuDetileContext;

struct GraphicContext
{
	static constexpr int QUEUES_NUM          = 11;
	static constexpr int QUEUE_GFX           = 8;
	static constexpr int QUEUE_GFX_NUM       = 1;
	static constexpr int QUEUE_UTIL          = 9;
	static constexpr int QUEUE_UTIL_NUM      = 1;
	static constexpr int QUEUE_PRESENT       = 10;
	static constexpr int QUEUE_PRESENT_NUM   = 1;
	static constexpr int QUEUE_COMPUTE_START = 0;
	static constexpr int QUEUE_COMPUTE_NUM   = 8;

	uint32_t                 screen_width    = 0;
	uint32_t                 screen_height   = 0;
	VkInstance               instance        = nullptr;
	VkDebugUtilsMessengerEXT debug_messenger = nullptr;
	VkPhysicalDevice         physical_device = nullptr;
	VkDevice                 device          = nullptr;
	VkPipelineCache          pipeline_cache  = nullptr;
	VulkanQueueInfo          queues[QUEUES_NUM];
	Core::Mutex              queue_mutexes[QUEUES_NUM];
	uint32_t                 queue_mutex_count = 0;
	// Diagnostic compute detile owns Vulkan objects through this exact context;
	// it is never cached only by a raw VkDevice handle.
	Core::Mutex       gpu_detile_mutex;
	GpuDetileContext* gpu_detile_context = nullptr;

	// VK_EXT_color_write_enable is unavailable on some drivers (notably MoltenVK
	// on Apple Silicon). When false, color write masking falls back to being
	// baked into the pipeline instead of set as dynamic state.
	bool color_write_enable_supported = true;

	// VK_EXT_depth_clip_enable is likewise absent on MoltenVK. When false, the
	// intended "depth clip disabled" state is emulated with core depthClampEnable.
	bool depth_clip_enable_supported = true;

	// VK_EXT_depth_clip_control selects Vulkan clip Z in [-W,+W] (OpenGL) vs [0,+W]
	// (DX). When false, OpenGL guest clip space cannot be expressed natively.
	bool depth_clip_control_supported = false;

	// VK_EXT_depth_range_unrestricted allows viewport min/maxDepth outside [0,1].
	bool depth_range_unrestricted_supported = false;

	// A read-only attachment sampled by the same draw must use STORE_OP_NONE;
	// STORE and DONT_CARE may still perform attachment writes.
	bool load_store_op_none_supported = false;

	// Core depthBiasClamp is optional and must be enabled explicitly at device creation.
	bool depth_bias_clamp_supported = false;

	// Per-sample fragment execution is optional and must be enabled explicitly
	// before a graphics pipeline can request sample shading.
	bool sample_rate_shading_supported = false;

	// Vulkan subgroup limits used to validate shaders that require an exact guest
	// wave width. A zero maximum means the physical-device query was unavailable.
	uint32_t subgroup_size                  = 0;
	uint32_t subgroup_min_size              = 0;
	uint32_t subgroup_max_size              = 0;
	VkShaderStageFlags subgroup_stages      = 0;
	VkSubgroupFeatureFlags subgroup_operations = 0;
	bool subgroup_size_control_supported    = false;

	// VK_EXT_sample_locations is optional at device discovery time. A draw that
	// programs custom guest locations is rejected when the selected host cannot
	// represent the exact state.
	VulkanSampleLocationCapabilities sample_location_capabilities;
};

struct VulkanMemory
{
	VkMemoryRequirements  requirements = {};
	VkMemoryPropertyFlags property     = 0;
	VkDeviceMemory        memory       = nullptr;
	VkDeviceSize          offset       = 0;
	uint32_t              type         = 0;
	uint64_t              unique_id    = 0;
};

enum class VulkanImageType
{
	Unknown,
	VideoOut,
	DepthStencil,
	Texture,
	StorageTexture,
	RenderTexture
};

struct VulkanImage
{
	static constexpr int VIEW_MAX                   = 9;
	static constexpr int VIEW_DEFAULT       = 0;
	static constexpr int VIEW_BGRA          = 1;
	static constexpr int VIEW_DEPTH_TEXTURE = 2;
	static constexpr int VIEW_ABGR          = 3;
	static constexpr int VIEW_ARRAY          = 4;
	static constexpr int VIEW_3D             = 5;
	static constexpr int VIEW_STENCIL_TEXTURE = 6;
	static constexpr int VIEW_DEPTH_TEXTURE_ARRAY = 7;
	static constexpr int VIEW_STORAGE_ARRAY       = 8;

	explicit VulkanImage(VulkanImageType type): type(type) {}

	void SetNativeExtent(uint32_t width, uint32_t height)
	{
		guest_extent = {width, height};
		extent       = guest_extent;
	}

	void SetHostExtent(uint32_t width, uint32_t height) { extent = {width, height}; }

	[[nodiscard]] VkExtent2D GetGuestExtent() const { return guest_extent; }

	[[nodiscard]] VkExtent2D GetHostExtent() const { return extent; }

	[[nodiscard]] bool MatchesGuestExtent(uint32_t width, uint32_t height) const
	{
		return guest_extent.width == width && guest_extent.height == height;
	}

	[[nodiscard]] bool IsResolutionScaled() const
	{
		return guest_extent.width != extent.width || guest_extent.height != extent.height;
	}

	VulkanImageType        type                 = VulkanImageType::Unknown;
	VkFormat               format               = VK_FORMAT_UNDEFINED;
	// Guest descriptor/layout calculations always use guest_extent. extent is
	// the independently selectable Vulkan storage extent.
	VkExtent2D             guest_extent         = {};
	VkExtent2D             extent               = {};
	// Exact allocation extent from VkImageCreateInfo. This can exceed the
	// logical extent when storage mips are packed into one host image.
	VkExtent3D             physical_extent      = {};
	VkImage                image                = nullptr;
	VkImageView            image_view[VIEW_MAX] = {};
	VkImageUsageFlags      usage                = 0;
	VkImageLayout          layout               = VK_IMAGE_LAYOUT_UNDEFINED;
	VkSampleCountFlagBits  samples               = VK_SAMPLE_COUNT_1_BIT;
	uint32_t               mip_levels            = 1;
	uint32_t               array_layers          = 1;
	Graphics::VulkanMemory memory;
	// Guest allocation size used by PreferGpuMemoryAliasIndex when sampling.
	uint64_t               guest_size           = 0;
};

struct VideoOutVulkanImage: public VulkanImage
{
	VideoOutVulkanImage(): VulkanImage(VulkanImageType::VideoOut) {}

	// Registration owns the guest resource identity immediately, while Vulkan
	// storage may be materialized by the first renderer/present consumer.
	Core::Mutex materialize_mutex;
	bool        host_extent_selected = false;
	uint64_t    guest_vaddr = 0;
	uint64_t    guest_pitch = 0;
	bool        tiled       = false;
	bool        neo         = false;
};

struct DepthStencilVulkanImage: public VulkanImage
{
	DepthStencilVulkanImage(): VulkanImage(VulkanImageType::DepthStencil) {}
	bool                       compressed                   = false;
	bool                       sample_locations_compatible = false;
	VulkanSampleLocationState last_sample_locations;
};

struct TextureVulkanImage: public VulkanImage
{
	TextureVulkanImage(): VulkanImage(VulkanImageType::Texture) {}
};

struct StorageTextureVulkanImage: public VulkanImage
{
	StorageTextureVulkanImage(): VulkanImage(VulkanImageType::StorageTexture) {}
	uint64_t guest_vaddr = 0;
};

struct RenderTextureVulkanImage: public VulkanImage
{
	RenderTextureVulkanImage(): VulkanImage(VulkanImageType::RenderTexture) {}
};

struct VulkanBuffer
{
	VkBuffer           buffer = nullptr;
	VulkanMemory       memory;
	VkBufferUsageFlags usage = 0;
};

struct StorageVulkanBuffer: public VulkanBuffer
{
	uint64_t              guest_addr      = 0;
	uint64_t              guest_size      = 0;
	uint64_t              depth_meta_addr = 0;
	GpuWritebackPageCache writeback_cache;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
