// Executes the production translation of MIMG DIM 4 (1D array) sampling and loads on a Vulkan
// device. The 1D array binds as the emitter expects: a one-row 2D array whose view starts at the
// T# base array. Every expected value is computed from the texel formula owned by this file.
// The fixture uploads host texels directly: it validates the translation and the host image
// representation, not the guest memory layout or upload of a 1D array.
//
// Contracts: AMD RDNA2 ISA, tables 42 and 43 (DIM 4 address order x, slice[, lod]; offsets first),
// table 45 (type 12 = 1D array, base array). Vulkan 1.4 image addressing defines wrap, border,
// level and layer selection on the host side.

#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Math/MathAll.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

using Kyty::Libs::Graphics::ShaderCode;
using Kyty::Libs::Graphics::ShaderComputeInputInfo;

constexpr int      kUnavailable = 77;
constexpr uint32_t kEndProgram  = 0xbf810000u;
constexpr uint32_t kWidth       = 4u;  // level 0; level 1 is 2 texels wide
constexpr uint32_t kLevels      = 2u;
constexpr uint32_t kLayers      = 4u;
constexpr uint32_t kBaseArray   = 1u;  // the T# base array: guest slice s reads layer s + 1
constexpr uint32_t kViewLayers  = kLayers - kBaseArray;

// MIMG operand registers of every case: T# s[32:39], S# s[20:23], VDATA v8, VADDR v4.
constexpr uint32_t kResourceGroup = 8u;
constexpr uint32_t kSamplerGroup  = 5u;
constexpr uint32_t kDataRegister  = 8u;
constexpr uint32_t kAddressBase   = 4u;

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "1D-array texture integration failure: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

[[noreturn]] void Unavailable(const std::string& message)
{
	std::fprintf(stderr, "Vulkan capability unavailable: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(kUnavailable);
}

void Check(VkResult result, const char* operation)
{
	if (result != VK_SUCCESS) { Fail(std::string(operation) + " returned VkResult " + std::to_string(static_cast<int>(result))); }
}

// Owned fixture: distinct values per level, layer and texel, exact in half precision.
float FloatTexel(uint32_t level, uint32_t layer, uint32_t x)
{
	return static_cast<float>(1000u * level + 100u * layer + 10u + x);
}

uint32_t UnsignedTexel(uint32_t layer, uint32_t x)
{
	return 0x10000u * layer + 0x100u + x;
}

uint16_t ToHalf(float value)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	if (value == 0.0f) { return static_cast<uint16_t>((bits >> 16u) & 0x8000u); }
	const int      exponent = static_cast<int>((bits >> 23u) & 0xffu) - 127 + 15;
	const uint32_t mantissa = bits & 0x7fffffu;
	if (exponent <= 0 || exponent >= 31 || (mantissa & 0x1fffu) != 0u) { Fail("fixture texel is not exact in half precision"); }
	return static_cast<uint16_t>(((bits >> 16u) & 0x8000u) | (static_cast<uint32_t>(exponent) << 10u) | (mantissa >> 13u));
}

uint32_t FloatBits(float value)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

void InitializeConfig()
{
	char program[] = "kyty_texture_one_dimensional_array_integration";
	char* argv[]   = {program, nullptr};
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
	if (!subsystems->InitAll(false)) { Fail("core/config/log subsystems must initialize"); }
	Kyty::Config::SetNextGen(true);

	class ValidationConfig final: public Kyty::Config::ConfigSource
	{
	public:
		bool Has(const Kyty::Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Kyty::Core::String&) const override { return 0; }
		bool GetBool(const Kyty::Core::String&) const override { return true; }
		Kyty::Core::String GetString(const Kyty::Core::String&) const override { return {}; }
	} validation;
	Kyty::Config::Load(validation);
}

enum class SamplerKind : uint32_t
{
	None,
	NearestEdge,
	NearestRepeat,
	NearestBorder,
	NearestEdgeRowBorder,
	LinearEdgeRowBorder,
};

struct Case
{
	const char*           name;
	uint32_t              opcode;
	uint32_t              dmask;
	uint32_t              guest_format; // 22 = R32_FLOAT, 20 = R32_UINT
	SamplerKind           sampler;
	std::vector<uint32_t> address_bits; // VGPRs v4, v5, ... in address order
	uint32_t              expected_bits;
	bool                  exact_bits;   // integer and nearest results compare bit-exactly
};

