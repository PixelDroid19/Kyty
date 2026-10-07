#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Gen5TextureVolumeLayout.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Graphics/ShaderStorageImage.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VulkanBlend.h"
#include "Emulator/Graphics/VulkanSampler.h"
#include "Emulator/Graphics/VulkanVertexInputFormat.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

UT_BEGIN(EmulatorGraphicsResources);

using namespace Libs::Graphics;

namespace {

uint32_t g_created_views = 0;

#if defined(_WIN32)
constexpr int kImageRejectedExit = 321;
#else
constexpr int kImageRejectedExit = 65;
#endif

bool CreateTestView(VkDevice, const VulkanImageViewDescriptor&, VkImageView* view)
{
	*view = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(++g_created_views + 100u));
	return true;
}

bool FailTestView(VkDevice, const VulkanImageViewDescriptor&, VkImageView*)
{
	return false;
}

VulkanImage TestImage(VulkanImageType type, VkFormat format = VK_FORMAT_R8G8_UNORM)
{
	VulkanImage image(type);
	image.image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(1u));
	image.format = format;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	image.SetNativeExtent(8u, 16u);
	return image;
}

// Independent Vulkan component-selection oracle applied to the planned view.
float SelectComponent(VkComponentSwizzle selector, const std::array<float, 4>& texel, uint32_t channel)
{
	switch (selector)
	{
		case VK_COMPONENT_SWIZZLE_IDENTITY: return texel[channel];
		case VK_COMPONENT_SWIZZLE_ZERO: return 0.0f;
		case VK_COMPONENT_SWIZZLE_ONE: return 1.0f;
		case VK_COMPONENT_SWIZZLE_R: return texel[0];
		case VK_COMPONENT_SWIZZLE_G: return texel[1];
		case VK_COMPONENT_SWIZZLE_B: return texel[2];
		case VK_COMPONENT_SWIZZLE_A: return texel[3];
		default: ADD_FAILURE(); return -1.0f;
	}
}

void InitializeImageModuleTest()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation;
	Config::Load(validation);
}

void RequireImageModule(bool condition, const char* reason)
{
	if (!condition)
	{
		std::fprintf(stderr, "image numeric control: %s\n", reason);
		std::_Exit(2);
	}
}

uint32_t CountImageModuleToken(const Core::String8& source, const char* token)
{
	uint32_t count = 0;
	const char* next = source.c_str();
	while ((next = std::strstr(next, token)) != nullptr)
	{
		++count;
		next += std::strlen(token);
	}
	return count;
}

struct ImageModuleFixture
{
	ShaderCode code;
	ShaderComputeInputInfo input {};
};

ImageModuleFixture MakeImageModule(uint16_t sampled_format, uint16_t storage_format, bool mip_store = false)
{
	ImageModuleFixture fixture;
	fixture.code.SetType(ShaderType::Compute);
	ShaderInstruction load {};
	load.type = ShaderInstructionType::ImageLoad;
	load.format = ShaderInstructionFormat::Vdata4Vaddr3StDmaskF;
	load.dst = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 4};
	load.src[0] = {.type = ShaderOperandType::Vgpr, .register_id = 4, .size = 3};
	load.src[1] = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 8};
	load.src_num = 2;
	load.mimg_dimension = 1;
	load.mimg_dmask = 15;
	fixture.code.GetInstructions().Add(load);
	ShaderInstruction store = load;
	store.pc = 8;
	store.type = mip_store ? ShaderInstructionType::ImageStoreMip : ShaderInstructionType::ImageStore;
	store.format = mip_store ? ShaderInstructionFormat::Vdata4Vaddr4StDmaskF : ShaderInstructionFormat::Vdata4Vaddr3StDmaskF;
	store.src[0].size = mip_store ? 4 : 3;
	store.src[1].register_id = 8;
	fixture.code.GetInstructions().Add(store);
	ShaderInstruction end {};
	end.pc = 16;
	end.type = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	fixture.code.GetInstructions().Add(end);
	fixture.input.threads_num[0] = 1;
	fixture.input.threads_num[1] = 1;
	fixture.input.threads_num[2] = 1;
	fixture.input.bind.push_constant_size = 64;
	auto& textures = fixture.input.bind.textures2D;
	textures.textures_num = 2;
	textures.textures2d_sampled_num = 1;
	textures.textures2d_sampled_uint_num = VulkanGen5ImageNumericType(sampled_format) == GuestImageNumericType::UnsignedInteger ? 1 : 0;
	textures.textures2d_storage_num = 1;
	textures.binding_sampled_uint_index = 4; // production's separate, sparse uint sampled bank
	textures.binding_storage_index = 1;
	for (int i = 0; i < 2; ++i)
	{
		auto& desc = textures.desc[i];
		desc.start_register = i * 8;
		desc.slot = i;
		desc.usage = i == 0 ? ShaderTextureUsage::ReadOnly : ShaderTextureUsage::ReadWrite;
		desc.textures2d_without_sampler = i == 1;
		desc.texture.fields[0] = 0x100u * (i + 1u); // distinct, synthetic complete descriptor identities
		desc.texture.fields[1] = static_cast<uint32_t>(i == 0 ? sampled_format : storage_format) << 20u;
		desc.texture.fields[3] = (9u << 28u) | DstSel(4u, 5u, 6u, 7u);
	}
	return fixture;
}

Core::String8 EmitValidatedImageModule(const ImageModuleFixture& fixture)
{
	const auto source = SpirvGenerateSource(fixture.code, nullptr, nullptr, &fixture.input);
	RequireImageModule(!source.IsEmpty(), "empty production module");
	RequireImageModule(Config::ShaderValidationEnabled(), "SPIR-V validation must be enabled");
	Vector<uint32_t> binary;
	Core::String8 error;
	const bool compiled = ShaderToolchain::Run(source, &binary, &error);
	RequireImageModule(compiled, error.c_str());
	RequireImageModule(!binary.IsEmpty(), "empty validated SPIR-V binary");
	return source;
}

void AddWritableImage(ImageModuleFixture* fixture, uint16_t format)
{
	auto& textures = fixture->input.bind.textures2D;
	const int index = textures.textures_num++;
	++textures.textures2d_storage_num;
	auto& descriptor = textures.desc[index];
	descriptor = textures.desc[1];
	descriptor.start_register = index * 8;
	descriptor.slot = index;
	descriptor.texture.fields[0] = 0x100u * (index + 1u);
	descriptor.texture.fields[1] = static_cast<uint32_t>(format) << 20u;
	fixture->input.bind.push_constant_size += 32;
}

