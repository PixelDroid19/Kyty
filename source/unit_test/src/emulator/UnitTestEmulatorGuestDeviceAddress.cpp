#include "Kyty/UnitTest.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/GpuMemoryFault.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GuestDeviceAddress.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderSpirvInternal.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
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
	GuestDeviceAddressRegisterRange(guest, bytes, false);
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

// A range without a population summary (private anonymous memory) is rescanned
// on every preparation: a page touched after one is imported by the next.
int AnonymousResidencyProbe()
{
	Device device;
	if (!device.Init()) { return 77; }
	constexpr uint64_t bytes = 64u * kGuestDeviceAddressPageBytes;
	const uint64_t guest = Core::VirtualMemory::Alloc(0, bytes, Core::VirtualMemory::Mode::ReadWrite);
	if (guest == 0) { return 2; }
	*reinterpret_cast<uint32_t*>(guest) = 0x12345678u;
	if (!Core::VirtualMemory::Protect(guest, bytes, Core::VirtualMemory::Mode::Read)) { return 2; }
	GuestDeviceAddressRegisterRange(guest, bytes, false);
	uint64_t table   = 0;
	uint32_t entries = 0;
	if (!GuestDeviceAddressPrepare(&device.context, &table, &entries) || entries != 1) { return 3; }
	const uint64_t touched = guest + 10u * kGuestDeviceAddressPageBytes;
	if (!Core::VirtualMemory::Protect(touched, kGuestDeviceAddressPageBytes, Core::VirtualMemory::Mode::ReadWrite)) { return 2; }
	*reinterpret_cast<uint32_t*>(touched) = 0x9abcdef0u;
	if (!Core::VirtualMemory::Protect(touched, kGuestDeviceAddressPageBytes, Core::VirtualMemory::Mode::Read)) { return 2; }
	if (!GuestDeviceAddressPrepare(&device.context, &table, &entries) || entries != 2)
	{
		std::fprintf(stderr, "entries=%u after the touch\n", entries);
		return 4;
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

// The owner's markers sit on the last host page of its first 16 KiB guest page
// and the first host page of the next, so they are resident and contiguous
// across the boundary of a protection change of the first guest page.
constexpr uint64_t kOwnerBytes      = 0x10000; // four 16 KiB guest pages
constexpr uint64_t kGuestPageBytes  = 0x4000;
constexpr uint64_t kWritableOffset  = kGuestPageBytes - kGuestDeviceAddressPageBytes;
constexpr uint64_t kNeighbourOffset = kGuestPageBytes;
constexpr uint32_t kMarkerA         = 0xa1a10000u;
constexpr uint32_t kMarkerB         = 0xb2b20000u;
constexpr uint32_t kNeighbourA      = 0xc3c30000u;
constexpr uint32_t kNeighbourB      = 0xd4d40000u;

// Routes the kernel's GPU-mapping lifecycle to the device-address registry of
// one owned device, as the graphics adapter does; the queue drains before any
// import is dropped, and a protection drops imports only once Kernel applied it.
struct LifecycleBridge
{
	Device   device;
	uint32_t releases       = 0;
	uint64_t released_vaddr = 0;
	uint64_t released_size  = 0;
	uint32_t protections    = 0;

	// When `race_owner` is set, a protection first has another thread unmap
	// that owner; its release waits until `release_permitted`, as an unmap
	// waits for the submission gate a protection holds.
	uint64_t                race_owner        = 0;
	int                     unmap_result      = -1;
	bool                    release_entered   = false;
	bool                    release_permitted = true;
	std::mutex              mutex;
	std::condition_variable changed;
	std::thread             unmapper;

	bool UnmapConcurrently()
	{
		unmapper = std::thread([this] { unmap_result = Kernel::Memory::KernelMunmap(race_owner, kOwnerBytes); });
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, std::chrono::seconds(5), [this] { return release_entered; });
	}

	void PermitRelease()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			release_permitted = true;
		}
		changed.notify_all();
		if (unmapper.joinable()) { unmapper.join(); }
	}

	static void Register(void* context, uint64_t vaddr, uint64_t size, Kernel::Memory::KernelGpuMappingBacking backing)
	{
		(void)context;
		GuestDeviceAddressRegisterRange(vaddr, size, backing == Kernel::Memory::KernelGpuMappingBacking::Physical);
	}

	static bool Invalidate(void* context, uint64_t vaddr, uint64_t size)
	{
		auto* bridge = static_cast<LifecycleBridge*>(context);
		if (vkQueueWaitIdle(bridge->device.queue) != VK_SUCCESS) { return false; }
		GuestDeviceAddressInvalidateRangeQuiesced(&bridge->device.context, vaddr, size);
		return true;
	}

	static bool Release(void* context, uint64_t vaddr, uint64_t size, Kernel::Memory::KernelGpuMappingCompletion completion, void* data)
	{
		auto* bridge = static_cast<LifecycleBridge*>(context);
		{
			std::unique_lock<std::mutex> lock(bridge->mutex);
			bridge->release_entered = true;
			bridge->changed.notify_all();
			if (!bridge->changed.wait_for(lock, std::chrono::seconds(5), [bridge] { return bridge->release_permitted; })) { return false; }
		}
		if (vkQueueWaitIdle(bridge->device.queue) != VK_SUCCESS) { return false; }
		GuestDeviceAddressReleaseRangeQuiesced(&bridge->device.context, vaddr, size);
		bridge->releases++;
		bridge->released_vaddr = vaddr;
		bridge->released_size  = size;
		return completion(data);
	}

	static bool Protect(void* context, uint64_t vaddr, uint64_t size, Kernel::Memory::KernelGpuMappingCompletion completion, void* data)
	{
		auto* bridge = static_cast<LifecycleBridge*>(context);
		bridge->protections++;
		if (vkQueueWaitIdle(bridge->device.queue) != VK_SUCCESS) { return false; }
		if (bridge->race_owner != 0 && !bridge->UnmapConcurrently()) { return false; }
		if (!completion(data)) { return false; }
		GuestDeviceAddressInvalidateRangeQuiesced(&bridge->device.context, vaddr, size);
		return true;
	}
};