std::vector<Case> MakeCases()
{
	const auto f = [](float value) { return FloatBits(value); };
	// Level-0 texel centres in normalized x: (texel + 0.5) / width.
	const auto centre = [](uint32_t texel, uint32_t width) { return (static_cast<float>(texel) + 0.5f) / static_cast<float>(width); };
	std::vector<Case> cases;
	// image_load (x, slice): integer coordinates, the slice relative to the base array.
	cases.push_back({"LoadFirstViewSlice", 0x00u, 0xfu, 22u, SamplerKind::None, {2u, 0u}, f(FloatTexel(0, 1, 2)), true});
	cases.push_back({"LoadLastViewSlice", 0x00u, 0xfu, 22u, SamplerKind::None, {3u, 2u}, f(FloatTexel(0, 3, 3)), true});
	cases.push_back({"LoadUnsignedSlice", 0x00u, 0xfu, 20u, SamplerKind::None, {1u, 1u}, UnsignedTexel(2, 1), true});
	// image_load_mip (x, slice, mip): level 1 of the last view slice.
	cases.push_back({"LoadMipLevel1", 0x01u, 0xfu, 22u, SamplerKind::None, {1u, 2u, 1u}, f(FloatTexel(1, 3, 1)), true});
	// image_sample_l (x, slice, lod).
	cases.push_back({"SampleLevel0", 0x24u, 0xfu, 22u, SamplerKind::NearestEdge, {f(centre(1, kWidth)), f(1.0f), f(0.0f)},
	                 f(FloatTexel(0, 2, 1)), true});
	cases.push_back({"SampleLevel1", 0x24u, 0xfu, 22u, SamplerKind::NearestEdge, {f(centre(1, kWidth / 2u)), f(2.0f), f(1.0f)},
	                 f(FloatTexel(1, 3, 1)), true});
	// x = 1.125 is texel coordinate 4.5, one half texel past the right edge.
	cases.push_back({"SampleRepeatX", 0x24u, 0xfu, 22u, SamplerKind::NearestRepeat, {f(1.125f), f(0.0f), f(0.0f)},
	                 f(FloatTexel(0, 1, 0)), true});
	cases.push_back({"SampleClampToEdgeX", 0x24u, 0xfu, 22u, SamplerKind::NearestEdge, {f(1.125f), f(0.0f), f(0.0f)},
	                 f(FloatTexel(0, 1, kWidth - 1u)), true});
	cases.push_back({"SampleClampToBorderX", 0x24u, 0xfu, 22u, SamplerKind::NearestBorder, {f(1.125f), f(0.0f), f(0.0f)}, f(0.0f),
	                 true});
	// Linear filtering midway between texels 1 and 2 with an opaque-white V border: the pinned row
	// gives the absent second row zero weight, so the result is the mean of the two texels.
	cases.push_back({"SampleLinearRowBorderInvariant", 0x24u, 0xfu, 22u, SamplerKind::LinearEdgeRowBorder, {f(0.5f), f(1.0f), f(0.0f)},
	                 f((FloatTexel(0, 2, 1) + FloatTexel(0, 2, 2)) * 0.5f), false});
	// image_sample_lz (x, slice).
	cases.push_back({"SampleLzSlice", 0x27u, 0xfu, 22u, SamplerKind::NearestEdge, {f(centre(0, kWidth)), f(2.0f)}, f(FloatTexel(0, 3, 0)),
	                 true});
	// image_sample_lz_o (offset, x, slice): x offset +1 moves texel 1 to texel 2; the y field (3)
	// would sample the V border if a 1D address applied it.
	cases.push_back({"SampleLzOffsetX", 0x37u, 0x1u, 22u, SamplerKind::NearestEdgeRowBorder,
	                 {1u | (3u << 8u), f(centre(1, kWidth)), f(0.0f)}, f(FloatTexel(0, 1, 2)), true});
	return cases;
}

std::vector<uint32_t> MakeGuestProgram(const Case& test_case)
{
	const uint32_t word0 = (0x3cu << 26u) | (test_case.opcode << 18u) | (test_case.dmask << 8u) | (4u << 3u);
	const uint32_t ssamp = test_case.sampler == SamplerKind::None ? 0u : kSamplerGroup;
	const uint32_t word1 = (ssamp << 21u) | (kResourceGroup << 16u) | (kDataRegister << 8u) | kAddressBase;
	return {word0, word1, kEndProgram};
}

