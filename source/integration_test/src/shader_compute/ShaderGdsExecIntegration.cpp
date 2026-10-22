#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Math/MathAll.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveRuntime.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace {

using Kyty::Libs::Graphics::ShaderCode;
using Kyty::Libs::Graphics::ShaderComputeInputInfo;
using Kyty::Libs::Graphics::ShaderToolchain::Run;

constexpr uint32_t     kMaxWorkgroups    = 2u;
constexpr uint32_t     kMaxLaneCount     = 64u;
constexpr uint32_t     kBufferWords      = 1u + kMaxWorkgroups * kMaxLaneCount;
constexpr VkDeviceSize kBufferBytes      = kBufferWords * sizeof(uint32_t);
constexpr uint32_t     kVcmpxEqU32Opcode = 0xd2u;
constexpr uint32_t     kVcmpxLeU32Opcode = 0xd3u;
constexpr uint32_t     kAppendOpcode     = 0x3eu;
constexpr uint32_t     kConsumeOpcode    = 0x3du;

enum class ProbeResult
{
	Success,
	Unavailable,
	Failure
};

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "GDS EXEC integration failure: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void InitializeConfig()
{
	char                        program[]  = "kyty_shader_gds_exec_integration";
	char*                       argv[]     = {program, nullptr};
	Kyty::Core::SubsystemsList* subsystems = Kyty::Core::SubsystemsListSingleton::Instance();
	subsystems->SetArgs(1, argv);
	using Kyty::Config::ConfigSubsystem;
	using Kyty::Core::CoreSubsystem;
	using Kyty::Core::ThreadsSubsystem;
	using Kyty::Log::LogSubsystem;
	using Kyty::Math::MathSubsystem;
	subsystems->Add(CoreSubsystem::Instance(), {});
	subsystems->Add(ConfigSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(MathSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(ThreadsSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(LogSubsystem::Instance(), {CoreSubsystem::Instance(), ConfigSubsystem::Instance(), ThreadsSubsystem::Instance()});
	if (!subsystems->InitAll(false))
	{
		Fail("core/config/log subsystems must initialize");
	}
	Kyty::Config::SetNextGen(true);

	class ValidationConfig final: public Kyty::Config::ConfigSource
	{
	public:
		bool Has(const Kyty::Core::String& key) const override
		{
			return key == U"ShaderValidationEnabled" || key == U"ShaderOptimizationType";
		}
		int64_t            GetInteger(const Kyty::Core::String&) const override { return 0; }
		bool               GetBool(const Kyty::Core::String&) const override { return true; }
		Kyty::Core::String GetString(const Kyty::Core::String&) const override { return {}; }
	} validation;
	Kyty::Config::Load(validation);
}

uint32_t Sop1Move(uint32_t destination, uint32_t source)
{
	return (0x17du << 23u) | (destination << 16u) | (0x03u << 8u) | source;
}

uint32_t Vop1Move(uint32_t destination, uint32_t source)
{
	return 0x7e000000u | (destination << 17u) | (0x01u << 9u) | source;
}

std::array<uint32_t, 8> MakeShaderWords(uint32_t opcode, uint32_t comparison_opcode, uint32_t comparison_value)
{
	return {
	    Sop1Move(14u, 14u),                                           // s_mov_b32 s14, s14: materialize the bound GDS pointer SGPR.
	    Sop1Move(124u, 128u),                                         // s_mov_b32 m0, 0
	    Vop1Move(2u, 256u),                                           // v_mov_b32 v2, v0
	    Vop1Move(1u, 128u + comparison_value),                        // v_mov_b32 v1, inline integer
	    0x7c000000u | (comparison_opcode << 17u) | (1u << 9u) | 256u, // v_cmpx_u32 vcc, v0, v1
	    0xd8000000u | (opcode << 18u) | (1u << 17u),                  // ds_append/ds_consume v2, gds
	    2u << 24u,
	    0xbf810000u, // s_endpgm
	};
}

bool BuildShader(uint32_t opcode, uint32_t comparison_opcode, uint32_t comparison_value, ShaderComputeInputInfo* input,
                 const Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan& plan, Kyty::Vector<uint32_t>* binary, std::string* error)
{
	if (input == nullptr || binary == nullptr || error == nullptr)
	{
		if (error != nullptr)
		{
			*error = "null shader build output";
		}
		return false;
	}
	auto       words = MakeShaderWords(opcode, comparison_opcode, comparison_value);
	ShaderCode code;
	code.SetType(Kyty::Libs::Graphics::ShaderType::Compute);
	if (!Kyty::Libs::Graphics::ShaderTryParseBounded(words.data(), static_cast<uint32_t>(sizeof(words)), &code))
	{
		*error = "bounded parser rejected GDS EXEC fixture";
		return false;
	}

	input->dispatch_mode          = plan.dispatch_mode;
	input->threads_num[0]         = plan.wave_layout.guest_local[0];
	input->threads_num[1]         = 1u;
	input->threads_num[2]         = 1u;
	input->thread_ids_num         = 1;
	input->wave_layout            = plan.wave_layout;
	input->native_wave_sensitive  = Kyty::Libs::Graphics::ShaderUsesNativeWaveState(code);
	input->required_subgroup_size = input->native_wave_sensitive ? 32u : 0u;
	if (!input->native_wave_sensitive || input->required_subgroup_size != 32u || plan.wave_layout.native_subgroup_size != 32u)
	{
		*error = "GDS EXEC fixture must require exact physical subgroup size 32";
		return false;
	}
	input->bind.gds_pointers.pointers_num      = 1;
	input->bind.gds_pointers.start_register[0] = 14;
	Kyty::Libs::Graphics::ShaderCalcBindingIndices(&input->bind);
	if (input->bind.gds_pointers.binding_index != 0 || input->bind.push_constant_size != 16u)
	{
		*error = "unexpected production GDS descriptor layout";
		return false;
	}

	const auto        generated = Kyty::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, input);
	std::string       source(generated.GetDataConst(), generated.Size());
	const std::string main_anchor = "%main       = OpFunction %void None %function_void";
	const auto        main_start  = source.find(main_anchor);
	const auto        main_return = main_start == std::string::npos ? std::string::npos : source.find("OpReturn", main_start);
	if (main_return == std::string::npos)
	{
		*error = "generated shader has no main return for the lane-result observation";
		return false;
	}
	std::string observation;
	if (plan.wave_layout.strategy == Kyty::Libs::Graphics::ShaderComputeWaveStrategy::Split64On32)
	{
		// One invocation per guest lane; the prolog's logical index is wave * 64 + guest lane.
		observation = "%gds_probe_group_ptr = OpAccessChain %_ptr_Input_uint %gl_WorkGroupID %uint_0\n"
		              "%gds_probe_group = OpLoad %uint %gds_probe_group_ptr\n"
		              "%gds_probe_group_base_half = OpIMul %uint %gds_probe_group %uint_32\n"
		              "%gds_probe_group_base = OpIAdd %uint %gds_probe_group_base_half %gds_probe_group_base_half\n"
		              "%gds_probe_float = OpLoad %float %v2\n"
		              "%gds_probe_value = OpBitcast %uint %gds_probe_float\n"
		              "%gds_probe_index_base = OpIAdd %uint %gds_probe_group_base %native_wave_logical_index\n"
		              "%gds_probe_index = OpIAdd %uint %gds_probe_index_base %uint_1\n"
		              "%gds_probe_ptr = OpAccessChain %_ptr_StorageBuffer_uint %gds %int_0 %gds_probe_index\n"
		              "OpStore %gds_probe_ptr %gds_probe_value\n";
	} else
	{
		observation = "%gds_probe_group_ptr = OpAccessChain %_ptr_Input_uint %gl_WorkGroupID %uint_0\n"
		              "%gds_probe_group = OpLoad %uint %gds_probe_group_ptr\n"
		              "%gds_probe_group_base = OpIMul %uint %gds_probe_group %uint_32\n"
		              "%gds_probe_lane_ptr = OpAccessChain %_ptr_Input_uint %gl_LocalInvocationID %uint_0\n"
		              "%gds_probe_lane = OpLoad %uint %gds_probe_lane_ptr\n"
		              "%gds_probe_float = OpLoad %float %v2\n"
		              "%gds_probe_value = OpBitcast %uint %gds_probe_float\n"
		              "%gds_probe_index_base = OpIAdd %uint %gds_probe_group_base %gds_probe_lane\n"
		              "%gds_probe_index = OpIAdd %uint %gds_probe_index_base %uint_1\n"
		              "%gds_probe_ptr = OpAccessChain %_ptr_StorageBuffer_uint %gds %int_0 %gds_probe_index\n"
		              "OpStore %gds_probe_ptr %gds_probe_value\n";
	}
	source.insert(main_return, observation);
	Kyty::Core::String8 assembly(source.c_str());
	Kyty::Core::String8 toolchain_error;
	if (!Run(assembly, binary, &toolchain_error) || binary->IsEmpty())
	{
		*error = toolchain_error.IsEmpty() ? "shader toolchain returned an empty module" : toolchain_error.c_str();
		return false;
	}
	error->clear();
	return true;
}

bool CheckVk(VkResult result, const char* operation, std::string* error)
{
	if (result == VK_SUCCESS)
	{
		return true;
	}
	*error = std::string(operation) + " failed with VkResult " + std::to_string(static_cast<int>(result));
	return false;
}

bool FindQueueFamily(VkPhysicalDevice physical, uint32_t* family)
{
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
	for (uint32_t index = 0; index < count; ++index)
	{
		if (families[index].queueCount > 0 && (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
		{
			*family = index;
			return true;
		}
	}
	return false;
}

bool FindHostVisibleMemory(VkPhysicalDevice physical, uint32_t type_bits, uint32_t* type_index, bool* coherent)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physical, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		const auto flags = properties.memoryTypes[index].propertyFlags;
		if ((type_bits & (1u << index)) != 0 && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
		    (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
		{
			*type_index = index;
			*coherent   = true;
			return true;
		}
	}
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		const auto flags = properties.memoryTypes[index].propertyFlags;
		if ((type_bits & (1u << index)) != 0 && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
		{
			*type_index = index;
			*coherent   = false;
			return true;
		}
	}
	return false;
}

class GdsExecVulkanProbe
{
public:
	~GdsExecVulkanProbe()
	{
		ResetDispatch();
		if (device_ != VK_NULL_HANDLE)
		{
			vkDestroyDevice(device_, nullptr);
		}
		if (instance_ != VK_NULL_HANDLE)
		{
			vkDestroyInstance(instance_, nullptr);
		}
	}

	ProbeResult Initialize(std::string* error)
	{
		auto enumerate_instance_version =
		    reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
		uint32_t loader_version = 0;
		if (enumerate_instance_version == nullptr || enumerate_instance_version(&loader_version) != VK_SUCCESS ||
		    loader_version < VK_API_VERSION_1_3)
		{
			*error = "Vulkan 1.3 loader is unavailable";
			return ProbeResult::Unavailable;
		}

		VkApplicationInfo application {};
		application.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		application.apiVersion = VK_API_VERSION_1_3;
		VkInstanceCreateInfo instance_info {};
		instance_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance_info.pApplicationInfo = &application;
		VkResult result                = vkCreateInstance(&instance_info, nullptr, &instance_);
		if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
		{
			*error = "Vulkan loader found no compatible driver";
			return ProbeResult::Unavailable;
		}
		if (!CheckVk(result, "vkCreateInstance", error))
		{
			return ProbeResult::Failure;
		}

		uint32_t physical_count = 0;
		if (!CheckVk(vkEnumeratePhysicalDevices(instance_, &physical_count, nullptr), "vkEnumeratePhysicalDevices(count)", error))
		{
			return ProbeResult::Failure;
		}
		if (physical_count == 0)
		{
			*error = "Vulkan exposes no physical devices";
			return ProbeResult::Unavailable;
		}
		std::vector<VkPhysicalDevice> physical_devices(physical_count);
		if (!CheckVk(vkEnumeratePhysicalDevices(instance_, &physical_count, physical_devices.data()), "vkEnumeratePhysicalDevices(list)",
		             error))
		{
			return ProbeResult::Failure;
		}
		for (const auto candidate: physical_devices)
		{
			VkPhysicalDeviceProperties properties {};
			vkGetPhysicalDeviceProperties(candidate, &properties);
			VkPhysicalDeviceSubgroupSizeControlProperties subgroup_size_control {};
			subgroup_size_control.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
			VkPhysicalDeviceSubgroupProperties subgroup {};
			subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
			subgroup.pNext = &subgroup_size_control;
			VkPhysicalDeviceProperties2 properties2 {};
			properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			properties2.pNext = &subgroup;
			vkGetPhysicalDeviceProperties2(candidate, &properties2);
			VkPhysicalDeviceVulkan13Features supported_features13 {};
			supported_features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			VkPhysicalDeviceFeatures2 supported_features {};
			supported_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			supported_features.pNext = &supported_features13;
			vkGetPhysicalDeviceFeatures2(candidate, &supported_features);
			const auto required_operations =
			    VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
			if (properties.apiVersion < VK_API_VERSION_1_3 || properties.limits.maxPushConstantsSize < 16u ||
			    properties.limits.maxStorageBufferRange < kBufferBytes || subgroup_size_control.minSubgroupSize > 32u ||
			    subgroup_size_control.maxSubgroupSize < 32u || subgroup_size_control.maxComputeWorkgroupSubgroups < 1u ||
			    (subgroup_size_control.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) == 0 ||
			    supported_features13.subgroupSizeControl != VK_TRUE || supported_features13.computeFullSubgroups != VK_TRUE ||
			    (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) == 0 ||
			    (subgroup.supportedOperations & required_operations) != required_operations || !FindQueueFamily(candidate, &queue_family_))
			{
				continue;
			}
			physical_                                           = candidate;
			wave_capabilities_.size_control_enabled             = true;
			wave_capabilities_.full_subgroups_enabled           = true;
			wave_capabilities_.compute_required_size_supported  = true;
			wave_capabilities_.compute_ballot_shuffle_supported = true;
			wave_capabilities_.min_subgroup_size                = subgroup_size_control.minSubgroupSize;
			wave_capabilities_.max_subgroup_size                = subgroup_size_control.maxSubgroupSize;
			wave_capabilities_.max_local_size[0]                = properties.limits.maxComputeWorkGroupSize[0];
			wave_capabilities_.max_local_size[1]                = properties.limits.maxComputeWorkGroupSize[1];
			wave_capabilities_.max_local_size[2]                = properties.limits.maxComputeWorkGroupSize[2];
			wave_capabilities_.max_group_count[0]               = properties.limits.maxComputeWorkGroupCount[0];
			wave_capabilities_.max_group_count[1]               = properties.limits.maxComputeWorkGroupCount[1];
			wave_capabilities_.max_group_count[2]               = properties.limits.maxComputeWorkGroupCount[2];
			wave_capabilities_.max_invocations                  = properties.limits.maxComputeWorkGroupInvocations;
			wave_capabilities_.max_subgroups                    = subgroup_size_control.maxComputeWorkgroupSubgroups;
			wave_capabilities_.max_shared_bytes                 = properties.limits.maxComputeSharedMemorySize;
			break;
		}
		if (physical_ == VK_NULL_HANDLE)
		{
			*error = "no Vulkan 1.3 compute device with exact Wave32, full subgroups, ballot/shuffle, and bounded storage";
			return ProbeResult::Unavailable;
		}
		Kyty::Libs::Graphics::ShaderComputeWavePreflightRequest request {};
		request.is_next_gen           = true;
		request.dispatch_mode         = 0x8041u;
		request.raw_dispatch_count[0] = request.raw_dispatch_count[1] = request.raw_dispatch_count[2] = 1u;
		request.local_size[0]                                                                         = 32u;
		request.local_size[1] = request.local_size[2] = 1u;
		request.lane_order                            = Kyty::Libs::Graphics::ShaderGuestLaneOrder::LinearXFirst;
		const auto preflight = Kyty::Libs::Graphics::ShaderBuildComputeWaveDispatchPlan(request, wave_capabilities_, &wave_plan_);
		if (preflight.status != Kyty::Libs::Graphics::ShaderComputeWavePreflightStatus::Supported)
		{
			*error = "native Wave32 dispatch preflight rejected the GDS fixture";
			return ProbeResult::Unavailable;
		}
		wave_plan_.wave_layout.native_subgroup_size = 32u;
		request.raw_dispatch_count[0]               = kMaxWorkgroups;
		const auto native_multi_preflight =
		    Kyty::Libs::Graphics::ShaderBuildComputeWaveDispatchPlan(request, wave_capabilities_, &native_multi_wave_plan_);
		native_multi_preflight_reason_ = native_multi_preflight.reason;
		native_multi_wave_plan_available_ =
		    native_multi_preflight.status == Kyty::Libs::Graphics::ShaderComputeWavePreflightStatus::Supported;
		if (native_multi_wave_plan_available_)
		{
			native_multi_wave_plan_.wave_layout.native_subgroup_size = 32u;
		}

		request.dispatch_mode         = 0x0041u;
		request.raw_dispatch_count[0] = 1u;
		request.local_size[0]         = 64u;
		const auto paired_preflight =
		    Kyty::Libs::Graphics::ShaderBuildComputeWaveDispatchPlan(request, wave_capabilities_, &paired_wave_plan_);
		paired_wave_preflight_reason_ = paired_preflight.reason;
		paired_wave_plan_available_   = paired_preflight.status == Kyty::Libs::Graphics::ShaderComputeWavePreflightStatus::Supported;
		if (paired_wave_plan_available_)
		{
			paired_wave_plan_.wave_layout.native_subgroup_size = 32u;
			request.raw_dispatch_count[0]                      = kMaxWorkgroups;
			const auto paired_multi_preflight =
			    Kyty::Libs::Graphics::ShaderBuildComputeWaveDispatchPlan(request, wave_capabilities_, &paired_multi_wave_plan_);
			paired_multi_preflight_reason_ = paired_multi_preflight.reason;
			paired_multi_wave_plan_available_ =
			    paired_multi_preflight.status == Kyty::Libs::Graphics::ShaderComputeWavePreflightStatus::Supported;
			if (paired_multi_wave_plan_available_)
			{
				paired_multi_wave_plan_.wave_layout.native_subgroup_size = 32u;
			}
		}

		const float             priority = 1.0f;
		VkDeviceQueueCreateInfo queue_info {};
		queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queue_info.queueFamilyIndex = queue_family_;
		queue_info.queueCount       = 1;
		queue_info.pQueuePriorities = &priority;
		VkDeviceCreateInfo device_info {};
		device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		VkPhysicalDeviceVulkan13Features enabled_features13 {};
		enabled_features13.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		enabled_features13.subgroupSizeControl  = VK_TRUE;
		enabled_features13.computeFullSubgroups = VK_TRUE;
		device_info.pNext                       = &enabled_features13;
		device_info.queueCreateInfoCount        = 1;
		device_info.pQueueCreateInfos           = &queue_info;
		if (!CheckVk(vkCreateDevice(physical_, &device_info, nullptr, &device_), "vkCreateDevice", error))
		{
			return ProbeResult::Failure;
		}
		vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
		return ProbeResult::Success;
	}

	ProbeResult Dispatch(const Kyty::Vector<uint32_t>& spirv, const ShaderComputeInputInfo& input, uint32_t workgroup_count,
	                     const std::array<uint32_t, kBufferWords>& initial, std::array<uint32_t, kBufferWords>* output, std::string* error)
	{
		if (device_ == VK_NULL_HANDLE || spirv.IsEmpty() || workgroup_count == 0 || workgroup_count > kMaxWorkgroups || output == nullptr ||
		    error == nullptr || input.bind.gds_pointers.binding_index != 0 || input.bind.push_constant_size != 16u)
		{
			if (error != nullptr)
			{
				*error = "invalid GDS compute dispatch input";
			}
			return ProbeResult::Failure;
		}
		ResetDispatch();
		if (!CreateBuffer(initial, error) || !CreateDescriptors(error) || !CreatePipeline(spirv, input, error) ||
		    !Submit(input, workgroup_count, output, error))
		{
			return ProbeResult::Failure;
		}
		return ProbeResult::Success;
	}

	[[nodiscard]] const Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan& WavePlan() const noexcept { return wave_plan_; }
	[[nodiscard]] const Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan* NativeMultiWavePlan() const noexcept
	{
		return native_multi_wave_plan_available_ ? &native_multi_wave_plan_ : nullptr;
	}
	[[nodiscard]] Kyty::Libs::Graphics::ShaderComputeWavePreflightReason NativeMultiWavePreflightReason() const noexcept
	{
		return native_multi_preflight_reason_;
	}
	[[nodiscard]] const Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan* PairedWavePlan() const noexcept
	{
		return paired_wave_plan_available_ ? &paired_wave_plan_ : nullptr;
	}
	[[nodiscard]] Kyty::Libs::Graphics::ShaderComputeWavePreflightReason PairedWavePreflightReason() const noexcept
	{
		return paired_wave_preflight_reason_;
	}
	[[nodiscard]] const Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan* PairedMultiWavePlan() const noexcept
	{
		return paired_multi_wave_plan_available_ ? &paired_multi_wave_plan_ : nullptr;
	}
	[[nodiscard]] Kyty::Libs::Graphics::ShaderComputeWavePreflightReason PairedMultiWavePreflightReason() const noexcept
	{
		return paired_multi_preflight_reason_;
	}

private:
	void ResetDispatch()
	{
		if (mapped_ != nullptr)
		{
			vkUnmapMemory(device_, memory_);
			mapped_ = nullptr;
		}
		if (fence_ != VK_NULL_HANDLE)
		{
			vkDestroyFence(device_, fence_, nullptr);
			fence_ = VK_NULL_HANDLE;
		}
		if (command_pool_ != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(device_, command_pool_, nullptr);
			command_pool_ = VK_NULL_HANDLE;
		}
		if (pipeline_ != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device_, pipeline_, nullptr);
			pipeline_ = VK_NULL_HANDLE;
		}
		if (pipeline_layout_ != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
			pipeline_layout_ = VK_NULL_HANDLE;
		}
		if (descriptor_pool_ != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
			descriptor_pool_ = VK_NULL_HANDLE;
		}
		if (descriptor_layout_ != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
			descriptor_layout_ = VK_NULL_HANDLE;
		}
		if (shader_ != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device_, shader_, nullptr);
			shader_ = VK_NULL_HANDLE;
		}
		if (buffer_ != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device_, buffer_, nullptr);
			buffer_ = VK_NULL_HANDLE;
		}
		if (memory_ != VK_NULL_HANDLE)
		{
			vkFreeMemory(device_, memory_, nullptr);
			memory_ = VK_NULL_HANDLE;
		}
	}

	bool CreateBuffer(const std::array<uint32_t, kBufferWords>& initial, std::string* error)
	{
		VkBufferCreateInfo buffer_info {};
		buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size        = kBufferBytes;
		buffer_info.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (!CheckVk(vkCreateBuffer(device_, &buffer_info, nullptr, &buffer_), "vkCreateBuffer", error))
		{
			return false;
		}
		VkMemoryRequirements requirements {};
		vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
		uint32_t memory_type = 0;
		if (!FindHostVisibleMemory(physical_, requirements.memoryTypeBits, &memory_type, &coherent_))
		{
			*error = "no host-visible storage-buffer memory type";
			return false;
		}
		VkMemoryAllocateInfo allocation {};
		allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocation.allocationSize  = requirements.size;
		allocation.memoryTypeIndex = memory_type;
		if (!CheckVk(vkAllocateMemory(device_, &allocation, nullptr, &memory_), "vkAllocateMemory", error) ||
		    !CheckVk(vkBindBufferMemory(device_, buffer_, memory_, 0), "vkBindBufferMemory", error) ||
		    !CheckVk(vkMapMemory(device_, memory_, 0, requirements.size, 0, &mapped_), "vkMapMemory", error))
		{
			return false;
		}
		std::memcpy(mapped_, initial.data(), kBufferBytes);
		if (!coherent_)
		{
			VkMappedMemoryRange range {};
			range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = memory_;
			range.size   = VK_WHOLE_SIZE;
			if (!CheckVk(vkFlushMappedMemoryRanges(device_, 1, &range), "vkFlushMappedMemoryRanges", error))
			{
				return false;
			}
		}
		return true;
	}

	bool CreateDescriptors(std::string* error)
	{
		VkDescriptorSetLayoutBinding binding {};
		binding.binding         = 0;
		binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		binding.descriptorCount = 1;
		binding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
		VkDescriptorSetLayoutCreateInfo layout_info {};
		layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layout_info.bindingCount = 1;
		layout_info.pBindings    = &binding;
		if (!CheckVk(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_layout_), "vkCreateDescriptorSetLayout",
		             error))
		{
			return false;
		}
		VkDescriptorPoolSize pool_size {};
		pool_size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		pool_size.descriptorCount = 1;
		VkDescriptorPoolCreateInfo pool_info {};
		pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool_info.maxSets       = 1;
		pool_info.poolSizeCount = 1;
		pool_info.pPoolSizes    = &pool_size;
		if (!CheckVk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_), "vkCreateDescriptorPool", error))
		{
			return false;
		}
		VkDescriptorSetAllocateInfo allocate_info {};
		allocate_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocate_info.descriptorPool     = descriptor_pool_;
		allocate_info.descriptorSetCount = 1;
		allocate_info.pSetLayouts        = &descriptor_layout_;
		if (!CheckVk(vkAllocateDescriptorSets(device_, &allocate_info, &descriptor_set_), "vkAllocateDescriptorSets", error))
		{
			return false;
		}
		VkDescriptorBufferInfo buffer_info {};
		buffer_info.buffer = buffer_;
		buffer_info.range  = kBufferBytes;
		VkWriteDescriptorSet write {};
		write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet          = descriptor_set_;
		write.dstBinding      = 0;
		write.descriptorCount = 1;
		write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write.pBufferInfo     = &buffer_info;
		vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
		return true;
	}

	bool CreatePipeline(const Kyty::Vector<uint32_t>& spirv, const ShaderComputeInputInfo& input, std::string* error)
	{
		if (input.required_subgroup_size != 32u)
		{
			*error = "GDS EXEC pipeline must request native subgroup size 32";
			return false;
		}
		VkPushConstantRange push_range {};
		push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		push_range.offset     = input.bind.push_constant_offset;
		push_range.size       = input.bind.push_constant_size;
		VkPipelineLayoutCreateInfo layout_info {};
		layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layout_info.setLayoutCount         = 1;
		layout_info.pSetLayouts            = &descriptor_layout_;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges    = &push_range;
		if (!CheckVk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_), "vkCreatePipelineLayout", error))
		{
			return false;
		}
		VkShaderModuleCreateInfo shader_info {};
		shader_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		shader_info.codeSize = static_cast<size_t>(spirv.Size()) * sizeof(uint32_t);
		shader_info.pCode    = spirv.GetDataConst();
		if (!CheckVk(vkCreateShaderModule(device_, &shader_info, nullptr, &shader_), "vkCreateShaderModule", error))
		{
			return false;
		}
		VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required_subgroup_size {};
		required_subgroup_size.sType                = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
		required_subgroup_size.requiredSubgroupSize = input.required_subgroup_size;
		VkPipelineShaderStageCreateInfo stage {};
		stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage.pNext  = &required_subgroup_size;
		stage.flags  = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
		stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
		stage.module = shader_;
		stage.pName  = "main";
		VkComputePipelineCreateInfo pipeline_info {};
		pipeline_info.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipeline_info.stage  = stage;
		pipeline_info.layout = pipeline_layout_;
		return CheckVk(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_),
		               "vkCreateComputePipelines", error);
	}

	bool Submit(const ShaderComputeInputInfo& input, uint32_t workgroup_count, std::array<uint32_t, kBufferWords>* output,
	            std::string* error)
	{
		VkCommandPoolCreateInfo pool_info {};
		pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		pool_info.queueFamilyIndex = queue_family_;
		if (!CheckVk(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool", error))
		{
			return false;
		}
		VkCommandBufferAllocateInfo allocate_info {};
		allocate_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate_info.commandPool        = command_pool_;
		allocate_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate_info.commandBufferCount = 1;
		VkCommandBuffer command          = VK_NULL_HANDLE;
		if (!CheckVk(vkAllocateCommandBuffers(device_, &allocate_info, &command), "vkAllocateCommandBuffers", error))
		{
			return false;
		}
		VkCommandBufferBeginInfo begin_info {};
		begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (!CheckVk(vkBeginCommandBuffer(command, &begin_info), "vkBeginCommandBuffer", error))
		{
			return false;
		}
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
		const std::array<uint32_t, 4> metadata {};
		vkCmdPushConstants(command, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, input.bind.push_constant_offset,
		                   input.bind.push_constant_size, metadata.data());
		vkCmdDispatch(command, workgroup_count, 1, 1);
		VkBufferMemoryBarrier barrier {};
		barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer              = buffer_;
		barrier.size                = kBufferBytes;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0,
		                     nullptr);
		if (!CheckVk(vkEndCommandBuffer(command), "vkEndCommandBuffer", error))
		{
			return false;
		}
		VkFenceCreateInfo fence_info {};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (!CheckVk(vkCreateFence(device_, &fence_info, nullptr, &fence_), "vkCreateFence", error))
		{
			return false;
		}
		VkSubmitInfo submit_info {};
		submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit_info.commandBufferCount = 1;
		submit_info.pCommandBuffers    = &command;
		if (!CheckVk(vkQueueSubmit(queue_, 1, &submit_info, fence_), "vkQueueSubmit", error) ||
		    !CheckVk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000ull), "vkWaitForFences", error))
		{
			return false;
		}
		if (!coherent_)
		{
			VkMappedMemoryRange range {};
			range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = memory_;
			range.size   = VK_WHOLE_SIZE;
			if (!CheckVk(vkInvalidateMappedMemoryRanges(device_, 1, &range), "vkInvalidateMappedMemoryRanges", error))
			{
				return false;
			}
		}
		std::memcpy(output->data(), mapped_, kBufferBytes);
		return true;
	}

	VkInstance                                             instance_          = VK_NULL_HANDLE;
	VkPhysicalDevice                                       physical_          = VK_NULL_HANDLE;
	VkDevice                                               device_            = VK_NULL_HANDLE;
	VkQueue                                                queue_             = VK_NULL_HANDLE;
	uint32_t                                               queue_family_      = 0;
	VkBuffer                                               buffer_            = VK_NULL_HANDLE;
	VkDeviceMemory                                         memory_            = VK_NULL_HANDLE;
	void*                                                  mapped_            = nullptr;
	bool                                                   coherent_          = false;
	VkDescriptorSetLayout                                  descriptor_layout_ = VK_NULL_HANDLE;
	VkDescriptorPool                                       descriptor_pool_   = VK_NULL_HANDLE;
	VkDescriptorSet                                        descriptor_set_    = VK_NULL_HANDLE;
	VkPipelineLayout                                       pipeline_layout_   = VK_NULL_HANDLE;
	VkShaderModule                                         shader_            = VK_NULL_HANDLE;
	VkPipeline                                             pipeline_          = VK_NULL_HANDLE;
	VkCommandPool                                          command_pool_      = VK_NULL_HANDLE;
	VkFence                                                fence_             = VK_NULL_HANDLE;
	Kyty::Libs::Graphics::ShaderComputeWaveCapabilities    wave_capabilities_ {};
	Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan    wave_plan_ {};
	Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan    native_multi_wave_plan_ {};
	Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan    paired_wave_plan_ {};
	Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan    paired_multi_wave_plan_ {};
	Kyty::Libs::Graphics::ShaderComputeWavePreflightReason native_multi_preflight_reason_ =
	    Kyty::Libs::Graphics::ShaderComputeWavePreflightReason::None;
	Kyty::Libs::Graphics::ShaderComputeWavePreflightReason paired_wave_preflight_reason_ =
	    Kyty::Libs::Graphics::ShaderComputeWavePreflightReason::None;
	Kyty::Libs::Graphics::ShaderComputeWavePreflightReason paired_multi_preflight_reason_ =
	    Kyty::Libs::Graphics::ShaderComputeWavePreflightReason::None;
	bool native_multi_wave_plan_available_ = false;
	bool paired_wave_plan_available_       = false;
	bool paired_multi_wave_plan_available_ = false;
};

