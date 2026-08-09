#include "Kyty/UnitTest.h"

#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/Gen5TextureArrayLayout.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"

#include "../../../emulator/src/Graphics/GraphicsRenderInternal.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace Kyty::Libs::Graphics {

// The production upload helper uses VulkanCreateBuffer, whose accounting is
// initialized by GpuMemoryInit. This test-only declaration lets the live test
// avoid reinitializing it when another graphics unit suite already did.
class GpuMemory;
extern GpuMemory* g_gpu_memory;

} // namespace Kyty::Libs::Graphics

UT_BEGIN(EmulatorTileDetile);

using namespace Libs::Graphics;

namespace {

void FillTiledFromLinear(std::vector<uint8_t>* tiled, const std::vector<uint8_t>& linear, uint32_t width, uint32_t height, uint32_t pitch,
                         uint32_t bpp, TileDetileLayout layout)
{
	auto offset_of = [&](uint32_t x, uint32_t y) -> uint64_t
	{
		if (layout == TileDetileLayout::Sw64kRx)
		{
			return TileGetSw64kRxOffset(x, y, pitch, bpp);
		}
		if (layout == TileDetileLayout::Standard64KB)
		{
			return TileGetStandard64KBOffset(x, y, pitch, bpp);
		}
		if (layout == TileDetileLayout::Standard4KB)
		{
			return TileGetStandard4KBOffset(x, y, pitch, bpp);
		}
		return TileGetDepth64KB32Offset(x, y, pitch);
	};

	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const uint64_t linear_off = (static_cast<uint64_t>(y) * width + x) * bpp;
			const uint64_t tiled_off  = offset_of(x, y);
			ASSERT_LT(tiled_off + bpp, tiled->size() + 1u);
			ASSERT_LT(linear_off + bpp, linear.size() + 1u);
			std::memcpy(tiled->data() + tiled_off, linear.data() + linear_off, bpp);
		}
	}
}

TileDetileRequest MakeRequest(void* dst, const void* src, uint32_t width, uint32_t height, uint32_t pitch, uint32_t dst_pitch, uint32_t bpp,
                              TileDetileLayout layout, uint64_t src_bytes)
{
	TileDetileRequest request {};
	request.dst               = dst;
	request.src               = src;
	request.width             = width;
	request.height            = height;
	request.pitch_elems       = pitch;
	request.dst_pitch_elems   = dst_pitch;
	request.bytes_per_element = bpp;
	request.layout            = layout;
	request.src_bytes         = src_bytes;
	return request;
}

void ExpectDetilePathsMatch(uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp, TileDetileLayout layout, uint64_t tiled_bytes)
{
	const uint64_t       linear_bytes = static_cast<uint64_t>(width) * height * bpp;
	std::vector<uint8_t> linear(static_cast<size_t>(linear_bytes));
	for (size_t i = 0; i < linear.size(); ++i)
	{
		linear[i] = static_cast<uint8_t>((i * 17u + 31u) & 0xffu);
	}

	std::vector<uint8_t> tiled(static_cast<size_t>(tiled_bytes), 0);
	FillTiledFromLinear(&tiled, linear, width, height, pitch, bpp, layout);

	std::vector<uint8_t> ref(static_cast<size_t>(linear_bytes), 0xAAu);
	std::vector<uint8_t> prod(static_cast<size_t>(linear_bytes), 0xBBu);
	std::vector<uint8_t> compute(static_cast<size_t>(linear_bytes), 0xCCu);

	const auto ref_req     = MakeRequest(ref.data(), tiled.data(), width, height, pitch, width, bpp, layout, tiled_bytes);
	const auto prod_req    = MakeRequest(prod.data(), tiled.data(), width, height, pitch, width, bpp, layout, tiled_bytes);
	const auto compute_req = MakeRequest(compute.data(), tiled.data(), width, height, pitch, width, bpp, layout, tiled_bytes);

	ASSERT_TRUE(TileDetileIsSupported(ref_req));
	ASSERT_TRUE(TileDetileReference(ref_req));
	ASSERT_TRUE(TileDetile(prod_req));
	ASSERT_TRUE(TileDetileComputeStyle(compute_req));

	EXPECT_EQ(ref, linear);
	EXPECT_EQ(prod, ref);
	EXPECT_EQ(compute, ref);

	// GPU diagnostics without a live device must report their exact strict
	// failure; the host detile remains canonical.
	EXPECT_EQ(TileGpuDetile(nullptr, prod_req), TileGpuDetileStatus::ContextUnavailable);
	TileGpuDetileImageCopy no_image_copy {};
	EXPECT_EQ(TileGpuDetileToImage(nullptr, prod_req, nullptr, no_image_copy, 0), TileGpuDetileStatus::InvalidRequest);
}

void InitializeProductionUploadTestGlobals()
{
	static std::once_flag initialized;
	std::call_once(
	    initialized,
	    []
	    {
		    if (g_gpu_memory == nullptr)
		    {
			    GpuMemoryInit();
		    }
		    // UtilFillImage waits through CommandBuffer::WaitForFence(), which
		    // drains completed label callbacks even when this upload has none.
		    LabelInit();
	    });
}

class VulkanDetileTestContext
{
public:
	~VulkanDetileTestContext()
	{
		if (context.device != VK_NULL_HANDLE)
		{
			if (renderer_context_bound)
			{
				(void)vkDeviceWaitIdle(context.device);
				EXIT_IF(!ReleaseProductionUploadPath());
			}
			if (context.gpu_detile_context != nullptr)
			{
				(void)vkDeviceWaitIdle(context.device);
				(void)TileGpuDetileReleaseContext(&context);
			}
			vkDestroyDevice(context.device, nullptr);
		}
		if (instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(instance, nullptr);
		}
	}

	[[nodiscard]] bool InitializeProductionUploadPath()
	{
		if (context.device == VK_NULL_HANDLE)
		{
			return false;
		}
		InitializeProductionUploadTestGlobals();
		if (!renderer_context_bound)
		{
			renderer_context_bound = GraphicsRenderBindContextForTesting(&context);
		}
		return renderer_context_bound;
	}

	[[nodiscard]] bool ReleaseProductionUploadPath()
	{
		if (!renderer_context_bound)
		{
			return true;
		}
		if (!GraphicsRenderUnbindContextForTesting(&context))
		{
			return false;
		}
		renderer_context_bound = false;
		return true;
	}