ShaderComputeInputInfo MakeInput(const Case& test_case)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
	auto& bind = input.bind;
	bind.textures2D.textures_num                 = 1;
	bind.textures2D.textures2d_array_sampled_num = 1;
	auto& descriptor                             = bind.textures2D.desc[0];
	descriptor.start_register                    = static_cast<int>(kResourceGroup * 4u);
	descriptor.usage                             = Kyty::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	descriptor.sampled_shape                     = Kyty::Libs::Graphics::ShaderGen5SampledTextureShape::TwoDimensionalArray;
	descriptor.texture.fields[0]                 = 0x100u;
	descriptor.texture.fields[1]                 = test_case.guest_format << 20u;
	descriptor.texture.fields[3]                 = (12u << 28u) | 4u | (5u << 3u) | (6u << 6u) | (7u << 9u);
	descriptor.texture.fields[4]                 = (kBaseArray << 16u) | (kLayers - 1u);
	if (test_case.sampler != SamplerKind::None)
	{
		bind.samplers.samplers_num      = 1;
		bind.samplers.start_register[0] = static_cast<int>(kSamplerGroup * 4u);
	}
	Kyty::Libs::Graphics::ShaderCalcBindingIndices(&bind);
	return input;
}

bool Insert(std::string* text, const std::string& anchor, const std::string& insertion, bool after)
{
	const size_t position = text->find(anchor);
	if (position == std::string::npos) { return false; }
	text->insert(after ? position + anchor.size() : position, insertion);
	return true;
}

// The production translation plus a test-only SSBO: address VGPRs are seeded with the case bits
// after the program's initial EXEC setup, and v8 (result component R) is exported as raw bits.
std::string BuildProbeSource(const Case& test_case, const ShaderComputeInputInfo& input, uint32_t probe_binding)
{
	const auto words = MakeGuestProgram(test_case);
	ShaderCode code;
	code.SetType(Kyty::Libs::Graphics::ShaderType::Compute);
	if (!Kyty::Libs::Graphics::ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code))
	{
		Fail(std::string(test_case.name) + ": bounded parser rejected the guest program");
	}
	const auto translated = Kyty::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, &input);
	std::string text(translated.GetDataConst(), translated.Size());

	std::string annotations = "OpDecorate %probe_words ArrayStride 4\nOpDecorate %probe_block Block\n"
	                          "OpMemberDecorate %probe_block 0 Offset 0\n";
	annotations += "OpDecorate %probe_buffer DescriptorSet " + std::to_string(input.bind.descriptor_set_slot) + "\n";
	annotations += "OpDecorate %probe_buffer Binding " + std::to_string(probe_binding) + "\n";
	std::string constants = "%probe_index_0 = OpConstant %uint 0\n";
	std::string seeds;
	for (size_t address = 0; address < test_case.address_bits.size(); ++address)
	{
		constants += "%probe_seed_bits_" + std::to_string(address) + " = OpConstant %uint " + std::to_string(test_case.address_bits[address]) +
		             "\n";
		seeds += "%probe_seed_" + std::to_string(address) + " = OpBitcast %float %probe_seed_bits_" + std::to_string(address) + "\n";
		seeds += "OpStore %v" + std::to_string(kAddressBase + address) + " %probe_seed_" + std::to_string(address) + "\n";
	}
	const std::string exports = "%probe_ptr = OpAccessChain %_ptr_StorageBuffer_uint %probe_buffer %probe_index_0 %probe_index_0\n"
	                            "%probe_value = OpLoad %float %v" + std::to_string(kDataRegister) + "\n"
	                            "%probe_bits = OpBitcast %uint %probe_value\nOpStore %probe_ptr %probe_bits\n";

	bool spliced = Insert(&text, "%void = OpTypeVoid", annotations, false) &&
	               Insert(&text, "%function_void = OpTypeFunction %void",
	                      "%probe_words = OpTypeRuntimeArray %uint\n%probe_block = OpTypeStruct %probe_words\n"
	                      "%probe_block_ptr = OpTypePointer StorageBuffer %probe_block\n",
	                      false) &&
	               Insert(&text, "%true = OpConstantTrue %bool", constants, false) &&
	               Insert(&text, "OpEntryPoint GLCompute %main \"main\"", " %probe_buffer", true) &&
	               Insert(&text, ";Variables\n%gl_LocalInvocationID", "%probe_buffer = OpVariable %probe_block_ptr StorageBuffer\n", false);
	const size_t main_start = text.find("%main       = OpFunction %void None %function_void");
	const size_t exec_init  = main_start == std::string::npos ? std::string::npos : text.find("OpStore %execz %mask_exec_z_initial", main_start);
	const size_t seed_at    = exec_init == std::string::npos ? std::string::npos : text.find('\n', exec_init);
	if (!spliced || seed_at == std::string::npos) { Fail(std::string(test_case.name) + ": generated source lacks a probe anchor"); }
	text.insert(seed_at + 1u, seeds);
	const size_t main_return = text.find("OpReturn", seed_at);
	if (main_return == std::string::npos) { Fail(std::string(test_case.name) + ": generated source lacks a main return"); }
	text.insert(main_return, exports);
	return text;
}