// The probe shader against the production table: each Expect dispatches one
// 32-word load of a guest address and compares it with a marker payload.
struct GuestLoad
{
	Device*          device   = nullptr;
	Buffer           output;
	VkPipelineLayout layout   = VK_NULL_HANDLE;
	VkPipeline       pipeline = VK_NULL_HANDLE;
	VkCommandBuffer  command  = VK_NULL_HANDLE;
	VkFence          fence    = VK_NULL_HANDLE;

	int Init(Device* owner)
	{
		device = owner;
		Vector<uint32_t> binary;
		String8          error;
		if (!ShaderToolchain::Run(LoadProbeSource(), &binary, &error))
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 10;
		}
		if (!output.Init(*device, 128)) { return 11; }
		const VkDevice vk = device->context.device;
		VkShaderModuleCreateInfo module_info {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		module_info.codeSize = binary.Size() * sizeof(uint32_t);
		module_info.pCode = binary.GetDataConst();
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(vk, &module_info, nullptr, &module) != VK_SUCCESS) { return 12; }
		VkPushConstantRange push_range {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
		VkPipelineLayoutCreateInfo layout_info {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = &push_range;
		if (vkCreatePipelineLayout(vk, &layout_info, nullptr, &layout) != VK_SUCCESS) { return 13; }
		VkComputePipelineCreateInfo pipeline_info {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		pipeline_info.stage.module = module;
		pipeline_info.stage.pName = "main";
		pipeline_info.layout = layout;
		if (vkCreateComputePipelines(vk, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) { return 14; }
		VkCommandPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pool_info.queueFamilyIndex = device->family;
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VkCommandPool pool = VK_NULL_HANDLE;
		if (vkCreateCommandPool(vk, &pool_info, nullptr, &pool) != VK_SUCCESS) { return 15; }
		VkCommandBufferAllocateInfo command_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		command_info.commandPool = pool;
		command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		command_info.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(vk, &command_info, &command) != VK_SUCCESS) { return 16; }
		VkFenceCreateInfo fence_info {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		return vkCreateFence(vk, &fence_info, nullptr, &fence) == VK_SUCCESS ? 0 : 17;
	}

	bool Dispatch(uint64_t table, uint32_t entries, uint64_t guest)
	{
		const VkDevice vk = device->context.device;
		const uint32_t push[] = {static_cast<uint32_t>(table), static_cast<uint32_t>(table >> 32), entries, 0,
		                         static_cast<uint32_t>(guest), static_cast<uint32_t>(guest >> 32),
		                         static_cast<uint32_t>(output.address), static_cast<uint32_t>(output.address >> 32)};
		std::fill(output.words, output.words + 32, 0xdeadbeefu);
		VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		if (vkResetFences(vk, 1, &fence) != VK_SUCCESS || vkResetCommandBuffer(command, 0) != VK_SUCCESS ||
		    vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) { return false; }
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(command, 1, 1, 1);
		VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		if (vkEndCommandBuffer(command) != VK_SUCCESS) { return false; }
		VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		return vkQueueSubmit(device->queue, 1, &submit, fence) == VK_SUCCESS &&
		       vkWaitForFences(vk, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
	}

	int Expect(const char* stage, uint64_t table, uint32_t entries, uint64_t guest, uint32_t marker)
	{
		if (!Dispatch(table, entries, guest))
		{
			std::fprintf(stderr, "%s: dispatch failed\n", stage);
			return 20;
		}
		for (unsigned word = 0; word < 32; ++word)
		{
			if (output.words[word] != (marker | word))
			{
				std::fprintf(stderr, "%s: word=%u actual=0x%08x expected=0x%08x\n", stage, word, output.words[word], marker | word);
				return 21;
			}
		}
		return 0;
	}
};

void WriteMarker(uint64_t guest, uint32_t marker)
{
	auto* words = reinterpret_cast<uint32_t*>(guest);
	for (unsigned word = 0; word < 32; ++word) { words[word] = marker | word; }
}

void RouteGuestFault(const Core::VirtualMemory::ExceptionHandler::ExceptionInfo* info)
{
	if (info->type == Core::VirtualMemory::ExceptionHandler::ExceptionType::AccessViolation &&
	    Kyty::Emulator::GpuMemoryFault::GetPort().HandleAccessViolation(info->access_violation_vaddr, info->access_violation_type)) { return; }
	Core::VirtualMemory::FatalFault(info);
}

// Brings up in this child what a runtime gives a GPU-visible mapping: Config
// and Log (through the probe source), kernel memory, the write-fault route
// that enables dirty tracking, and the process-lifetime lifecycle port.
int StartGpuMappingRuntime(LifecycleBridge* bridge, GuestLoad* load)
{
	if (!bridge->device.Init()) { return 77; }
	if (const int status = load->Init(&bridge->device); status != 0) { return status; }
	Kernel::Memory::MemorySubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	const Kyty::Emulator::GpuMemoryFault::Callbacks fault {
	    [](uint64_t address, Kyty::Emulator::GpuMemoryFault::AccessViolationKind access)
	    { return GpuDirtyPageTrackerHandleAccessFault(address, access); },
	    GpuDirtyPageTrackerNotifyFaultHandlerInstalled};
	if (!Kyty::Emulator::GpuMemoryFault::GetPort().Install(fault) || !Core::VirtualMemory::ExceptionHandler::InstallVectored(RouteGuestFault))
	{
		return 2;
	}
	Kyty::Emulator::GpuMemoryFault::GetPort().NotifyFaultHandlerInstalled();
	const Kernel::Memory::GpuMappingLifecycleCallbacks callbacks {bridge, LifecycleBridge::Register, LifecycleBridge::Invalidate,
	                                                             LifecycleBridge::Release, LifecycleBridge::Protect};
	return Kernel::Memory::GetGpuMappingLifecyclePort().Install(callbacks) ? 0 : 3;
}

// A CPU-writable flexible owner aligned to its own size, seeded with markers.
uint64_t MapSeededOwner()
{
	void* owner = nullptr;
	if (Kernel::Memory::KernelReserveVirtualRange(&owner, kOwnerBytes, 0, kOwnerBytes) != 0 ||
	    Kernel::Memory::KernelMapNamedFlexibleMemory(&owner, kOwnerBytes, 0x03, 0x10, "device-address-owner") != 0) { return 0; }
	const auto base = reinterpret_cast<uint64_t>(owner);
	WriteMarker(base + kWritableOffset, kMarkerA);
	WriteMarker(base + kNeighbourOffset, kNeighbourA);
	return base;
}

// Unmapping an owner that was ever GPU-visible releases it through the port
// exactly once, after which the table maps nothing.
int ExpectOwnerReleased(LifecycleBridge* bridge, uint64_t base)
{
	if (Kernel::Memory::KernelMunmap(base, kOwnerBytes) != 0) { return 50; }
	if (bridge->releases != 1 || bridge->released_vaddr != base || bridge->released_size != kOwnerBytes)
	{
		std::fprintf(stderr, "releases=%u vaddr=0x%" PRIx64 " size=0x%" PRIx64 "\n", bridge->releases, bridge->released_vaddr,
		             bridge->released_size);
		return 51;
	}
	uint64_t table   = 0;
	uint32_t entries = 0;
	if (!GuestDeviceAddressPrepare(&bridge->device.context, &table, &entries) || entries != 0) { return 52; }
	return 0;
}

// A read-only GPU-visible owner is imported as a copy that cannot change until
// another mprotect. Making its first guest page CPU-writable again must stop the
// GPU from reading that copy and keep following every later CPU write there,
// while the read-only neighbour on the adjacent resident page keeps its own
// contents and preparation stays stable, until the whole owner becomes writable.
int WritableTransitionProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0) { return status; }
	const uint64_t base = MapSeededOwner();
	if (base == 0) { return 40; }
	auto* const    owner     = reinterpret_cast<void*>(base);
	auto* const    ctx       = &bridge.device.context;
	const uint64_t writable  = base + kWritableOffset;
	const uint64_t neighbour = base + kNeighbourOffset;
	uint64_t       table     = 0;
	uint32_t       entries   = 0;
	if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x11) != 0) { return 41; }
	if (!GuestDeviceAddressPrepare(ctx, &table, &entries)) { return 42; }
	if (const int status = load.Expect("read-only", table, entries, writable, kMarkerA); status != 0) { return status; }
	if (const int status = load.Expect("read-only neighbour", table, entries, neighbour, kNeighbourA); status != 0) { return status; }

