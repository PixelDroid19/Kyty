#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Math/MathAll.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GdsRange.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GpuSubmissionCoordinator.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/StorageBuffer.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Log.h"

#include "../../emulator/src/Graphics/GraphicsRenderInternal.h"
#include "../../emulator/src/Graphics/GraphicsRunInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <spawn.h>
#include <sys/wait.h>
extern char** environ; // NOLINT
#endif

// GDS transfers through the production command processor: DMA_DATA packets go to the real parser,
// commands land in the processor's recording, and completion runs the exact submission
// publication (labels, then deferred releases). Strict refusals run in child processes.

namespace Kyty::Libs::Graphics {

namespace {

constexpr int      kSkipExit   = 77;
constexpr int      kStrictExit = 65; // EXIT status, as asserted by the shader refusal tests
constexpr uint64_t kWindow     = kGraphicsGdsDwords;
constexpr uint32_t kSelMemory  = 0;
constexpr uint32_t kSelGds     = 1;
constexpr uint32_t kSelData    = 2;

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "GDS transfer integration failed: %s\n", message);
	std::fflush(stderr);
	std::_Exit(1);
}

void Expect(bool condition, const char* message)
{
	if (!condition)
	{
		Die(message);
	}
}

void InitializeSubsystems()
{
	char                        program[]  = "kyty_gds_transfer_integration";
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
	Expect(subsystems->InitAll(false), "subsystems must initialize");
	Kyty::Config::SetNextGen(true);
}

std::atomic<uint32_t> g_validation_errors {0};

VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/)
{
	if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
	{
		g_validation_errors.fetch_add(1);
		std::fprintf(stderr, "validation error: %s\n", data != nullptr && data->pMessage != nullptr ? data->pMessage : "?");
	}
	return VK_FALSE;
}

// A Vulkan device on the graphics family, bound through the render-context test seam. Validation
// layers are enabled when installed (KYTY_GDS_VALIDATION=0 opts out); any validation error fails
// the run. The subgroup size-control state mirrors the window device so the production compute
// path admits wave dispatches where the device supports them.
class GdsVulkanContext
{
public:
	~GdsVulkanContext()
	{
		if (bound)
		{
			// Releases GDS, command pools and samplers while the device is still alive.
			Expect(GraphicsRenderUnbindContextForTesting(&context), "GDS test context must unbind");
		}
		if (context.device != VK_NULL_HANDLE)
		{
			vkDestroyDevice(context.device, nullptr);
		}
		if (messenger != VK_NULL_HANDLE)
		{
			auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
			    vkGetInstanceProcAddr(context.instance, "vkDestroyDebugUtilsMessengerEXT"));
			if (destroy != nullptr)
			{
				destroy(context.instance, messenger, nullptr);
			}
		}
		if (context.instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(context.instance, nullptr);
		}
		if (g_validation_errors.load() != 0)
		{
			Die("Vulkan validation reported errors");
		}
	}

	[[nodiscard]] bool Initialize()
	{
		if (!CreateInstance())
		{
			return false;
		}
		uint32_t physical_count = 0;
		if (vkEnumeratePhysicalDevices(context.instance, &physical_count, nullptr) != VK_SUCCESS || physical_count == 0)
		{
			return false;
		}
		std::vector<VkPhysicalDevice> physical_devices(physical_count);
		if (vkEnumeratePhysicalDevices(context.instance, &physical_count, physical_devices.data()) != VK_SUCCESS)
		{
			return false;
		}
		for (const auto physical: physical_devices)
		{
			if (CreateDevice(physical))
			{
				bound = GraphicsRenderBindContextForTesting(&context);
				return bound;
			}
		}
		return false;
	}

	[[nodiscard]] GraphicContext* Context() { return &context; }
	[[nodiscard]] bool            WaveDispatchAvailable() const
	{
		const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(context.compute_wave_vulkan_state);
		return capabilities.size_control_enabled && capabilities.full_subgroups_enabled && capabilities.compute_required_size_supported;
	}

