#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

UT_BEGIN(EmulatorPackedMipNative);

using namespace Libs::Graphics;

namespace {

constexpr uint32_t kImageWidth      = 64;
constexpr uint32_t kImageHeight     = 1;
constexpr uint32_t kSentinel        = 0xdeadbeefu;
constexpr uint32_t kStorageFormat   = 20; // Gen5 format 20 is R32_UINT.
constexpr uint32_t kTextureType     = 9;  // Flat 2D T#.
constexpr VkFormat kVkStorageFormat = VK_FORMAT_R32_UINT;

enum class NativeResult
{
	Success,
	Unavailable,
	Failure
};

bool CheckVk(VkResult result, const char* operation, std::string* reason)
{
	if (result == VK_SUCCESS)
	{
		return true;
	}
	if (reason != nullptr)
	{
		*reason = std::string(operation) + " failed with VkResult " + std::to_string(static_cast<int>(result));
	}
	return false;
}

bool HasValidationLayer()
{
	uint32_t count = 0;
	if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0)
	{
		return false;
	}
	std::vector<VkLayerProperties> layers(count);
	if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
	{
		return false;
	}
	return std::any_of(layers.begin(), layers.end(),
	                   [](const auto& layer) { return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
}

bool HasInstanceExtension(const char* name)
{
	uint32_t count = 0;
	if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
	{
		return false;
	}
	std::vector<VkExtensionProperties> extensions(count);
	if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS)
	{
		return false;
	}
	return std::any_of(extensions.begin(), extensions.end(),
	                   [name](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; });
}

class NativeVulkanContext
{
public:
	NativeVulkanContext()                                      = default;
	NativeVulkanContext(const NativeVulkanContext&)            = delete;
	NativeVulkanContext& operator=(const NativeVulkanContext&) = delete;

	~NativeVulkanContext()
	{
		if (device != VK_NULL_HANDLE)
		{
			vkDestroyDevice(device, nullptr);
		}
		if (debug_messenger != VK_NULL_HANDLE && destroy_debug_messenger != nullptr)
		{
			destroy_debug_messenger(instance, debug_messenger, nullptr);
		}
		if (instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(instance, nullptr);
		}
	}