	if (Kernel::Memory::KernelMprotect(owner, kGuestPageBytes, 0x12) != 0) { return 43; }
	uint32_t stable = 0;
	uint32_t marker = kMarkerB;
	for (unsigned round = 0; round < 3; ++round)
	{
		// Every round writes after the previous preparation imported the page.
		marker = kMarkerB + (round << 8u);
		WriteMarker(writable, marker);
		if (!GuestDeviceAddressPrepare(ctx, &table, &entries)) { return 44; }
		if (round != 0 && entries != stable)
		{
			std::fprintf(stderr, "round=%u entries=%u expected=%u\n", round, entries, stable);
			return 45;
		}
		stable = entries;
		if (const int status = load.Expect("partial writable", table, entries, writable, marker); status != 0) { return status; }
		if (const int status = load.Expect("unchanged neighbour", table, entries, neighbour, kNeighbourA); status != 0) { return status; }
	}

	if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x12) != 0) { return 46; }
	WriteMarker(neighbour, kNeighbourB);
	if (!GuestDeviceAddressPrepare(ctx, &table, &entries)) { return 47; }
	if (const int status = load.Expect("writable", table, entries, writable, marker); status != 0) { return status; }
	if (const int status = load.Expect("writable neighbour", table, entries, neighbour, kNeighbourB); status != 0) { return status; }
	// Repeated permission changes retire and rebuild tables at quiescence. Every
	// rebuilt import must still follow writes and preserve its neighbour.
	for (unsigned cycle = 0; cycle < 4; ++cycle)
	{
		if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x11) != 0) { return 48; }
		if (!GuestDeviceAddressPrepare(ctx, &table, &entries)) { return 49; }
		if (const int status = load.Expect("read-only cycle", table, entries, writable, marker); status != 0) { return status; }
		if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x12) != 0) { return 53; }
		marker = kMarkerB + ((cycle + 4u) << 8u);
		WriteMarker(writable, marker);
		if (!GuestDeviceAddressPrepare(ctx, &table, &entries)) { return 54; }
		if (const int status = load.Expect("writable cycle", table, entries, writable, marker); status != 0) { return status; }
		if (const int status = load.Expect("cycle neighbour", table, entries, neighbour, kNeighbourB); status != 0) { return status; }
	}
	return ExpectOwnerReleased(&bridge, base);
}