	[[nodiscard]] bool Initialize()
	{
		VkApplicationInfo app_info {};
		app_info.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app_info.apiVersion = VK_API_VERSION_1_0;

		const char* validation_layer = nullptr;
		uint32_t    layer_count      = 0;
		if (vkEnumerateInstanceLayerProperties(&layer_count, nullptr) == VK_SUCCESS && layer_count != 0u)
		{
			std::vector<VkLayerProperties> layers(layer_count);
			if (vkEnumerateInstanceLayerProperties(&layer_count, layers.data()) == VK_SUCCESS)
			{
				for (const auto& layer: layers)
				{
					if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
					{
						validation_layer = "VK_LAYER_KHRONOS_validation";
						break;
					}
				}
			}
		}

		VkInstanceCreateInfo instance_info {};
		instance_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance_info.pApplicationInfo = &app_info;
		if (validation_layer != nullptr)
		{
			instance_info.enabledLayerCount = 1;
			instance_info.ppEnabledLayerNames = &validation_layer;
		}
		if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
		{
			return false;
		}

		uint32_t physical_device_count = 0;
		if (vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr) != VK_SUCCESS || physical_device_count == 0u)
		{
			return false;
		}
		std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
		if (vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.data()) != VK_SUCCESS)
		{
			return false;
		}

		for (const auto physical_device: physical_devices)
		{
			uint32_t queue_family_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
			std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());
			for (uint32_t family = 0; family < queue_family_count; ++family)
			{
				const auto& queue_family = queue_families[family];
				if (queue_family.queueCount == 0u || (queue_family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
				                                         (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
				{
					continue;
				}

				constexpr float         priority = 1.0f;
				VkDeviceQueueCreateInfo queue_info {};
				queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				queue_info.queueFamilyIndex = family;
				queue_info.queueCount       = 1;
				queue_info.pQueuePriorities = &priority;
				VkDeviceCreateInfo device_info {};
				device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
				device_info.queueCreateInfoCount = 1;
				device_info.pQueueCreateInfos    = &queue_info;
				VkDevice device                  = VK_NULL_HANDLE;
				if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS)
				{
					continue;
				}

				VkQueue queue = VK_NULL_HANDLE;
				vkGetDeviceQueue(device, family, 0, &queue);
				if (queue == VK_NULL_HANDLE)
				{
					vkDestroyDevice(device, nullptr);
					continue;
				}

				context.instance                                   = instance;
				context.physical_device                            = physical_device;
				context.device                                     = device;
					context.queues[GraphicContext::QUEUE_GFX].vk_queue = queue;
					context.queues[GraphicContext::QUEUE_GFX].family   = family;
					context.queues[GraphicContext::QUEUE_GFX].index    = 0;
					context.queues[GraphicContext::QUEUE_GFX].mutex    = &context.queue_mutexes[0];
					context.queues[GraphicContext::QUEUE_UTIL].vk_queue = queue;
					context.queues[GraphicContext::QUEUE_UTIL].family   = family;
					context.queues[GraphicContext::QUEUE_UTIL].index    = 0;
					context.queues[GraphicContext::QUEUE_UTIL].mutex    = &context.queue_mutexes[0];
					context.queue_mutex_count                           = 1;
					return true;
				}
		}
		return false;
	}

	GraphicContext context {};

private:
	VkInstance instance               = VK_NULL_HANDLE;
	bool       renderer_context_bound = false;
};

bool FindTestMemoryType(GraphicContext* context, uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t* memory_type)
{
	if (context == nullptr || context->physical_device == VK_NULL_HANDLE || memory_type == nullptr)
	{
		return false;
	}
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(context->physical_device, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((type_bits & (static_cast<uint32_t>(1u) << index)) != 0u &&
		    (properties.memoryTypes[index].propertyFlags & required) == required)
		{
			*memory_type = index;
			return true;
		}
	}
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((type_bits & (static_cast<uint32_t>(1u) << index)) != 0u)
		{
			*memory_type = index;
			return true;
		}
	}
	return false;
}

class ScopedBc1UploadImage
{
public:
	explicit ScopedBc1UploadImage(GraphicContext* context): m_context(context) {}
	~ScopedBc1UploadImage() { Reset(); }

	KYTY_CLASS_NO_COPY(ScopedBc1UploadImage);

	[[nodiscard]] bool Create()
	{
		if (m_context == nullptr || m_context->device == VK_NULL_HANDLE)
		{
			return false;
		}
		m_image.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
		m_image.SetNativeExtent(7u, 5u);

		VkImageCreateInfo image_info {};
		image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.imageType     = VK_IMAGE_TYPE_2D;
		image_info.format        = m_image.format;
		image_info.extent        = {7u, 5u, 1u};
		image_info.mipLevels     = 1;
		image_info.arrayLayers   = 1;
		image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(m_context->device, &image_info, nullptr, &m_image.image) != VK_SUCCESS || m_image.image == VK_NULL_HANDLE)
		{
			m_image.image = VK_NULL_HANDLE;
			return false;
		}

		vkGetImageMemoryRequirements(m_context->device, m_image.image, &m_image.memory.requirements);
		uint32_t memory_type = 0;
		if (!FindTestMemoryType(m_context, m_image.memory.requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type))
		{
			Reset();
			return false;
		}
		VkMemoryAllocateInfo allocate_info {};
		allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate_info.allocationSize  = m_image.memory.requirements.size;
		allocate_info.memoryTypeIndex = memory_type;
		if (vkAllocateMemory(m_context->device, &allocate_info, nullptr, &m_image.memory.memory) != VK_SUCCESS ||
		    m_image.memory.memory == VK_NULL_HANDLE)
		{
			Reset();
			return false;
		}
		m_image.memory.offset = 0;
		m_image.memory.type   = memory_type;
		if (vkBindImageMemory(m_context->device, m_image.image, m_image.memory.memory, m_image.memory.offset) != VK_SUCCESS)
		{
			Reset();
			return false;
		}
		m_image.usage = image_info.usage;
		return true;
	}

	void Reset()
	{
		if (m_context == nullptr || m_context->device == VK_NULL_HANDLE)
		{
			return;
		}
		if (m_image.image != VK_NULL_HANDLE)
		{
			vkDestroyImage(m_context->device, m_image.image, nullptr);
			m_image.image = VK_NULL_HANDLE;
		}
		if (m_image.memory.memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(m_context->device, m_image.memory.memory, nullptr);
			m_image.memory.memory = VK_NULL_HANDLE;
		}
		m_image.memory = {};
		m_image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	[[nodiscard]] TextureVulkanImage* Get() { return &m_image; }

private:
	GraphicContext*     m_context = nullptr;
	TextureVulkanImage m_image;
};

class ScopedVulkanBuffer
{
public:
	explicit ScopedVulkanBuffer(GraphicContext* context): m_context(context) {}
	~ScopedVulkanBuffer() { Reset(); }

	KYTY_CLASS_NO_COPY(ScopedVulkanBuffer);

	[[nodiscard]] VulkanBuffer* Get() { return &m_buffer; }

	void Reset()
	{
		if (m_context != nullptr && m_context->device != VK_NULL_HANDLE && m_buffer.buffer != VK_NULL_HANDLE)
		{
			VulkanDeleteBuffer(m_context, &m_buffer);
		}
	}

private:
	GraphicContext* m_context = nullptr;
	VulkanBuffer    m_buffer;
};

class ScopedGpuDetileTestFault
{
public:
	explicit ScopedGpuDetileTestFault(TileGpuDetileTestFault fault)
	{
		TileGpuDetileSetTestFaultForTesting(fault);
	}

