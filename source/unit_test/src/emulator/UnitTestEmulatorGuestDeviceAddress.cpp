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
#include "Emulator/Kernel/GpuMappingLifecycle.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderSpirvInternal.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
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
	bool timeline_semaphore_supported = false;

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
			VkPhysicalDeviceTimelineSemaphoreFeatures timeline {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
			address.pNext = &timeline;
			VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			features.pNext = &address;
			vkGetPhysicalDeviceFeatures2(physical, &features);
			if (!address.bufferDeviceAddress || !features.features.shaderInt64) { continue; }
			const bool supports_timeline_semaphore = timeline.timelineSemaphore == VK_TRUE;
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
			timeline.timelineSemaphore = supports_timeline_semaphore ? VK_TRUE : VK_FALSE;
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
			timeline_semaphore_supported = supports_timeline_semaphore;
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
constexpr uint32_t kMarkerC         = 0xc3c30000u;
constexpr uint32_t kNeighbourA      = 0xc3c30000u;
constexpr uint32_t kNeighbourB      = 0xd4d40000u;
constexpr uint32_t kPhysicalRegistrationMarker = 0xe5e50000u;
constexpr uint32_t kPhysicalReleaseMarker     = 0xf6f60000u;
constexpr uint32_t kRetiredTablePressureBudget = 32;

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
	std::mutex              registration_mutex;
	std::condition_variable registration_changed;
	bool                    hold_next_registration = false;
	bool                    registration_entered   = false;
	bool                    registration_permitted = true;

	void HoldNextRegistration()
	{
		std::lock_guard<std::mutex> lock(registration_mutex);
		hold_next_registration = true;
		registration_entered   = false;
		registration_permitted = false;
	}

	bool WaitForRegistration(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(registration_mutex);
		return registration_changed.wait_for(lock, timeout, [this] { return registration_entered; });
	}

	void PermitRegistration()
	{
		{
			std::lock_guard<std::mutex> lock(registration_mutex);
			registration_permitted = true;
		}
		registration_changed.notify_all();
	}

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
		auto* bridge = static_cast<LifecycleBridge*>(context);
		GuestDeviceAddressRegisterRange(vaddr, size, backing == Kernel::Memory::KernelGpuMappingBacking::Physical);
		std::unique_lock<std::mutex> lock(bridge->registration_mutex);
		if (bridge->hold_next_registration)
		{
			bridge->hold_next_registration = false;
			bridge->registration_entered   = true;
			bridge->registration_changed.notify_all();
			(void)bridge->registration_changed.wait_for(lock, std::chrono::seconds(5),
			                                           [bridge] { return bridge->registration_permitted; });
		}
	}

	static bool Invalidate(void* context, const Kernel::Memory::KernelGpuMappingRange* ranges, size_t range_count)
	{
		auto* bridge = static_cast<LifecycleBridge*>(context);
		if (ranges == nullptr || range_count == 0) { return false; }
		for (size_t i = 0; i < range_count; ++i)
		{
			if (ranges[i].vaddr == 0 || ranges[i].size == 0 ||
			    ranges[i].vaddr > std::numeric_limits<uint64_t>::max() - ranges[i].size)
			{
				return false;
			}
		}
		if (vkQueueWaitIdle(bridge->device.queue) != VK_SUCCESS) { return false; }
		for (size_t i = 0; i < range_count; ++i)
		{
			GuestDeviceAddressReleaseRangeQuiesced(&bridge->device.context, ranges[i].vaddr, ranges[i].size);
		}
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

	bool Record(uint64_t table, uint32_t entries, uint64_t guest)
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
		return vkEndCommandBuffer(command) == VK_SUCCESS;
	}

	bool Submit(VkSemaphore wait_timeline = VK_NULL_HANDLE, uint64_t wait_value = 0)
	{
		VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		VkTimelineSemaphoreSubmitInfo timeline_info {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		if (wait_timeline != VK_NULL_HANDLE)
		{
			timeline_info.waitSemaphoreValueCount = 1;
			timeline_info.pWaitSemaphoreValues = &wait_value;
			submit.pNext = &timeline_info;
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &wait_timeline;
			submit.pWaitDstStageMask = &wait_stage;
		}
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		return vkQueueSubmit(device->queue, 1, &submit, fence) == VK_SUCCESS;
	}

	VkResult Wait(uint64_t timeout = 5000000000ull) const
	{
		return vkWaitForFences(device->context.device, 1, &fence, VK_TRUE, timeout);
	}

	int CheckOutput(const char* stage, uint32_t marker) const
	{
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

	bool Dispatch(uint64_t table, uint32_t entries, uint64_t guest)
	{
		return Record(table, entries, guest) && Submit() && Wait() == VK_SUCCESS;
	}

	int Expect(const char* stage, uint64_t table, uint32_t entries, uint64_t guest, uint32_t marker)
	{
		if (!Dispatch(table, entries, guest))
		{
			std::fprintf(stderr, "%s: dispatch failed\n", stage);
			return 20;
		}
		return CheckOutput(stage, marker);
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

// A command buffer recorded against the production table retains its original
// snapshot even when CPU writes arrive before that command buffer is submitted.
int RecordedGuestDeviceAddressReadRetainsSnapshotProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0) { return status; }
	const uint64_t base = MapSeededOwner();
	if (base == 0) { return 40; }

	auto* const    owner = reinterpret_cast<void*>(base);
	const uint64_t guest = base + kWritableOffset;
	GuestDeviceAddressTable table {};
	SubmissionId dependency {};
	if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x12) != 0) { return 41; }
	const SubmissionId initial_reader {GpuQueueId(0), 1};
	if (GuestDeviceAddressPrepareForRead(&bridge.device.context, initial_reader, &table, &dependency) !=
	    GuestDeviceAddressPrepareResult::Ready)
	{
		return 42;
	}
	if (table.entry_count == 0)
	{
		std::fprintf(stderr, "initial GDA table has no entries\n");
		return 43;
	}
	if (load.Expect("initial GDA load", table.address, table.entry_count, guest, kMarkerA) != 0) { return 44; }
	if (!GuestDeviceAddressAcknowledgeReader(initial_reader)) { return 45; }

	const SubmissionId recorded_reader {GpuQueueId(0), 2};
	if (GuestDeviceAddressPrepareForRead(&bridge.device.context, recorded_reader, &table, &dependency) !=
	    GuestDeviceAddressPrepareResult::Ready)
	{
		return 46;
	}
	const GuestDeviceAddressTable recorded_table = table;
	if (!load.Record(recorded_table.address, recorded_table.entry_count, guest)) { return 47; }

	WriteMarker(guest, kMarkerB);
	const SubmissionId retry_reader {GpuQueueId(0), 3};
	GuestDeviceAddressTable pending_table {0xabcdefu, 77u};
	const auto pending_result = GuestDeviceAddressPrepareForRead(&bridge.device.context, retry_reader, &pending_table, &dependency);
	const bool pending_table_stable = pending_table.address == 0xabcdefu && pending_table.entry_count == 77u;
	const bool submitted = load.Submit();
	const VkResult completed = submitted ? load.Wait() : VK_ERROR_UNKNOWN;
	const bool acknowledged = completed == VK_SUCCESS && GuestDeviceAddressAcknowledgeReader(recorded_reader);
	int status = 0;
	if (pending_result != GuestDeviceAddressPrepareResult::PendingReader || dependency != recorded_reader) { status = 48; }
	if (!pending_table_stable && status == 0) { status = 49; }
	if ((!submitted || completed != VK_SUCCESS) && status == 0) { status = 50; }
	if (!acknowledged && status == 0) { status = 51; }
	if (completed == VK_SUCCESS && load.CheckOutput("recorded snapshot", kMarkerA) != 0 && status == 0) { status = 52; }
	GuestDeviceAddressTable refreshed_table {};
	const SubmissionId refreshed_reader {GpuQueueId(0), 4};
	const auto refreshed = completed == VK_SUCCESS
	                           ? GuestDeviceAddressPrepareForRead(&bridge.device.context, refreshed_reader, &refreshed_table,
	                                                             &dependency)
	                           : GuestDeviceAddressPrepareResult::Failed;
	if (refreshed != GuestDeviceAddressPrepareResult::Ready && status == 0) { status = 53; }
	if (refreshed == GuestDeviceAddressPrepareResult::Ready &&
	    load.Expect("recorded snapshot refresh", refreshed_table.address, refreshed_table.entry_count, guest, kMarkerB) != 0 &&
	    status == 0)
	{
		status = 54;
	}
	if (refreshed == GuestDeviceAddressPrepareResult::Ready && !GuestDeviceAddressAcknowledgeReader(refreshed_reader) && status == 0)
	{
		status = 55;
	}
	if (completed == VK_SUCCESS)
	{
		const int release_status = ExpectOwnerReleased(&bridge, base);
		if (release_status != 0 && status == 0) { status = release_status; }
	}
	if (status != 0)
	{
		std::fprintf(stderr,
		             "pending_result=%u dependency=%" PRIu64 " pending_table_stable=%d submitted=%d completed=%d "
		             "recorded_entries=%u refreshed_entries=%u\n",
		             static_cast<unsigned>(pending_result), dependency.sequence, pending_table_stable ? 1 : 0, submitted ? 1 : 0,
		             completed, recorded_table.entry_count, refreshed_table.entry_count);
	}
	return status;
}