ImageModuleFixture MakeAtomicImageModule(bool mip_store = false, uint16_t store_format = 71u)
{
	auto fixture = MakeImageModule(20u, store_format, mip_store);
	AddWritableImage(&fixture, 20u);
	ShaderInstruction atomic {};
	atomic.pc = 16;
	atomic.type = ShaderInstructionType::ImageAtomicAdd;
	atomic.format = ShaderInstructionFormat::Vdata1Vaddr2StVsrc2Dmask1;
	atomic.dst = {.type = ShaderOperandType::Vgpr, .register_id = 8, .size = 1};
	atomic.src[0] = {.type = ShaderOperandType::Vgpr, .register_id = 4, .size = 2};
	atomic.src[1] = {.type = ShaderOperandType::Sgpr, .register_id = 16, .size = 8};
	atomic.src[2] = atomic.dst;
	atomic.src_num = 3;
	atomic.mimg_dimension = 1;
	atomic.mimg_dmask = 1;
	atomic.mimg_return_old_value = true;
	auto end = fixture.code.GetInstructions().At(2);
	end.pc = 24;
	fixture.code.GetInstructions()[2] = atomic;
	fixture.code.GetInstructions().Add(end);
	return fixture;
}

ImageModuleFixture MakeSampledShapesImageModule(uint32_t shapes, ShaderInstructionType operation)
{
	auto fixture = MakeImageModule(20u, 71u);
	auto& textures = fixture.input.bind.textures2D;
	const auto sampled = textures.desc[0];
	textures.textures2d_sampled_num = 0;
	textures.textures2d_sampled_uint_num = 0;
	textures.binding_sampled_array_index = 2;
	textures.binding_sampled_3d_index = 3;
	textures.binding_sampled_array_uint_index = 5;
	textures.binding_sampled_3d_uint_index = 6;
	bool first = true;
	for (uint32_t shape = 0; shape < 3; ++shape)
	{
		if ((shapes & (1u << shape)) == 0u) { continue; }
		const int descriptor_index = first ? 0 : textures.textures_num++;
		auto& descriptor = textures.desc[descriptor_index];
		descriptor = sampled;
		descriptor.start_register = descriptor_index * 8;
		descriptor.slot = descriptor_index;
		descriptor.texture.fields[0] = 0x100u * (descriptor_index + 1u);
		descriptor.texture.fields[3] = ((shape == 0u ? 9u : shape == 1u ? 13u : 10u) << 28u) | DstSel(4u, 5u, 6u, 7u);
		if (first) { fixture.code.GetInstructions()[0].mimg_dimension = shape == 0u ? 1u : shape == 1u ? 5u : 2u; }
		first = false;
		if (shape == 0u) { textures.textures2d_sampled_num = textures.textures2d_sampled_uint_num = 1; }
		if (shape == 1u) { textures.textures2d_array_sampled_num = textures.textures2d_array_sampled_uint_num = 1; }
		if (shape == 2u) { textures.textures3d_sampled_num = textures.textures3d_sampled_uint_num = 1; }
	}
	fixture.input.bind.push_constant_size = 32u * textures.textures_num;
	auto& instruction = fixture.code.GetInstructions()[0];
	instruction.type = operation;
	if (operation == ShaderInstructionType::ImageGetResinfo)
	{
		instruction.format = ShaderInstructionFormat::VdataVaddrStDmask;
		instruction.src[0].size = 1;
	} else if (operation == ShaderInstructionType::ImageGather4)
	{
		instruction.format = ShaderInstructionFormat::Vdata4Vaddr3StSsMimgDmask;
		instruction.mimg_dmask = 1u;
		instruction.src[2] = {.type = ShaderOperandType::Sgpr, .register_id = textures.textures_num * 8, .size = 4};
		instruction.src_num = 3;
		fixture.input.bind.samplers.samplers_num = 1;
		fixture.input.bind.samplers.binding_index = 7;
		fixture.input.bind.samplers.start_register[0] = instruction.src[2].register_id;
		fixture.input.bind.push_constant_size += 16;
	}
	return fixture;
}

} // namespace

TEST(EmulatorGraphicsResources, EmitsAndValidatesUintSampledWithFloatStorage)
{
	// Production red control: the original non-atomic classifier incorrectly
	// selected uint/R32ui from the read-only descriptor. A valid module alone
	// cannot prove that its storage declaration agrees with the bound VkFormat.
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    const auto source = EmitValidatedImageModule(MakeImageModule(20u, 71u));
		    RequireImageModule(source.ContainsStr("%ImageL = OpTypeImage %float 2D 0 0 0 2 Unknown"), "float writable declaration");
		    RequireImageModule(source.ContainsStr("OpImageFetch %v4uint"), "unsigned sampled fetch");
		    RequireImageModule(source.ContainsStr("%t88_1 = OpCompositeConstruct %v4float"), "float writable texel");
		    RequireImageModule(source.ContainsStr("OpCapability StorageImageWriteWithoutFormat"), "formatless write capability");
		    RequireImageModule(CountImageModuleToken(source, "OpTypeImage %uint 2D 0 0 0 1 Unknown") == 1u, "one uint sampled type");
		    RequireImageModule(CountImageModuleToken(source, "OpTypeSampledImage %ImageS\n") == 1u, "one sampled-image wrapper");
		    RequireImageModule(source.ContainsStr("OpDecorate %textures2D_U Binding 4"), "uint sampled binding remains distinct");
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, EmitsAndValidatesFloatSampledWithUintStorage)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    const auto source = EmitValidatedImageModule(MakeImageModule(71u, 20u));
		    RequireImageModule(source.ContainsStr("%ImageL = OpTypeImage %uint 2D 0 0 0 2 R32ui"), "R32 writable declaration");
		    RequireImageModule(source.ContainsStr("%t88_1 = OpCompositeConstruct %v4uint"), "unsigned writable texel");
		    RequireImageModule(!source.ContainsStr("OpCapability StorageImageWriteWithoutFormat"), "typed write needs no formatless capability");
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, EmitsAndValidatesExactHomogeneousUintStorageFormats)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    for (uint16_t format: {5u, 20u, 62u, 75u})
		    {
			    const auto source = EmitValidatedImageModule(MakeImageModule(71u, format));
			    const char* token = format == 5u ? "R8ui" : format == 20u ? "R32ui" : format == 62u ? "Rg32ui" : "Rgba32ui";
			    const auto declaration = Core::String8::FromPrintf("%%ImageL = OpTypeImage %%uint 2D 0 0 0 2 %s", token);
			    RequireImageModule(source.ContainsStr(declaration), "exact typed storage format");
			    RequireImageModule(source.ContainsStr("OpCapability StorageImageExtendedFormats") == (format == 5u || format == 62u),
			                       "extended format capability agrees with SPIR-V format");
			    RequireImageModule(source.ContainsStr("%image_store_component_1_0 = OpBitcast %uint"), "store preserves uint bits");
			    auto array_fixture = MakeImageModule(71u, format);
			    array_fixture.input.bind.textures2D.desc[1].texture.fields[3] = (13u << 28u) | DstSel(4u, 5u, 6u, 7u);
			    const auto array_source = EmitValidatedImageModule(array_fixture);
			    const auto array_declaration = Core::String8::FromPrintf("%%ImageL = OpTypeImage %%uint 2D 0 1 0 2 %s", token);
			    RequireImageModule(array_source.ContainsStr(array_declaration), "array storage keeps exact unsigned format");
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

ImageModuleFixture MakeVolumeStorageModule(uint16_t storage_format)
{
	auto fixture = MakeImageModule(20u, storage_format);
	fixture.input.bind.textures2D.desc[1].texture.fields[3] = (10u << 28u) | DstSel(4u, 5u, 6u, 7u);
	fixture.code.GetInstructions()[1].mimg_dimension = 2; // MIMG DIM 3D: three coordinates
	return fixture;
}

TEST(EmulatorGraphicsResources, PlansAWritableVolumeBankAsItsOwnShape)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    for (uint16_t format: {71u, 5u, 20u, 62u, 75u})
		    {
			    auto fixture = MakeVolumeStorageModule(format);
			    const auto plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
			    RequireImageModule(plan.supported && plan.three_dimensional, "a homogeneous 3D writable bank is supported");
		    }
		    auto flat = MakeImageModule(20u, 71u);
		    RequireImageModule(!ShaderPlanStorageImages(flat.code, &flat.input.bind).three_dimensional, "a 2D bank is not a volume");
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, EmitsAndValidatesVolumeStorageStores)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    for (uint16_t format: {71u, 5u, 20u, 62u, 75u})
		    {
			    const auto source = EmitValidatedImageModule(MakeVolumeStorageModule(format));
			    const bool is_float = format == 71u;
			    const char* token = format == 5u ? "R8ui" : format == 20u ? "R32ui" : format == 62u ? "Rg32ui" : format == 75u ? "Rgba32ui" : "Unknown";
			    const auto declaration = Core::String8::FromPrintf("%%ImageL = OpTypeImage %%%s 3D 0 0 0 2 %s", is_float ? "float" : "uint", token);
			    RequireImageModule(source.ContainsStr(declaration), "volume storage declaration");
			    RequireImageModule(source.ContainsStr("OpImageQuerySize %v3int"), "three-component extent");
			    RequireImageModule(source.ContainsStr("OpImageWrite %t27_1 %t73_1 %t88_1"), "the store writes the volume");
			    RequireImageModule(source.ContainsStr("%t73_1 = OpCompositeConstruct %v3uint"), "three-component coordinate");
			    RequireImageModule(source.ContainsStr("%image_store_in_bounds_1_2"), "all three coordinates are bounds checked");
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, RejectsAVolumeStoreThatUsesATwoDimensionalInstruction)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    auto fixture = MakeVolumeStorageModule(71u);
		    fixture.code.GetInstructions()[1].mimg_dimension = 1; // 2D coordinates cannot address a 3D descriptor
		    (void)SpirvGenerateSource(fixture.code, nullptr, nullptr, &fixture.input);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(kImageRejectedExit), "");
}