	~ScopedGpuDetileTestFault()
	{
		TileGpuDetileSetTestFaultForTesting(TileGpuDetileTestFault::None);
	}

	KYTY_CLASS_NO_COPY(ScopedGpuDetileTestFault);
};

struct GpuDiagnosticFixture
{
	static constexpr uint32_t k_width     = 32u;
	static constexpr uint32_t k_height    = 16u;
	static constexpr uint32_t k_dst_pitch = 36u;
	static constexpr uint32_t k_bpp       = 4u;

	GpuDiagnosticFixture()
	{
		TileGetRenderTargetSize(k_width, k_height, k_width, 0x1bu, k_bpp, &tile_size);
		canonical.resize(static_cast<size_t>(k_width) * k_height * k_bpp);
		for (size_t index = 0; index < canonical.size(); ++index)
		{
			canonical[index] = static_cast<uint8_t>((index * 29u + 7u) & 0xffu);
		}
		tiled.resize(tile_size.size, 0u);
		FillTiledFromLinear(&tiled, canonical, k_width, k_height, k_width, k_bpp, TileDetileLayout::Sw64kRx);
		output.assign(static_cast<size_t>(k_dst_pitch) * k_height * k_bpp, 0xA5u);
		request = MakeRequest(output.data(), tiled.data(), k_width, k_height, k_width, k_dst_pitch, k_bpp,
		                      TileDetileLayout::Sw64kRx, tile_size.size);
	}

	TileSizeAlign              tile_size {};
	std::vector<uint8_t>       canonical;
	std::vector<uint8_t>       tiled;
	std::vector<uint8_t>       output;
	TileDetileRequest          request {};
};

} // namespace

TEST(EmulatorTileDetile, Sw64kRx4bppProductionMatchesReference)
{
	constexpr uint32_t k_w   = 160u;
	constexpr uint32_t k_h   = 96u;
	constexpr uint32_t k_bpp = 4u;
	TileSizeAlign      size {};
	TileGetRenderTargetSize(k_w, k_h, k_w, 0x1b, k_bpp, &size);
	ASSERT_GT(size.size, 0u);
	ExpectDetilePathsMatch(k_w, k_h, k_w, k_bpp, TileDetileLayout::Sw64kRx, size.size);
}

TEST(EmulatorTileDetile, Sw64kRx8bppProductionMatchesReference)
{
	constexpr uint32_t k_w   = 128u;
	constexpr uint32_t k_h   = 64u;
	constexpr uint32_t k_bpp = 8u;
	TileSizeAlign      size {};
	TileGetRenderTargetSize(k_w, k_h, k_w, 0x1b, k_bpp, &size);
	ASSERT_GT(size.size, 0u);
	ExpectDetilePathsMatch(k_w, k_h, k_w, k_bpp, TileDetileLayout::Sw64kRx, size.size);
}

TEST(EmulatorTileDetile, Standard64KB4bppProductionMatchesReference)
{
	constexpr uint32_t k_w   = 96u;
	constexpr uint32_t k_h   = 80u;
	constexpr uint32_t k_bpp = 4u;
	const uint32_t     pitch = TileAlign64KBPitch(k_w, k_bpp);
	ASSERT_NE(pitch, 0u);
	const uint32_t block_w  = TileGet64KBBlockWidth(k_bpp);
	const uint32_t block_h  = 65536u / (block_w * k_bpp);
	const uint32_t blocks_x = pitch / block_w;
	const uint32_t blocks_y = (k_h + block_h - 1u) / block_h;
	const uint64_t tiled    = static_cast<uint64_t>(blocks_x) * blocks_y * 65536u;
	ExpectDetilePathsMatch(k_w, k_h, pitch, k_bpp, TileDetileLayout::Standard64KB, tiled);
}

TEST(EmulatorTileDetile, Standard4KB4bppProductionMatchesReference)
{
	constexpr uint32_t k_w      = 40u;
	constexpr uint32_t k_h      = 36u;
	constexpr uint32_t k_bpp    = 4u;
	constexpr uint32_t pitch    = 64u;
	const uint32_t     blocks_x = (pitch + 31u) / 32u;
	const uint32_t     blocks_y = (k_h + 31u) / 32u;
	const uint64_t     tiled    = static_cast<uint64_t>(blocks_x) * blocks_y * 4096u;
	ExpectDetilePathsMatch(k_w, k_h, pitch, k_bpp, TileDetileLayout::Standard4KB, tiled);
}

TEST(EmulatorTileDetile, Standard4KBContiguousProductionPreservesDestinationPitch)
{
	constexpr uint32_t k_width     = 40u;
	constexpr uint32_t k_height    = 36u;
	constexpr uint32_t k_src_pitch = 64u;
	constexpr uint32_t k_dst_pitch = 48u;
	constexpr uint32_t k_bpp       = 4u;
	const uint64_t     tiled_bytes = static_cast<uint64_t>((k_src_pitch + 31u) / 32u) * ((k_height + 31u) / 32u) * 4096u;

	std::vector<uint8_t> canonical(static_cast<size_t>(k_width) * k_height * k_bpp);
	for (size_t index = 0; index < canonical.size(); ++index)
	{
		canonical[index] = static_cast<uint8_t>((index * 7u + 19u) & 0xffu);
	}
	std::vector<uint8_t> tiled(static_cast<size_t>(tiled_bytes), 0u);
	FillTiledFromLinear(&tiled, canonical, k_width, k_height, k_src_pitch, k_bpp, TileDetileLayout::Standard4KB);

	std::vector<uint8_t> reference(static_cast<size_t>(k_dst_pitch) * k_height * k_bpp, 0xA5u);
	std::vector<uint8_t> production(reference);
	const auto ref_request = MakeRequest(reference.data(), tiled.data(), k_width, k_height, k_src_pitch, k_dst_pitch, k_bpp,
	                                     TileDetileLayout::Standard4KB, tiled_bytes);
	const auto production_request = MakeRequest(production.data(), tiled.data(), k_width, k_height, k_src_pitch, k_dst_pitch, k_bpp,
	                                            TileDetileLayout::Standard4KB, tiled_bytes);
	EXPECT_EQ(TileDetileGetProductionPathForTesting(production_request), TileDetileProductionPath::Standard4KBContiguous);
	ASSERT_TRUE(TileDetileReference(ref_request));
	ASSERT_TRUE(TileDetile(production_request));
	EXPECT_EQ(production, reference);
	for (uint32_t y = 0; y < k_height; ++y)
	{
		const size_t active_offset = static_cast<size_t>(y) * k_dst_pitch * k_bpp;
		EXPECT_EQ(0, std::memcmp(production.data() + active_offset, canonical.data() + static_cast<size_t>(y) * k_width * k_bpp,
		                         static_cast<size_t>(k_width) * k_bpp));
		for (uint32_t x = k_width; x < k_dst_pitch; ++x)
		{
			for (uint32_t byte = 0; byte < k_bpp; ++byte)
			{
				EXPECT_EQ(production[(static_cast<size_t>(y) * k_dst_pitch + x) * k_bpp + byte], 0xA5u);
			}
		}
	}
}