bool IsActiveLane(uint32_t lane, uint32_t comparison_opcode, uint32_t comparison_value)
{
	if (comparison_opcode == kVcmpxEqU32Opcode)
	{
		return lane == comparison_value;
	}
	return comparison_opcode == kVcmpxLeU32Opcode && lane <= comparison_value;
}

void RunCase(uint32_t opcode, uint32_t comparison_opcode, uint32_t comparison_value, uint32_t initial_counter, uint32_t expected_counter,
             uint32_t lane_count, uint32_t workgroup_count, const Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan& plan,
             const char* name, GdsExecVulkanProbe* vulkan)
{
	if (lane_count == 0 || lane_count > kMaxLaneCount || workgroup_count == 0 || workgroup_count > kMaxWorkgroups ||
	    1u + lane_count * workgroup_count > kBufferWords)
	{
		Fail(std::string(name) + ": invalid workgroup or lane count");
	}
	uint32_t active_lane_count = 0;
	for (uint32_t lane = 0; lane < lane_count; ++lane)
	{
		if (IsActiveLane(lane, comparison_opcode, comparison_value))
		{
			++active_lane_count;
		}
	}
	const uint32_t total_delta     = active_lane_count * workgroup_count;
	uint32_t       derived_counter = initial_counter;
	if (opcode == kAppendOpcode)
	{
		if (total_delta > std::numeric_limits<uint32_t>::max() - initial_counter)
		{
			Fail(std::string(name) + ": expected append counter overflows uint32");
		}
		derived_counter += total_delta;
	} else if (opcode == kConsumeOpcode)
	{
		if (total_delta > initial_counter)
		{
			Fail(std::string(name) + ": expected consume counter underflows uint32");
		}
		derived_counter -= total_delta;
	} else
	{
		Fail(std::string(name) + ": unsupported GDS operation");
	}
	if (expected_counter != derived_counter)
	{
		Fail(std::string(name) + ": fixture expected counter does not match the active-lane delta");
	}
	ShaderComputeInputInfo input {};
	Kyty::Vector<uint32_t> spirv;
	std::string            error;
	if (!BuildShader(opcode, comparison_opcode, comparison_value, &input, plan, &spirv, &error))
	{
		Fail(std::string(name) + ": " + error);
	}
	std::array<uint32_t, kBufferWords> initial {};
	initial[0] = initial_counter;
	std::array<uint32_t, kBufferWords> expected {};
	expected[0] = expected_counter;
	std::array<bool, kBufferWords> active_output_words {};
	for (uint32_t workgroup = 0; workgroup < workgroup_count; ++workgroup)
	{
		for (uint32_t lane = 0; lane < lane_count; ++lane)
		{
			const uint32_t word       = 1u + workgroup * lane_count + lane;
			const bool     active     = IsActiveLane(lane, comparison_opcode, comparison_value);
			active_output_words[word] = active;
			if (!active)
			{
				expected[word] = lane;
			}
		}
	}
	std::array<uint32_t, kBufferWords> actual {};
	const auto                         result = vulkan->Dispatch(spirv, input, workgroup_count, initial, &actual, &error);
	if (result != ProbeResult::Success)
	{
		Fail(std::string(name) + ": Vulkan dispatch failed: " + error);
	}
	for (uint32_t word = 0; word < kBufferWords; ++word)
	{
		if (!active_output_words[word] && actual[word] != expected[word])
		{
			Fail(std::string(name) + ": GDS word " + std::to_string(word) + " expected " + std::to_string(expected[word]) + ", got " +
			     std::to_string(actual[word]));
		}
	}
	if (active_lane_count > 0)
	{
		std::array<uint32_t, kMaxWorkgroups> reservation_starts {};
		const bool                           is_append          = opcode == kAppendOpcode;
		uint32_t                             second_reservation = initial_counter;
		if (workgroup_count == 2)
		{
			second_reservation = is_append ? initial_counter + active_lane_count : initial_counter - active_lane_count;
		}
		for (uint32_t workgroup = 0; workgroup < workgroup_count; ++workgroup)
		{
			bool has_reservation = false;
			for (uint32_t lane = 0; lane < lane_count; ++lane)
			{
				if (!IsActiveLane(lane, comparison_opcode, comparison_value))
				{
					continue;
				}
				const uint32_t word = 1u + workgroup * lane_count + lane;
				if (!has_reservation)
				{
					reservation_starts[workgroup] = actual[word];
					has_reservation               = true;
				} else if (actual[word] != reservation_starts[workgroup])
				{
					Fail(std::string(name) + ": active lanes in one workgroup received different reservations");
				}
			}
			if (!has_reservation)
			{
				Fail(std::string(name) + ": active-lane reservation was not written");
			}
		}
		if (workgroup_count == 1 && reservation_starts[0] != initial_counter)
		{
			Fail(std::string(name) + ": single workgroup returned an unexpected reservation start");
		}
		if (workgroup_count == 2)
		{
			const bool reservations_match_unordered =
			    (reservation_starts[0] == initial_counter && reservation_starts[1] == second_reservation) ||
			    (reservation_starts[1] == initial_counter && reservation_starts[0] == second_reservation);
			if (reservation_starts[0] == reservation_starts[1] || !reservations_match_unordered)
			{
				Fail(std::string(name) + ": workgroups did not receive the two unique unordered reservations");
			}
		}
	}
	std::printf("%s PASS\n", name);
}

} // namespace