struct Buffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void*          mapped = nullptr;
};

struct Image
{
	VkImage        image  = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView    view   = VK_NULL_HANDLE;
};

class Device
{
public:
	Device() = default;
	Device(const Device&) = delete;
	Device& operator=(const Device&) = delete;
	~Device();

	void Initialize();
	Buffer CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
	void   DestroyBuffer(Buffer* buffer);
	Image  CreateFixture(VkFormat format, uint32_t levels, const std::vector<uint8_t>& texels, uint32_t texel_bytes);
	void   DestroyImage(Image* image);
	VkSampler CreateSampler(SamplerKind kind);
	void      DestroySampler(VkSampler sampler) { vkDestroySampler(device_, sampler, nullptr); }
	uint32_t  Dispatch(const Kyty::Vector<uint32_t>& spirv, const ShaderComputeInputInfo& input, uint32_t probe_binding,
	                   const std::vector<std::pair<std::string, uint32_t>>& image_bindings, const Image& float_image,
	                   const Image& uint_image, VkSampler sampler);

private:
	uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
	void     Submit(const std::function<void(VkCommandBuffer)>& record);

	VkInstance       instance_       = VK_NULL_HANDLE;
	VkPhysicalDevice physical_       = VK_NULL_HANDLE;
	VkDevice         device_         = VK_NULL_HANDLE;
	VkQueue          queue_          = VK_NULL_HANDLE;
	uint32_t         queue_family_   = 0;
	VkCommandPool    command_pool_   = VK_NULL_HANDLE;
};

Device::~Device()
{
	if (device_ != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(device_);
		if (command_pool_ != VK_NULL_HANDLE) { vkDestroyCommandPool(device_, command_pool_, nullptr); }
		vkDestroyDevice(device_, nullptr);
	}
	if (instance_ != VK_NULL_HANDLE) { vkDestroyInstance(instance_, nullptr); }
}

void Device::Initialize()
{
	const auto enumerate_version =
	    reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
	uint32_t loader_version = 0;
	if (enumerate_version == nullptr || enumerate_version(&loader_version) != VK_SUCCESS || loader_version < VK_API_VERSION_1_4)
	{
		Unavailable("a Vulkan 1.4 instance API is required");
	}
	VkApplicationInfo app_info {};
	app_info.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "Kyty 1D-array texture integration";
	app_info.pEngineName      = "Kyty";
	app_info.apiVersion       = VK_API_VERSION_1_4;
	VkInstanceCreateInfo instance_info {};
	instance_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &app_info;
	const VkResult created = vkCreateInstance(&instance_info, nullptr, &instance_);
	if (created == VK_ERROR_INCOMPATIBLE_DRIVER) { Unavailable("the Vulkan loader found no compatible driver"); }
	Check(created, "vkCreateInstance");

	uint32_t count = 0;
	Check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices(count)");
	std::vector<VkPhysicalDevice> devices(count);
	Check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices(list)");
	for (const VkPhysicalDevice candidate: devices)
	{
		VkPhysicalDeviceProperties properties {};
		vkGetPhysicalDeviceProperties(candidate, &properties);
		VkFormatProperties half {};
		VkFormatProperties uint {};
		vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R16G16B16A16_SFLOAT, &half);
		vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R32_UINT, &uint);
		const VkFormatFeatureFlags filtered = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
		                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
		const VkFormatFeatureFlags fetched  = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
		if (properties.apiVersion < VK_API_VERSION_1_4 || (half.optimalTilingFeatures & filtered) != filtered ||
		    (uint.optimalTilingFeatures & fetched) != fetched)
		{
			continue;
		}
		uint32_t family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
		std::vector<VkQueueFamilyProperties> families(family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
		for (uint32_t family = 0; family < family_count && physical_ == VK_NULL_HANDLE; ++family)
		{
			if (families[family].queueCount > 0 && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
			{
				physical_     = candidate;
				queue_family_ = family;
			}
		}
		if (physical_ != VK_NULL_HANDLE)
		{
			std::printf("1D-array texture device: %s\n", properties.deviceName);
			break;
		}
	}
	if (physical_ == VK_NULL_HANDLE) { Unavailable("no Vulkan 1.4 compute device samples and filters the fixture formats"); }

	const float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info {};
	queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = queue_family_;
	queue_info.queueCount       = 1;
	queue_info.pQueuePriorities = &queue_priority;
	VkDeviceCreateInfo device_info {};
	device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos    = &queue_info;
	Check(vkCreateDevice(physical_, &device_info, nullptr, &device_), "vkCreateDevice");
	vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

	VkCommandPoolCreateInfo pool_info {};
	pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_;
	Check(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool");
}

uint32_t Device::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const
{
	VkPhysicalDeviceMemoryProperties memory {};
	vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
	for (uint32_t type = 0; type < memory.memoryTypeCount; ++type)
	{
		if ((type_bits & (1u << type)) != 0u && (memory.memoryTypes[type].propertyFlags & flags) == flags) { return type; }
	}
	Fail("no memory type satisfies the fixture allocation");
}

Buffer Device::CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
	Buffer result;
	VkBufferCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size        = size;
	info.usage       = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	Check(vkCreateBuffer(device_, &info, nullptr, &result.buffer), "vkCreateBuffer");
	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(device_, result.buffer, &requirements);
	VkMemoryAllocateInfo allocate {};
	allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
	                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	Check(vkAllocateMemory(device_, &allocate, nullptr, &result.memory), "vkAllocateMemory(buffer)");
	Check(vkBindBufferMemory(device_, result.buffer, result.memory, 0), "vkBindBufferMemory");
	Check(vkMapMemory(device_, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped), "vkMapMemory");
	std::memset(result.mapped, 0, static_cast<size_t>(size));
	return result;
}