private:
	[[nodiscard]] bool CreateInstance()
	{
		const char* disable = std::getenv("KYTY_GDS_VALIDATION");
		const bool  wanted  = disable == nullptr || std::strcmp(disable, "0") != 0;
		uint32_t    layer_count = 0;
		vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
		std::vector<VkLayerProperties> layers(layer_count);
		vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
		const char* validation_layer = "VK_LAYER_KHRONOS_validation";
		const bool  has_layer        = std::any_of(layers.cbegin(), layers.cend(),
		                                           [&](const auto& layer) { return std::strcmp(layer.layerName, validation_layer) == 0; });
		uint32_t extension_count = 0;
		vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
		std::vector<VkExtensionProperties> extensions(extension_count);
		vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());
		const bool has_debug_utils = std::any_of(extensions.cbegin(), extensions.cend(), [](const auto& extension)
		                                         { return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0; });
		const bool validate = wanted && has_layer && has_debug_utils;
		const char* debug_utils = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

		VkApplicationInfo application {};
		application.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		application.apiVersion = VK_API_VERSION_1_4;
		VkInstanceCreateInfo instance_info {};
		instance_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance_info.pApplicationInfo        = &application;
		instance_info.enabledLayerCount       = validate ? 1u : 0u;
		instance_info.ppEnabledLayerNames     = validate ? &validation_layer : nullptr;
		instance_info.enabledExtensionCount   = validate ? 1u : 0u;
		instance_info.ppEnabledExtensionNames = validate ? &debug_utils : nullptr;
		if (vkCreateInstance(&instance_info, nullptr, &context.instance) != VK_SUCCESS)
		{
			return false;
		}
		if (validate)
		{
			auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			    vkGetInstanceProcAddr(context.instance, "vkCreateDebugUtilsMessengerEXT"));
			VkDebugUtilsMessengerCreateInfoEXT messenger_info {};
			messenger_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			messenger_info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
			messenger_info.pfnUserCallback = ValidationCallback;
			Expect(create != nullptr && create(context.instance, &messenger_info, nullptr, &messenger) == VK_SUCCESS,
			       "validation messenger must be created when the layer is enabled");
		}
		return true;
	}

	[[nodiscard]] bool CreateDevice(VkPhysicalDevice physical)
	{
		VkPhysicalDeviceProperties properties {};
		vkGetPhysicalDeviceProperties(physical, &properties);
		if (properties.apiVersion < VK_API_VERSION_1_4)
		{
			return false;
		}

		// Every supported core feature is enabled so emitted SPIR-V capabilities are available.
		VkPhysicalDeviceVulkan13Features features13 {};
		features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		VkPhysicalDeviceVulkan12Features features12 {};
		features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		features12.pNext = &features13;
		VkPhysicalDeviceVulkan11Features features11 {};
		features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
		features11.pNext = &features12;
		VkPhysicalDeviceFeatures2 features {};
		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &features11;
		vkGetPhysicalDeviceFeatures2(physical, &features);

		uint32_t extension_count = 0;
		vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr);
		std::vector<VkExtensionProperties> extensions(extension_count);
		vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data());
		// Subgroup size control is core since Vulkan 1.3. As in the window device, its features are
		// enabled through the Vulkan 1.3 aggregate above and the extension is recorded as a diagnostic.
		uint32_t size_control_revision = 0;
		for (const auto& extension: extensions)
		{
			if (std::strcmp(extension.extensionName, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) == 0)
			{
				size_control_revision = extension.specVersion;
			}
		}

		uint32_t family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
		std::vector<VkQueueFamilyProperties> families(family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
		for (uint32_t family = 0; family < family_count; ++family)
		{
			if (families[family].queueCount == 0 || (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 ||
			    (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0)
			{
				continue;
			}
			constexpr float priority = 1.0f;
			VkDeviceQueueCreateInfo queue_info {};
			queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queue_info.queueFamilyIndex = family;
			queue_info.queueCount       = 1;
			queue_info.pQueuePriorities = &priority;
			VkDeviceCreateInfo device_info {};
			device_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			device_info.pNext                   = &features;
			device_info.queueCreateInfoCount    = 1;
			device_info.pQueueCreateInfos       = &queue_info;
			if (vkCreateDevice(physical, &device_info, nullptr, &context.device) != VK_SUCCESS)
			{
				continue;
			}

			VkQueue queue = VK_NULL_HANDLE;
			vkGetDeviceQueue(context.device, family, 0, &queue);
			context.physical_device                             = physical;
			context.queues[GraphicContext::QUEUE_GFX].vk_queue  = queue;
			context.queues[GraphicContext::QUEUE_GFX].family    = family;
			context.queues[GraphicContext::QUEUE_GFX].index     = 0;
			context.queues[GraphicContext::QUEUE_GFX].mutex     = &context.queue_mutexes[0];
			context.queues[GraphicContext::QUEUE_UTIL].vk_queue = queue;
			context.queues[GraphicContext::QUEUE_UTIL].family   = family;
			context.queues[GraphicContext::QUEUE_UTIL].index    = 0;
			context.queues[GraphicContext::QUEUE_UTIL].mutex    = &context.queue_mutexes[0];
			context.queue_mutex_count                           = 1;
			FillWaveState(physical, properties, size_control_revision, features13);
			return true;
		}
		return false;
	}

	void FillWaveState(VkPhysicalDevice physical, const VkPhysicalDeviceProperties& properties, uint32_t size_control_revision,
	                   const VkPhysicalDeviceVulkan13Features& features13)
	{
		VkPhysicalDeviceSubgroupSizeControlProperties size_control {};
		size_control.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
		VkPhysicalDeviceSubgroupProperties subgroup {};
		subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
		subgroup.pNext = &size_control;
		VkPhysicalDeviceProperties2 properties2 {};
		properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		properties2.pNext = &subgroup;
		vkGetPhysicalDeviceProperties2(physical, &properties2);

		context.subgroup_size       = subgroup.subgroupSize;
		context.subgroup_stages     = subgroup.supportedStages;
		context.subgroup_operations = subgroup.supportedOperations;
		context.subgroup_min_size   = size_control.minSubgroupSize;
		context.subgroup_max_size   = size_control.maxSubgroupSize;
		auto& wave                  = context.compute_wave_vulkan_state;
		wave.extension_advertised   = size_control_revision != 0;
		wave.extension_revision     = size_control_revision;
		wave.extension_enabled      = false;
		wave.size_control_feature_supported   = features13.subgroupSizeControl == VK_TRUE;
		wave.full_subgroups_feature_supported = features13.computeFullSubgroups == VK_TRUE;
		wave.size_control_feature_enabled     = wave.size_control_feature_supported;
		wave.full_subgroups_feature_enabled   = wave.full_subgroups_feature_supported;
		wave.compute_required_size_supported  = (size_control.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
		wave.fragment_required_size_supported = (size_control.requiredSubgroupSizeStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0;
		wave.vertex_required_size_supported   = (size_control.requiredSubgroupSizeStages & VK_SHADER_STAGE_VERTEX_BIT) != 0;
		wave.quad_operations_in_all_stages    = subgroup.quadOperationsInAllStages == VK_TRUE;
		wave.compute_ballot_shuffle_supported = (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
		                                        (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0 &&
		                                        (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0;
		wave.min_subgroup_size = size_control.minSubgroupSize;
		wave.max_subgroup_size = size_control.maxSubgroupSize;
		for (int axis = 0; axis < 3; ++axis)
		{
			wave.max_local_size[axis]  = properties.limits.maxComputeWorkGroupSize[axis];
			wave.max_group_count[axis] = properties.limits.maxComputeWorkGroupCount[axis];
		}
		wave.max_invocations  = properties.limits.maxComputeWorkGroupInvocations;
		wave.max_subgroups    = size_control.maxComputeWorkgroupSubgroups;
		wave.max_shared_bytes = properties.limits.maxComputeSharedMemorySize;
	}

	GraphicContext           context {};
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	bool                     bound     = false;
};

// Guest-owned pages registered as a GPU mapping. Release happens only after every submission
// that references them has been published, mirroring the quiesced mapping release.
class GuestPages
{
public:
	GuestPages(GraphicContext* ctx, uint64_t bytes): m_ctx(ctx)
	{
		const uint64_t page = Kyty::Core::VirtualMemory::GetPageSize();
		m_size              = (bytes + page - 1u) / page * page;
		m_address           = Kyty::Core::VirtualMemory::Alloc(0, m_size, Kyty::Core::VirtualMemory::Mode::ReadWrite);
		Expect(m_address != 0, "guest pages must allocate");
		std::memset(reinterpret_cast<void*>(m_address), 0, m_size);
		GpuMemorySetAllocatedRange(m_address, m_size);
	}
	~GuestPages()
	{
		if (m_address != 0)
		{
			GpuMemoryFreeMappedRangeQuiesced(m_ctx, m_address, m_size);
			Expect(Kyty::Core::VirtualMemory::Free(m_address), "guest pages must free");
		}
	}
	GuestPages(const GuestPages&)            = delete;
	GuestPages& operator=(const GuestPages&) = delete;

	[[nodiscard]] uint64_t  Address() const { return m_address; }
	[[nodiscard]] uint64_t  Size() const { return m_size; }
	[[nodiscard]] uint32_t* Words() const { return reinterpret_cast<uint32_t*>(m_address); }
	// The test frees the mapping itself to model an unmapped destination.
	void Abandon()
	{
		Expect(Kyty::Core::VirtualMemory::Free(m_address), "abandoned guest pages must free");
		m_address = 0;
	}

private:
	GraphicContext* m_ctx     = nullptr;
	uint64_t        m_address = 0;
	uint64_t        m_size    = 0;
};

// Hardware IT_DMA_DATA through the production parser. Selectors: 0 memory, 1 GDS, 2 immediate source.
void Dma(CommandProcessor* cp, uint32_t src_sel, uint64_t src, uint32_t dst_sel, uint64_t dst, uint32_t bytes)
{
	const std::array<uint32_t, 7> packet {
	    KYTY_PM4(7, Pm4::IT_DMA_DATA, 0u),       (dst_sel << 20u) | (src_sel << 29u), static_cast<uint32_t>(src),
	    static_cast<uint32_t>(src >> 32u),       static_cast<uint32_t>(dst),          static_cast<uint32_t>(dst >> 32u),
	    bytes};
	const auto consumed = cp_op_dma_data(cp, packet[0], packet.data() + 1, 6u, 6u);
	Expect(consumed == 6u, "DMA_DATA consumes exactly its six body dwords");
}

uint64_t GdsByte(uint64_t dw_offset)
{
	return dw_offset * 4u;
}

struct Fixture
{
	GdsVulkanContext          vulkan;
	GpuSubmissionCoordinator  coordinator;
	CommandProcessor*         processor = nullptr;
};

// Command processors are process-lifetime objects; the fixture keeps that lifetime.
Fixture* CreateFixture()
{
	auto* fixture = new Fixture;
	if (!fixture->vulkan.Initialize())
	{
		std::fprintf(stderr, "skip: no Vulkan 1.4 graphics+compute device\n");
		std::_Exit(kSkipExit);
	}
	// The production graphics subsystem initializes these before any submission; the shader map
	// backs every Gen5 dispatch lookup.
	GpuMemoryInit();
	LabelInit();
	ShaderInit();
	fixture->processor = new CommandProcessor(&fixture->coordinator, GraphicContext::QUEUE_GFX);
	fixture->processor->BufferInit();
	return fixture;
}

uint64_t PendingPublications()
{
	return g_render_ctx->GetGdsBuffer()->PendingPublications();
}

void VerifyRoundtripAndLastDwords(Fixture* f)
{
	GuestPages source(f->vulkan.Context(), 64);
	GuestPages first(f->vulkan.Context(), 64);
	GuestPages last(f->vulkan.Context(), 64);
	for (uint32_t i = 0; i < 16; ++i)
	{
		source.Words()[i] = 0x1000u + i;
	}
	Dma(f->processor, kSelMemory, source.Address(), kSelGds, GdsByte(0), 64);
	Dma(f->processor, kSelMemory, source.Address(), kSelGds, GdsByte(kWindow - 4), 16);
	Dma(f->processor, kSelGds, GdsByte(0), kSelMemory, first.Address(), 64);
	Dma(f->processor, kSelGds, GdsByte(kWindow - 4), kSelMemory, last.Address(), 16);
	Expect(first.Words()[0] == 0u && last.Words()[0] == 0u, "a GDS read publishes only when its submission completes");
	f->processor->SubmitAndWait();
	Expect(std::equal(source.Words(), source.Words() + 16, first.Words()), "memory->GDS->memory returns the source dwords");
	Expect(std::equal(source.Words(), source.Words() + 4, last.Words()), "the last dwords of the window round-trip");
	Expect(PendingPublications() == 0, "published reads release their staging at publication");
}

void VerifyZeroCountRecordsNothing(Fixture* f)
{
	GuestPages destination(f->vulkan.Context(), 16);
	destination.Words()[0] = 0x5a5a5a5au;
	f->processor->ClearGds(kWindow, 0, 0x1u);
	f->processor->CopyGds(0, kWindow, 0);
	f->processor->ReadGds(destination.Words(), kWindow, 0);
	Expect(PendingPublications() == 0, "an empty read reserves no staging and records no label");
	f->processor->SubmitAndWait();
	Expect(destination.Words()[0] == 0x5a5a5a5au, "an empty read writes nothing");
}

void VerifyStreamOrderSnapshotIsolation(Fixture* f)
{
	GuestPages source(f->vulkan.Context(), 16);
	GuestPages before(f->vulkan.Context(), 16);
	GuestPages after(f->vulkan.Context(), 16);
	for (uint32_t i = 0; i < 4; ++i)
	{
		source.Words()[i] = 1u + i;
	}
	Dma(f->processor, kSelMemory, source.Address(), kSelGds, GdsByte(32), 16);
	Dma(f->processor, kSelGds, GdsByte(32), kSelMemory, before.Address(), 16);
	Dma(f->processor, kSelData, 0x77u, kSelGds, GdsByte(32), 16);
	Dma(f->processor, kSelGds, GdsByte(32), kSelMemory, after.Address(), 16);
	f->processor->SubmitAndWait();
	Expect(std::equal(source.Words(), source.Words() + 4, before.Words()), "a read before a later write keeps the earlier contents");
	for (uint32_t i = 0; i < 4; ++i)
	{
		Expect(after.Words()[i] == 0x77u, "a read after a fill observes the fill");
	}
}

void VerifyGdsToGdsCopies(Fixture* f)
{
	GuestPages source(f->vulkan.Context(), 16);
	GuestPages middle(f->vulkan.Context(), 16);
	GuestPages tail(f->vulkan.Context(), 16);
	for (uint32_t i = 0; i < 4; ++i)
	{
		source.Words()[i] = 0x51u + i;
	}
	Dma(f->processor, kSelMemory, source.Address(), kSelGds, GdsByte(64), 16);
	Dma(f->processor, kSelGds, GdsByte(64), kSelGds, GdsByte(68), 16); // adjacent, not overlapping
	Dma(f->processor, kSelGds, GdsByte(64), kSelGds, GdsByte(kWindow - 4), 16);
	Dma(f->processor, kSelGds, GdsByte(68), kSelMemory, middle.Address(), 16);
	Dma(f->processor, kSelGds, GdsByte(kWindow - 4), kSelMemory, tail.Address(), 16);
	f->processor->SubmitAndWait();
	Expect(std::equal(source.Words(), source.Words() + 4, middle.Words()), "an adjacent GDS copy reproduces the span");
	Expect(std::equal(source.Words(), source.Words() + 4, tail.Words()), "a GDS copy into the window end reproduces the span");
}

// A destination the dirty tracker has armed read-only receives the publication through the
// host-write lease: no fault, and the write generation advances.
void VerifyArmedDestinationPublication(Fixture* f)
{
	auto& tracker = GpuDirtyPageTracker::Instance();
	if (!tracker.Enabled())
	{
		std::fprintf(stderr, "note: dirty tracking disabled; armed-destination case not exercised\n");
		return;
	}
	GuestPages source(f->vulkan.Context(), 16);
	GuestPages destination(f->vulkan.Context(), 16);
	source.Words()[0] = 0xc0ffee01u;
	Expect(tracker.RegisterRange(destination.Address(), destination.Size()), "destination registers with the tracker");
	Expect(tracker.PrepareForRead(destination.Address(), destination.Size()), "destination pages arm read-only");
	const uint64_t generation = tracker.SnapshotGeneration(destination.Address(), destination.Size());
	Dma(f->processor, kSelMemory, source.Address(), kSelGds, GdsByte(96), 4);
	Dma(f->processor, kSelGds, GdsByte(96), kSelMemory, destination.Address(), 4);
	f->processor->SubmitAndWait();
	Expect(destination.Words()[0] == 0xc0ffee01u, "an armed destination receives the published dword");
	Expect(tracker.ChangedSince(destination.Address(), destination.Size(), generation), "publication advances the write generation");
	Expect(tracker.UnregisterRange(destination.Address(), destination.Size()), "destination unregisters");
}

// Write access is a publication-time requirement. A destination the guest keeps read-only when the
// read is recorded and makes writable again, as the same mapping, before the submission completes
// receives exactly the GDS span; its neighbouring dwords keep their bytes.
void VerifyReadOnlyAtRecordDestinationPublishes(Fixture* f)
{
	GuestPages source(f->vulkan.Context(), 16);
	GuestPages destination(f->vulkan.Context(), 24);
	for (uint32_t i = 0; i < 4; ++i)
	{
		source.Words()[i] = 0xd00du + i;
	}
	for (uint32_t i = 0; i < 6; ++i)
	{
		destination.Words()[i] = 0xa5a5a5a5u;
	}
	Dma(f->processor, kSelMemory, source.Address(), kSelGds, GdsByte(192), 16);
	Expect(Kyty::Core::VirtualMemory::ProtectGuest(destination.Address(), destination.Size(), Kyty::Core::VirtualMemory::Mode::Read),
	       "destination is read-only when the read is recorded");
	Dma(f->processor, kSelGds, GdsByte(192), kSelMemory, destination.Address() + 4u, 16);
	Expect(Kyty::Core::VirtualMemory::ProtectGuest(destination.Address(), destination.Size(), Kyty::Core::VirtualMemory::Mode::ReadWrite),
	       "the same destination mapping becomes writable before completion");
	f->processor->SubmitAndWait();
	Expect(std::equal(source.Words(), source.Words() + 4, destination.Words() + 1), "the GDS span is published at completion");
	Expect(destination.Words()[0] == 0xa5a5a5a5u && destination.Words()[5] == 0xa5a5a5a5u, "neighbouring dwords keep their bytes");
}

// A producer whose submission has completed and been published leaves guest bytes that are
// current: memory->GDS reads the new value it wrote, not the value it replaced.
void VerifyCompletedProducerIsCurrent(Fixture* f)
{
	GuestPages staging(f->vulkan.Context(), 16);
	GuestPages result(f->vulkan.Context(), 16);
	staging.Words()[0] = 0x0101u;
	Dma(f->processor, kSelMemory, staging.Address(), kSelGds, GdsByte(128), 4);
	Dma(f->processor, kSelData, 0x0202u, kSelGds, GdsByte(129), 4);
	Dma(f->processor, kSelGds, GdsByte(129), kSelMemory, staging.Address(), 4); // old 0x0101 replaced at publication
	f->processor->SubmitAndWait();
	Expect(staging.Words()[0] == 0x0202u, "the producer's publication completed");
	Dma(f->processor, kSelMemory, staging.Address(), kSelGds, GdsByte(130), 4);
	Dma(f->processor, kSelGds, GdsByte(130), kSelMemory, result.Address(), 4);
	f->processor->SubmitAndWait();
	Expect(result.Words()[0] == 0x0202u, "memory->GDS reads the completed producer's new value, never the old one");
}

// A GPU writer owns the source: a writable storage object over the range is written on the device in
// the same recording, so the guest bytes are stale until write-back. memory->GDS must copy the
// device bytes at its stream position.
void VerifyGpuProducerSourceIsDeviceBuffer(Fixture* f)
{
	GraphicContext* ctx = f->vulkan.Context();
	GuestPages      produced(ctx, 16);
	GuestPages      result(ctx, 16);
	for (uint32_t i = 0; i < 4; ++i)
	{
		produced.Words()[i] = 0x01u + i; // the old guest bytes
	}
	CommandBuffer* buffer = f->processor->RecordingBufferForTesting();
	Expect(buffer != nullptr, "the processor has an open recording");
	auto* storage = static_cast<StorageVulkanBuffer*>(
	    GpuMemoryCreateObject(0, ctx, buffer, produced.Address(), 16, StorageBufferGpuObject(4, 4, false)));
	Expect(storage != nullptr && storage->buffer != nullptr, "a writable storage object is created for the recording");

	// Replace only this fixture's just-created backing before its first recorded
	// access. The device-fill producer requires transfer-destination usage;
	// production storage-buffer usage remains unchanged.
	const auto fixture_memory_properties = storage->memory.property;
	VulkanDeleteBuffer(ctx, storage);
	storage->usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	storage->memory.property = fixture_memory_properties;
	VulkanCreateBuffer(ctx, 16, storage);
	Expect(storage->buffer != nullptr, "the fixture backing admits a device fill");

	const VkCommandBuffer cmd = buffer->GetPool()->buffers[buffer->GetIndex()];
	VkBufferMemoryBarrier barrier {};
	barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = storage->buffer;
	barrier.offset              = 0;
	barrier.size                = 16;
	barrier.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
	vkCmdFillBuffer(cmd, storage->buffer, 0, 16, 0x0b0b0b0bu); // the new device bytes
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);

	Dma(f->processor, kSelMemory, produced.Address(), kSelGds, GdsByte(160), 16);
	Dma(f->processor, kSelGds, GdsByte(160), kSelMemory, result.Address(), 16);
	f->processor->SubmitAndWait();
	for (uint32_t i = 0; i < 4; ++i)
	{
		Expect(result.Words()[i] == 0x0b0b0b0bu, "memory->GDS reads the GPU producer's new bytes, never the stale guest bytes");
	}
}

// Production compute dispatch of owned shader words through DispatchDirect. The binder takes the
// GDS descriptor from the same GdsBuffer the DMA path writes, so the seeded counter, the append,
// and the published read share one backing. DS_APPEND adds count_bits(EXEC) at M0.base + offset
// (RDNA2 ISA, DS opcode 62); offset is zero for GDS.
void VerifyProductionAppend(Fixture* f)
{
	if (!f->vulkan.WaveDispatchAvailable())
	{
		std::fprintf(stderr, "skip: device lacks subgroup size control for wave dispatch\n");
		std::_Exit(kSkipExit);
	}
	GuestPages shader(f->vulkan.Context(), 64);
	const std::array<uint32_t, 7> words {
	    0xbefc0300u,              // s_mov_b32 m0, s0
	    0xbefe03ffu, 0x0000ffffu, // s_mov_b32 exec_lo, 0x0000ffff
	    0xbeff0380u,              // s_mov_b32 exec_hi, 0
	    0xd8fa0000u, 0x00000000u, // ds_append v0 gds
	    0xbf810000u,              // s_endpgm
	};
	std::memcpy(shader.Words(), words.data(), sizeof(words));
	// GraphicsCreateShader maps a Gen5 shader's code range and user-data block before a dispatch can
	// reference it; a dispatch of an unmapped shader is refused. This shader declares no resources, so
	// its user-data block is empty and the GDS pointer comes from the observed M0 source, as in the
	// ordered-append binding unit test.
	static ShaderUserData user_data {};
	ShaderMappedData      mapped;
	mapped.user_data       = &user_data;
	mapped.code_size_bytes = static_cast<uint32_t>(sizeof(words));
	ShaderMapUserData(shader.Address(), mapped);

	GuestPages seed(f->vulkan.Context(), 16);
	GuestPages before(f->vulkan.Context(), 16);
	GuestPages after(f->vulkan.Context(), 16);
	seed.Words()[0] = 41u;
	seed.Words()[1] = 7u;
	constexpr uint64_t kCounterDw = 4;
	Dma(f->processor, kSelMemory, seed.Address(), kSelGds, GdsByte(kCounterDw), 8);
	Dma(f->processor, kSelGds, GdsByte(kCounterDw), kSelMemory, before.Address(), 8);

	HW::CsStageRegisters regs {};
	regs.data_addr    = shader.Address();
	regs.chksum       = 0x6d5a1e0047d50001ull;
	regs.num_thread_x = 64;
	regs.num_thread_y = 1;
	regs.num_thread_z = 1;
	regs.user_sgpr    = 1;
	auto* sh          = f->processor->GetShCtx();
	sh->SetCsShader(regs, 0);
	sh->SetCsNumThreadX(64);
	sh->SetCsNumThreadY(1);
	sh->SetCsNumThreadZ(1);
	// M0 = {base[15:0] bytes, size[15:0] bytes}: the counter dword only.
	sh->SetCsUserSgpr(0, (static_cast<uint32_t>(GdsByte(kCounterDw)) << 16u) | 4u, HW::UserSgprType::Unknown);
	f->processor->DispatchDirect(1, 1, 1, 1);

	Dma(f->processor, kSelGds, GdsByte(kCounterDw), kSelMemory, after.Address(), 8);
	f->processor->SubmitAndWait();
	Expect(before.Words()[0] == 41u && before.Words()[1] == 7u, "the read recorded before the dispatch keeps the seeded counter");
	Expect(after.Words()[0] == 41u + 16u, "DS_APPEND adds the 16 active lanes once to the shared GDS counter");
	Expect(after.Words()[1] == 7u, "the neighbouring GDS dword is untouched");
}

// Strict refusals. Each runs in its own process and must end with the EXIT status, never with a
// crash, a skipped packet, or stale bytes.
void RefuseDestinationPastWindow(Fixture* f)
{
	GuestPages source(f->vulkan.Context(), 16);
	Dma(f->processor, kSelMemory, source.Address(), kSelGds, GdsByte(kWindow), 4);
}

void RefuseOverlappingGdsCopy(Fixture* f)
{
	Dma(f->processor, kSelGds, GdsByte(0), kSelGds, GdsByte(2), 16);
}

void RefuseUnalignedMemorySource(Fixture* f)
{
	GuestPages source(f->vulkan.Context(), 16);
	Dma(f->processor, kSelMemory, source.Address() + 2u, kSelGds, GdsByte(0), 4);
}

void RefuseUnreadableMemorySource(Fixture* f)
{
	GuestPages source(f->vulkan.Context(), 16);
	Expect(Kyty::Core::VirtualMemory::ProtectGuest(source.Address(), source.Size(), Kyty::Core::VirtualMemory::Mode::NoAccess),
	       "source becomes unreadable");
	Dma(f->processor, kSelMemory, source.Address(), kSelGds, GdsByte(0), 4);
}

void RefuseUnallocatedDestination(Fixture* f)
{
	std::array<uint32_t, 4> host {};
	Dma(f->processor, kSelGds, GdsByte(0), kSelMemory, reinterpret_cast<uint64_t>(host.data()), 16);
}

void RefuseZeroByteGdsPacket(Fixture* f)
{
	GuestPages destination(f->vulkan.Context(), 16);
	Dma(f->processor, kSelGds, GdsByte(0), kSelMemory, destination.Address(), 0);
}

// The destination loses write access before the publication completes. In production a mapping
// release drains every processor first; a protection change does not, so the publication must
// refuse instead of writing.
void RefuseReadOnlyDestinationAtPublication(Fixture* f)
{
	GuestPages destination(f->vulkan.Context(), 16);
	Dma(f->processor, kSelGds, GdsByte(0), kSelMemory, destination.Address(), 16);
	Expect(Kyty::Core::VirtualMemory::ProtectGuest(destination.Address(), destination.Size(), Kyty::Core::VirtualMemory::Mode::Read),
	       "destination becomes read-only");
	f->processor->SubmitAndWait();
}

// The destination is unmapped behind the processor's back. The publication must refuse rather than
// write into a released range.
void RefuseUnmappedDestinationAtPublication(Fixture* f)
{
	GuestPages destination(f->vulkan.Context(), 16);
	Dma(f->processor, kSelGds, GdsByte(0), kSelMemory, destination.Address(), 16);
	destination.Abandon();
	f->processor->SubmitAndWait();
}

// The destination is released and a new writable mapping takes its address before the publication
// completes. The new mapping passes the GPU-range, ownership and write checks; only the mapping
// identity recorded with the read tells it apart, so the publication must refuse rather than write
// the earlier destination's bytes into it.
void RefuseRemappedDestinationAtPublication(Fixture* f)
{
	GuestPages     destination(f->vulkan.Context(), 16);
	const uint64_t address = destination.Address();
	const uint64_t size    = destination.Size();
	Dma(f->processor, kSelData, 0x5au, kSelGds, GdsByte(0), 16);
	Dma(f->processor, kSelGds, GdsByte(0), kSelMemory, address, 16);
	destination.Abandon();
	Expect(Kyty::Core::VirtualMemory::AllocFixed(address, size, Kyty::Core::VirtualMemory::Mode::ReadWrite),
	       "a new mapping takes the released destination address");
	std::memset(reinterpret_cast<void*>(address), 0xa5, size);
	f->processor->SubmitAndWait();
}

struct Scenario
{
	const char* name;
	void (*run)(Fixture*);
};

constexpr std::array<Scenario, 9> kRefusals {{
    {"--refuse-destination-past-window", RefuseDestinationPastWindow},
    {"--refuse-overlapping-gds-copy", RefuseOverlappingGdsCopy},
    {"--refuse-unaligned-memory-source", RefuseUnalignedMemorySource},
    {"--refuse-unreadable-memory-source", RefuseUnreadableMemorySource},
    {"--refuse-unallocated-destination", RefuseUnallocatedDestination},
    {"--refuse-zero-byte-gds-packet", RefuseZeroByteGdsPacket},
    {"--refuse-read-only-destination", RefuseReadOnlyDestinationAtPublication},
    {"--refuse-unmapped-destination", RefuseUnmappedDestinationAtPublication},
    {"--refuse-remapped-destination", RefuseRemappedDestinationAtPublication},
}};

#if !defined(_WIN32)
int RunChild(const char* self, const char* scenario)
{
	char* child_argv[] = {const_cast<char*>(self), const_cast<char*>(scenario), nullptr};
	pid_t pid          = 0;
	if (posix_spawn(&pid, self, nullptr, nullptr, child_argv, environ) != 0)
	{
		Die("refusal child must spawn");
	}
	int status = 0;
	if (waitpid(pid, &status, 0) != pid)
	{
		Die("refusal child must be waited for");
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

} // namespace

} // namespace Kyty::Libs::Graphics

int main(int argc, char** argv)
{
	using namespace Kyty::Libs::Graphics;
	const std::string scenario = argc == 2 ? argv[1] : "";

	for (const auto& refusal: kRefusals)
	{
		if (scenario == refusal.name)
		{
			InitializeSubsystems();
			auto* fixture = CreateFixture();
			refusal.run(fixture);
			// Reaching here means the refusal did not happen.
			return 2;
		}
	}

	InitializeSubsystems();
	if (scenario == "--append")
	{
		auto* fixture = CreateFixture();
		VerifyProductionAppend(fixture);
		delete fixture;
		return 0;
	}
	Expect(scenario.empty(), "unknown scenario");

	auto* fixture = CreateFixture();
	VerifyRoundtripAndLastDwords(fixture);
	VerifyZeroCountRecordsNothing(fixture);
	VerifyStreamOrderSnapshotIsolation(fixture);
	VerifyGdsToGdsCopies(fixture);
	VerifyArmedDestinationPublication(fixture);
	VerifyReadOnlyAtRecordDestinationPublishes(fixture);
	VerifyCompletedProducerIsCurrent(fixture);
	VerifyGpuProducerSourceIsDeviceBuffer(fixture);
	delete fixture;

#if !defined(_WIN32)
	for (const auto& refusal: kRefusals)
	{
		const int status = RunChild(argv[0], refusal.name);
		if (status != kStrictExit)
		{
			std::fprintf(stderr, "%s exited with %d, expected %d\n", refusal.name, status, kStrictExit);
			Die("a GDS refusal did not fail strictly");
		}
	}
#endif
	return 0;
}