TEST(EmulatorTileDetile, Standard4KBProductionContiguousPathRemainsBounded)
{
	constexpr uint32_t k_width      = 512u;
	constexpr uint32_t k_height     = 256u;
	constexpr uint32_t k_pitch      = 512u;
	constexpr uint32_t k_bpp        = 4u;
	constexpr uint32_t k_iterations = 48u;
	const uint64_t     tiled_bytes  = static_cast<uint64_t>((k_pitch + 31u) / 32u) * ((k_height + 31u) / 32u) * 4096u;
	const uint64_t     linear_bytes = static_cast<uint64_t>(k_width) * k_height * k_bpp;

	std::vector<uint8_t> canonical(static_cast<size_t>(linear_bytes));
	for (size_t index = 0; index < canonical.size(); ++index)
	{
		canonical[index] = static_cast<uint8_t>((index * 13u + 5u) & 0xffu);
	}
	std::vector<uint8_t> tiled(static_cast<size_t>(tiled_bytes), 0u);
	FillTiledFromLinear(&tiled, canonical, k_width, k_height, k_pitch, k_bpp, TileDetileLayout::Standard4KB);

	std::vector<uint8_t> generic(static_cast<size_t>(linear_bytes), 0u);
	std::vector<uint8_t> production(static_cast<size_t>(linear_bytes), 0u);
	const auto generic_request =
		MakeRequest(generic.data(), tiled.data(), k_width, k_height, k_pitch, k_pitch, k_bpp, TileDetileLayout::Standard4KB, tiled_bytes);
	const auto production_request = MakeRequest(production.data(), tiled.data(), k_width, k_height, k_pitch, k_pitch, k_bpp,
	                                             TileDetileLayout::Standard4KB, tiled_bytes);

	// The production selector is the exact branch used by TileDetile; assert it
	// before the bounded counter/timing guard so this test is not timing-only.
	ASSERT_EQ(TileDetileGetProductionPathForTesting(production_request), TileDetileProductionPath::Standard4KBContiguous);
	// The counter proves both bounded loops actually run, while the loose timing
	// bound catches a gross regression without imposing a host-specific
	// microbenchmark threshold.
	static_cast<void>(DebugStatsGetPerformanceSnapshot(/*reset=*/true));
	const auto generic_start = std::chrono::steady_clock::now();
	for (uint32_t iteration = 0; iteration < k_iterations; ++iteration)
	{
		ASSERT_TRUE(TileDetileComputeStyle(generic_request));
	}
	const auto generic_elapsed = std::chrono::steady_clock::now() - generic_start;

	const auto production_start = std::chrono::steady_clock::now();
	for (uint32_t iteration = 0; iteration < k_iterations; ++iteration)
	{
		ASSERT_TRUE(TileDetile(production_request));
	}
	const auto production_elapsed = std::chrono::steady_clock::now() - production_start;

	EXPECT_EQ(generic, canonical);
	EXPECT_EQ(production, generic);
	const auto counters = DebugStatsGetPerformanceSnapshot(/*reset=*/true);
	EXPECT_EQ(counters.detile_calls, static_cast<uint64_t>(k_iterations) * 2u);
	EXPECT_EQ(counters.detile_bytes, linear_bytes * k_iterations * 2u);

	const auto max_elapsed = generic_elapsed * 2 + std::chrono::milliseconds(25);
	EXPECT_LE(std::chrono::duration_cast<std::chrono::nanoseconds>(production_elapsed).count(),
	          std::chrono::duration_cast<std::chrono::nanoseconds>(max_elapsed).count());
}

TEST(EmulatorTileDetile, Depth64KB32ProductionMatchesReference)
{
	constexpr uint32_t k_w   = 128u;
	constexpr uint32_t k_h   = 64u;
	constexpr uint32_t k_bpp = 4u;
	constexpr uint32_t pitch = 128u;
	const uint64_t     tiled = 65536u;
	ExpectDetilePathsMatch(k_w, k_h, pitch, k_bpp, TileDetileLayout::Depth64KB32, tiled);
}

TEST(EmulatorTileDetile, RejectsUnsupportedRequests)
{
	uint8_t dst[16] {};
	uint8_t src[16] {};
	auto    bad = MakeRequest(dst, src, 4, 4, 4, 4, 3, TileDetileLayout::Sw64kRx, 16);
	EXPECT_FALSE(TileDetileIsSupported(bad));
	EXPECT_FALSE(TileDetile(bad));
	EXPECT_FALSE(TileDetileReference(bad));

	// Host detile still requires a destination; geometry alone is supported for GPU→image.
	auto null_dst = MakeRequest(nullptr, src, 4, 4, 4, 4, 4, TileDetileLayout::Sw64kRx, 65536u);
	EXPECT_TRUE(TileDetileIsSupported(null_dst));
	EXPECT_FALSE(TileDetile(null_dst));
	EXPECT_FALSE(TileDetileReference(null_dst));
	EXPECT_EQ(TileGpuDetile(nullptr, null_dst), TileGpuDetileStatus::InvalidRequest);

	auto null_src = MakeRequest(dst, nullptr, 4, 4, 4, 4, 4, TileDetileLayout::Sw64kRx, 65536u);
	EXPECT_FALSE(TileDetileIsSupported(null_src));
	EXPECT_EQ(TileGpuDetile(nullptr, null_src), TileGpuDetileStatus::InvalidRequest);
}

