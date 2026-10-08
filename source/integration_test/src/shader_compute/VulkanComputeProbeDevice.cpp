#include "VulkanComputeProbe.h"

#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"
#include "VulkanComputeProbeInternal.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace Kyty::Libs::Graphics {

namespace {

constexpr VkDeviceSize kMaximumProbeBufferBytes = 4096u * sizeof(uint32_t);

bool FindExtensionRevision(VkPhysicalDevice device, const char* name, uint32_t* revision)
{
	if (name == nullptr || revision == nullptr) { return false; }
	*revision = 0;
	uint32_t count = 0;
	if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) { return false; }
	std::vector<VkExtensionProperties> extensions(count);
	if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS) { return false; }
	for (const auto& extension: extensions)
	{
		if (std::strcmp(extension.extensionName, name) == 0)
		{
			*revision = extension.specVersion;
			return true;
		}
	}
	return true;
}

bool FindComputeQueue(VkPhysicalDevice device, uint32_t* queue_family_index)
{
	if (queue_family_index == nullptr) { return false; }
	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
	for (uint32_t family = 0; family < family_count; ++family)
	{
		if (families[family].queueCount > 0 && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
		{
			*queue_family_index = family;
			return true;
		}
	}
	return false;
}

bool QueryWaveState(VkPhysicalDevice device, ShaderComputeWaveVulkanState* state)
{
	if (state == nullptr) { return false; }
	uint32_t revision = 0;
	if (!FindExtensionRevision(device, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, &revision) ||
	    revision < VK_EXT_SUBGROUP_SIZE_CONTROL_SPEC_VERSION)
	{
		return false;
	}

	VkPhysicalDeviceSubgroupSizeControlFeaturesEXT subgroup_features {};
	subgroup_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT;
	VkPhysicalDeviceFeatures2 features {};
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &subgroup_features;
	vkGetPhysicalDeviceFeatures2(device, &features);

	VkPhysicalDeviceSubgroupSizeControlPropertiesEXT size_properties {};
	size_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT;
	VkPhysicalDeviceSubgroupProperties subgroup_properties {};
	subgroup_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
	subgroup_properties.pNext = &size_properties;
	VkPhysicalDeviceProperties2 properties {};
	properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties.pNext = &subgroup_properties;
	vkGetPhysicalDeviceProperties2(device, &properties);

	state->extension_advertised = true;
	state->extension_revision = revision;
	state->extension_enabled = true;
	state->size_control_feature_supported = subgroup_features.subgroupSizeControl == VK_TRUE;
	state->full_subgroups_feature_supported = subgroup_features.computeFullSubgroups == VK_TRUE;
	state->size_control_feature_enabled = state->size_control_feature_supported;
	state->full_subgroups_feature_enabled = state->full_subgroups_feature_supported;
	state->compute_required_size_supported =
	    (size_properties.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
	state->compute_ballot_shuffle_supported = (subgroup_properties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
	                                          (subgroup_properties.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0 &&
	                                          (subgroup_properties.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0;
	state->min_subgroup_size = size_properties.minSubgroupSize;
	state->max_subgroup_size = size_properties.maxSubgroupSize;
	state->max_local_size[0] = properties.properties.limits.maxComputeWorkGroupSize[0];
	state->max_local_size[1] = properties.properties.limits.maxComputeWorkGroupSize[1];
	state->max_local_size[2] = properties.properties.limits.maxComputeWorkGroupSize[2];
	state->max_group_count[0] = properties.properties.limits.maxComputeWorkGroupCount[0];
	state->max_group_count[1] = properties.properties.limits.maxComputeWorkGroupCount[1];
	state->max_group_count[2] = properties.properties.limits.maxComputeWorkGroupCount[2];
	state->max_invocations = properties.properties.limits.maxComputeWorkGroupInvocations;
	state->max_subgroups = size_properties.maxComputeWorkgroupSubgroups;
	state->max_shared_bytes = properties.properties.limits.maxComputeSharedMemorySize;
	return true;
}

bool SupportsWaveProbe(const ShaderComputeWaveVulkanState& state)
{
	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(state);
	const ShaderComputeWaveRequest request {{64, 1, 1}, {1, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	ShaderComputeWaveLayout layout {};
	return ShaderBuildPairedComputeWaveLayout(request, capabilities, &layout) == ShaderComputeWaveLayoutStatus::Supported;
}

} // namespace

VulkanComputeProbe::Result VulkanComputeProbe::Initialize(std::string* message)
{
	return InitializeInternal(message, false);
}

VulkanComputeProbe::Result VulkanComputeProbe::InitializeWave(std::string* message)
{
	return InitializeInternal(message, true);
}

VulkanComputeProbe::Result VulkanComputeProbe::InitializeInternal(std::string* message, bool request_wave_features)
{
	if (message == nullptr) { return Result::Failure; }
	*message = "";
	if (instance_ != VK_NULL_HANDLE || device_ != VK_NULL_HANDLE)
	{
		*message = "Vulkan compute probe is already initialized";
		return Result::Failure;
	}
	wave_capabilities_ = {};
	physical_device_name_.clear();
	default_subgroup_size_ = 0;

	{
		const auto enumerate_instance_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
		    vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
		uint32_t loader_version = 0;
		if (enumerate_instance_version == nullptr || enumerate_instance_version(&loader_version) != VK_SUCCESS ||
		    loader_version < VK_API_VERSION_1_4)
		{
			*message = "compute probe requires a Vulkan 1.4 instance API";
			return Result::Unavailable;
		}
	}

	VkApplicationInfo app_info {};
	app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName   = request_wave_features ? "Kyty paired-wave compute integration" : "Kyty shader compute integration";
	app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	app_info.pEngineName        = "Kyty";
	app_info.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
	app_info.apiVersion         = VK_API_VERSION_1_4;

	VkInstanceCreateInfo instance_info {};
	instance_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &app_info;
	VkResult result = vkCreateInstance(&instance_info, nullptr, &instance_);
	if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
	{
		*message = "Vulkan loader found no compatible driver";
		return Result::Unavailable;
	}
	if (result != VK_SUCCESS)
	{
		VulkanComputeProbeInternal::SetVkError(message, "vkCreateInstance", result);
		return Result::Failure;
	}

	uint32_t physical_device_count = 0;
	result = vkEnumeratePhysicalDevices(instance_, &physical_device_count, nullptr);
	if (result != VK_SUCCESS)
	{
		VulkanComputeProbeInternal::SetVkError(message, "vkEnumeratePhysicalDevices(count)", result);
		return Result::Failure;
	}
	if (physical_device_count == 0)
	{
		*message = "Vulkan loader exposes no physical devices";
		return Result::Unavailable;
	}

	std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
	result = vkEnumeratePhysicalDevices(instance_, &physical_device_count, physical_devices.data());
	if (result != VK_SUCCESS)
	{
		VulkanComputeProbeInternal::SetVkError(message, "vkEnumeratePhysicalDevices(list)", result);
		return Result::Failure;
	}

	uint32_t selected_queue_family = 0;
	for (const VkPhysicalDevice candidate: physical_devices)
	{
		VkPhysicalDeviceProperties properties {};
		vkGetPhysicalDeviceProperties(candidate, &properties);
		if (properties.apiVersion < VK_API_VERSION_1_4) { continue; }
		const VkDeviceSize required_storage_range = request_wave_features ? kMaximumProbeBufferBytes :
		                                                                         VulkanComputeProbeInternal::kProgramBaseProbeBufferSize;
		if (properties.limits.maxStorageBufferRange < required_storage_range) { continue; }

		uint32_t queue_family = 0;
		if (!FindComputeQueue(candidate, &queue_family)) { continue; }

		ShaderComputeWaveVulkanState candidate_wave_state {};
		if (request_wave_features && (!QueryWaveState(candidate, &candidate_wave_state) || !SupportsWaveProbe(candidate_wave_state)))
		{
			continue;
		}

		physical_device_ = candidate;
		selected_queue_family = queue_family;
		queue_family_index_ = queue_family;
		max_storage_buffer_range_ = properties.limits.maxStorageBufferRange;
		if (request_wave_features) { wave_capabilities_ = ShaderComputeWaveVulkanBuildCapabilities(candidate_wave_state); }
		break;
	}
	if (physical_device_ == VK_NULL_HANDLE)
	{
		*message = request_wave_features ?
		                "no Vulkan 1.4 compute device supports revision-2 subgroup size control, full compute subgroups and the paired size-32 layout" :
		                "no Vulkan 1.4 device exposes a compute queue with a 32-byte storage buffer limit";
		return Result::Unavailable;
	}

	VkPhysicalDeviceProperties selected_properties {};
	vkGetPhysicalDeviceProperties(physical_device_, &selected_properties);
	std::printf("Compute probe device: %s (type=%u, vendor=0x%x, driver=%u)\n", selected_properties.deviceName,
	            static_cast<unsigned>(selected_properties.deviceType), selected_properties.vendorID, selected_properties.driverVersion);
	physical_device_name_ = selected_properties.deviceName;
	VkPhysicalDeviceSubgroupProperties selected_subgroup {};
	selected_subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
	VkPhysicalDeviceProperties2 selected_properties2 {};
	selected_properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	selected_properties2.pNext = &selected_subgroup;
	vkGetPhysicalDeviceProperties2(physical_device_, &selected_properties2);
	default_subgroup_size_ = selected_subgroup.subgroupSize;

	const float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info {};
	queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = selected_queue_family;
	queue_info.queueCount       = 1;
	queue_info.pQueuePriorities = &queue_priority;

	VkPhysicalDeviceSubgroupSizeControlFeaturesEXT wave_features {};
	wave_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT;
	wave_features.subgroupSizeControl = wave_capabilities_.size_control_enabled ? VK_TRUE : VK_FALSE;
	wave_features.computeFullSubgroups = wave_capabilities_.full_subgroups_enabled ? VK_TRUE : VK_FALSE;
	VkDeviceCreateInfo device_info {};
	device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos    = &queue_info;
	const char* extension_name = VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
	if (request_wave_features)
	{
		device_info.pNext = &wave_features;
		device_info.enabledExtensionCount = 1;
		device_info.ppEnabledExtensionNames = &extension_name;
	}
	result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
	if (result != VK_SUCCESS)
	{
		VulkanComputeProbeInternal::SetVkError(message, "vkCreateDevice", result);
		wave_capabilities_ = {};
		return Result::Failure;
	}
	vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
	if (queue_ == VK_NULL_HANDLE)
	{
		*message = "vkGetDeviceQueue returned a null compute queue";
		return Result::Failure;
	}
	return Result::Success;
}

} // namespace Kyty::Libs::Graphics