TEST(EmulatorGraphicsResources, EmitsAndValidatesTypedMipStores)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    for (uint16_t format: {5u, 20u, 62u, 75u, 71u})
		    {
			    const auto source = EmitValidatedImageModule(MakeImageModule(20u, format, true));
			    RequireImageModule(source.ContainsStr(format == 71u ? "%t88_1 = OpCopyObject %v4float" : "%t88_1 = OpBitcast %v4uint"),
			                       "mip store texel agrees with writable descriptor");
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, PreservesFloatStorageAndR32AtomicAliases)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    for (bool mip_store: {false, true})
		    {
			    const auto source = EmitValidatedImageModule(MakeAtomicImageModule(mip_store));
			    RequireImageModule(source.ContainsStr("%ImageL = OpTypeImage %float 2D 0 0 0 2 Unknown"), "float primary bank");
			    RequireImageModule(source.ContainsStr("%ImageLU = OpTypeImage %uint 2D 0 0 0 2 R32ui"), "R32 atomic alias");
			    RequireImageModule(source.ContainsStr("OpAtomicIAdd %uint"), "actual integer atomic");
			    RequireImageModule(source.ContainsStr("%t27_1 = OpLoad %ImageL"), "float store uses primary bank");
			    const auto uint_source = EmitValidatedImageModule(MakeAtomicImageModule(mip_store, 20u));
			    RequireImageModule(uint_source.ContainsStr("%t27_1 = OpLoad %ImageLU"), "uint store uses atomic bank");
			    RequireImageModule(CountImageModuleToken(uint_source, "OpTypeImage %uint 2D 0 0 0 2 R32ui") == 1u,
			                       "identical storage and atomic views share one image type");
			    RequireImageModule(uint_source.ContainsStr("%_arr_ImageL_uint_2 = OpTypeArray %ImageLU") &&
			                       uint_source.ContainsStr("%_arr_ImageLU_uint_2 = OpTypeArray %ImageLU"),
			                       "both storage arrays reference the shared type");
			    RequireImageModule(uint_source.ContainsStr("OpDecorate %textures2D_L Binding 1") &&
			                       uint_source.ContainsStr("OpDecorate %textures2D_LU Binding 1"), "storage alias binding is preserved");
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, ReusesUintSampledTypesForLoadsAndQueriesAcrossShapes)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    for (uint32_t shapes = 1u; shapes < 8u; ++shapes)
		    {
			    for (auto operation: {ShaderInstructionType::ImageLoad, ShaderInstructionType::ImageGetResinfo})
			    {
				    const auto source = EmitValidatedImageModule(MakeSampledShapesImageModule(shapes, operation));
				    RequireImageModule(CountImageModuleToken(source, "OpTypeImage %uint 2D 0 0 0 1 Unknown") == ((shapes & 1u) != 0u),
				                       "one flat uint sampled type");
				    RequireImageModule(CountImageModuleToken(source, "OpTypeImage %uint 2D 0 1 0 1 Unknown") == ((shapes & 2u) != 0u),
				                       "one array uint sampled type");
				    RequireImageModule(CountImageModuleToken(source, "OpTypeImage %uint 3D 0 0 0 1 Unknown") == ((shapes & 4u) != 0u),
				                       "one volume uint sampled type");
				    RequireImageModule(source.ContainsStr("%ImageL = OpTypeImage %float 2D 0 0 0 2 Unknown"),
				                       "sampled shapes cannot change floating storage");
			    }
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, ReusesUintGatherTypesAndPreservesResultIdentity)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    for (uint32_t shapes = 1u; shapes < 4u; ++shapes)
		    {
			    const auto source = EmitValidatedImageModule(MakeSampledShapesImageModule(shapes, ShaderInstructionType::ImageGather4));
			    RequireImageModule(source.ContainsStr("OpImageGather %v4uint"), "integer gather result");
			    RequireImageModule(CountImageModuleToken(source, "OpTypeSampledImage %ImageS\n") == ((shapes & 1u) != 0u),
			                       "flat gather shares primary sampled type");
			    RequireImageModule(CountImageModuleToken(source, "OpTypeSampledImage %ImageSA\n") == ((shapes & 2u) != 0u),
			                       "array gather shares primary sampled type");
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, KeepsMixedSampledNumericTypesDistinct)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    for (auto operation: {ShaderInstructionType::ImageLoad, ShaderInstructionType::ImageGetResinfo})
		    {
			    auto fixture = MakeSampledShapesImageModule(1u, operation);
			    auto& textures = fixture.input.bind.textures2D;
			    textures.desc[2] = textures.desc[0];
			    textures.desc[2].start_register = 16;
			    textures.desc[2].slot = 2;
			    textures.desc[2].texture.fields[0] = 0x300u;
			    textures.desc[2].texture.fields[1] = 71u << 20u;
			    textures.textures_num = 3;
			    textures.textures2d_sampled_num = 2;
			    fixture.input.bind.push_constant_size = 96;
			    const auto source = EmitValidatedImageModule(fixture);
			    RequireImageModule(source.ContainsStr("%ImageS = OpTypeImage %float 2D 0 0 0 1 Unknown"), "floating sampled type");
			    RequireImageModule(source.ContainsStr("%ImageU = OpTypeImage %uint 2D 0 0 0 1 Unknown"), "distinct uint sampled type");
			    RequireImageModule(source.ContainsStr("%SampledImageU = OpTypeSampledImage %ImageU"), "distinct uint sampled wrapper");
			    RequireImageModule(source.ContainsStr("%ImageL = OpTypeImage %float 2D 0 0 0 2 Unknown"), "floating writable type");
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, WritableNumericAdmissionIgnoresSampledDescriptors)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    auto fixture = MakeImageModule(20u, 71u);
		    auto& textures = fixture.input.bind.textures2D;
		    for (uint16_t sampled_format: {5u, 20u, 62u, 75u, 71u})
		    {
			    textures.desc[0].texture.fields[1] = static_cast<uint32_t>(sampled_format) << 20u;
			    const auto plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
			    RequireImageModule(plan.supported && plan.formatless && !plan.unsigned_primary, "sampled type cannot change writable class");
		    }
		    AddWritableImage(&fixture, 20u);
		    auto plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
		    RequireImageModule(!plan.supported && std::strstr(plan.reason, "mixed float and uint") != nullptr &&
		                       plan.descriptor_index == 2 && plan.guest_format == 20u, "precise mixed numeric rejection");
		    textures.desc[1].texture.fields[1] = 5u << 20u;
		    plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
		    RequireImageModule(!plan.supported && std::strstr(plan.reason, "mixed writable uint formats") != nullptr,
		                       "R8ui and R32ui cannot share a declaration");
		    textures.desc[1].texture.fields[1] = 70u << 20u;
		    plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
		    RequireImageModule(!plan.supported && std::strstr(plan.reason, "signed writable") != nullptr, "signed bank is unsupported");
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, RejectsMixedWritableNumericDomainsBeforeModuleEmission)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    auto fixture = MakeImageModule(20u, 71u);
		    AddWritableImage(&fixture, 20u);
		    (void)SpirvGenerateSource(fixture.code, nullptr, nullptr, &fixture.input);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(kImageRejectedExit), "");
}