void Device::DestroyBuffer(Buffer* buffer)
{
	if (buffer->buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device_, buffer->buffer, nullptr); }
	if (buffer->memory != VK_NULL_HANDLE) { vkFreeMemory(device_, buffer->memory, nullptr); }
	*buffer = {};
}

void Device::Submit(const std::function<void(VkCommandBuffer)>& record)
{
	VkCommandBufferAllocateInfo allocate {};
	allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool        = command_pool_;
	allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	VkCommandBuffer command = VK_NULL_HANDLE;
	Check(vkAllocateCommandBuffers(device_, &allocate, &command), "vkAllocateCommandBuffers");
	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	Check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
	record(command);
	Check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
	VkFenceCreateInfo fence_info {};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence = VK_NULL_HANDLE;
	Check(vkCreateFence(device_, &fence_info, nullptr, &fence), "vkCreateFence");
	VkSubmitInfo submit {};
	submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	Check(vkQueueSubmit(queue_, 1, &submit, fence), "vkQueueSubmit");
	// Bounded wait: ten seconds for a single-invocation dispatch or a few-texel upload.
	Check(vkWaitForFences(device_, 1, &fence, VK_TRUE, 10000000000ull), "vkWaitForFences");
	vkDestroyFence(device_, fence, nullptr);
	vkFreeCommandBuffers(device_, command_pool_, 1, &command);
}

// One-row 2D array, all layers and levels uploaded from `texels` (level-major, then layer, then x).
// The view starts at the T# base array, as the bind path creates it.
Image Device::CreateFixture(VkFormat format, uint32_t levels, const std::vector<uint8_t>& texels, uint32_t texel_bytes)
{
	Image result;
	VkImageCreateInfo info {};
	info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType     = VK_IMAGE_TYPE_2D;
	info.format        = format;
	info.extent        = {kWidth, 1u, 1u};
	info.mipLevels     = levels;
	info.arrayLayers   = kLayers;
	info.samples       = VK_SAMPLE_COUNT_1_BIT;
	info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	Check(vkCreateImage(device_, &info, nullptr, &result.image), "vkCreateImage");
	VkMemoryRequirements requirements {};
	vkGetImageMemoryRequirements(device_, result.image, &requirements);
	VkMemoryAllocateInfo allocate {};
	allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, 0);
	Check(vkAllocateMemory(device_, &allocate, nullptr, &result.memory), "vkAllocateMemory(image)");
	Check(vkBindImageMemory(device_, result.image, result.memory, 0), "vkBindImageMemory");

	Buffer staging = CreateHostBuffer(texels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	std::memcpy(staging.mapped, texels.data(), texels.size());
	std::vector<VkBufferImageCopy> regions;
	VkDeviceSize offset = 0;
	for (uint32_t level = 0; level < levels; ++level)
	{
		const uint32_t width = kWidth >> level;
		VkBufferImageCopy region {};
		region.bufferOffset     = offset;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, kLayers};
		region.imageExtent      = {width, 1u, 1u};
		regions.push_back(region);
		offset += static_cast<VkDeviceSize>(width) * kLayers * texel_bytes;
	}
	if (offset != texels.size()) { Fail("fixture texel buffer does not match its levels and layers"); }
	Submit([&](VkCommandBuffer command) {
		VkImageMemoryBarrier barrier {};
		barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = result.image;
		barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, levels, 0u, kLayers};
		barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &barrier);
		vkCmdCopyBufferToImage(command, staging.buffer, result.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                       static_cast<uint32_t>(regions.size()), regions.data());
		barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &barrier);
	});
	DestroyBuffer(&staging);

	VkImageViewCreateInfo view {};
	view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image            = result.image;
	view.viewType         = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	view.format           = format;
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, levels, kBaseArray, kViewLayers};
	Check(vkCreateImageView(device_, &view, nullptr, &result.view), "vkCreateImageView");
	return result;
}