// A queued shader waits for a host-signaled timeline value. Dirty refresh must
// report a dependency promptly, leave the reader's bytes stable, and let a
// retry observe the CPU write after the reader fence completes.
int PendingGuestDeviceAddressReaderDefersRefreshProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0) { return status; }
	if (!bridge.device.timeline_semaphore_supported) { return 77; }
	const uint64_t base = MapSeededOwner();
	if (base == 0) { return 40; }

	auto* const    owner = reinterpret_cast<void*>(base);
	const uint64_t guest = base + kWritableOffset;
	GuestDeviceAddressTable table {};
	SubmissionId dependency {};
	if (Kernel::Memory::KernelMprotect(owner, kOwnerBytes, 0x12) != 0) { return 41; }
	const SubmissionId initial_reader {GpuQueueId(0), 10};
	if (GuestDeviceAddressPrepareForRead(&bridge.device.context, initial_reader, &table, &dependency) !=
	    GuestDeviceAddressPrepareResult::Ready)
	{
		return 42;
	}
	if (table.entry_count == 0)
	{
		std::fprintf(stderr, "initial GDA table has no entries\n");
		return 43;
	}
	if (load.Expect("initial GDA load", table.address, table.entry_count, guest, kMarkerA) != 0) { return 44; }
	if (!GuestDeviceAddressAcknowledgeReader(initial_reader)) { return 45; }
	const SubmissionId pending_reader {GpuQueueId(0), 20};
	if (GuestDeviceAddressPrepareForRead(&bridge.device.context, pending_reader, &table, &dependency) !=
	    GuestDeviceAddressPrepareResult::Ready)
	{
		return 46;
	}
	VkSemaphoreTypeCreateInfo type_info {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	type_info.initialValue = 0;
	VkSemaphoreCreateInfo semaphore_info {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	semaphore_info.pNext = &type_info;
	VkSemaphore gate = VK_NULL_HANDLE;
	if (vkCreateSemaphore(bridge.device.context.device, &semaphore_info, nullptr, &gate) != VK_SUCCESS) { return 47; }

	const bool recorded = load.Record(table.address, table.entry_count, guest);
	const bool submitted = recorded && load.Submit(gate, 1);
	const VkResult pending = submitted ? load.Wait(10000000ull) : VK_ERROR_UNKNOWN;
	WriteMarker(guest, kMarkerB);

	struct PrepareResult
	{
		std::mutex                         mutex;
		std::condition_variable            changed;
		bool                               returned = false;
		GuestDeviceAddressPrepareResult    result = GuestDeviceAddressPrepareResult::Failed;
		GuestDeviceAddressTable            table {0xabcdefu, 77u};
		SubmissionId                       dependency {};
	};
	auto result = std::make_shared<PrepareResult>();
	std::thread prepare_thread;
	if (submitted && pending == VK_TIMEOUT)
	{
		const SubmissionId retry_reader {GpuQueueId(0), 21};
		prepare_thread = std::thread([result, ctx = &bridge.device.context, retry_reader]
		{
			GuestDeviceAddressTable refreshed_table = result->table;
			SubmissionId dependency {};
			const auto prepare = GuestDeviceAddressPrepareForRead(ctx, retry_reader, &refreshed_table, &dependency);
			{
				std::lock_guard<std::mutex> lock(result->mutex);
				result->result = prepare;
				result->table = refreshed_table;
				result->dependency = dependency;
				result->returned = true;
			}
			result->changed.notify_all();
		});
	}
	bool returned_before_release = false;
	{
		std::unique_lock<std::mutex> lock(result->mutex);
		returned_before_release = result->changed.wait_for(lock, std::chrono::milliseconds(250), [result] { return result->returned; });
	}

	// Always release the queue wait before joining, including a failed setup
	// path, so the isolated child cannot strand a queue submission.
	VkSemaphoreSignalInfo signal_info {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
	signal_info.semaphore = gate;
	signal_info.value = 1;
	const VkResult signal_result = vkSignalSemaphore(bridge.device.context.device, &signal_info);
	const VkResult completed = submitted ? load.Wait() : VK_ERROR_UNKNOWN;
	const bool acknowledged = completed == VK_SUCCESS && GuestDeviceAddressAcknowledgeReader(pending_reader);
	bool returned_after_release = false;
	if (prepare_thread.joinable())
	{
		{
			std::unique_lock<std::mutex> lock(result->mutex);
			returned_after_release = result->changed.wait_for(lock, std::chrono::seconds(5), [result] { return result->returned; });
		}
		if (returned_after_release) { prepare_thread.join(); }
		else { prepare_thread.detach(); }
	}
	// Keep the semaphore alive if the bounded completion wait failed: a submitted queue may still reference it.
	// RunIsolated exits this child after the probe, so such a failed fixture cannot leak into another test.
	if (!submitted || completed == VK_SUCCESS) { vkDestroySemaphore(bridge.device.context.device, gate, nullptr); }

	GuestDeviceAddressPrepareResult pending_prepare = GuestDeviceAddressPrepareResult::Failed;
	GuestDeviceAddressTable pending_table {};
	SubmissionId pending_dependency {};
	if (returned_before_release || returned_after_release)
	{
		std::lock_guard<std::mutex> lock(result->mutex);
		pending_prepare = result->result;
		pending_table = result->table;
		pending_dependency = result->dependency;
	}
	const bool pending_table_stable = pending_table.address == 0xabcdefu && pending_table.entry_count == 77u;

	int status = 0;
	if (!recorded || !submitted) { status = 46; }
	if (pending != VK_TIMEOUT && status == 0) { status = 47; }
	if (!returned_before_release && status == 0) { status = 48; }
	if (pending_prepare != GuestDeviceAddressPrepareResult::PendingReader && status == 0) { status = 49; }
	if (pending_dependency != pending_reader && status == 0) { status = 50; }
	if (!pending_table_stable && status == 0) { status = 51; }
	if (signal_result != VK_SUCCESS && status == 0) { status = 52; }
	if (completed != VK_SUCCESS && status == 0) { status = 53; }
	if (!acknowledged && status == 0) { status = 54; }
	if (completed == VK_SUCCESS && load.CheckOutput("pending reader", kMarkerA) != 0 && status == 0) { status = 55; }

	GuestDeviceAddressTable retry_table {};
	SubmissionId retry_reader {GpuQueueId(0), 22};
	SubmissionId retry_dependency {};
	const auto retry_prepare = completed == VK_SUCCESS && returned_after_release
	                               ? GuestDeviceAddressPrepareForRead(&bridge.device.context, retry_reader, &retry_table,
	                                                                 &retry_dependency)
	                               : GuestDeviceAddressPrepareResult::Failed;
	int retry_output = -1;
	if (retry_prepare != GuestDeviceAddressPrepareResult::Ready && status == 0) { status = 56; }
	if (retry_prepare == GuestDeviceAddressPrepareResult::Ready)
	{
		retry_output = load.Expect("post-fence refresh", retry_table.address, retry_table.entry_count, guest, kMarkerB);
		if (retry_output != 0 && status == 0) { status = 57; }
		if (!GuestDeviceAddressAcknowledgeReader(retry_reader) && status == 0) { status = 58; }
	}

	// This native fixture owns one Vulkan queue. Distinct logical queue IDs
	// exercise the registry's independent slots and exact-ACK behavior only.
	const SubmissionId stale_reader {GpuQueueId(0), 100};
	const SubmissionId latest_reader {GpuQueueId(0), 101};
	const SubmissionId other_queue_reader {GpuQueueId(1), 1};
	const SubmissionId exact_retry_reader {GpuQueueId(0), 102};
	GuestDeviceAddressTable exact_table {};
	SubmissionId exact_dependency {};
	auto exact_prepare = GuestDeviceAddressPrepareForRead(&bridge.device.context, stale_reader, &exact_table, &exact_dependency);
	if (exact_prepare != GuestDeviceAddressPrepareResult::Ready && status == 0) { status = 59; }
	if (exact_prepare == GuestDeviceAddressPrepareResult::Ready &&
	    load.Expect("stale reader", exact_table.address, exact_table.entry_count, guest, kMarkerB) != 0 && status == 0)
	{
		status = 60;
	}
	exact_prepare = GuestDeviceAddressPrepareForRead(&bridge.device.context, latest_reader, &exact_table, &exact_dependency);
	if (exact_prepare != GuestDeviceAddressPrepareResult::Ready && status == 0) { status = 61; }
	if (exact_prepare == GuestDeviceAddressPrepareResult::Ready &&
	    load.Expect("latest same-queue reader", exact_table.address, exact_table.entry_count, guest, kMarkerB) != 0 && status == 0)
	{
		status = 62;
	}
	// An out-of-order registration for an older sequence must not replace the newer reader.
	exact_prepare = GuestDeviceAddressPrepareForRead(&bridge.device.context, stale_reader, &exact_table, &exact_dependency);
	if (exact_prepare != GuestDeviceAddressPrepareResult::Ready && status == 0) { status = 73; }
	exact_prepare =
	    GuestDeviceAddressPrepareForRead(&bridge.device.context, other_queue_reader, &exact_table, &exact_dependency);
	if (exact_prepare != GuestDeviceAddressPrepareResult::Ready && status == 0) { status = 63; }
	if (exact_prepare == GuestDeviceAddressPrepareResult::Ready &&
	    load.Expect("other-queue reader", exact_table.address, exact_table.entry_count, guest, kMarkerB) != 0 && status == 0)
	{
		status = 64;
	}
	if (GuestDeviceAddressAcknowledgeReader(stale_reader) && status == 0) { status = 65; }

	WriteMarker(guest, kMarkerC);
	GuestDeviceAddressTable exact_pending_table {0x123456u, 99u};
	const auto pending_latest = GuestDeviceAddressPrepareForRead(&bridge.device.context, exact_retry_reader, &exact_pending_table,
	                                                            &exact_dependency);
	if ((pending_latest != GuestDeviceAddressPrepareResult::PendingReader || exact_dependency != latest_reader ||
	     exact_pending_table.address != 0x123456u || exact_pending_table.entry_count != 99u) &&
	    status == 0)
	{
		status = 66;
	}
	if (!GuestDeviceAddressAcknowledgeReader(latest_reader) && status == 0) { status = 67; }
	exact_pending_table = {0x123456u, 99u};
	const auto pending_other = GuestDeviceAddressPrepareForRead(&bridge.device.context, exact_retry_reader, &exact_pending_table,
	                                                           &exact_dependency);
	if ((pending_other != GuestDeviceAddressPrepareResult::PendingReader || exact_dependency != other_queue_reader ||
	     exact_pending_table.address != 0x123456u || exact_pending_table.entry_count != 99u) &&
	    status == 0)
	{
		status = 68;
	}
	if (!GuestDeviceAddressAcknowledgeReader(other_queue_reader) && status == 0) { status = 69; }
	exact_prepare = GuestDeviceAddressPrepareForRead(&bridge.device.context, exact_retry_reader, &exact_table, &exact_dependency);
	if (exact_prepare != GuestDeviceAddressPrepareResult::Ready && status == 0) { status = 70; }
	if (exact_prepare == GuestDeviceAddressPrepareResult::Ready &&
	    load.Expect("independent queue refresh", exact_table.address, exact_table.entry_count, guest, kMarkerC) != 0 && status == 0)
	{
		status = 71;
	}
	if (exact_prepare == GuestDeviceAddressPrepareResult::Ready &&
	    !GuestDeviceAddressAcknowledgeReader(exact_retry_reader) && status == 0)
	{
		status = 72;
	}

	if (completed == VK_SUCCESS && returned_after_release)
	{
		const int release_status = ExpectOwnerReleased(&bridge, base);
		if (release_status != 0 && status == 0) { status = release_status; }
	}
	if (status != 0)
	{
		std::fprintf(stderr,
		             "recorded=%d submitted=%d pending=%d returned_before_release=%d returned_after_release=%d "
		             "prepare=%u dependency=%u:%" PRIu64 " table_stable=%d signal=%d completed=%d retry=%u "
		             "exact=%u\n",
		             recorded ? 1 : 0, submitted ? 1 : 0, pending, returned_before_release ? 1 : 0,
		             returned_after_release ? 1 : 0, static_cast<unsigned>(pending_prepare), pending_dependency.queue.Value(),
		             pending_dependency.sequence, pending_table_stable ? 1 : 0, signal_result, completed,
		             static_cast<unsigned>(retry_prepare), static_cast<unsigned>(exact_prepare));
	}
	return status;
}

// Keep a real GPU read of the first GDA table pending while repeated range
// additions retire table versions. The registry must stop before exceeding
// its retired-table budget, preserve the queue's newest reader dependency, and
// resume only after that reader's fence completes.
int RetiredGuestDeviceAddressTablesStayReadableProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	GuestLoad              latest_load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0)
	{
		return status;
	}
	if (!bridge.device.timeline_semaphore_supported)
	{
		return 77;
	}
	if (const int status = latest_load.Init(&bridge.device); status != 0)
	{
		return status;
	}

	constexpr uint64_t page_bytes = kGuestDeviceAddressPageBytes;
	constexpr uint64_t page_count = kRetiredTablePressureBudget + 2u;
	constexpr uint64_t owner_size = page_count * page_bytes;
	const uint64_t     owner      = Core::VirtualMemory::Alloc(0, owner_size, Core::VirtualMemory::Mode::ReadWrite);
	if (owner == 0)
	{
		return 40;
	}
	for (uint64_t page = 0; page < page_count; ++page)
	{
		WriteMarker(owner + page * page_bytes, kMarkerA);
	}
	if (!Core::VirtualMemory::Protect(owner, owner_size, Core::VirtualMemory::Mode::Read))
	{
		return 41;
	}

	const SubmissionId      first_reader {GpuQueueId(0), 200};
	const SubmissionId      latest_reader {GpuQueueId(0), 201};
	SubmissionId            dependency {};
	GuestDeviceAddressTable table {};
	GuestDeviceAddressRegisterRange(owner, page_bytes, false);
	if (GuestDeviceAddressPrepareForRead(&bridge.device.context, first_reader, &table, &dependency) !=
	        GuestDeviceAddressPrepareResult::Ready ||
	    table.entry_count != 1)
	{
		return 42;
	}
	GuestDeviceAddressTable latest_table {};
	if (GuestDeviceAddressPrepareForRead(&bridge.device.context, latest_reader, &latest_table, &dependency) !=
	        GuestDeviceAddressPrepareResult::Ready ||
	    latest_table.address != table.address || latest_table.entry_count != table.entry_count)
	{
		return 43;
	}
	if (!load.Record(table.address, table.entry_count, owner) || !latest_load.Record(table.address, table.entry_count, owner))
	{
		return 44;
	}

	VkSemaphoreTypeCreateInfo type_info {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	type_info.initialValue  = 0;
	VkSemaphoreCreateInfo semaphore_info {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	semaphore_info.pNext = &type_info;
	VkSemaphore gate     = VK_NULL_HANDLE;
	if (vkCreateSemaphore(bridge.device.context.device, &semaphore_info, nullptr, &gate) != VK_SUCCESS)
	{
		return 45;
	}

	const bool     submitted_first  = load.Submit(gate, 1);
	const bool     submitted_latest = submitted_first && latest_load.Submit(gate, 1);
	const VkResult first_pending    = submitted_first ? load.Wait(10000000ull) : VK_ERROR_UNKNOWN;
	const VkResult latest_pending   = submitted_latest ? latest_load.Wait(10000000ull) : VK_ERROR_UNKNOWN;
	int            status           = 0;
	if (!submitted_first || !submitted_latest)
	{
		status = 46;
	}
	if (first_pending != VK_TIMEOUT || latest_pending != VK_TIMEOUT)
	{
		if (status == 0)
		{
			status = 47;
		}
	}

	GuestDeviceAddressTable last_table         = latest_table;
	uint32_t                completed_rebuilds = 0;
	for (uint32_t page = 1; page <= kRetiredTablePressureBudget; ++page)
	{
		GuestDeviceAddressRegisterRange(owner + page * page_bytes, page_bytes, false);
		GuestDeviceAddressTable next_table {};
		const auto              result = GuestDeviceAddressPrepareForRead(&bridge.device.context, first_reader, &next_table, &dependency);
		if (result != GuestDeviceAddressPrepareResult::Ready || next_table.entry_count != page + 1u)
		{
			if (status == 0)
			{
				status = 48;
			}
			break;
		}
		last_table = next_table;
		completed_rebuilds++;
	}

	GuestDeviceAddressRegisterRange(owner + (kRetiredTablePressureBudget + 1u) * page_bytes, page_bytes, false);
	GuestDeviceAddressTable pending_table {0xabcdefu, 77u};
	const auto         pending_result = GuestDeviceAddressPrepareForRead(&bridge.device.context, first_reader, &pending_table, &dependency);
	const SubmissionId pressure_dependency      = dependency;
	const bool         pending_output_unchanged = pending_table.address == 0xabcdefu && pending_table.entry_count == 77u;

	// Always release and drain every submitted command before any return path.
	VkSemaphoreSignalInfo signal_info {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
	signal_info.semaphore           = gate;
	signal_info.value               = 1;
	const VkResult signal_result    = vkSignalSemaphore(bridge.device.context.device, &signal_info);
	const VkResult first_completed  = submitted_first ? load.Wait() : VK_ERROR_UNKNOWN;
	const VkResult latest_completed = submitted_latest ? latest_load.Wait() : VK_ERROR_UNKNOWN;
	if ((!submitted_first || first_completed == VK_SUCCESS) && (!submitted_latest || latest_completed == VK_SUCCESS))
	{
		vkDestroySemaphore(bridge.device.context.device, gate, nullptr);
	}

	if (completed_rebuilds != kRetiredTablePressureBudget && status == 0)
	{
		status = 49;
	}
	if (pending_result != GuestDeviceAddressPrepareResult::PendingReader && status == 0)
	{
		status = 50;
	}
	if (pressure_dependency != latest_reader && status == 0)
	{
		status = 51;
	}
	if (!pending_output_unchanged && status == 0)
	{
		status = 52;
	}
	if (signal_result != VK_SUCCESS && status == 0)
	{
		status = 53;
	}
	if (first_completed != VK_SUCCESS || latest_completed != VK_SUCCESS)
	{
		if (status == 0)
		{
			status = 54;
		}
	}
	if (first_completed == VK_SUCCESS && load.CheckOutput("retired table first reader", kMarkerA) != 0 && status == 0)
	{
		status = 55;
	}
	if (latest_completed == VK_SUCCESS && latest_load.CheckOutput("retired table latest reader", kMarkerA) != 0 && status == 0)
	{
		status = 56;
	}
	if (GuestDeviceAddressAcknowledgeReader(first_reader) && status == 0)
	{
		status = 57;
	}
	if (latest_completed == VK_SUCCESS && !GuestDeviceAddressAcknowledgeReader(latest_reader) && status == 0)
	{
		status = 58;
	}

	GuestDeviceAddressTable retry_table {};
	const SubmissionId      retry_reader {GpuQueueId(0), 202};
	const auto retry_result = latest_completed == VK_SUCCESS
	                              ? GuestDeviceAddressPrepareForRead(&bridge.device.context, retry_reader, &retry_table, &dependency)
	                              : GuestDeviceAddressPrepareResult::Failed;
	if ((retry_result != GuestDeviceAddressPrepareResult::Ready || retry_table.entry_count != page_count) && status == 0)
	{
		status = 59;
	}
	if (retry_result == GuestDeviceAddressPrepareResult::Ready)
	{
		if (load.Expect("retired table post-fence retry", retry_table.address, retry_table.entry_count, owner, kMarkerA) != 0 &&
		    status == 0)
		{
			status = 60;
		}
		if (!GuestDeviceAddressAcknowledgeReader(retry_reader) && status == 0)
		{
			status = 61;
		}
	}
	if (status != 0)
	{
		std::fprintf(stderr,
		             "rebuilds=%u pending=%u dependency=%u:%" PRIu64 " output_stable=%d signal=%d first=%d latest=%d "
		             "last_entries=%u pending_entries=%u retry=%u retry_entries=%u\n",
		             completed_rebuilds, static_cast<unsigned>(pending_result), pressure_dependency.queue.Value(),
		             pressure_dependency.sequence, pending_output_unchanged ? 1 : 0, signal_result, first_completed, latest_completed,
		             last_table.entry_count, pending_table.entry_count, static_cast<unsigned>(retry_result), retry_table.entry_count);
	}
	return status;
}

// A physical owner becomes visible in the GDA registry before its kernel map
// registration completes. Keep that callback open while another thread writes
// the backing, maps a direct alias, and prepares a shader load from the range.
int PhysicalRegistrationAliasProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0) { return status; }

	int64_t physical = 0;
	if (Kernel::Memory::KernelAllocateMainDirectMemory(kOwnerBytes, kOwnerBytes, 12, &physical) != 0) { return 40; }
	void* mapping = nullptr;
	int   map_result = -1;
	bridge.HoldNextRegistration();
	std::thread mapper(
	    [&] { map_result = Kernel::Memory::KernelMapDirectMemory(&mapping, kOwnerBytes, 0x12, 0, physical, kOwnerBytes); });

	const bool registration_held = bridge.WaitForRegistration(std::chrono::seconds(2));
	uint64_t   alias              = 0;
	uint64_t   table              = 0;
	uint32_t   entries            = 0;
	bool       alias_created      = false;
	bool       alias_shares_bytes = false;
	bool       prepared           = false;
	int        load_status        = -1;
	int        status             = 0;
	if (!registration_held)
	{
		status = 41;
	} else if (mapping == nullptr)
	{
		status = 42;
	} else
	{
		const auto guest = reinterpret_cast<uint64_t>(mapping);
		WriteMarker(guest, kPhysicalRegistrationMarker);
		alias = Kernel::Memory::KernelMapPhysicalAlias(guest, kOwnerBytes);
		if (alias != 0)
		{
			alias_created = true;
			const auto* guest_words = reinterpret_cast<const uint32_t*>(guest);
			const auto* alias_words = reinterpret_cast<const uint32_t*>(alias);
			alias_shares_bytes = guest_words[0] == (kPhysicalRegistrationMarker | 0u) &&
			                     alias_words[0] == (kPhysicalRegistrationMarker | 0u) &&
			                     guest_words[31] == (kPhysicalRegistrationMarker | 31u) &&
			                     alias_words[31] == (kPhysicalRegistrationMarker | 31u);
			(void)Kernel::Memory::KernelUnmapPhysicalAlias(alias);
			alias = 0;
		}
		prepared = GuestDeviceAddressPrepare(&bridge.device.context, &table, &entries);
		if (prepared)
		{
			load_status = load.Expect("physical registration in flight", table, entries, guest, kPhysicalRegistrationMarker);
		}
		if (!alias_created) { status = 43; }
		if (!alias_shares_bytes && status == 0) { status = 44; }
		if (!prepared && status == 0) { status = 45; }
		if (load_status != 0 && status == 0) { status = 46; }
	}

	// Always release the callback before joining, including every failed probe
	// path, so this isolated child cannot retain a blocked mapper thread.
	bridge.PermitRegistration();
	mapper.join();
	const bool mapped = map_result == 0 && mapping != nullptr;
	const int  unmap_result = mapped ? Kernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(mapping), kOwnerBytes) : 0;
	const int  release_result = Kernel::Memory::KernelCheckedReleaseDirectMemory(physical, kOwnerBytes);
	if (!mapped && status == 0) { status = 47; }
	if (unmap_result != 0 && status == 0) { status = 48; }
	if (release_result != 0 && status == 0) { status = 49; }
	if (status != 0)
	{
		std::fprintf(stderr,
		             "registration_held=%d map=%d alias=%d shared=%d prepared=%d entries=%u load=%d unmap=%d release=%d\n",
		             registration_held ? 1 : 0, map_result, alias_created ? 1 : 0, alias_shares_bytes ? 1 : 0,
		             prepared ? 1 : 0, entries, load_status, unmap_result, release_result);
	}
	return status;
}