TEST(EmulatorGraphicsResources, RejectsMixedUintFormatsBeforeModuleEmission)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    auto fixture = MakeImageModule(20u, 5u);
		    AddWritableImage(&fixture, 20u);
		    (void)SpirvGenerateSource(fixture.code, nullptr, nullptr, &fixture.input);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(kImageRejectedExit), "");
}

TEST(EmulatorGraphicsResources, RejectsStorageShapesAndAtomicFormatsWithoutInventingTypes)
{
	ASSERT_EXIT(
	    {
		    InitializeImageModuleTest();
		    auto fixture = MakeAtomicImageModule();
		    fixture.input.bind.textures2D.desc[2].texture.fields[1] = 5u << 20u;
		    auto plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
		    RequireImageModule(!plan.supported && std::strstr(plan.reason, "atomic uint alias") != nullptr, "R8 atomic must be rejected");
		    fixture = MakeImageModule(20u, 71u);
		    AddWritableImage(&fixture, 71u);
		    fixture.input.bind.textures2D.desc[2].texture.fields[3] = 13u << 28u;
		    plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
		    RequireImageModule(!plan.supported && std::strstr(plan.reason, "separate shape banks") != nullptr, "mixed shape rejection");
		    fixture = MakeImageModule(20u, 71u);
		    AddWritableImage(&fixture, 71u);
		    fixture.input.bind.textures2D.desc[2].texture.fields[3] = 10u << 28u;
		    plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
		    RequireImageModule(!plan.supported && std::strstr(plan.reason, "separate shape banks") != nullptr, "2D and 3D are separate banks");
		    fixture = MakeAtomicImageModule();
		    fixture.input.bind.textures2D.desc[1].texture.fields[3] = 10u << 28u;
		    plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
		    RequireImageModule(!plan.supported, "the 2D atomic alias cannot address a volume bank");
		    fixture = MakeImageModule(20u, 71u, true);
		    fixture.input.bind.textures2D.desc[1].texture.fields[3] = 10u << 28u;
		    plan = ShaderPlanStorageImages(fixture.code, &fixture.input.bind);
		    RequireImageModule(!plan.supported && std::strstr(plan.reason, "packed-mip store") != nullptr, "packed-mip stores are 2D only");
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsResources, BlendDevicePlanEnablesOnlyRequestedSupportedFeatures)
{
	for (uint32_t supported_mask = 0; supported_mask < 4; ++supported_mask)
	{
		VkPhysicalDeviceFeatures supported {};
		supported.independentBlend = (supported_mask & 1u) != 0u;
		supported.dualSrcBlend = (supported_mask & 2u) != 0u;
		for (uint32_t requested = 0; requested < 4; ++requested)
		{
			const auto enabled = VulkanPlanBlendFeatures(supported, (requested & 1u) != 0u, (requested & 2u) != 0u);
			EXPECT_EQ(enabled.independent_blend != VK_FALSE, (supported_mask & requested & 1u) != 0u);
			EXPECT_EQ(enabled.dual_source_blend != VK_FALSE, (supported_mask & requested & 2u) != 0u);
		}
	}
}

TEST(EmulatorGraphicsResources, IndependentMrtMasksRequireEnabledFeatureWithoutFlattening)
{
	VulkanBlendCapabilities capabilities {};
	capabilities.max_color_attachments = 8u;
	VkPipelineColorBlendAttachmentState attachments[2] {};
	attachments[0].colorWriteMask = 1u;
	attachments[1].colorWriteMask = 15u;
	EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, attachments, 2u, 0u),
	          VulkanBlendAdmission::IndependentBlendNotEnabled);
	capabilities.enabled.independent_blend = VK_TRUE;
	EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, attachments, 2u, 0u), VulkanBlendAdmission::Supported);
	EXPECT_EQ(attachments[0].colorWriteMask, 1u);
	EXPECT_EQ(attachments[1].colorWriteMask, 15u);
	capabilities.enabled.independent_blend = VK_FALSE;
	attachments[1] = attachments[0];
	EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, attachments, 2u, 0u), VulkanBlendAdmission::Supported);
	EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, attachments, 1u, 0u), VulkanBlendAdmission::Supported);
	EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, nullptr, 0u, 0u), VulkanBlendAdmission::Supported);
	capabilities.max_color_attachments = 1u;
	EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, attachments, 2u, 0u), VulkanBlendAdmission::InvalidAttachments);
}