void Device::DestroyImage(Image* image)
{
	if (image->view != VK_NULL_HANDLE) { vkDestroyImageView(device_, image->view, nullptr); }
	if (image->image != VK_NULL_HANDLE) { vkDestroyImage(device_, image->image, nullptr); }
	if (image->memory != VK_NULL_HANDLE) { vkFreeMemory(device_, image->memory, nullptr); }
	*image = {};
}

VkSampler Device::CreateSampler(SamplerKind kind)
{
	VkSamplerCreateInfo info {};
	info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter    = kind == SamplerKind::LinearEdgeRowBorder ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.minFilter    = info.magFilter;
	info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.maxLod       = static_cast<float>(kLevels - 1u);
	info.borderColor  = kind == SamplerKind::LinearEdgeRowBorder ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	VkSamplerAddressMode u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSamplerAddressMode v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	switch (kind)
	{
		case SamplerKind::NearestRepeat: u = v = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
		case SamplerKind::NearestBorder: u = v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; break;
		case SamplerKind::NearestEdgeRowBorder:
		case SamplerKind::LinearEdgeRowBorder: v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; break;
		default: break;
	}
	info.addressModeU = u;
	info.addressModeV = v;
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSampler sampler = VK_NULL_HANDLE;
	Check(vkCreateSampler(device_, &info, nullptr, &sampler), "vkCreateSampler");
	return sampler;
}