// Releasing the physical allocation retires its GPU address range while the
// CPU virtual mapping remains queryable until the guest explicitly unmaps it.
int PhysicalReleaseRetiresDeviceAddressRangeProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0) { return status; }

	int64_t physical = 0;
	if (Kernel::Memory::KernelAllocateMainDirectMemory(kOwnerBytes, kOwnerBytes, 12, &physical) != 0) { return 40; }
	void* mapping = nullptr;
	const int map_result = Kernel::Memory::KernelMapDirectMemory(&mapping, kOwnerBytes, 0x12, 0, physical, kOwnerBytes);
	if (map_result != 0 || mapping == nullptr)
	{
		if (mapping != nullptr) { (void)Kernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(mapping), kOwnerBytes); }
		(void)Kernel::Memory::KernelCheckedReleaseDirectMemory(physical, kOwnerBytes);
		return 41;
	}

	const auto guest = reinterpret_cast<uint64_t>(mapping);
	WriteMarker(guest, kPhysicalReleaseMarker);
	uint64_t table = 0;
	uint32_t entries = 0;
	const bool prepared_before = GuestDeviceAddressPrepare(&bridge.device.context, &table, &entries);
	const int load_status = prepared_before
	                            ? load.Expect("physical owner before release", table, entries, guest, kPhysicalReleaseMarker)
	                            : -1;
	const int release_result = Kernel::Memory::KernelCheckedReleaseDirectMemory(physical, kOwnerBytes);
	int       protection = -1;
	const bool mapping_stays_live = Kernel::Memory::KernelQueryMemoryProtection(mapping, nullptr, nullptr, &protection) == 0 &&
	                               protection == 0x12 && Core::VirtualMemory::IsRangeWritable(guest, kOwnerBytes);
	table = 0;
	entries = 0;
	const bool prepared_after = release_result == 0 && GuestDeviceAddressPrepare(&bridge.device.context, &table, &entries);
	const uint32_t entries_after_release = entries;
	int status = 0;
	if (!prepared_before || load_status != 0) { status = 42; }
	if (release_result != 0 && status == 0) { status = 43; }
	if (!mapping_stays_live && status == 0) { status = 44; }
	if (!prepared_after && status == 0) { status = 45; }
	if (prepared_after && entries_after_release != 0 && status == 0) { status = 46; }

	const int unmap_result = Kernel::Memory::KernelMunmap(guest, kOwnerBytes);
	const int retry_release_result = release_result == 0 ? 0 : Kernel::Memory::KernelCheckedReleaseDirectMemory(physical, kOwnerBytes);
	uint64_t empty_table = 0;
	uint32_t empty_entries = 0;
	const bool empty_after_unmap = unmap_result == 0 && GuestDeviceAddressPrepare(&bridge.device.context, &empty_table, &empty_entries) &&
	                               empty_entries == 0;
	if (unmap_result != 0 && status == 0) { status = 47; }
	if (retry_release_result != 0 && status == 0) { status = 48; }
	if (!empty_after_unmap && status == 0) { status = 49; }
	if (status != 0)
	{
		std::fprintf(stderr,
		             "map=%d prepared_before=%d load=%d release=%d mapping_live=%d prepared_after=%d entries_after_release=%u "
		             "unmap=%d retry_release=%d empty_after_unmap=%d\n",
		             map_result, prepared_before ? 1 : 0, load_status, release_result, mapping_stays_live ? 1 : 0,
		             prepared_after ? 1 : 0, entries_after_release, unmap_result, retry_release_result, empty_after_unmap ? 1 : 0);
	}
	return status;
}