TEST(EmulatorGraphicsResources, DualSourceChecksAllFactorsLimitsAndSecondaryExports)
{
	for (auto factor: {VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
	                   VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA})
	{
		for (uint32_t component = 0u; component < 4u; ++component)
		{
			VulkanBlendCapabilities capabilities {};
			capabilities.max_color_attachments = 8u;
			VkPipelineColorBlendAttachmentState attachment {};
			attachment.blendEnable = VK_TRUE;
			VkBlendFactor* fields[] = {&attachment.srcColorBlendFactor, &attachment.dstColorBlendFactor,
			                          &attachment.srcAlphaBlendFactor, &attachment.dstAlphaBlendFactor};
			*fields[component] = factor;
			EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, &attachment, 1u, 1u),
			          VulkanBlendAdmission::DualSourceBlendNotEnabled);
			capabilities.enabled.dual_source_blend = VK_TRUE;
			EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, &attachment, 1u, 1u),
			          VulkanBlendAdmission::DualSourceAttachmentLimit);
			capabilities.max_dual_source_attachments = 1u;
			EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, &attachment, 1u, 0u),
			          VulkanBlendAdmission::MissingSecondaryOutput);
			EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, &attachment, 1u, 1u), VulkanBlendAdmission::Supported);
		}
	}
}

TEST(EmulatorGraphicsResources, DualSourceLimitTracksUsedOutputsRatherThanUnusedAttachments)
{
	VulkanBlendCapabilities capabilities {{VK_TRUE, VK_TRUE}, 8u, 1u};
	VkPipelineColorBlendAttachmentState attachments[2] {};
	attachments[0].blendEnable = VK_TRUE;
	attachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
	EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, attachments, 2u, 1u, 1u), VulkanBlendAdmission::Supported);
	EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, attachments, 2u, 1u, 3u),
	          VulkanBlendAdmission::DualSourceAttachmentLimit);
	attachments[0].blendEnable = VK_FALSE;
	EXPECT_EQ(VulkanValidateBlendAttachments(capabilities, attachments, 2u, 0u, 3u), VulkanBlendAdmission::Supported);
}

TEST(EmulatorGraphicsResources, RawFormat5PreservesUnsignedByteValuesAndAliasType)
{
	ShaderTextureResource descriptor {};
	descriptor.fields[1] = 5u << 20u; // T# FORMAT [60:52], not a catalog ID.
	ASSERT_EQ(descriptor.Format(), 5u);
	const auto format = VulkanResolveGuestImageFormat(GuestImageUsage::Sampled, 0u, 0u, descriptor.Format());
	EXPECT_EQ(format, VK_FORMAT_R8_UINT);
	EXPECT_EQ(VulkanGen5ImageNumericType(descriptor.Format()), GuestImageNumericType::UnsignedInteger);
	EXPECT_TRUE(VulkanGen5SampleFormatMatches(5u, VK_FORMAT_R8_UINT));
	EXPECT_FALSE(VulkanGen5SampleFormatMatches(5u, VK_FORMAT_R8_UNORM));
	EXPECT_EQ(ShaderGen5TextureBytesPerElement(5u), 1u);
	// Host UINT values are not UNORM fractions. A point/fetch oracle avoids
	// making any assumption about linear filtering support for integer images.
	for (uint8_t byte: {uint8_t(0), uint8_t(127), uint8_t(255)})
	{
		const float sampled = format == VK_FORMAT_R8_UINT ? static_cast<float>(byte) : static_cast<float>(byte) / 255.0f;
		EXPECT_EQ(sampled, static_cast<float>(byte));
	}
	// Storage admission is paired with the exact R8ui emission regression above.
	EXPECT_TRUE(VulkanSupportsGen5ImageFormat(GuestImageUsage::Storage, 5u));
	EXPECT_EQ(VulkanResolveGuestImageFormat(GuestImageUsage::Storage, 0u, 0u, 5u), VK_FORMAT_R8_UINT);
}

TEST(EmulatorGraphicsResources, RawRgb565AndHistoricalBc1HaveSeparateNamespaces)
{
	ShaderTextureResource descriptor {};
	descriptor.fields[1] = 133u << 20u;
	ASSERT_EQ(descriptor.Format(), 133u);
	const auto raw_format = VulkanResolveGuestImageFormat(GuestImageUsage::Sampled, 0u, 0u, descriptor.Format());
	EXPECT_EQ(raw_format, VK_FORMAT_B5G6R5_UNORM_PACK16);
	EXPECT_EQ(ShaderGen5TextureBytesPerElement(descriptor.Format()), 2u);
	EXPECT_FALSE(ShaderGen5TextureIsBlockCompressed(descriptor.Format()));
	EXPECT_FALSE(Gen5IsBc1PackageFormat(descriptor.Format()));
	EXPECT_FALSE(VulkanGen5SampleFormatMatches(133u, VK_FORMAT_BC1_RGBA_UNORM_BLOCK));
	EXPECT_FALSE(VulkanSupportsGen5ImageFormat(GuestImageUsage::Storage, descriptor.Format()));
	// AMD's first channel is the low field. These packed words are pure red,
	// green and blue, respectively; Vulkan R5G6B5 would reverse red and blue.
	const uint16_t words[] = {0x001fu, 0x07e0u, 0xf800u};
	for (uint32_t channel = 0u; channel < 3u; ++channel)
	{
		const uint32_t red_shift = raw_format == VK_FORMAT_B5G6R5_UNORM_PACK16 ? 0u : 11u;
		const uint32_t blue_shift = raw_format == VK_FORMAT_B5G6R5_UNORM_PACK16 ? 11u : 0u;
		EXPECT_EQ((words[channel] >> red_shift) & 31u, channel == 0u ? 31u : 0u);
		EXPECT_EQ((words[channel] >> 5u) & 63u, channel == 1u ? 63u : 0u);
		EXPECT_EQ((words[channel] >> blue_shift) & 31u, channel == 2u ? 31u : 0u);
	}
	const auto catalog = Gen5ImageFormatFromCatalog(Gen5CatalogImageFormat::Bc1Unorm);
	EXPECT_EQ(catalog, 169u);
	EXPECT_EQ(ShaderGen5TextureBytesPerElement(catalog), 8u);
	EXPECT_TRUE(ShaderGen5TextureIsBlockCompressed(catalog));
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(27u, catalog, false));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, catalog, true));
	EXPECT_EQ(VulkanResolveGuestImageFormat(GuestImageUsage::Sampled, 0u, 0u, catalog), VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
	EXPECT_EQ(VulkanResolveGuestImageFormat(GuestImageUsage::Sampled, 0u, 0u, catalog, true), VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
	EXPECT_TRUE(Gen5IsBc1PackageFormat(170u));
	EXPECT_EQ(VulkanResolveGuestImageFormat(GuestImageUsage::Sampled, 10u, 0u, 0u), VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(VulkanResolveGen5VertexInputFormat(66u).format, VK_FORMAT_R16G16B16A16_SNORM);
}

TEST(EmulatorGraphicsResources, SampledComponentViewsAgreeAcrossUploadRenderAndStorageBackings)
{
	for (auto type: {VulkanImageType::Texture, VulkanImageType::RenderTexture, VulkanImageType::StorageTexture})
	{
		auto image = TestImage(type);
		VulkanImageViewDescriptor view {};
		ASSERT_TRUE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
		                                     0u, 1u, 0u, 1u, DstSel(5, 4, 0, 1), &view));
		const std::array<float, 4> rg = {64.0f / 255.0f, 192.0f / 255.0f, 0.0f, 1.0f};
		EXPECT_FLOAT_EQ(SelectComponent(view.components.r, rg, 0u), 192.0f / 255.0f);
		EXPECT_FLOAT_EQ(SelectComponent(view.components.g, rg, 1u), 64.0f / 255.0f);
		EXPECT_FLOAT_EQ(SelectComponent(view.components.b, rg, 2u), 0.0f);
		EXPECT_FLOAT_EQ(SelectComponent(view.components.a, rg, 3u), 1.0f);
	}
}

