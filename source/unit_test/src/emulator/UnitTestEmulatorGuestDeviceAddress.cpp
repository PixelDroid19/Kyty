#include "Kyty/UnitTest.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GuestDeviceAddress.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderSpirvInternal.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

UT_BEGIN(EmulatorGuestDeviceAddress);
using namespace Libs::Graphics;

namespace {

// The probes exit in separate processes: the global guest-address registry
// must not retain a table belonging to a device destroyed by an earlier test.
struct Device
{
	VkInstance instance = VK_NULL_HANDLE;
	GraphicContext context {};
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;

	bool Init()
	{
		VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.apiVersion = VK_API_VERSION_1_4;
		VkInstanceCreateInfo create {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		create.pApplicationInfo = &app;
		if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) { return false; }
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(instance, &count, devices.data());
		for (auto physical: devices)
		{
			VkPhysicalDeviceProperties properties {};
			vkGetPhysicalDeviceProperties(physical, &properties);
			if (properties.apiVersion < VK_API_VERSION_1_4) { continue; }
			vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
			std::vector<VkExtensionProperties> extensions(count);
			vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
			if (std::none_of(extensions.begin(), extensions.end(), [](const auto& e)
			                { return std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0; })) { continue; }
			VkPhysicalDeviceBufferDeviceAddressFeatures address {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
			VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			features.pNext = &address;
			vkGetPhysicalDeviceFeatures2(physical, &features);
			if (!address.bufferDeviceAddress || !features.features.shaderInt64) { continue; }
			vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
			std::vector<VkQueueFamilyProperties> families(count);
			vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
			for (family = 0; family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT); ++family) {}
			if (family == count) { continue; }
			const float priority = 1.0f;
			VkDeviceQueueCreateInfo queue_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
			queue_info.queueFamilyIndex = family;
			queue_info.queueCount = 1;
			queue_info.pQueuePriorities = &priority;
			const char* extension = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
			VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
			address.bufferDeviceAddressCaptureReplay = VK_FALSE;
			address.bufferDeviceAddressMultiDevice = VK_FALSE;
			VkPhysicalDeviceFeatures enabled {};
			enabled.shaderInt64 = VK_TRUE;
			device_info.pNext = &address;
			device_info.pEnabledFeatures = &enabled;
			device_info.queueCreateInfoCount = 1;
			device_info.pQueueCreateInfos = &queue_info;
			device_info.enabledExtensionCount = 1;
			device_info.ppEnabledExtensionNames = &extension;
			if (vkCreateDevice(physical, &device_info, nullptr, &context.device) != VK_SUCCESS) { continue; }
			context.physical_device = physical;
			context.guest_device_address_supported = true;
			vkGetDeviceQueue(context.device, family, 0, &queue);
			return true;
		}
		return false;
	}
};

struct Buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint32_t* words = nullptr;
	uint64_t address = 0;