// Physical pages written one at a time are imported as their whole 2 MiB block:
// two separate writes give one entry, repeated preparation imports nothing new
// and leaves the untouched block unpopulated, and a write there adds its block.
int PhysicalImportCoversResidentBlockProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0) { return status; }

	constexpr uint64_t kBlockBytes = 0x200000;
	constexpr uint64_t kBytes      = 2u * kBlockBytes;
	constexpr uint64_t kBlockPages = kBlockBytes / kGuestDeviceAddressPageBytes;
	int64_t            physical    = 0;
	if (Kernel::Memory::KernelAllocateMainDirectMemory(kBytes, kBlockBytes, 12, &physical) != 0) { return 40; }
	void* mapping = nullptr;
	if (Kernel::Memory::KernelMapDirectMemory(&mapping, kBytes, 0x12, 0, physical, kBlockBytes) != 0 || mapping == nullptr)
	{
		(void)Kernel::Memory::KernelCheckedReleaseDirectMemory(physical, kBytes);
		return 41;
	}
	const auto guest = reinterpret_cast<uint64_t>(mapping);
	const auto second_block_resident = [&]
	{
		std::vector<uint8_t> resident(kBlockPages);
		return Core::VirtualMemory::QueryResidentPages(guest + kBlockBytes, kBlockBytes, resident.data())
		           ? std::count_if(resident.begin(), resident.end(), [](uint8_t value) { return value != 0; })
		           : -1;
	};
	WriteMarker(guest, kMarkerA);
	WriteMarker(guest + kBlockBytes / 2u, kMarkerB);
	uint64_t table   = 0;
	uint32_t entries = 0;
	int      status  = 0;
	if (!GuestDeviceAddressPrepare(&bridge.device.context, &table, &entries) || entries != 1) { status = 42; }
	if (status == 0) { status = load.Expect("block head", table, entries, guest, kMarkerA); }
	if (status == 0) { status = load.Expect("block middle", table, entries, guest + kBlockBytes / 2u, kMarkerB); }
	for (unsigned round = 0; round < 4 && status == 0; ++round)
	{
		if (!GuestDeviceAddressPrepare(&bridge.device.context, &table, &entries) || entries != 1) { status = 43; }
		if (status == 0 && second_block_resident() != 0) { status = 44; }
	}
	WriteMarker(guest + kBlockBytes + kGuestPageBytes, kMarkerC);
	if (status == 0 && (!GuestDeviceAddressPrepare(&bridge.device.context, &table, &entries) || entries != 2)) { status = 45; }
	if (status == 0) { status = load.Expect("second block", table, entries, guest + kBlockBytes + kGuestPageBytes, kMarkerC); }

	const int unmap_result   = Kernel::Memory::KernelMunmap(guest, kBytes);
	const int release_result = Kernel::Memory::KernelCheckedReleaseDirectMemory(physical, kBytes);
	if (unmap_result != 0 && status == 0) { status = 46; }
	if (release_result != 0 && status == 0) { status = 47; }
	if (status != 0)
	{
		std::fprintf(stderr, "status=%d entries=%u second_block_resident=%ld unmap=%d release=%d\n", status, entries,
		             static_cast<long>(second_block_resident()), unmap_result, release_result);
	}
	return status;
}