int main()
{
	InitializeConfig();
	GdsExecVulkanProbe vulkan;
	std::string        error;
	const auto         initialized = vulkan.Initialize(&error);
	if (initialized == ProbeResult::Unavailable)
	{
		std::fprintf(stderr, "Vulkan compute capability unavailable: %s\n", error.c_str());
		return 77;
	}
	if (initialized != ProbeResult::Success)
	{
		Fail("Vulkan initialization failed: " + error);
	}

	// A full native Wave32 workgroup exercises zero, one, and multiple lanes in EXEC.
	// Results retain the original lane value for inactive destinations.
	RunCase(kAppendOpcode, kVcmpxEqU32Opcode, 32u, 41u, 41u, 32u, 1u, vulkan.WavePlan(), "EmptyExecAppend", &vulkan);
	RunCase(kAppendOpcode, kVcmpxEqU32Opcode, 1u, 41u, 42u, 32u, 1u, vulkan.WavePlan(), "SingleLaneAppend", &vulkan);
	RunCase(kAppendOpcode, kVcmpxLeU32Opcode, 1u, 41u, 43u, 32u, 1u, vulkan.WavePlan(), "TwoLaneAppend", &vulkan);
	RunCase(kConsumeOpcode, kVcmpxEqU32Opcode, 32u, 41u, 41u, 32u, 1u, vulkan.WavePlan(), "EmptyExecConsume", &vulkan);
	RunCase(kConsumeOpcode, kVcmpxEqU32Opcode, 1u, 41u, 40u, 32u, 1u, vulkan.WavePlan(), "SingleLaneConsume", &vulkan);
	RunCase(kConsumeOpcode, kVcmpxLeU32Opcode, 1u, 41u, 39u, 32u, 1u, vulkan.WavePlan(), "TwoLaneConsume", &vulkan);
	if (const auto* native_multi_plan = vulkan.NativeMultiWavePlan(); native_multi_plan != nullptr)
	{
		// Each workgroup reserves two lanes from one shared GDS counter. Result
		// reservations are checked as an unordered pair because Vulkan does not
		// promise which workgroup reaches the atomic first.
		RunCase(kAppendOpcode, kVcmpxLeU32Opcode, 1u, 41u, 45u, 32u, 2u, *native_multi_plan, "TwoWorkgroupAppend", &vulkan);
		RunCase(kConsumeOpcode, kVcmpxLeU32Opcode, 1u, 41u, 37u, 32u, 2u, *native_multi_plan, "TwoWorkgroupConsume", &vulkan);
	} else
	{
		std::fprintf(stderr, "Native Wave32 multi-workgroup GDS EXEC fixture unavailable: preflight reason=%u\n",
		             static_cast<unsigned>(vulkan.NativeMultiWavePreflightReason()));
	}

	if (const auto* paired_plan = vulkan.PairedWavePlan(); paired_plan != nullptr)
	{
		// The single-lane cases select logical lane 33 to exercise EXEC_HI; the
		// two-lane cases retain lanes 0 and 1 to check the paired low bank.
		RunCase(kAppendOpcode, kVcmpxEqU32Opcode, 64u, 41u, 41u, 64u, 1u, *paired_plan, "PairedEmptyExecAppend", &vulkan);
		RunCase(kAppendOpcode, kVcmpxEqU32Opcode, 33u, 41u, 42u, 64u, 1u, *paired_plan, "PairedSingleHighLaneAppend", &vulkan);
		RunCase(kAppendOpcode, kVcmpxLeU32Opcode, 1u, 41u, 43u, 64u, 1u, *paired_plan, "PairedTwoLaneAppend", &vulkan);
		RunCase(kConsumeOpcode, kVcmpxEqU32Opcode, 64u, 41u, 41u, 64u, 1u, *paired_plan, "PairedEmptyExecConsume", &vulkan);
		RunCase(kConsumeOpcode, kVcmpxEqU32Opcode, 33u, 41u, 40u, 64u, 1u, *paired_plan, "PairedSingleHighLaneConsume", &vulkan);
		RunCase(kConsumeOpcode, kVcmpxLeU32Opcode, 1u, 41u, 39u, 64u, 1u, *paired_plan, "PairedTwoLaneConsume", &vulkan);
		if (const auto* paired_multi_plan = vulkan.PairedMultiWavePlan(); paired_multi_plan != nullptr)
		{
			RunCase(kAppendOpcode, kVcmpxLeU32Opcode, 1u, 41u, 45u, 64u, 2u, *paired_multi_plan, "PairedTwoWorkgroupAppend", &vulkan);
			RunCase(kConsumeOpcode, kVcmpxLeU32Opcode, 1u, 41u, 37u, 64u, 2u, *paired_multi_plan, "PairedTwoWorkgroupConsume", &vulkan);
		} else
		{
			std::fprintf(stderr, "Split64On32 multi-workgroup GDS EXEC fixture unavailable: preflight reason=%u\n",
			             static_cast<unsigned>(vulkan.PairedMultiWavePreflightReason()));
		}
	} else
	{
		std::fprintf(stderr, "Split64On32 GDS EXEC fixture unavailable: preflight reason=%u\n",
		             static_cast<unsigned>(vulkan.PairedWavePreflightReason()));
	}
	return 0;
}