// The set layout follows the generated module's own binding decorations, so the test binds what
// the production translation declares and nothing else.
uint32_t Device::Dispatch(const Kyty::Vector<uint32_t>& spirv, const ShaderComputeInputInfo& input, uint32_t probe_binding,
                          const std::vector<std::pair<std::string, uint32_t>>& image_bindings, const Image& float_image,
                          const Image& uint_image, VkSampler sampler)
{
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	std::vector<VkWriteDescriptorSet>         writes;
	std::vector<VkDescriptorImageInfo>        image_infos;
	image_infos.reserve(image_bindings.size());
	for (const auto& [variable, binding]: image_bindings)
	{
		VkDescriptorSetLayoutBinding layout {};
		layout.binding         = binding;
		layout.descriptorCount = 1;
		layout.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
		VkDescriptorImageInfo image_info {};
		if (variable == "samplers")
		{
			if (sampler == VK_NULL_HANDLE) { Fail("generated module declares a sampler the case does not use"); }
			layout.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			image_info.sampler    = sampler;
		} else if (variable == "textures2DA_S" || variable == "textures2DA_U")
		{
			layout.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			image_info.imageView   = variable == "textures2DA_S" ? float_image.view : uint_image.view;
			image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		} else
		{
			Fail("generated module declares an unexpected descriptor: " + variable);
		}
		bindings.push_back(layout);
		image_infos.push_back(image_info);
	}
	VkDescriptorSetLayoutBinding output {};
	output.binding         = probe_binding;
	output.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	output.descriptorCount = 1;
	output.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings.push_back(output);

	const uint32_t set = input.bind.descriptor_set_slot;
	std::vector<VkDescriptorSetLayout> set_layouts(set + 1u, VK_NULL_HANDLE);
	for (uint32_t index = 0; index <= set; ++index)
	{
		VkDescriptorSetLayoutCreateInfo layout_info {};
		layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layout_info.bindingCount = index == set ? static_cast<uint32_t>(bindings.size()) : 0u;
		layout_info.pBindings    = index == set ? bindings.data() : nullptr;
		Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &set_layouts[index]), "vkCreateDescriptorSetLayout");
	}
	if (input.bind.vsharp_uniform_buffer) { Fail("descriptor metadata moved to a uniform buffer; the fixture binds push constants only"); }
	VkPushConstantRange push_range {};
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.offset     = input.bind.push_constant_offset;
	push_range.size       = input.bind.push_constant_size;
	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.setLayoutCount         = static_cast<uint32_t>(set_layouts.size());
	pipeline_layout_info.pSetLayouts            = set_layouts.data();
	pipeline_layout_info.pushConstantRangeCount = push_range.size > 0u ? 1u : 0u;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	Check(vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr, &pipeline_layout), "vkCreatePipelineLayout");

	VkShaderModuleCreateInfo module_info {};
	module_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = spirv.Size() * sizeof(uint32_t);
	module_info.pCode    = spirv.GetDataConst();
	VkShaderModule module = VK_NULL_HANDLE;
	Check(vkCreateShaderModule(device_, &module_info, nullptr, &module), "vkCreateShaderModule");
	VkComputePipelineCreateInfo pipeline_info {};
	pipeline_info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName  = "main";
	pipeline_info.layout       = pipeline_layout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	Check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline), "vkCreateComputePipelines");

	std::array<VkDescriptorPoolSize, 3> pool_sizes = {{{VK_DESCRIPTOR_TYPE_SAMPLER, 1u},
	                                                  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2u},
	                                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u}}};
	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
	pool_info.pPoolSizes    = pool_sizes.data();
	VkDescriptorPool pool = VK_NULL_HANDLE;
	Check(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool), "vkCreateDescriptorPool");
	VkDescriptorSetAllocateInfo set_info {};
	set_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool     = pool;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts        = &set_layouts[set];
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
	Check(vkAllocateDescriptorSets(device_, &set_info, &descriptor_set), "vkAllocateDescriptorSets");

	Buffer result = CreateHostBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	VkDescriptorBufferInfo buffer_info {result.buffer, 0, sizeof(uint32_t)};
	for (size_t index = 0; index < image_bindings.size(); ++index)
	{
		VkWriteDescriptorSet write {};
		write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet          = descriptor_set;
		write.dstBinding      = bindings[index].binding;
		write.descriptorCount = 1;
		write.descriptorType  = bindings[index].descriptorType;
		write.pImageInfo      = &image_infos[index];
		writes.push_back(write);
	}
	VkWriteDescriptorSet output_write {};
	output_write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	output_write.dstSet          = descriptor_set;
	output_write.dstBinding      = probe_binding;
	output_write.descriptorCount = 1;
	output_write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	output_write.pBufferInfo     = &buffer_info;
	writes.push_back(output_write);
	vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

	// Every known resource index is a constant in the translation; the metadata words stay zero.
	const std::vector<uint8_t> push_words(input.bind.push_constant_size, 0u);
	Submit([&](VkCommandBuffer command) {
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, set, 1, &descriptor_set, 0, nullptr);
		if (push_range.size > 0u)
		{
			vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, push_range.offset, push_range.size, push_words.data());
		}
		vkCmdDispatch(command, 1, 1, 1);
		VkMemoryBarrier barrier {};
		barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0,
		                     nullptr);
	});
	uint32_t value = 0;
	std::memcpy(&value, result.mapped, sizeof(value));

	DestroyBuffer(&result);
	vkDestroyDescriptorPool(device_, pool, nullptr);
	vkDestroyPipeline(device_, pipeline, nullptr);
	vkDestroyShaderModule(device_, module, nullptr);
	vkDestroyPipelineLayout(device_, pipeline_layout, nullptr);
	for (auto layout: set_layouts) { vkDestroyDescriptorSetLayout(device_, layout, nullptr); }
	return value;
}

// Reads `OpDecorate %<name> Binding <n>` for every descriptor the translation declares.
std::vector<std::pair<std::string, uint32_t>> ImageBindings(const std::string& source)
{
	std::vector<std::pair<std::string, uint32_t>> result;
	const std::string decoration = "OpDecorate %";
	for (size_t position = source.find(decoration); position != std::string::npos; position = source.find(decoration, position + 1u))
	{
		const size_t name_start = position + decoration.size();
		const size_t name_end   = source.find(' ', name_start);
		const size_t line_end   = source.find('\n', name_start);
		if (name_end == std::string::npos || line_end == std::string::npos || name_end > line_end) { continue; }
		const std::string rest = source.substr(name_end + 1u, line_end - name_end - 1u);
		if (rest.compare(0, 8, "Binding ") != 0) { continue; }
		const std::string name = source.substr(name_start, name_end - name_start);
		if (name == "probe_buffer") { continue; }
		result.emplace_back(name, static_cast<uint32_t>(std::stoul(rest.substr(8))));
	}
	return result;
}