TEST(EmulatorGraphicsResources, SimultaneousSampledViewsDoNotChangeStorageWriteView)
{
	auto image = TestImage(VulkanImageType::StorageTexture);
	const auto storage_view = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(10u));
	image.image_view[VulkanImage::VIEW_DEFAULT] = storage_view;
	VulkanImageViewDescriptor identity {}, swapped {};
	ASSERT_TRUE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
	                                     0u, 1u, 0u, 1u, DstSel(4, 5, 6, 7), &identity));
	ASSERT_TRUE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
	                                     0u, 1u, 0u, 1u, DstSel(5, 4, 0, 1), &swapped));
	g_created_views = 0u;
	const int first = VulkanGetOrCreateSampledImageView(VK_NULL_HANDLE, &image, identity, CreateTestView);
	const int second = VulkanGetOrCreateSampledImageView(VK_NULL_HANDLE, &image, swapped, CreateTestView);
	ASSERT_GE(first, VulkanImage::VIEW_MAX);
	ASSERT_GE(second, VulkanImage::VIEW_MAX);
	EXPECT_NE(first, second);
	EXPECT_NE(image.image_view[first], image.image_view[second]);
	EXPECT_EQ(VulkanGetOrCreateSampledImageView(VK_NULL_HANDLE, &image, identity, CreateTestView), first);
	EXPECT_EQ(g_created_views, 2u);
	int storage_index = -1;
	ASSERT_TRUE(VulkanResolveStorageImageView(&image, false, false, &storage_index));
	EXPECT_EQ(image.image_view[storage_index], storage_view);
}

TEST(EmulatorGraphicsResources, SampledViewIdentityIncludesSubresourcesShapeFormatAndAspect)
{
	auto image = TestImage(VulkanImageType::StorageTexture, VK_FORMAT_R8G8B8A8_SRGB);
	image.mip_levels = 4u;
	image.array_layers = 6u;
	VulkanImageViewDescriptor first {}, second {};
	ASSERT_TRUE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT,
	                                     1u, 2u, 2u, 3u, DstSel(6, 5, 4, 7), &first));
	const auto info = VulkanBuildImageViewCreateInfo(first);
	EXPECT_EQ(info.format, VK_FORMAT_R8G8B8A8_SRGB);
	EXPECT_EQ(info.subresourceRange.baseMipLevel, 1u);
	EXPECT_EQ(info.subresourceRange.levelCount, 2u);
	EXPECT_EQ(info.subresourceRange.baseArrayLayer, 2u);
	EXPECT_EQ(info.subresourceRange.layerCount, 3u);
	second = first;
	EXPECT_TRUE(VulkanImageViewDescriptorsEqual(first, second));
	second.base_mip_level = 0u;
	EXPECT_FALSE(VulkanImageViewDescriptorsEqual(first, second));
	second = first;
	second.base_array_layer = 0u;
	EXPECT_FALSE(VulkanImageViewDescriptorsEqual(first, second));
	second = first;
	second.format = VK_FORMAT_R8G8B8A8_UNORM;
	EXPECT_FALSE(VulkanImageViewDescriptorsEqual(first, second));
	EXPECT_EQ(VulkanGetOrCreateSampledImageView(VK_NULL_HANDLE, &image, second, CreateTestView), -1);
	EXPECT_FALSE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_ASPECT_COLOR_BIT,
	                                      0u, 1u, 0u, 1u, DstSel(4, 5, 6, 7), &second));
	EXPECT_FALSE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT,
	                                      3u, 2u, 0u, 1u, DstSel(4, 5, 6, 7), &second));
	EXPECT_FALSE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT,
	                                      0u, 1u, 5u, 2u, DstSel(4, 5, 6, 7), &second));
	EXPECT_FALSE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
	                                      0u, 1u, 0u, 1u, DstSel(2, 5, 6, 7), &second));
	image.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
	ASSERT_TRUE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT,
	                                     0u, 1u, 0u, 1u, DstSel(4, 4, 4, 4), &first));
	ASSERT_TRUE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_STENCIL_BIT,
	                                     0u, 1u, 0u, 1u, DstSel(4, 4, 4, 4), &second));
	EXPECT_FALSE(VulkanImageViewDescriptorsEqual(first, second));
}

TEST(EmulatorGraphicsResources, SampledViewCreationFailureDoesNotPublishAnIndex)
{
	auto image = TestImage(VulkanImageType::RenderTexture);
	VulkanImageViewDescriptor view {};
	ASSERT_TRUE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
	                                     0u, 1u, 0u, 1u, DstSel(7, 6, 5, 4), &view));
	EXPECT_EQ(VulkanGetOrCreateSampledImageView(VK_NULL_HANDLE, &image, view, FailTestView), -1);
	EXPECT_TRUE(image.sampled_view_descriptors.empty());
	EXPECT_EQ(image.image_view.size(), static_cast<size_t>(VulkanImage::VIEW_MAX));
}