TEST(EmulatorTileDetile, GpuImageCopyValidatesBc1BlocksAgainstTexels)
{
	uint8_t source = 0u;
	// A 7x5 BC1 image occupies a 2x2 grid of 8-byte blocks. The linear
	// buffer row is therefore two BC1 blocks wide even though the copy extent is
	// only seven texels wide.
	const auto request = MakeRequest(nullptr, &source, 2u, 2u, 2u, 2u, 8u, TileDetileLayout::Sw64kRx, 65536u);
	ASSERT_TRUE(TileDetileIsSupported(request));

	TileBc1BufferCopyLayout tight_bc1 {};
	ASSERT_TRUE(TileGetBc1BufferCopyLayout(7u, 5u, 0u, &tight_bc1));
	EXPECT_EQ(tight_bc1.copy_width_blocks, 2u);
	EXPECT_EQ(tight_bc1.copy_height_blocks, 2u);
	EXPECT_EQ(tight_bc1.row_pitch_blocks, 2u);
	EXPECT_EQ(tight_bc1.buffer_row_length_texels, 8u);

	TileGpuDetileImageCopy copy {};
	copy.buffer_row_length_texels = tight_bc1.buffer_row_length_texels;
	copy.copy_width_texels        = 7u;
	copy.copy_height_texels       = 5u;
	copy.texels_per_element_x     = 4u;
	copy.texels_per_element_y     = 4u;
	EXPECT_TRUE(TileGpuDetileImageCopyIsSupported(request, copy, 7u, 5u));

	copy.buffer_row_length_texels = 0u;
	// Vulkan's zero row length resolves to ceil(7 / 4) == 2 blocks, matching
	// the tight two-element destination pitch.
	EXPECT_TRUE(TileGpuDetileImageCopyIsSupported(request, copy, 7u, 5u));
	const auto padded_request = MakeRequest(nullptr, &source, 2u, 2u, 2u, 3u, 8u, TileDetileLayout::Sw64kRx, 65536u);
	ASSERT_TRUE(TileDetileIsSupported(padded_request));
	EXPECT_FALSE(TileGpuDetileImageCopyIsSupported(padded_request, copy, 7u, 5u));
	copy.buffer_row_length_texels = 12u;
	EXPECT_TRUE(TileGpuDetileImageCopyIsSupported(padded_request, copy, 7u, 5u));

	copy.buffer_row_length_texels = 7u;
	EXPECT_FALSE(TileGpuDetileImageCopyIsSupported(request, copy, 7u, 5u));

	copy.buffer_row_length_texels = 8u;
	copy.copy_width_texels        = 8u;
	EXPECT_FALSE(TileGpuDetileImageCopyIsSupported(request, copy, 7u, 5u));

	copy.copy_width_texels    = 7u;
	copy.texels_per_element_y = 1u;
	EXPECT_FALSE(TileGpuDetileImageCopyIsSupported(request, copy, 7u, 5u));
}

TEST(EmulatorTileDetile, Bc1BufferCopyLayoutRoundsProductionRowsToBlocks)
{
	TileBc1BufferCopyLayout tight {};
	ASSERT_TRUE(TileGetBc1BufferCopyLayout(7u, 5u, 0u, &tight));
	EXPECT_EQ(tight.copy_width_blocks, 2u);
	EXPECT_EQ(tight.copy_height_blocks, 2u);
	EXPECT_EQ(tight.row_pitch_blocks, 2u);
	EXPECT_EQ(tight.buffer_row_length_texels, 8u);

	TileBc1BufferCopyLayout padded {};
	ASSERT_TRUE(TileGetBc1BufferCopyLayout(7u, 5u, 9u, &padded));
	EXPECT_EQ(padded.copy_width_blocks, 2u);
	EXPECT_EQ(padded.copy_height_blocks, 2u);
	EXPECT_EQ(padded.row_pitch_blocks, 3u);
	EXPECT_EQ(padded.buffer_row_length_texels, 12u);
	EXPECT_FALSE(TileGetBc1BufferCopyLayout(7u, 5u, 6u, &padded));
}