	NativeResult Initialize(std::string* reason)
	{
		if (reason == nullptr)
		{
			return NativeResult::Failure;
		}
		if (!HasValidationLayer() || !HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
		{
			*reason = "VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils are required for this fixture";
			return NativeResult::Unavailable;
		}
		uint32_t loader_version = VK_API_VERSION_1_0;
		if (vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS || loader_version < VK_API_VERSION_1_3)
		{
			*reason = "Vulkan 1.3 instance support is required for paired-wave subgroup control";
			return NativeResult::Unavailable;
		}

		VkApplicationInfo app_info {};
		app_info.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app_info.apiVersion = VK_API_VERSION_1_3;

		const char*                        layer     = "VK_LAYER_KHRONOS_validation";
		const char*                        extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		VkDebugUtilsMessengerCreateInfoEXT debug_info {};
		debug_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debug_info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
		debug_info.pfnUserCallback = ValidationCallback;
		debug_info.pUserData       = this;

		VkInstanceCreateInfo instance_info {};
		instance_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance_info.pNext                   = &debug_info;
		instance_info.pApplicationInfo        = &app_info;
		instance_info.enabledLayerCount       = 1;
		instance_info.ppEnabledLayerNames     = &layer;
		instance_info.enabledExtensionCount   = 1;
		instance_info.ppEnabledExtensionNames = &extension;
		if (!CheckVk(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance", reason))
		{
			return NativeResult::Failure;
		}

		const auto create_messenger =
		    reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
		destroy_debug_messenger =
		    reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
		if (create_messenger == nullptr || destroy_debug_messenger == nullptr ||
		    !CheckVk(create_messenger(instance, &debug_info, nullptr, &debug_messenger), "vkCreateDebugUtilsMessengerEXT", reason))
		{
			if (reason->empty())
			{
				*reason = "Vulkan debug-utils entry points are unavailable";
			}
			return NativeResult::Failure;
		}

		uint32_t physical_count = 0;
		if (!CheckVk(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr), "vkEnumeratePhysicalDevices", reason))
		{
			return NativeResult::Failure;
		}
		if (physical_count == 0)
		{
			*reason = "no Vulkan physical device is available";
			return NativeResult::Unavailable;
		}
		std::vector<VkPhysicalDevice> physical_devices(physical_count);
		if (!CheckVk(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()), "vkEnumeratePhysicalDevices", reason))
		{
			return NativeResult::Failure;
		}

		for (const auto candidate: physical_devices)
		{
			VkPhysicalDeviceProperties2 properties2 {};
			properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			VkPhysicalDeviceSubgroupProperties subgroup {};
			subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
			VkPhysicalDeviceSubgroupSizeControlProperties subgroup_size {};
			subgroup_size.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
			subgroup.pNext      = &subgroup_size;
			properties2.pNext   = &subgroup;
			vkGetPhysicalDeviceProperties2(candidate, &properties2);
			if (properties2.properties.apiVersion < VK_API_VERSION_1_3)
			{
				continue;
			}

			VkPhysicalDeviceVulkan13Features features13 {};
			features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			VkPhysicalDeviceFeatures2 features {};
			features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			features.pNext = &features13;
			vkGetPhysicalDeviceFeatures2(candidate, &features);
			const auto required_operations =
			    VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
			if (features13.subgroupSizeControl != VK_TRUE || features13.computeFullSubgroups != VK_TRUE ||
			    (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) == 0 ||
			    (subgroup.supportedOperations & required_operations) != required_operations || subgroup_size.minSubgroupSize > 32u ||
			    subgroup_size.maxSubgroupSize < 32u || (subgroup_size.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) == 0)
			{
				continue;
			}

			const auto& limits = properties2.properties.limits;
			if (limits.maxComputeWorkGroupInvocations < 32u || limits.maxComputeWorkGroupSize[0] < 32u ||
			    limits.maxComputeWorkGroupCount[0] < 1u || limits.maxPushConstantsSize < 48u || limits.maxDescriptorSetStorageImages < 1u ||
			    limits.maxPerStageDescriptorStorageImages < 1u)
			{
				continue;
			}

			VkFormatProperties format_properties {};
			vkGetPhysicalDeviceFormatProperties(candidate, kVkStorageFormat, &format_properties);
			const auto required_format_features =
			    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			if ((format_properties.optimalTilingFeatures & required_format_features) != required_format_features)
			{
				continue;
			}

			uint32_t queue_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
			std::vector<VkQueueFamilyProperties> queues(queue_count);
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());
			uint32_t compute_family = UINT32_MAX;
			for (uint32_t index = 0; index < queue_count; ++index)
			{
				if (queues[index].queueCount > 0 && (queues[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
				{
					compute_family = index;
					break;
				}
			}
			if (compute_family == UINT32_MAX)
			{
				continue;
			}

			const float             priority = 1.0f;
			VkDeviceQueueCreateInfo queue_info {};
			queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queue_info.queueFamilyIndex = compute_family;
			queue_info.queueCount       = 1;
			queue_info.pQueuePriorities = &priority;

			VkPhysicalDeviceVulkan13Features enabled13 {};
			enabled13.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			enabled13.subgroupSizeControl  = VK_TRUE;
			enabled13.computeFullSubgroups = VK_TRUE;
			VkDeviceCreateInfo device_info {};
			device_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			device_info.pNext                   = &enabled13;
			device_info.queueCreateInfoCount    = 1;
			device_info.pQueueCreateInfos       = &queue_info;
			device_info.enabledExtensionCount   = 0;
			device_info.ppEnabledExtensionNames = nullptr;
			device_info.pEnabledFeatures        = nullptr;
			if (!CheckVk(vkCreateDevice(candidate, &device_info, nullptr, &device), "vkCreateDevice", reason))
			{
				return NativeResult::Failure;
			}

			physical_device = candidate;
			queue_family    = compute_family;
			vkGetDeviceQueue(device, queue_family, 0, &queue);
			if (queue == VK_NULL_HANDLE)
			{
				*reason = "vkGetDeviceQueue returned a null compute queue";
				return NativeResult::Failure;
			}
			return NativeResult::Success;
		}

		*reason = "no device meets the Vulkan 1.3, subgroup-size, compute, R32_UINT storage-image, and transfer requirements";
		return NativeResult::Unavailable;
	}

	[[nodiscard]] uint32_t ValidationErrors() const { return validation_errors.load(); }

	bool ShutdownDevice(std::string* reason)
	{
		if (device == VK_NULL_HANDLE)
		{
			return true;
		}
		if (!CheckVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle", reason))
		{
			return false;
		}
		vkDestroyDevice(device, nullptr);
		device          = VK_NULL_HANDLE;
		physical_device = VK_NULL_HANDLE;
		queue           = VK_NULL_HANDLE;
		return true;
	}

	VkInstance       instance        = VK_NULL_HANDLE;
	VkDevice         device          = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkQueue          queue           = VK_NULL_HANDLE;
	uint32_t         queue_family    = 0;

private:
	static VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	                                                         VkDebugUtilsMessageTypeFlagsEXT,
	                                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data)
	{
		auto* context = static_cast<NativeVulkanContext*>(user_data);
		if (context != nullptr && (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
		{
			context->validation_errors.fetch_add(1u);
		}
		if (data != nullptr && data->pMessage != nullptr &&
		    (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0)
		{
			std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
		}
		return VK_FALSE;
	}

	VkDebugUtilsMessengerEXT            debug_messenger         = VK_NULL_HANDLE;
	PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger = nullptr;
	std::atomic<uint32_t>               validation_errors {0};
};

bool FindMemoryType(VkPhysicalDevice physical_device, uint32_t type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                    uint32_t* index)
{
	if (index == nullptr)
	{
		return false;
	}
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
	int fallback = -1;
	for (uint32_t type = 0; type < properties.memoryTypeCount; ++type)
	{
		if ((type_bits & (1u << type)) == 0 || (properties.memoryTypes[type].propertyFlags & required) != required)
		{
			continue;
		}
		if ((properties.memoryTypes[type].propertyFlags & preferred) == preferred)
		{
			*index = type;
			return true;
		}
		if (fallback < 0)
		{
			fallback = static_cast<int>(type);
		}
	}
	if (fallback < 0)
	{
		return false;
	}
	*index = static_cast<uint32_t>(fallback);
	return true;
}

class NativeImageTarget
{
public:
	NativeImageTarget()                                    = default;
	NativeImageTarget(const NativeImageTarget&)            = delete;
	NativeImageTarget& operator=(const NativeImageTarget&) = delete;

	~NativeImageTarget()
	{
		if (view != VK_NULL_HANDLE)
		{
			vkDestroyImageView(device, view, nullptr);
		}
		if (image != VK_NULL_HANDLE)
		{
			vkDestroyImage(device, image, nullptr);
		}
		if (memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, memory, nullptr);
		}
	}

	bool Initialize(const NativeVulkanContext& context, std::string* reason)
	{
		device          = context.device;
		physical_device = context.physical_device;
		VkImageCreateInfo image_info {};
		image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.imageType     = VK_IMAGE_TYPE_2D;
		image_info.format        = kVkStorageFormat;
		image_info.extent        = {kImageWidth, kImageHeight, 1};
		image_info.mipLevels     = 1;
		image_info.arrayLayers   = 1;
		image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (!CheckVk(vkCreateImage(device, &image_info, nullptr, &image), "vkCreateImage", reason))
		{
			return false;
		}

		VkMemoryRequirements requirements {};
		vkGetImageMemoryRequirements(device, image, &requirements);
		uint32_t memory_type = 0;
		if (!FindMemoryType(physical_device, requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type))
		{
			*reason = "no image-compatible Vulkan memory type is available";
			return false;
		}
		VkMemoryAllocateInfo allocation {};
		allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocation.allocationSize  = requirements.size;
		allocation.memoryTypeIndex = memory_type;
		if (!CheckVk(vkAllocateMemory(device, &allocation, nullptr, &memory), "vkAllocateMemory(image)", reason) ||
		    !CheckVk(vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory", reason))
		{
			return false;
		}

		VkImageViewCreateInfo view_info {};
		view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image                           = image;
		view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format                          = kVkStorageFormat;
		view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		view_info.subresourceRange.baseMipLevel   = 0;
		view_info.subresourceRange.levelCount     = 1;
		view_info.subresourceRange.baseArrayLayer = 0;
		view_info.subresourceRange.layerCount     = 1;
		return CheckVk(vkCreateImageView(device, &view_info, nullptr, &view), "vkCreateImageView", reason);
	}

	VkDevice       device = VK_NULL_HANDLE;
	VkImage        image  = VK_NULL_HANDLE;
	VkImageView    view   = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;

private:
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
};

class ReadbackBuffer
{
public:
	ReadbackBuffer()                                 = default;
	ReadbackBuffer(const ReadbackBuffer&)            = delete;
	ReadbackBuffer& operator=(const ReadbackBuffer&) = delete;

	~ReadbackBuffer()
	{
		if (mapped != nullptr)
		{
			vkUnmapMemory(device, memory);
		}
		if (buffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, buffer, nullptr);
		}
		if (memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, memory, nullptr);
		}
	}

	bool Initialize(const NativeVulkanContext& context, std::string* reason)
	{
		device = context.device;
		VkBufferCreateInfo buffer_info {};
		buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size        = sizeof(uint32_t) * kImageWidth * kImageHeight;
		buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (!CheckVk(vkCreateBuffer(device, &buffer_info, nullptr, &buffer), "vkCreateBuffer(readback)", reason))
		{
			return false;
		}
		VkMemoryRequirements requirements {};
		vkGetBufferMemoryRequirements(device, buffer, &requirements);
		uint32_t memory_type = 0;
		if (!FindMemoryType(context.physical_device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &memory_type))
		{
			*reason = "no readback-compatible host-visible Vulkan memory type is available";
			return false;
		}
		VkPhysicalDeviceMemoryProperties memory_properties {};
		vkGetPhysicalDeviceMemoryProperties(context.physical_device, &memory_properties);
		coherent = (memory_properties.memoryTypes[memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
		VkMemoryAllocateInfo allocation {};
		allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocation.allocationSize  = requirements.size;
		allocation.memoryTypeIndex = memory_type;
		if (!CheckVk(vkAllocateMemory(device, &allocation, nullptr, &memory), "vkAllocateMemory(readback)", reason) ||
		    !CheckVk(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory(readback)", reason) ||
		    !CheckVk(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(readback)", reason))
		{
			return false;
		}
		return true;
	}

	bool Read(std::array<uint32_t, kImageWidth>* pixels, std::string* reason) const
	{
		if (pixels == nullptr || mapped == nullptr)
		{
			*reason = "readback buffer is not mapped";
			return false;
		}
		if (!coherent)
		{
			VkMappedMemoryRange range {};
			range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = memory;
			range.offset = 0;
			range.size   = VK_WHOLE_SIZE;
			if (!CheckVk(vkInvalidateMappedMemoryRanges(device, 1, &range), "vkInvalidateMappedMemoryRanges", reason))
			{
				return false;
			}
		}
		std::memcpy(pixels->data(), mapped, pixels->size() * sizeof(uint32_t));
		return true;
	}

	VkDevice       device   = VK_NULL_HANDLE;
	VkBuffer       buffer   = VK_NULL_HANDLE;
	VkDeviceMemory memory   = VK_NULL_HANDLE;
	void*          mapped   = nullptr;
	bool           coherent = false;
};

class NativeComputePipeline
{
public:
	NativeComputePipeline()                                        = default;
	NativeComputePipeline(const NativeComputePipeline&)            = delete;
	NativeComputePipeline& operator=(const NativeComputePipeline&) = delete;

	~NativeComputePipeline()
	{
		if (pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, pipeline, nullptr);
		}
		if (layout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, layout, nullptr);
		}
		if (descriptor_pool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
		}
		if (descriptor_layout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
		}
	}

	bool Initialize(const NativeVulkanContext& context, const ShaderBindResources& bind, const std::vector<uint32_t>& spirv,
	                const NativeImageTarget& image, std::string* reason)
	{
		device = context.device;
		VkDescriptorSetLayoutBinding image_binding {};
		image_binding.binding         = static_cast<uint32_t>(bind.textures2D.binding_storage_index);
		image_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		image_binding.descriptorCount = 1;
		image_binding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
		VkDescriptorSetLayoutCreateInfo descriptor_layout_info {};
		descriptor_layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		descriptor_layout_info.bindingCount = 1;
		descriptor_layout_info.pBindings    = &image_binding;
		if (!CheckVk(vkCreateDescriptorSetLayout(device, &descriptor_layout_info, nullptr, &descriptor_layout),
		             "vkCreateDescriptorSetLayout", reason))
		{
			return false;
		}

		VkDescriptorPoolSize pool_size {};
		pool_size.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		pool_size.descriptorCount = 1;
		VkDescriptorPoolCreateInfo pool_info {};
		pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool_info.maxSets       = 1;
		pool_info.poolSizeCount = 1;
		pool_info.pPoolSizes    = &pool_size;
		if (!CheckVk(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool), "vkCreateDescriptorPool", reason))
		{
			return false;
		}
		VkDescriptorSetAllocateInfo set_info {};
		set_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		set_info.descriptorPool     = descriptor_pool;
		set_info.descriptorSetCount = 1;
		set_info.pSetLayouts        = &descriptor_layout;
		if (!CheckVk(vkAllocateDescriptorSets(device, &set_info, &descriptor_set), "vkAllocateDescriptorSets", reason))
		{
			return false;
		}
		VkDescriptorImageInfo image_info {};
		image_info.imageView   = image.view;
		image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		VkWriteDescriptorSet write {};
		write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet          = descriptor_set;
		write.dstBinding      = image_binding.binding;
		write.descriptorCount = 1;
		write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		write.pImageInfo      = &image_info;
		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

		VkPushConstantRange push_range {};
		push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		push_range.offset     = bind.push_constant_offset;
		push_range.size       = bind.push_constant_size;
		VkPipelineLayoutCreateInfo layout_info {};
		layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layout_info.setLayoutCount         = 1;
		layout_info.pSetLayouts            = &descriptor_layout;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges    = &push_range;
		if (!CheckVk(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout), "vkCreatePipelineLayout", reason))
		{
			return false;
		}

		VkShaderModuleCreateInfo module_info {};
		module_info.sType     = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		module_info.codeSize  = spirv.size() * sizeof(uint32_t);
		module_info.pCode     = spirv.data();
		VkShaderModule module = VK_NULL_HANDLE;
		if (!CheckVk(vkCreateShaderModule(device, &module_info, nullptr, &module), "vkCreateShaderModule", reason))
		{
			return false;
		}

		VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required_size {};
		required_size.sType                = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
		required_size.requiredSubgroupSize = 32;
		VkPipelineShaderStageCreateInfo stage {};
		stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage.pNext  = &required_size;
		stage.flags  = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
		stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
		stage.module = module;
		stage.pName  = "main";
		VkComputePipelineCreateInfo pipeline_info {};
		pipeline_info.sType             = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipeline_info.stage             = stage;
		pipeline_info.layout            = layout;
		pipeline_info.basePipelineIndex = -1;
		const auto pipeline_result      = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
		vkDestroyShaderModule(device, module, nullptr);
		return CheckVk(pipeline_result, "vkCreateComputePipelines", reason);
	}

	VkDevice              device            = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
	VkDescriptorPool      descriptor_pool   = VK_NULL_HANDLE;
	VkDescriptorSet       descriptor_set    = VK_NULL_HANDLE;
	VkPipelineLayout      layout            = VK_NULL_HANDLE;
	VkPipeline            pipeline          = VK_NULL_HANDLE;
};

class NativeCommandBuffer
{
public:
	NativeCommandBuffer()                                      = default;
	NativeCommandBuffer(const NativeCommandBuffer&)            = delete;
	NativeCommandBuffer& operator=(const NativeCommandBuffer&) = delete;

	~NativeCommandBuffer()
	{
		if (pool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(device, pool, nullptr);
		}
	}

	bool Initialize(const NativeVulkanContext& context, std::string* reason)
	{
		device = context.device;
		VkCommandPoolCreateInfo pool_info {};
		pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = context.queue_family;
		if (!CheckVk(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "vkCreateCommandPool", reason))
		{
			return false;
		}
		VkCommandBufferAllocateInfo allocate_info {};
		allocate_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate_info.commandPool        = pool;
		allocate_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate_info.commandBufferCount = 1;
		return CheckVk(vkAllocateCommandBuffers(device, &allocate_info, &command_buffer), "vkAllocateCommandBuffers", reason);
	}

	VkDevice        device         = VK_NULL_HANDLE;
	VkCommandPool   pool           = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
};

bool RunImageCase(const NativeVulkanContext& context, NativeImageTarget* image, const ReadbackBuffer& readback,
                  const NativeComputePipeline& pipeline, NativeCommandBuffer* command, const std::vector<uint32_t>& metadata,
                  bool first_run, std::array<uint32_t, kImageWidth>* pixels, std::string* reason)
{
	if (image == nullptr || command == nullptr || pixels == nullptr || metadata.size() * sizeof(uint32_t) != 48u)
	{
		*reason = "invalid packed-mip native dispatch input";
		return false;
	}
	if (!CheckVk(vkResetCommandBuffer(command->command_buffer, 0), "vkResetCommandBuffer", reason))
	{
		return false;
	}
	VkCommandBufferBeginInfo begin_info {};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (!CheckVk(vkBeginCommandBuffer(command->command_buffer, &begin_info), "vkBeginCommandBuffer", reason))
	{
		return false;
	}

	VkImageMemoryBarrier prepare_image {};
	prepare_image.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	prepare_image.srcAccessMask       = first_run ? 0u : VK_ACCESS_TRANSFER_READ_BIT;
	prepare_image.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	prepare_image.oldLayout           = image->layout;
	prepare_image.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	prepare_image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	prepare_image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	prepare_image.image               = image->image;
	prepare_image.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(command->command_buffer, first_run ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &prepare_image);

	VkClearColorValue clear_value {};
	clear_value.uint32[0] = kSentinel;
	const VkImageSubresourceRange subresource_range {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(command->command_buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &subresource_range);

	VkImageMemoryBarrier clear_to_shader = prepare_image;
	clear_to_shader.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
	clear_to_shader.dstAccessMask        = VK_ACCESS_SHADER_WRITE_BIT;
	clear_to_shader.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	clear_to_shader.newLayout            = VK_IMAGE_LAYOUT_GENERAL;
	vkCmdPipelineBarrier(command->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
	                     nullptr, 1, &clear_to_shader);

	vkCmdBindPipeline(command->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
	vkCmdBindDescriptorSets(command->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &pipeline.descriptor_set, 0,
	                        nullptr);
	vkCmdPushConstants(command->command_buffer, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 48u, metadata.data());
	vkCmdDispatch(command->command_buffer, 1, 1, 1);

	VkImageMemoryBarrier shader_to_copy = clear_to_shader;
	shader_to_copy.srcAccessMask        = VK_ACCESS_SHADER_WRITE_BIT;
	shader_to_copy.dstAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
	shader_to_copy.oldLayout            = VK_IMAGE_LAYOUT_GENERAL;
	shader_to_copy.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(command->command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                     nullptr, 1, &shader_to_copy);

	VkBufferImageCopy copy_region {};
	copy_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy_region.imageExtent      = {kImageWidth, kImageHeight, 1};
	vkCmdCopyImageToBuffer(command->command_buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy_region);
	VkBufferMemoryBarrier copy_to_host {};
	copy_to_host.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	copy_to_host.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	copy_to_host.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	copy_to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	copy_to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	copy_to_host.buffer              = readback.buffer;
	copy_to_host.offset              = 0;
	copy_to_host.size                = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(command->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
	                     &copy_to_host, 0, nullptr);
	if (!CheckVk(vkEndCommandBuffer(command->command_buffer), "vkEndCommandBuffer", reason))
	{
		return false;
	}

	VkFenceCreateInfo fence_info {};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence    = VK_NULL_HANDLE;
	if (!CheckVk(vkCreateFence(context.device, &fence_info, nullptr, &fence), "vkCreateFence", reason))
	{
		return false;
	}
	VkSubmitInfo submit_info {};
	submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers    = &command->command_buffer;
	const auto submit_result       = vkQueueSubmit(context.queue, 1, &submit_info, fence);
	if (submit_result != VK_SUCCESS)
	{
		vkDestroyFence(context.device, fence, nullptr);
		return CheckVk(submit_result, "vkQueueSubmit", reason);
	}
	const auto wait_result = vkWaitForFences(context.device, 1, &fence, VK_TRUE, 15'000'000'000ull);
	vkDestroyFence(context.device, fence, nullptr);
	if (!CheckVk(wait_result, "vkWaitForFences (15-second limit)", reason))
	{
		return false;
	}

	image->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	return readback.Read(pixels, reason);
}

void InitializeShaderHarness()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool         Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t      GetInteger(const Core::String&) const override { return 0; }
		bool         GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation;
	Config::Load(validation);
}

bool BuildStoreShader(bool packed_mip, ShaderCode* code, ShaderComputeInputInfo* input, std::string* reason)
{
	if (code == nullptr || input == nullptr || reason == nullptr)
	{
		return false;
	}
	const uint32_t                 mimg_word0 = (0x3cu << 26u) | ((packed_mip ? 0x09u : 0x08u) << 18u) | (0xfu << 8u) | (1u << 3u);
	const uint32_t                 mimg_word1 = (8u << 16u) | (8u << 8u) | 4u; // T# s[32:39], vdata v[8:11], vaddr v[4:7].
	const uint32_t                 s_mov_exec = (0x17du << 23u) | (126u << 16u) | (0x04u << 8u) | 4u; // s_mov_b64 exec, s[4:5]
	auto                           v_mov      = [](uint32_t dst, uint32_t src) { return 0x7e000000u | (dst << 17u) | (0x01u << 9u) | src; };
	const std::array<uint32_t, 12> words {
	    v_mov(4u, 256u), // v4 = logical lane id, used as the image X coordinate.
	    v_mov(5u, 128u), // v5 = 0.
	    v_mov(6u, 128u), // v6 = LOD 0 for image_store_mip.
	    v_mov(7u, 128u),  v_mov(8u, 256u), v_mov(9u, 256u), v_mov(10u, 256u),
	    v_mov(11u, 256u), // all channels carry the lane id so component swizzles are immaterial.
	    s_mov_exec,       mimg_word0,      mimg_word1,      0xbf810000u,
	};
	code->SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words.data(), static_cast<uint32_t>(sizeof(words)), code))
	{
		*reason = "the production parser rejected the native EXEC image-store program";
		return false;
	}

	input->dispatch_mode  = 0x8041u;
	input->threads_num[0] = 64u;
	input->threads_num[1] = 1u;
	input->threads_num[2] = 1u;
	input->thread_ids_num = 1;
	input->wave_layout    = {ShaderComputeWaveStrategy::Paired64On32, {64u, 1u, 1u}, {32u, 1u, 1u}, 64u, 32u, 2u, 1u, 0u};

	auto& bind                             = input->bind;
	bind.textures2D.textures_num           = 1;
	bind.textures2D.textures2d_storage_num = 1;
	auto& descriptor                       = bind.textures2D.desc[0];
	descriptor.slot                        = 0;
	descriptor.start_register              = 32;
	descriptor.usage                       = ShaderTextureUsage::ReadWrite;
	descriptor.textures2d_without_sampler  = true;
	descriptor.sampled_shape               = ShaderGen5SampledTextureShape::TwoDimensional;
	descriptor.texture.fields[0]           = 0x100u;
	descriptor.texture.fields[1]           = kStorageFormat << 20u;
	descriptor.texture.fields[2]           = (kImageWidth - 1u) | ((kImageHeight - 1u) << 14u);
	descriptor.texture.fields[3]           = (kTextureType << 28u) | 4u | (5u << 3u) | (6u << 6u) | (7u << 9u);
	bind.direct_sgprs.sgprs_num            = 2;
	bind.direct_sgprs.start_register[0]    = 4;
	bind.direct_sgprs.start_register[1]    = 5;
	ShaderCalcBindingIndices(&bind);
	if (bind.textures2D.binding_storage_index != 1 || bind.push_constant_size != 48u || bind.vsharp_uniform_buffer)
	{
		*reason = "the test shader did not resolve to the expected portable image and EXEC metadata layout";
		return false;
	}
	return true;
}

std::vector<uint32_t> MakeMetadata(const ShaderBindResources& bind, uint32_t exec_low, uint32_t exec_high)
{
	std::vector<uint32_t> metadata(bind.push_constant_size / sizeof(uint32_t), 0u);
	const auto&           fields = bind.textures2D.desc[0].texture.fields;
	std::copy(std::begin(fields), std::end(fields), metadata.begin());
	metadata[8] = exec_low;
	metadata[9] = exec_high;
	return metadata;
}

bool CompileStoreShader(const ShaderCode& code, const ShaderComputeInputInfo& input, std::vector<uint32_t>* binary, std::string* reason)
{
	if (binary == nullptr || reason == nullptr)
	{
		return false;
	}
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	if (source.IsEmpty())
	{
		*reason = "production SPIR-V source generation returned an empty module";
		return false;
	}
	Vector<uint32_t> compiled;
	Core::String8    error;
	if (!ShaderToolchain::Run(source, &compiled, &error) || compiled.IsEmpty())
	{
		*reason = std::string("production SPIR-V validation/assembly failed: ") + error.c_str();
		return false;
	}
	binary->assign(compiled.GetData(), compiled.GetData() + compiled.Size());
	return true;
}

struct ExecMaskCase
{
	const char* name;
	uint32_t    low;
	uint32_t    high;
};

void ExpectPixelsMatchMask(const std::array<uint32_t, kImageWidth>& pixels, uint32_t low, uint32_t high, const char* operation,
                           const char* mask_name)
{
	for (uint32_t x = 0; x < kImageWidth; ++x)
	{
		const bool active = x < 32u ? ((low >> x) & 1u) != 0u : ((high >> (x - 32u)) & 1u) != 0u;
		EXPECT_EQ(pixels[x], active ? x : kSentinel) << operation << " / " << mask_name << " / x=" << x;
	}
}

bool RunNativeCases(const NativeVulkanContext& context, std::string* reason)
{
	NativeImageTarget image;
	if (!image.Initialize(context, reason))
	{
		return false;
	}
	ReadbackBuffer readback;
	if (!readback.Initialize(context, reason))
	{
		return false;
	}
	NativeCommandBuffer command;
	if (!command.Initialize(context, reason))
	{
		return false;
	}

	const std::array<ExecMaskCase, 4> masks {{
	    {"empty", 0u, 0u},
	    {"partial-low", 1u << 3u, 0u},
	    {"partial-high", 0u, 1u << 5u},
	    {"mixed", (1u << 1u) | (1u << 7u) | (1u << 19u), (1u << 0u) | (1u << 8u) | (1u << 29u)},
	}};
	bool                              first_run = true;
	for (const bool packed_mip: {false, true})
	{
		ShaderCode             code;
		ShaderComputeInputInfo input {};
		if (!BuildStoreShader(packed_mip, &code, &input, reason))
		{
			return false;
		}
		std::vector<uint32_t> binary;
		if (!CompileStoreShader(code, input, &binary, reason))
		{
			return false;
		}
		NativeComputePipeline pipeline;
		if (!pipeline.Initialize(context, input.bind, binary, image, reason))
		{
			return false;
		}

		for (const auto& mask: masks)
		{
			const auto                        metadata = MakeMetadata(input.bind, mask.low, mask.high);
			std::array<uint32_t, kImageWidth> pixels {};
			if (!RunImageCase(context, &image, readback, pipeline, &command, metadata, first_run, &pixels, reason))
			{
				return false;
			}
			first_run = false;
			ExpectPixelsMatchMask(pixels, mask.low, mask.high, packed_mip ? "image_store_mip (LOD 0)" : "image_store", mask.name);
		}
	}
	return true;
}

} // namespace

TEST(EmulatorPackedMipNative, ImageStoreMipAndOrdinaryStoreHonorEachExecBank)
{
	InitializeShaderHarness();
	NativeVulkanContext context;
	std::string         reason;
	const auto          initialized = context.Initialize(&reason);
	if (initialized == NativeResult::Unavailable)
	{
		GTEST_SKIP() << reason;
	}
	ASSERT_EQ(initialized, NativeResult::Success) << reason;
	ASSERT_TRUE(RunNativeCases(context, &reason)) << reason;
	ASSERT_TRUE(context.ShutdownDevice(&reason)) << reason;
	EXPECT_EQ(context.ValidationErrors(), 0u) << "Vulkan validation reported errors while tearing down the native image fixture";
}

UT_END();