// A partial direct-memory release retires only the released tail and leaves
// the still-allocated head usable by the GPU and by the guest CPU mapping.
int PartialPhysicalReleasePreservesHeadProbe()
{
	static LifecycleBridge bridge;
	GuestLoad              load;
	if (const int status = StartGpuMappingRuntime(&bridge, &load); status != 0) { return status; }

	int64_t physical = 0;
	if (Kernel::Memory::KernelAllocateMainDirectMemory(kOwnerBytes, kOwnerBytes, 12, &physical) != 0) { return 40; }
	void* mapping = nullptr;
	const int map_result = Kernel::Memory::KernelMapDirectMemory(&mapping, kOwnerBytes, 0x12, 0, physical, kOwnerBytes);
	if (map_result != 0 || mapping == nullptr)
	{
		if (mapping != nullptr) { (void)Kernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(mapping), kOwnerBytes); }
		(void)Kernel::Memory::KernelCheckedReleaseDirectMemory(physical, kOwnerBytes);
		return 41;
	}

	const auto guest = reinterpret_cast<uint64_t>(mapping);
	const auto tail = guest + 3 * kGuestPageBytes;
	WriteMarker(guest, kMarkerA);
	WriteMarker(tail, kMarkerB);
	uint64_t table = 0;
	uint32_t entries = 0;
	bool prepared_before = GuestDeviceAddressPrepare(&bridge.device.context, &table, &entries);
	int load_head_before = prepared_before ? load.Expect("partial physical owner head before release", table, entries, guest, kMarkerA) : -1;
	int load_tail_before = prepared_before ? load.Expect("partial physical owner tail before release", table, entries, tail, kMarkerB) : -1;
	const int release_result = Kernel::Memory::KernelCheckedReleaseDirectMemory(physical + 3 * kGuestPageBytes, kGuestPageBytes);
	int protection = -1;
	const bool head_stays_live = Kernel::Memory::KernelQueryMemoryProtection(mapping, nullptr, nullptr, &protection) == 0 &&
	                             protection == 0x12 && Core::VirtualMemory::IsRangeWritable(guest, kGuestPageBytes);
	table = 0;
	entries = 0;
	const bool prepared_after = release_result == 0 && GuestDeviceAddressPrepare(&bridge.device.context, &table, &entries);
	const int load_head_after = prepared_after ? load.Expect("partial physical release preserves head", table, entries, guest, kMarkerA) : -1;
	int status = 0;
	if (!prepared_before || load_head_before != 0 || load_tail_before != 0) { status = 42; }
	if (release_result != 0 && status == 0) { status = 43; }
	if (!head_stays_live && status == 0) { status = 44; }
	if (!prepared_after && status == 0) { status = 45; }
	if (load_head_after != 0 && status == 0) { status = 46; }

	const int unmap_result = Kernel::Memory::KernelMunmap(guest, kOwnerBytes);
	const int release_remaining_result = release_result == 0
	                                         ? Kernel::Memory::KernelCheckedReleaseDirectMemory(physical, 3 * kGuestPageBytes)
	                                         : Kernel::Memory::KernelCheckedReleaseDirectMemory(physical, kOwnerBytes);
	if (unmap_result != 0 && status == 0) { status = 47; }
	if (release_remaining_result != 0 && status == 0) { status = 48; }
	if (status != 0)
	{
		std::fprintf(stderr,
		             "map=%d prepared_before=%d head_before=%d tail_before=%d release=%d head_live=%d prepared_after=%d "
		             "entries_after=%u head_after=%d unmap=%d release_remaining=%d\n",
		             map_result, prepared_before ? 1 : 0, load_head_before, load_tail_before, release_result,
		             head_stays_live ? 1 : 0, prepared_after ? 1 : 0, entries, load_head_after, unmap_result,
		             release_remaining_result);
	}
	return status;
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
TEST(EmulatorGuestDeviceAddress, RecordedGuestDeviceAddressReadRetainsSnapshot) { RunIsolated(RecordedGuestDeviceAddressReadRetainsSnapshotProbe); }
TEST(EmulatorGuestDeviceAddress, PendingGuestDeviceAddressReaderDefersRefresh) { RunIsolated(PendingGuestDeviceAddressReaderDefersRefreshProbe); }
TEST(EmulatorGuestDeviceAddress, RetiredGuestDeviceAddressTablesStayReadableAndBoundRebuilds)
{
	RunIsolated(RetiredGuestDeviceAddressTablesStayReadableProbe);
}
TEST(EmulatorGuestDeviceAddress, PhysicalAliasIsAvailableDuringOwnerRegistration) { RunIsolated(PhysicalRegistrationAliasProbe); }
TEST(EmulatorGuestDeviceAddress, PhysicalReleaseRetiresDeviceAddressRange) { RunIsolated(PhysicalReleaseRetiresDeviceAddressRangeProbe); }
TEST(EmulatorGuestDeviceAddress, PartialPhysicalReleasePreservesHead) { RunIsolated(PartialPhysicalReleasePreservesHeadProbe); }
TEST(EmulatorGuestDeviceAddress, PhysicalImportCoversResidentBlock) { RunIsolated(PhysicalImportCoversResidentBlockProbe); }
TEST(EmulatorGuestDeviceAddress, AProtectionRefusedForAPendingUnmapKeepsRightsAndImports) { RunIsolated(PendingUnmapProtectionProbe); }

UT_END();