std::vector<uint8_t> FloatFixtureTexels()
{
	std::vector<uint8_t> bytes;
	for (uint32_t level = 0; level < kLevels; ++level)
	{
		for (uint32_t layer = 0; layer < kLayers; ++layer)
		{
			for (uint32_t x = 0; x < (kWidth >> level); ++x)
			{
				const std::array<uint16_t, 4> texel = {ToHalf(FloatTexel(level, layer, x)), ToHalf(0.0f), ToHalf(0.0f), ToHalf(1.0f)};
				const auto* raw = reinterpret_cast<const uint8_t*>(texel.data());
				bytes.insert(bytes.end(), raw, raw + sizeof(texel));
			}
		}
	}
	return bytes;
}

std::vector<uint8_t> UnsignedFixtureTexels()
{
	std::vector<uint8_t> bytes;
	for (uint32_t layer = 0; layer < kLayers; ++layer)
	{
		for (uint32_t x = 0; x < kWidth; ++x)
		{
			const uint32_t value = UnsignedTexel(layer, x);
			const auto*    raw   = reinterpret_cast<const uint8_t*>(&value);
			bytes.insert(bytes.end(), raw, raw + sizeof(value));
		}
	}
	return bytes;
}

} // namespace

int main()
{
	InitializeConfig();
	const auto cases = MakeCases();

	struct Prepared
	{
		ShaderComputeInputInfo                         input;
		uint32_t                                       probe_binding = 0;
		std::vector<std::pair<std::string, uint32_t>>  image_bindings;
		Kyty::Vector<uint32_t>                         spirv;
	};
	std::vector<Prepared> prepared(cases.size());
	for (size_t index = 0; index < cases.size(); ++index)
	{
		auto& entry = prepared[index];
		entry.input = MakeInput(cases[index]);
		const auto& bind = entry.input.bind;
		int highest = bind.textures2D.binding_sampled_array_index;
		highest     = std::max(highest, bind.textures2D.binding_sampled_array_uint_index);
		if (bind.samplers.samplers_num > 0) { highest = std::max(highest, bind.samplers.binding_index); }
		entry.probe_binding = static_cast<uint32_t>(highest + 1);
		const std::string source = BuildProbeSource(cases[index], entry.input, entry.probe_binding);
		entry.image_bindings     = ImageBindings(source);
		Kyty::Core::String8 error;
		if (!Kyty::Libs::Graphics::ShaderToolchain::Run(Kyty::Core::String8(source.c_str()), &entry.spirv, &error) || entry.spirv.IsEmpty())
		{
			Fail(std::string(cases[index].name) + ": SPIR-V validation/assembly failed: " + error.c_str());
		}
	}

	Device device;
	device.Initialize();
	Image float_image = device.CreateFixture(VK_FORMAT_R16G16B16A16_SFLOAT, kLevels, FloatFixtureTexels(), 4u * sizeof(uint16_t));
	Image uint_image  = device.CreateFixture(VK_FORMAT_R32_UINT, 1u, UnsignedFixtureTexels(), sizeof(uint32_t));

	for (size_t index = 0; index < cases.size(); ++index)
	{
		const auto& test_case = cases[index];
		const VkSampler sampler = test_case.sampler == SamplerKind::None ? VK_NULL_HANDLE : device.CreateSampler(test_case.sampler);
		const uint32_t actual = device.Dispatch(prepared[index].spirv, prepared[index].input, prepared[index].probe_binding,
		                                        prepared[index].image_bindings, float_image, uint_image, sampler);
		if (sampler != VK_NULL_HANDLE) { device.DestroySampler(sampler); }
		float actual_value   = 0.0f;
		float expected_value = 0.0f;
		std::memcpy(&actual_value, &actual, sizeof(actual));
		std::memcpy(&expected_value, &test_case.expected_bits, sizeof(expected_value));
		const bool matches = test_case.exact_bits ? actual == test_case.expected_bits : std::fabs(actual_value - expected_value) <= 1.0e-3f;
		if (!matches)
		{
			char detail[96];
			std::snprintf(detail, sizeof(detail), ": expected 0x%08x (%g) got 0x%08x (%g)", test_case.expected_bits,
			              static_cast<double>(expected_value), actual, static_cast<double>(actual_value));
			Fail(std::string(test_case.name) + detail);
		}
		std::printf("%s PASS\n", test_case.name);
	}
	device.DestroyImage(&float_image);
	device.DestroyImage(&uint_image);
	std::fflush(stdout);
	return 0;
}