TEST(EmulatorGraphicsResources, SampledViewCacheIsBoundedAndKeepsExistingIndices)
{
	auto image = TestImage(VulkanImageType::StorageTexture, VK_FORMAT_R32_UINT);
	constexpr uint32_t selectors[] = {0u, 1u, 4u, 5u, 6u, 7u};
	VulkanImageViewDescriptor first {};
	for (uint32_t i = 0u; i <= VulkanImage::VIEW_CACHE_LIMIT - VulkanImage::VIEW_MAX; ++i)
	{
		VulkanImageViewDescriptor view {};
		const uint32_t swizzle = DstSel(selectors[i % 6u], selectors[(i / 6u) % 6u],
		                                selectors[(i / 36u) % 6u], selectors[(i / 216u) % 6u]);
		ASSERT_TRUE(VulkanPlanSampledImageView(image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
		                                     0u, 1u, 0u, 1u, swizzle, &view));
		if (i == 0u) { first = view; }
		const int index = VulkanGetOrCreateSampledImageView(VK_NULL_HANDLE, &image, view, CreateTestView);
		EXPECT_EQ(index, i < VulkanImage::VIEW_CACHE_LIMIT - VulkanImage::VIEW_MAX ?
		                     VulkanImage::VIEW_MAX + static_cast<int>(i) : -1);
	}
	EXPECT_EQ(image.image_view.size(), static_cast<size_t>(VulkanImage::VIEW_CACHE_LIMIT));
	EXPECT_EQ(VulkanGetOrCreateSampledImageView(VK_NULL_HANDLE, &image, first, CreateTestView), VulkanImage::VIEW_MAX);
}

TEST(EmulatorGraphicsResources, VolumeAdmissionNeverTurnsRejectedTilingIntoLinearBytes)
{
	for (uint32_t format: {1u, 7u, 56u, 71u, 75u}) // one, two, four, eight, sixteen bytes
	{
		for (uint32_t tile: {0u, 5u, 9u, 24u, 27u})
		{
			for (uint32_t levels: {1u, 2u})
			{
				for (uint32_t depth: {1u, 9u})
				{
					Gen5TextureVolumeLayout layout {};
					const bool admitted = Gen5GetVolumeTextureLayout(format, 8u, 16u, depth, 8u, levels, tile, &layout);
					// SW mode 0 (linear) is a byte-for-byte layout and SW modes 5 and 9 the SW_4KB_S and
					// SW_64KB_S volume patterns of every element size. SW mode 27 is a thin volume whose
					// slices are 2D-array layers. Any other tile mode, or a mip chain, must stay rejected
					// instead of being read as linear.
					Gen5TextureArrayLayout slices {};
					const bool thin = tile == 27u && Gen5GetTextureArrayLayout(format, 8u, 16u, 8u, 1u, 27u, depth, &slices);
					EXPECT_EQ(admitted, levels == 1u && (tile == 0u || tile == 5u || tile == 9u || thin));
					if (!admitted)
					{
						EXPECT_EQ(layout.tiled.size, 0u);
						EXPECT_EQ(layout.linear_size, 0u);
						EXPECT_FALSE(Gen5ValidateTextureVolumeUpload(layout, UINT64_MAX));
					} else if (tile == 5u || tile == 9u)
					{
						// 4 KiB blocks: 16x16x16 (1 B), 8x16x16 (2 B), 8x16x8 (4 B), 8x8x8 (8 B), 4x8x8 (16 B).
						// 64 KiB blocks extend the same pattern by four address bits.
						const uint32_t bytes       = ShaderGen5TextureBytesPerElement(format);
						const uint32_t block_bytes = tile == 5u ? 4096u : 65536u;
						uint32_t       bw = 0, bh = 0, bd = 0;
						ASSERT_TRUE(TileGetStandardVolumeBlock(bytes, block_bytes, &bw, &bh, &bd));
						EXPECT_EQ(static_cast<uint64_t>(bw) * bh * bd * bytes, block_bytes);
						const uint32_t pitch  = (8u + bw - 1u) / bw * bw;
						const uint32_t blocks = (pitch / bw) * ((16u + bh - 1u) / bh) * ((depth + bd - 1u) / bd);
						EXPECT_FALSE(layout.linear);
						EXPECT_EQ(layout.pitch, pitch);
						EXPECT_EQ(layout.tiled.size, blocks * block_bytes);
						EXPECT_EQ(layout.linear_size, static_cast<uint64_t>(pitch) * 16u * depth * bytes);
					} else if (tile == 27u)
					{
						EXPECT_TRUE(layout.thin);
						EXPECT_FALSE(layout.linear);
						EXPECT_EQ(layout.tiled.size, slices.tiled_size);
						EXPECT_EQ(layout.linear_size, slices.linear_size);
					} else
					{
						EXPECT_TRUE(layout.linear);
						EXPECT_EQ(layout.linear_size, 8u * 16u * depth * ShaderGen5TextureBytesPerElement(format));
						EXPECT_EQ(layout.tiled.align, 256u);
					}
				}
			}
		}
	}
}

TEST(EmulatorGraphicsResources, LinearVolumeLayoutIsTightAndAlignedTo256)
{
	Gen5TextureVolumeLayout layout {};
	// A 240x135x64 RGBA16F volume with a 256-texel pitch (captured descriptor shape).
	ASSERT_TRUE(Gen5GetVolumeTextureLayout(71u, 240u, 135u, 64u, 256u, 1u, 0u, &layout));
	EXPECT_TRUE(layout.linear);
	EXPECT_EQ(layout.bytes_per_element, 8u);
	EXPECT_EQ(layout.linear_size, 256u * 135u * 64u * 8u);
	EXPECT_EQ(layout.tiled.size, 256u * 135u * 64u * 8u); // already a multiple of 256
	EXPECT_EQ(layout.tiled.align, 256u);
	EXPECT_EQ(layout.width, 240u);
	EXPECT_EQ(layout.pitch, 256u);
	// Odd extents: the slice is pitch*height rows, the allocation rounds up to 256.
	ASSERT_TRUE(Gen5GetVolumeTextureLayout(56u, 3u, 3u, 3u, 3u, 1u, 0u, &layout));
	EXPECT_EQ(layout.linear_size, 3u * 3u * 3u * 4u);
	EXPECT_EQ(layout.tiled.size, 256u);
	EXPECT_TRUE(Gen5ValidateTextureVolumeUpload(layout, layout.linear_size));
	EXPECT_FALSE(Gen5ValidateTextureVolumeUpload(layout, layout.linear_size - 1u));
}

TEST(EmulatorGraphicsResources, LinearVolumeRejectsBlockCompressedUnknownAndOverflowingShapes)
{
	Gen5TextureVolumeLayout layout {};
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(173u, 8u, 16u, 9u, 8u, 1u, 0u, &layout)); // BC3 blocks are not texels
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(9999u, 8u, 16u, 9u, 8u, 1u, 0u, &layout)); // unknown element size
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(56u, 8u, 16u, 9u, 7u, 1u, 0u, &layout));   // pitch below width
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(56u, 8u, 16u, 0u, 8u, 1u, 0u, &layout));
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(75u, 65536u, 65536u, 65536u, 65536u, 1u, 0u, &layout));
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(56u, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 1u, 0u, &layout));
	EXPECT_EQ(layout.tiled.size, 0u);
	EXPECT_EQ(layout.linear_size, 0u);
	EXPECT_FALSE(layout.linear);
}