TEST(EmulatorTileDetile, ProductionBc1UploadRoundTripsPaddedBlockRows)
{
	VulkanDetileTestContext vulkan {};
	if (!vulkan.Initialize())
	{
		GTEST_SKIP() << "no Vulkan graphics+compute device is available";
	}

	VkFormatProperties format_properties {};
	vkGetPhysicalDeviceFormatProperties(vulkan.context.physical_device, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &format_properties);
	const VkFormatFeatureFlags required_features = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if ((format_properties.optimalTilingFeatures & required_features) != required_features)
	{
		GTEST_SKIP() << "Vulkan device does not support BC1 optimal-tiling transfer copies";
	}
	ASSERT_TRUE(vulkan.InitializeProductionUploadPath());

	TileBc1BufferCopyLayout layout {};
	ASSERT_TRUE(TileGetBc1BufferCopyLayout(7u, 5u, 9u, &layout));
	EXPECT_EQ(layout.copy_width_blocks, 2u);
	EXPECT_EQ(layout.copy_height_blocks, 2u);
	EXPECT_EQ(layout.row_pitch_blocks, 3u);
	EXPECT_EQ(layout.buffer_row_length_texels, 12u);

	// Guest pitch is nine texels. The production Texture path resolves that to
	// three BC1 blocks (12 texels) per row. Give each copied block a distinct
	// byte pattern and make the third source block a different padding sentinel.
	constexpr uint8_t k_upload_padding   = 0xD3u;
	constexpr uint8_t k_readback_padding = 0x5Au;
	constexpr std::array<uint8_t, 8> k_block_00 {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u};
	constexpr std::array<uint8_t, 8> k_block_01 {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u};
	constexpr std::array<uint8_t, 8> k_block_10 {0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 0x36u, 0x37u};
	constexpr std::array<uint8_t, 8> k_block_11 {0x40u, 0x41u, 0x42u, 0x43u, 0x44u, 0x45u, 0x46u, 0x47u};
	constexpr size_t                  k_row_bytes = 24u;
	std::array<uint8_t, 48>          upload {};
	upload.fill(k_upload_padding);
	std::memcpy(upload.data() + 0u, k_block_00.data(), k_block_00.size());
	std::memcpy(upload.data() + 8u, k_block_01.data(), k_block_01.size());
	std::memcpy(upload.data() + k_row_bytes, k_block_10.data(), k_block_10.size());
	std::memcpy(upload.data() + k_row_bytes + 8u, k_block_11.data(), k_block_11.size());

	{
		ScopedBc1UploadImage image(&vulkan.context);
		ASSERT_TRUE(image.Create());

		Vector<BufferImageCopy> regions(1);
		regions[0].offset          = 0u;
		regions[0].pitch           = 12u; // VkBufferImageCopy::bufferRowLength, in texels.
		regions[0].dst_level       = 0u;
		regions[0].width           = 7u;
		regions[0].height          = 5u;
		regions[0].depth           = 1u;
		regions[0].dst_array_layer = 0u;
		regions[0].dst_x           = 0;
		regions[0].dst_y           = 0;
		regions[0].dst_z           = 0;
		UtilFillImage(&vulkan.context, image.Get(), upload.data(), upload.size(), regions,
		              static_cast<uint64_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

		ScopedVulkanBuffer readback(&vulkan.context);
		readback.Get()->usage           = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		readback.Get()->memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VulkanCreateBuffer(&vulkan.context, upload.size(), readback.Get());
		ASSERT_NE(readback.Get()->buffer, VK_NULL_HANDLE);
		ASSERT_NE(readback.Get()->memory.memory, VK_NULL_HANDLE);
		void* mapped = nullptr;
		VulkanMapMemory(&vulkan.context, &readback.Get()->memory, &mapped);
		ASSERT_NE(mapped, nullptr);
		std::memset(mapped, k_readback_padding, upload.size());
		VulkanUnmapMemory(&vulkan.context, &readback.Get()->memory);

		{
			CommandBuffer command(GraphicContext::QUEUE_UTIL);
			ASSERT_FALSE(command.IsInvalid());
			command.Begin();
			UtilImageToBuffer(&command, image.Get(), readback.Get(), 12u, static_cast<uint64_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
			command.End();
			command.Execute();
			command.WaitForFence();
		}

		std::array<uint8_t, 48> actual {};
		VulkanMapMemory(&vulkan.context, &readback.Get()->memory, &mapped);
		ASSERT_NE(mapped, nullptr);
		std::memcpy(actual.data(), mapped, actual.size());
		VulkanUnmapMemory(&vulkan.context, &readback.Get()->memory);

		std::array<uint8_t, 48> expected {};
		expected.fill(k_readback_padding);
		std::memcpy(expected.data() + 0u, k_block_00.data(), k_block_00.size());
		std::memcpy(expected.data() + 8u, k_block_01.data(), k_block_01.size());
		std::memcpy(expected.data() + k_row_bytes, k_block_10.data(), k_block_10.size());
		std::memcpy(expected.data() + k_row_bytes + 8u, k_block_11.data(), k_block_11.size());
		EXPECT_EQ(actual, expected);
	}
	EXPECT_TRUE(vulkan.ReleaseProductionUploadPath());
}

TEST(EmulatorTileDetile, GpuImagePathIsStrictlyUnsupportedWithoutCreatingASession)
{
	uint8_t    source  = 0u;
	const auto request = MakeRequest(nullptr, &source, 2u, 2u, 2u, 2u, 8u, TileDetileLayout::Sw64kRx, 65536u);

	TileGpuDetileImageCopy copy {};
	copy.buffer_row_length_texels = 8u;
	copy.copy_width_texels        = 7u;
	copy.copy_height_texels       = 5u;
	copy.texels_per_element_x     = 4u;
	copy.texels_per_element_y     = 4u;
	ASSERT_TRUE(TileGpuDetileImageCopyIsSupported(request, copy, 7u, 5u));

	TextureVulkanImage image {};
	image.image = reinterpret_cast<VkImage>(0x1);
	image.SetHostExtent(7u, 5u);

	GraphicContext context {};
	context.device          = reinterpret_cast<VkDevice>(0x1);
	context.physical_device = reinterpret_cast<VkPhysicalDevice>(0x1);
	EXPECT_EQ(TileGpuDetileToImage(&context, request, &image, copy, 0), TileGpuDetileStatus::ImagePathUnsupported);
	EXPECT_EQ(context.gpu_detile_context, nullptr);
}

TEST(EmulatorTileDetile, GpuDiagnosticSessionCapRejectsBeforeContextCreation)
{
	// This is a pure validation test: the fake pointers must never be
	// dereferenced because the paired source/destination allocation exceeds the
	// conservative diagnostic-session cap before any Vulkan query or allocation.
	uint8_t source = 0u;
	uint8_t target = 0u;
	const auto request = MakeRequest(&target, &source, 128u, 32768u, 128u, 128u, 4u, TileDetileLayout::Sw64kRx,
	                                 16u * 1024u * 1024u);
	ASSERT_TRUE(TileDetileIsSupported(request));

	GraphicContext context {};
	context.device          = reinterpret_cast<VkDevice>(0x1);
	context.physical_device = reinterpret_cast<VkPhysicalDevice>(0x1);
	EXPECT_EQ(TileGpuDetile(&context, request), TileGpuDetileStatus::DiagnosticCapacityExceeded);
	EXPECT_EQ(context.gpu_detile_context, nullptr);
}

TEST(EmulatorTileDetile, GpuDiagnosticMatchesCpuAndReusesContextSessionWhenVulkanAvailable)
{
	VulkanDetileTestContext vulkan {};
	if (!vulkan.Initialize())
	{
		GTEST_SKIP() << "no Vulkan graphics+compute device is available";
	}

	constexpr uint32_t k_width     = 32u;
	constexpr uint32_t k_height    = 16u;
	constexpr uint32_t k_dst_pitch = 36u;
	constexpr uint32_t k_bpp       = 4u;
	TileSizeAlign      tile_size {};
	TileGetRenderTargetSize(k_width, k_height, k_width, 0x1bu, k_bpp, &tile_size);
	ASSERT_GT(tile_size.size, 0u);

	std::vector<uint8_t> canonical(static_cast<size_t>(k_width) * k_height * k_bpp);
	for (size_t index = 0; index < canonical.size(); ++index)
	{
		canonical[index] = static_cast<uint8_t>((index * 29u + 7u) & 0xffu);
	}
	std::vector<uint8_t> tiled(tile_size.size, 0u);
	FillTiledFromLinear(&tiled, canonical, k_width, k_height, k_width, k_bpp, TileDetileLayout::Sw64kRx);

	std::vector<uint8_t> expected(static_cast<size_t>(k_dst_pitch) * k_height * k_bpp, 0xA5u);
	auto                 expected_request = MakeRequest(expected.data(), tiled.data(), k_width, k_height, k_width, k_dst_pitch, k_bpp,
	                                                    TileDetileLayout::Sw64kRx, tile_size.size);
	ASSERT_TRUE(TileDetileReference(expected_request));

	std::vector<uint8_t> actual(expected.size(), 0xA5u);
	auto                 actual_request =
	    MakeRequest(actual.data(), tiled.data(), k_width, k_height, k_width, k_dst_pitch, k_bpp, TileDetileLayout::Sw64kRx, tile_size.size);
	ASSERT_EQ(TileGpuDetile(&vulkan.context, actual_request), TileGpuDetileStatus::Success);
	// This is a live GPU→host readback comparison over the full pitched output,
	// including sentinel padding that the compute kernel must not overwrite.
	EXPECT_EQ(actual, expected);
	const auto* first_session = vulkan.context.gpu_detile_context;
	ASSERT_NE(first_session, nullptr);

	std::fill(expected.begin(), expected.end(), 0x5Au);
	expected_request.dst = expected.data();
	ASSERT_TRUE(TileDetileReference(expected_request));
	std::fill(actual.begin(), actual.end(), 0x5Au);
	actual_request.dst = actual.data();
	ASSERT_EQ(TileGpuDetile(&vulkan.context, actual_request), TileGpuDetileStatus::Success);
	EXPECT_EQ(actual, expected);
	EXPECT_EQ(vulkan.context.gpu_detile_context, first_session);

	ASSERT_TRUE(TileGpuDetileReleaseContext(&vulkan.context));
	EXPECT_EQ(vulkan.context.gpu_detile_context, nullptr);
}

TEST(EmulatorTileDetile, GpuDiagnosticRetainedCapacityCapRejectsComplementaryGrowth)
{
	VulkanDetileTestContext vulkan {};
	if (!vulkan.Initialize())
	{
		GTEST_SKIP() << "no Vulkan graphics+compute device is available";
	}

	// Each request is exactly 16 MiB, but retaining the larger source capacity
	// from one and the larger linear capacity from the other would retain 30 MiB.
	constexpr uint32_t k_width            = 128u;
	constexpr uint32_t k_height           = 2048u;
	constexpr uint32_t k_bpp              = 4u;
	constexpr uint32_t k_source_heavy_pitch = 1920u;
	constexpr uint64_t k_one_mib          = 1024u * 1024u;
	constexpr uint64_t k_source_heavy_bytes = 15u * k_one_mib;
	constexpr uint64_t k_linear_heavy_bytes = 15u * k_one_mib;

	std::vector<uint8_t> canonical(static_cast<size_t>(k_one_mib));
	for (size_t index = 0; index < canonical.size(); ++index)
	{
		canonical[index] = static_cast<uint8_t>((index * 11u + 23u) & 0xffu);
	}
	std::vector<uint8_t> tiled(static_cast<size_t>(k_source_heavy_bytes), 0u);
	FillTiledFromLinear(&tiled, canonical, k_width, k_height, k_source_heavy_pitch, k_bpp, TileDetileLayout::Sw64kRx);

	std::vector<uint8_t> expected(canonical.size(), 0xA5u);
	std::vector<uint8_t> actual(canonical.size(), 0xA5u);
	auto expected_request = MakeRequest(expected.data(), tiled.data(), k_width, k_height, k_source_heavy_pitch, k_width, k_bpp,
	                                    TileDetileLayout::Sw64kRx, k_source_heavy_bytes);
	auto first_request = MakeRequest(actual.data(), tiled.data(), k_width, k_height, k_source_heavy_pitch, k_width, k_bpp,
	                                 TileDetileLayout::Sw64kRx, k_source_heavy_bytes);
	ASSERT_TRUE(TileDetileIsSupported(first_request));
	ASSERT_TRUE(TileDetileReference(expected_request));
	ASSERT_EQ(TileGpuDetile(&vulkan.context, first_request), TileGpuDetileStatus::Success);
	EXPECT_EQ(actual, expected);
	const auto* first_session = vulkan.context.gpu_detile_context;
	ASSERT_NE(first_session, nullptr);

	std::vector<uint8_t> complementary_destination(static_cast<size_t>(k_linear_heavy_bytes), 0xA5u);
	const auto complementary_request = MakeRequest(complementary_destination.data(), tiled.data(), k_width, k_height, k_width,
	                                               k_source_heavy_pitch, k_bpp, TileDetileLayout::Sw64kRx, k_one_mib);
	ASSERT_TRUE(TileDetileIsSupported(complementary_request));
	{
		// If the prospective-capacity gate moved after allocation, this injected
		// memory failure would instead be observed as ResourceUnavailable.
		ScopedGpuDetileTestFault fault(TileGpuDetileTestFault::AllocateMemory);
		EXPECT_EQ(TileGpuDetile(&vulkan.context, complementary_request), TileGpuDetileStatus::DiagnosticCapacityExceeded);
	}
	EXPECT_EQ(vulkan.context.gpu_detile_context, first_session);

	std::fill(actual.begin(), actual.end(), 0x5Au);
	std::fill(expected.begin(), expected.end(), 0x5Au);
	expected_request.dst = expected.data();
	ASSERT_TRUE(TileDetileReference(expected_request));
	first_request.dst = actual.data();
	ASSERT_EQ(TileGpuDetile(&vulkan.context, first_request), TileGpuDetileStatus::Success);
	EXPECT_EQ(actual, expected);
	EXPECT_EQ(vulkan.context.gpu_detile_context, first_session);
	EXPECT_TRUE(TileGpuDetileReleaseContext(&vulkan.context));
	EXPECT_EQ(vulkan.context.gpu_detile_context, nullptr);
}

TEST(EmulatorTileDetile, GpuDiagnosticPartialInitializationReleaseIsIdempotent)
{
	VulkanDetileTestContext vulkan {};
	if (!vulkan.Initialize())
	{
		GTEST_SKIP() << "no Vulkan graphics+compute device is available";
	}
	GpuDiagnosticFixture fixture {};
	ASSERT_GT(fixture.tile_size.size, 0u);
	ASSERT_TRUE(TileDetileIsSupported(fixture.request));

	{
		ScopedGpuDetileTestFault fault(TileGpuDetileTestFault::CreateFence);
		EXPECT_EQ(TileGpuDetile(&vulkan.context, fixture.request), TileGpuDetileStatus::ResourceUnavailable);
	}
	ASSERT_NE(vulkan.context.gpu_detile_context, nullptr);
	EXPECT_TRUE(TileGpuDetileReleaseContext(&vulkan.context));
	EXPECT_EQ(vulkan.context.gpu_detile_context, nullptr);
	// A second release is a no-op, proving partial-init cleanup cleared every
	// handle/pool relationship before the owning state was deleted.
	EXPECT_TRUE(TileGpuDetileReleaseContext(&vulkan.context));

	// Fail after a VkBuffer exists but before its memory is allocated. The
	// candidate buffer must be destroyed and the reusable context must remain
	// explicitly releasable rather than carrying a half-owned allocation.
	{
		ScopedGpuDetileTestFault fault(TileGpuDetileTestFault::AllocateMemory);
		EXPECT_EQ(TileGpuDetile(&vulkan.context, fixture.request), TileGpuDetileStatus::ResourceUnavailable);
	}
	ASSERT_NE(vulkan.context.gpu_detile_context, nullptr);
	EXPECT_TRUE(TileGpuDetileReleaseContext(&vulkan.context));
	EXPECT_EQ(vulkan.context.gpu_detile_context, nullptr);
}

TEST(EmulatorTileDetile, GpuDiagnosticSubmissionLifecycleRetainsTimedOutSession)
{
	VulkanDetileTestContext vulkan {};
	if (!vulkan.Initialize())
	{
		GTEST_SKIP() << "no Vulkan graphics+compute device is available";
	}
	GpuDiagnosticFixture fixture {};
	ASSERT_GT(fixture.tile_size.size, 0u);
	ASSERT_TRUE(TileDetileIsSupported(fixture.request));

	// Failing immediately before submission must leave in_flight clear, so the
	// session is safe to release without waiting.
	{
		ScopedGpuDetileTestFault fault(TileGpuDetileTestFault::ResetFence);
		EXPECT_EQ(TileGpuDetile(&vulkan.context, fixture.request), TileGpuDetileStatus::FenceFailed);
	}
	ASSERT_NE(vulkan.context.gpu_detile_context, nullptr);
	EXPECT_TRUE(TileGpuDetileReleaseContext(&vulkan.context));
	EXPECT_EQ(vulkan.context.gpu_detile_context, nullptr);

	// A forced host wait timeout happens only after successful submission.
	// Finish the isolated queue, then submit again to exercise the signaled
	// fence recycle path (command buffer reset before the next fence reset).
	{
		ScopedGpuDetileTestFault fault(TileGpuDetileTestFault::WaitTimeout);
		EXPECT_EQ(TileGpuDetile(&vulkan.context, fixture.request), TileGpuDetileStatus::FenceTimeout);
	}
	ASSERT_NE(vulkan.context.gpu_detile_context, nullptr);
	{
		Core::LockGuard queue_lock(*vulkan.context.queues[GraphicContext::QUEUE_GFX].mutex);
		ASSERT_EQ(vkQueueWaitIdle(vulkan.context.queues[GraphicContext::QUEUE_GFX].vk_queue), VK_SUCCESS);
	}
	EXPECT_EQ(TileGpuDetile(&vulkan.context, fixture.request), TileGpuDetileStatus::Success);

	{
		ScopedGpuDetileTestFault fault(TileGpuDetileTestFault::WaitTimeout);
		EXPECT_EQ(TileGpuDetile(&vulkan.context, fixture.request), TileGpuDetileStatus::FenceTimeout);
	}
	const auto* retained_session = vulkan.context.gpu_detile_context;
	ASSERT_NE(retained_session, nullptr);
	{
		ScopedGpuDetileTestFault fault(TileGpuDetileTestFault::ReleaseTimeout);
		EXPECT_FALSE(TileGpuDetileReleaseContext(&vulkan.context));
	}
	EXPECT_EQ(vulkan.context.gpu_detile_context, retained_session);
	EXPECT_TRUE(TileGpuDetileReleaseContext(&vulkan.context));
	EXPECT_EQ(vulkan.context.gpu_detile_context, nullptr);
}

TEST(EmulatorTileDetile, RejectsTruncatedAndOverflowingBufferRanges)
{
	constexpr uint32_t k_w   = 64u;
	constexpr uint32_t k_h   = 48u;
	constexpr uint32_t k_bpp = 4u;
	TileSizeAlign      size {};
	TileGetRenderTargetSize(k_w, k_h, k_w, 0x1bu, k_bpp, &size);
	ASSERT_GT(size.size, 0u);

	// Keep the physical allocation intact: only the guest-visible range is truncated.
	std::vector<uint8_t> tiled(size.size, 0u);
	std::vector<uint8_t> linear(static_cast<size_t>(k_w) * k_h * k_bpp, 0u);
	auto truncated = MakeRequest(linear.data(), tiled.data(), k_w, k_h, k_w, k_w, k_bpp, TileDetileLayout::Sw64kRx, size.size - 1u);
	EXPECT_FALSE(TileDetileIsSupported(truncated));
	EXPECT_FALSE(TileDetileReference(truncated));
	EXPECT_FALSE(TileDetile(truncated));
	EXPECT_FALSE(TileDetileComputeStyle(truncated));

	// Reject dimensions whose byte range cannot be represented before any loop or pointer arithmetic.
	auto overflow = MakeRequest(linear.data(), tiled.data(), UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 16u,
	                            TileDetileLayout::Standard4KB, UINT64_MAX);
	EXPECT_FALSE(TileDetileIsSupported(overflow));
}

TEST(EmulatorTileDetile, ConvertWrappersMatchReference)
{
	constexpr uint32_t k_w   = 64u;
	constexpr uint32_t k_h   = 48u;
	constexpr uint32_t k_bpp = 4u;
	TileSizeAlign      size {};
	TileGetRenderTargetSize(k_w, k_h, k_w, 0x1b, k_bpp, &size);

	std::vector<uint8_t> linear(static_cast<size_t>(k_w) * k_h * k_bpp);
	for (size_t i = 0; i < linear.size(); ++i)
	{
		linear[i] = static_cast<uint8_t>(i & 0xffu);
	}
	std::vector<uint8_t> tiled(size.size, 0);
	FillTiledFromLinear(&tiled, linear, k_w, k_h, k_w, k_bpp, TileDetileLayout::Sw64kRx);

	std::vector<uint8_t> via_wrapper(linear.size(), 0);
	std::vector<uint8_t> via_ref(linear.size(), 0);
	TileConvertSw64kRxToLinear(via_wrapper.data(), tiled.data(), k_w, k_h, k_w, k_bpp);
	const auto ref_req = MakeRequest(via_ref.data(), tiled.data(), k_w, k_h, k_w, k_w, k_bpp, TileDetileLayout::Sw64kRx, size.size);
	ASSERT_TRUE(TileDetileReference(ref_req));
	EXPECT_EQ(via_wrapper, via_ref);
	EXPECT_EQ(via_wrapper, linear);
}

// Standard4KB BC7 array with a complete mip chain and multiple layers.
TEST(EmulatorTileDetile, AcceptsStandard4KBBc7ArrayWithMips)
{
	constexpr uint32_t k_format = 181u; // BC7 UNORM
	constexpr uint32_t k_width  = 4096u;
	constexpr uint32_t k_height = 4096u;
	constexpr uint32_t k_pitch  = 4096u;
	constexpr uint32_t k_levels = 13u;
	constexpr uint32_t k_tile   = 5u;
	constexpr uint32_t k_layers = 6u;

	Gen5TextureArrayLayout layout {};
	ASSERT_TRUE(Gen5GetTextureArrayLayout(k_format, k_width, k_height, k_pitch, k_levels, k_tile, k_layers, &layout));
	EXPECT_TRUE(layout.has_mip_layout);
	EXPECT_EQ(layout.levels, k_levels);
	EXPECT_EQ(layout.layers, k_layers);
	EXPECT_EQ(layout.tile, k_tile);
	EXPECT_GT(layout.tiled_slice.size, 0u);
	EXPECT_EQ(layout.tiled_size, static_cast<uint64_t>(layout.tiled_slice.size) * k_layers);
	EXPECT_EQ(layout.linear_size, layout.linear_slice_size * k_layers);
	EXPECT_EQ(layout.mip_layout.levels, k_levels);
	EXPECT_EQ(layout.mip_layout.bytes_per_element, 16u);
	EXPECT_EQ(layout.mip_layout.texels_per_element_x, 4u);

	// Round-trip detile for a compact synthetic array; the full layout is multi-MiB.
	Gen5TextureArrayLayout small {};
	ASSERT_TRUE(Gen5GetTextureArrayLayout(k_format, 64u, 64u, 64u, 7u, k_tile, 2u, &small));
	ASSERT_TRUE(small.has_mip_layout);
	std::vector<uint8_t> tiled(static_cast<size_t>(small.tiled_size), 0x5A);
	std::vector<uint8_t> linear(static_cast<size_t>(small.linear_size), 0);
	ASSERT_TRUE(Gen5DetileTextureArray(linear.data(), linear.size(), tiled.data(), tiled.size(), small));
	// At least one non-zero dword after detile (input was non-zero).
	bool any = false;
	for (uint8_t b: linear)
	{
		if (b != 0)
		{
			any = true;
			break;
		}
	}
	EXPECT_TRUE(any);
	EXPECT_TRUE(Gen5ValidateTextureArrayUpload(small, 0u, small.tiled_size));
	EXPECT_FALSE(Gen5ValidateTextureArrayUpload(small, 0u, small.tiled_size - 1u));
}

TEST(EmulatorTileDetile, RejectsMultiMipArrayOnNonStandard4KBTiles)
{
	Gen5TextureArrayLayout layout {};
	EXPECT_FALSE(Gen5GetTextureArrayLayout(56u, 128u, 128u, 128u, 4u, 9u, 2u, &layout));
	EXPECT_FALSE(Gen5GetTextureArrayLayout(56u, 128u, 128u, 128u, 4u, 27u, 2u, &layout));
}

UT_END();