	bool Init(Device& device, uint64_t bytes)
	{
		VkBufferCreateInfo create {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		create.size = bytes;
		create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		if (vkCreateBuffer(device.context.device, &create, nullptr, &buffer) != VK_SUCCESS) { return false; }
		VkMemoryRequirements requirements {};
		vkGetBufferMemoryRequirements(device.context.device, buffer, &requirements);
		VkPhysicalDeviceMemoryProperties properties {};
		vkGetPhysicalDeviceMemoryProperties(device.context.physical_device, &properties);
		uint32_t type = 0;
		constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		while (type < properties.memoryTypeCount &&
		       (!(requirements.memoryTypeBits & (1u << type)) || (properties.memoryTypes[type].propertyFlags & flags) != flags)) { ++type; }
		if (type == properties.memoryTypeCount) { return false; }
		VkMemoryAllocateFlagsInfo address_flags {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
		address_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		VkMemoryAllocateInfo allocate {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocate.pNext = &address_flags;
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = type;
		void* mapped = nullptr;
		if (vkAllocateMemory(device.context.device, &allocate, nullptr, &memory) != VK_SUCCESS ||
		    vkBindBufferMemory(device.context.device, buffer, memory, 0) != VK_SUCCESS ||
		    vkMapMemory(device.context.device, memory, 0, bytes, 0, &mapped) != VK_SUCCESS) { return false; }
		words = static_cast<uint32_t*>(mapped);
		std::memset(words, 0, bytes);
		VkBufferDeviceAddressInfo info {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
		info.buffer = buffer;
		address = vkGetBufferDeviceAddress(device.context.device, &info);
		return address != 0;
	}
};

int StableResidencyProbe()
{
	Device device;
	if (!device.Init()) { return 77; }
	constexpr uint64_t bytes = 64u * kGuestDeviceAddressPageBytes;
	const uint64_t guest = Core::VirtualMemory::Alloc(0, bytes, Core::VirtualMemory::Mode::ReadWrite);
	if (guest == 0) { return 2; }
	*reinterpret_cast<uint32_t*>(guest) = 0x12345678u;
	if (!Core::VirtualMemory::Protect(guest, bytes, Core::VirtualMemory::Mode::Read)) { return 3; }
	GuestDeviceAddressRegisterRange(guest, bytes);
	for (unsigned round = 0; round < 8; ++round)
	{
		uint64_t table = 0;
		uint32_t entries = 0;
		if (!GuestDeviceAddressPrepare(&device.context, &table, &entries)) { return 4; }
		std::array<uint8_t, 64> resident {};
		if (!Core::VirtualMemory::QueryResidentPages(guest, bytes, resident.data())) { return 5; }
		const auto count = std::count(resident.begin(), resident.end(), 1);
		if (entries != 1 || count != 1)
		{
			std::fprintf(stderr, "round=%u entries=%u resident=%zu\n", round, entries, static_cast<size_t>(count));
			return 6;
		}
	}
	return 0;
}

String8 LoadProbeSource()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	const uint32_t end[] = {0xbf810000u};
	ShaderParse(end, sizeof(end), &code);
	ShaderComputeInputInfo input {};
	input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
	input.bind.device_address_used = true;
	ShaderCalcBindingIndices(&input.bind);
	Spirv spirv;
	spirv.SetCode(code);
	spirv.SetCsInputInfo(&input);
	spirv.GenerateSource();
	String8 source = R"(
OpCapability Shader
OpCapability Int64
OpCapability PhysicalStorageBufferAddresses
OpExtension "SPV_KHR_physical_storage_buffer"
OpMemoryModel PhysicalStorageBuffer64 GLSL450
OpEntryPoint GLCompute %main "main" %vsharp
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %push_array ArrayStride 16
OpMemberDecorate %push_block 0 Offset 0
OpDecorate %push_block Block
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%uint_0 = OpConstant %uint 0
%uint_1 = OpConstant %uint 1
%uint_2 = OpConstant %uint 2
%uint_32 = OpConstant %uint 32
%int_0 = OpConstant %int 0
%int_1 = OpConstant %int 1
%int_2 = OpConstant %int 2
%int_3 = OpConstant %int 3
%v4uint = OpTypeVector %uint 4
%push_array = OpTypeArray %v4uint %uint_2
%push_block = OpTypeStruct %push_array
%ptr_push_block = OpTypePointer PushConstant %push_block
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
%vsharp = OpVariable %ptr_push_block PushConstant
%fn_main = OpTypeFunction %void
)";
	source += spirv.GuestDeviceAddressTypes(false);
	source += R"(
%main = OpFunction %void None %fn_main
%entry = OpLabel
%guest_lo_p = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_1 %int_0
%guest_hi_p = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_1 %int_1
%output_lo_p = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_1 %int_2
%output_hi_p = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_1 %int_3
%guest_lo = OpLoad %uint %guest_lo_p
%guest_hi = OpLoad %uint %guest_hi_p
%output_lo = OpLoad %uint %output_lo_p
%output_hi = OpLoad %uint %output_hi_p
%output_lo64 = OpUConvert %ulong %output_lo
%output_hi64 = OpUConvert %ulong %output_hi
%output_high = OpShiftLeftLogical %ulong %output_hi64 %uint_32
%output_address = OpBitwiseOr %ulong %output_lo64 %output_high
)";
	if (!spirv.EmitGuestLoad("guest_lo", "guest_hi", 32, "probe", &source)) { return {}; }
	for (unsigned word = 0; word < 32; ++word)
	{
		source += String8::FromPrintf("%%out_%u = OpIAdd %%ulong %%output_address %%gda_u64_%u\n"
		                             "%%out_ptr_%u = OpConvertUToPtr %%_ptr_PhysicalStorageBuffer_uint %%out_%u\n"
		                             "OpStore %%out_ptr_%u %%probe_d%u Aligned 4\n", word, word * 4u, word, word, word, word);
	}
	source += "OpReturn\nOpFunctionEnd\n";
	source += spirv.GuestDeviceAddressFunction();
	return source;
}

int PageBoundaryLoadProbe()
{
	Device device;
	if (!device.Init()) { return 77; }
	Vector<uint32_t> binary;
	String8 error;
	if (!ShaderToolchain::Run(LoadProbeSource(), &binary, &error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 10;
	}
	Buffer first, second, table, output;
	if (!first.Init(device, 4096) || !second.Init(device, 4096) || !table.Init(device, 320) || !output.Init(device, 128)) { return 11; }
	for (unsigned word = 0; word < 1024; ++word) { first.words[word] = 1000 + word; second.words[word] = 3000 + word; }
	const uint64_t entries[] = {0x100000, 4096, first.address, 4096, 0x101000, 4096, second.address, 4096};
	std::memcpy(table.words + 64, entries, sizeof(entries));
	VkShaderModuleCreateInfo module_info {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	module_info.codeSize = binary.Size() * sizeof(uint32_t);
	module_info.pCode = binary.GetDataConst();
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device.context.device, &module_info, nullptr, &module) != VK_SUCCESS) { return 12; }
	VkPushConstantRange push_range {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
	VkPipelineLayoutCreateInfo layout_info {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	if (vkCreatePipelineLayout(device.context.device, &layout_info, nullptr, &layout) != VK_SUCCESS) { return 13; }
	VkComputePipelineCreateInfo pipeline_info {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName = "main";
	pipeline_info.layout = layout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	if (vkCreateComputePipelines(device.context.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) { return 14; }
	VkCommandPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.queueFamilyIndex = device.family;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VkCommandPool pool = VK_NULL_HANDLE;
	if (vkCreateCommandPool(device.context.device, &pool_info, nullptr, &pool) != VK_SUCCESS) { return 15; }
	VkCommandBufferAllocateInfo command_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	command_info.commandPool = pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	VkCommandBuffer command = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(device.context.device, &command_info, &command) != VK_SUCCESS) { return 16; }
	VkFenceCreateInfo fence_info {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence = VK_NULL_HANDLE;
	if (vkCreateFence(device.context.device, &fence_info, nullptr, &fence) != VK_SUCCESS) { return 17; }
	for (unsigned test = 0; test < 3; ++test)
	{
		const uint64_t guest = test == 2 ? 0x100f80 : 0x100fc0;
		const uint32_t push[] = {static_cast<uint32_t>(table.address), static_cast<uint32_t>(table.address >> 32), test == 1 ? 1u : 2u, 0,
		                         static_cast<uint32_t>(guest), 0, static_cast<uint32_t>(output.address), static_cast<uint32_t>(output.address >> 32)};
		std::fill(output.words, output.words + 32, 0xdeadbeefu);
		VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) { return 18; }
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(command, 1, 1, 1);
		VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		if (vkEndCommandBuffer(command) != VK_SUCCESS) { return 19; }
		VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		if (vkQueueSubmit(device.queue, 1, &submit, fence) != VK_SUCCESS ||
		    vkWaitForFences(device.context.device, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) { return 20; }
		for (unsigned word = 0; word < 32; ++word)
		{
			const uint32_t expected = test == 2 ? 1992 + word : word < 16 ? 2008 + word : test == 1 ? 0 : 3000 + word - 16;
			if (output.words[word] != expected)
			{
				std::fprintf(stderr, "case=%u word=%u actual=%u expected=%u\n", test, word, output.words[word], expected);
				return 21;
			}
		}
		vkResetFences(device.context.device, 1, &fence);
		vkResetCommandBuffer(command, 0);
	}
	return 0;
}

void RunIsolated(int (*probe)())
{
#if defined(__linux__)
	const pid_t child = fork();
	ASSERT_GE(child, 0);
	if (child == 0) { std::_Exit(probe()); }
	int status = 0;
	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status));
	if (WEXITSTATUS(status) == 77) { GTEST_SKIP() << "Vulkan device with host import and device addressing is unavailable"; }
	EXPECT_EQ(WEXITSTATUS(status), 0);
#else
	(void)probe;
	GTEST_SKIP() << "isolated host residency probe requires Linux";
#endif
}

} // namespace

TEST(EmulatorGuestDeviceAddress, RepeatedPrepareDoesNotImportUntouchedNeighbours) { RunIsolated(StableResidencyProbe); }
TEST(EmulatorGuestDeviceAddress, VulkanLoadPreservesWordsAcrossImportedPageBoundary) { RunIsolated(PageBoundaryLoadProbe); }

UT_END();