// CPU-only protection keeps the owner's GPU cleanup obligation and lets the CPU
// write it; once GPU read access returns, the GPU reads those writes.
int CpuOnlyTransitionProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0) { return status; }
	const uint64_t base = MapSeededOwner();
	if (base == 0) { return 40; }
	auto* const    owner    = reinterpret_cast<void*>(base);
	auto* const    ctx      = &bridge.device.context;
	const uint64_t writable = base + kWritableOffset;
	uint64_t       table    = 0;
	uint32_t       entries  = 0;
	if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x11) != 0) { return 41; }
	if (!GuestDeviceAddressPrepare(ctx, &table, &entries)) { return 42; }
	if (const int status = load.Expect("read-only", table, entries, writable, kMarkerA); status != 0) { return status; }

	if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x3) != 0) { return 43; }
	WriteMarker(writable, kMarkerB);
	if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x11) != 0) { return 44; }
	if (!GuestDeviceAddressPrepare(ctx, &table, &entries)) { return 45; }
	if (const int status = load.Expect("read-only after cpu-only", table, entries, writable, kMarkerB); status != 0) { return status; }
	return ExpectOwnerReleased(&bridge, base);
}

// An unmap claimed while a writable protection waits for quiescence makes the
// protection fail: the owner keeps its read-only rights, the GPU keeps reading
// the same imports through the same table, and the unmap then completes.
int PendingUnmapProtectionProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0) { return status; }
	const uint64_t base = MapSeededOwner();
	if (base == 0) { return 40; }
	auto* const    owner     = reinterpret_cast<void*>(base);
	auto* const    ctx       = &bridge.device.context;
	const uint64_t writable  = base + kWritableOffset;
	const uint64_t neighbour = base + kNeighbourOffset;
	uint64_t       table     = 0;
	uint32_t       entries   = 0;
	if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x11) != 0) { return 41; }
	if (!GuestDeviceAddressPrepare(ctx, &table, &entries)) { return 42; }
	if (const int status = load.Expect("read-only", table, entries, writable, kMarkerA); status != 0) { return status; }
	const uint64_t imported_table   = table;
	const uint32_t imported_entries = entries;

	bridge.race_owner        = base;
	bridge.release_permitted = false;
	const int protect_result = Kernel::Memory::KernelMprotect(owner, kGuestPageBytes, 0x12);
	int       status         = 0;
	if (protect_result != Kernel::KERNEL_ERROR_EBUSY || bridge.protections != 1 || !bridge.release_entered)
	{
		std::fprintf(stderr, "protect=%d protections=%u release_entered=%d\n", protect_result, bridge.protections,
		             bridge.release_entered ? 1 : 0);
		status = 43;
	} else if (Core::VirtualMemory::IsRangeWritable(base, kGuestPageBytes))
	{
		status = 44;
	} else if (!GuestDeviceAddressPrepare(ctx, &table, &entries) || table != imported_table || entries != imported_entries)
	{
		std::fprintf(stderr, "entries=%u expected=%u table_changed=%d\n", entries, imported_entries, table != imported_table ? 1 : 0);
		status = 45;
	} else if (status = load.Expect("refused protection", table, entries, writable, kMarkerA); status == 0)
	{
		status = load.Expect("refused protection neighbour", table, entries, neighbour, kNeighbourA);
	}
	bridge.PermitRelease();
	if (status != 0) { return status; }
	if (bridge.unmap_result != 0 || bridge.releases != 1 || bridge.released_vaddr != base) { return 46; }
	return GuestDeviceAddressPrepare(ctx, &table, &entries) && entries == 0 ? 0 : 47;
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
TEST(EmulatorGuestDeviceAddress, AnAnonymousRangeImportsATouchedPageOnTheNextPreparation) { RunIsolated(AnonymousResidencyProbe); }
TEST(EmulatorGuestDeviceAddress, AWritableGpuProtectionStopsReadingTheReadOnlyCopy) { RunIsolated(WritableTransitionProbe); }
TEST(EmulatorGuestDeviceAddress, RestoredGpuReadAfterCpuOnlyProtectionSeesCpuWrites) { RunIsolated(CpuOnlyTransitionProbe); }
TEST(EmulatorGuestDeviceAddress, AProtectionRefusedForAPendingUnmapKeepsRightsAndImports) { RunIsolated(PendingUnmapProtectionProbe); }

UT_END();