TEST(EmulatorGraphicsResources, LinearVolumeUploadIsAnExactCheckedCopy)
{
	Gen5TextureVolumeLayout layout {};
	ASSERT_TRUE(Gen5GetVolumeTextureLayout(71u, 3u, 2u, 2u, 4u, 1u, 0u, &layout));
	ASSERT_EQ(layout.linear_size, 4u * 2u * 2u * 8u);
	std::vector<uint8_t> source(layout.tiled.size, 0u);
	for (size_t i = 0; i < source.size(); ++i) { source[i] = static_cast<uint8_t>(i * 7u + 1u); }
	std::vector<uint8_t> destination(layout.linear_size, 0xeeu);
	EXPECT_FALSE(Gen5DetileTextureVolume(destination.data(), layout.linear_size - 1u, source.data(), source.size(), layout));
	EXPECT_FALSE(Gen5DetileTextureVolume(destination.data(), destination.size(), source.data(), layout.linear_size - 1u, layout));
	EXPECT_EQ(destination[0], 0xeeu);
	ASSERT_TRUE(Gen5DetileTextureVolume(destination.data(), destination.size(), source.data(), source.size(), layout));
	EXPECT_EQ(std::memcmp(destination.data(), source.data(), layout.linear_size), 0);
	// A forged layout cannot steer the copy out of bounds.
	auto forged = layout;
	forged.linear_size += 8u;
	EXPECT_FALSE(Gen5ValidateTextureVolumeUpload(forged, UINT64_MAX));
	forged = layout;
	forged.pitch = 2u; // below width
	EXPECT_FALSE(Gen5ValidateTextureVolumeUpload(forged, UINT64_MAX));
}

TEST(EmulatorGraphicsResources, VolumeLayoutRejectsInvalidAndOverflowingSizes)
{
	Gen5TextureVolumeLayout layout {};
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(56u, 0u, 16u, 9u, 8u, 1u, 5u, &layout));
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(56u, 8u, 0u, 9u, 8u, 1u, 5u, &layout));
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(56u, 8u, 16u, 0u, 8u, 1u, 5u, &layout));
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(56u, 8u, 16u, 9u, 7u, 1u, 5u, &layout));
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(56u, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 1u, 5u, &layout));
	EXPECT_FALSE(Gen5GetVolumeTextureLayout(56u, 65536u, 65536u, 65536u, 65536u, 1u, 5u, &layout));
	EXPECT_EQ(layout.tiled.size, 0u);
	EXPECT_EQ(layout.linear_size, 0u);
}

TEST(EmulatorGraphicsResources, VolumeUploadUsesCheckedSlabsAndPreservesPitchPadding)
{
	Gen5TextureVolumeLayout layout {};
	ASSERT_TRUE(Gen5GetVolumeTextureLayout(56u, 7u, 15u, 9u, 8u, 1u, 5u, &layout));
	ASSERT_EQ(layout.tiled.size, 8192u); // two 8x16x8 four-byte slabs
	std::vector<uint32_t> tiled(2048u);
	for (uint32_t i = 0; i < tiled.size(); ++i) { tiled[i] = i < 1024u ? 0x12345678u : 0x87654321u; }
	std::vector<uint32_t> linear(layout.linear_size / 4u, 0xdeadbeefu);
	EXPECT_FALSE(Gen5DetileTextureVolume(linear.data(), layout.linear_size, tiled.data(), layout.tiled.size - 1u, layout));
	EXPECT_FALSE(Gen5DetileTextureVolume(linear.data(), layout.linear_size - 1u, tiled.data(), layout.tiled.size, layout));
	EXPECT_EQ(linear[0], 0xdeadbeefu);
	ASSERT_TRUE(Gen5DetileTextureVolume(linear.data(), layout.linear_size, tiled.data(), layout.tiled.size, layout));
	for (uint32_t z = 0; z < 9u; ++z)
	{
		for (uint32_t y = 0; y < 15u; ++y)
		{
			for (uint32_t x = 0; x < 8u; ++x)
			{
				EXPECT_EQ(linear[(z * 15u + y) * 8u + x], x == 7u ? 0xdeadbeefu : (z < 8u ? 0x12345678u : 0x87654321u));
			}
		}
	}
}

TEST(EmulatorGraphicsResources, MirrorOnceBorderIdentityCannotBecomeOrdinaryBorderClamp)
{
	using namespace State;
	EXPECT_EQ(ResolveSamplerAddressMode(0u), SamplerAddressMode::Repeat);
	EXPECT_EQ(ResolveSamplerAddressMode(1u), SamplerAddressMode::MirroredRepeat);
	EXPECT_EQ(ResolveSamplerAddressMode(2u), SamplerAddressMode::ClampToEdge);
	EXPECT_EQ(ResolveSamplerAddressMode(6u), SamplerAddressMode::ClampToBorder);
	EXPECT_EQ(ResolveSamplerAddressMode(3u), SamplerAddressMode::MirrorOnceLastTexel);
	EXPECT_EQ(ResolveSamplerAddressMode(4u), SamplerAddressMode::ClampHalfBorder);
	EXPECT_EQ(ResolveSamplerAddressMode(5u), SamplerAddressMode::MirrorOnceHalfBorder);
	EXPECT_EQ(ResolveSamplerAddressMode(7u), SamplerAddressMode::MirrorOnceBorder);
	EXPECT_NE(ResolveSamplerAddressMode(7u), ResolveSamplerAddressMode(6u));
	for (uint8_t mode: {0u, 1u, 2u, 6u}) { EXPECT_TRUE(SamplerAddressModeHasExactHostMapping(ResolveSamplerAddressMode(mode))); }
	for (uint8_t mode: {3u, 4u, 5u, 7u}) { EXPECT_FALSE(SamplerAddressModeHasExactHostMapping(ResolveSamplerAddressMode(mode))); }
	const auto lod = ResolveSamplerLodRange(0u, 0x100u, 0x3fc0u, 1u);
	EXPECT_FLOAT_EQ(lod.lod_bias, -0.25f);
	EXPECT_TRUE(ResolveSamplerComparison(1u, ImageSampleOperation::DepthReference).enabled);
	EXPECT_FALSE(ResolveSamplerComparison(1u, ImageSampleOperation::Regular).enabled);
}

TEST(EmulatorGraphicsResources, NativeMirrorOnceLastTexelRequiresEnabledFeatureAndNormalizedCoordinates)
{
	using namespace State;
	for (VkBool32 supported: {VK_FALSE, VK_TRUE})
	{
		VkPhysicalDeviceVulkan12Features host {};
		host.samplerMirrorClampToEdge = supported;
		const auto enabled = VulkanPlanSamplerFeatures(host);
		EXPECT_EQ(enabled.mirror_clamp_to_edge, supported);
		for (bool unnormalized: {false, true})
		{
			VkSamplerAddressMode address = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			const bool exact = VulkanResolveSamplerAddressMode(ResolveSamplerAddressMode(3u), enabled, unnormalized, &address);
			EXPECT_EQ(exact, supported != VK_FALSE && !unnormalized);
			EXPECT_EQ(address, exact ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT);
			for (uint8_t mode: {4u, 5u, 7u})
			{
				EXPECT_FALSE(VulkanResolveSamplerAddressMode(ResolveSamplerAddressMode(mode), enabled, unnormalized, &address));
			}
		}
	}
}

UT_END();
