#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdlib>

UT_BEGIN(EmulatorShaderEmitterPreconditions);

using namespace Libs::Graphics;

// An emitter whose operands are not the register kinds its text template needs
// must reject the instruction. Continuing emits SPIR-V with an empty or
// mistyped result id, which only surfaces later as an opaque toolchain error.
// Rejection goes through the generator's "shader emitter missing" failure,
// which names the instruction in the emulator log (not on stderr, so the death
// tests cannot match the text). Attribution instead comes from the paired
// positive control: each case differs from a program that generates and exits 0
// only in the corrupted operand, and the pre-fix emitters exited 0 for all of
// them.

namespace {

#if defined(_WIN32)
constexpr int kRejectedExit = 321;
#else
constexpr int kRejectedExit = 65;
#endif
constexpr const char* kRejectedMessage = "";

constexpr uint32_t kSEndpgm = 0xbf810000u;

void InitializeEmitterTest()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

ShaderInstruction EndProgram()
{
	ShaderInstruction end {};
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	return end;
}

ShaderOperand Operand(ShaderOperandType type, int reg, int size = 1)
{
	ShaderOperand operand {};
	operand.type        = type;
	operand.register_id = reg;
	operand.size        = size;
	return operand;
}

ShaderPixelInputInfo PixelInput()
{
	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;
	return input;
}

// Pixel program with a sampled 2D texture at s8 and a sampler at s16.
ShaderPixelInputInfo SampleInput()
{
	ShaderPixelInputInfo input {};
	input.bind.push_constant_size                    = 48;
	input.bind.textures2D.textures_num               = 1;
	input.bind.textures2D.textures2d_sampled_num     = 1;
	input.bind.textures2D.desc[0].start_register     = 8;
	input.bind.textures2D.desc[0].usage              = ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].texture.fields[1]  = 1u << 20u;
	input.bind.textures2D.desc[0].texture.fields[3]  = (9u << 28u) | DstSel(4, 4, 4, 4);
	input.bind.samplers.samplers_num                 = 1;
	input.bind.samplers.start_register[0]            = 16;
	ShaderCalcBindingIndices(&input.bind);
	return input;
}

ShaderInstruction SampleLzDmaskB()
{
	ShaderInstruction sample {};
	sample.type           = ShaderInstructionType::ImageSampleLz;
	sample.format         = ShaderInstructionFormat::VdataVaddr3StSsMimgDmask;
	sample.mimg_dmask     = 0xb;
	sample.dst            = Operand(ShaderOperandType::Vgpr, 3, 3);
	sample.src[0]         = Operand(ShaderOperandType::Vgpr, 0, 3);
	sample.src[1]         = Operand(ShaderOperandType::Sgpr, 8, 8);
	sample.src[2]         = Operand(ShaderOperandType::Sgpr, 16, 4);
	sample.src_num        = 3;
	sample.mimg_dimension = 1;
	return sample;
}

struct ImplicitSampleFormat
{
	ShaderInstructionFormat::Format format;
	uint8_t                         dmask;
	uint32_t                        destination_num;
};

constexpr ImplicitSampleFormat kImplicitSampleFormats[] = {
	{ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1, 0x1u, 1u},
	{ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2, 0x2u, 1u},
	{ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3, 0x3u, 2u},
	{ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4, 0x4u, 1u},
	{ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5, 0x5u, 2u},
	{ShaderInstructionFormat::VdataVaddr3StSsMimgDmask, 0x6u, 2u},
	{ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7, 0x7u, 3u},
	{ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8, 0x8u, 1u},
	{ShaderInstructionFormat::Vdata2Vaddr3StSsDmask9, 0x9u, 2u},
	{ShaderInstructionFormat::Vdata2Vaddr3StSsDmaskA, 0xau, 2u},
	{ShaderInstructionFormat::Vdata3Vaddr3StSsDmaskB, 0xbu, 3u},
	{ShaderInstructionFormat::Vdata2Vaddr3StSsDmaskC, 0xcu, 2u},
	{ShaderInstructionFormat::Vdata3Vaddr3StSsDmaskD, 0xdu, 3u},
	{ShaderInstructionFormat::VdataVaddr3StSsMimgDmask, 0xeu, 3u},
	{ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF, 0xfu, 4u},
};

ShaderPixelInputInfo ImplicitSampleInput(bool include_flat, bool include_array, bool include_volume)
{
	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;
	input.bind.push_constant_size = 128;
	input.bind.textures2D.textures_num =
	    static_cast<int>(include_flat) + static_cast<int>(include_array) + static_cast<int>(include_volume);
	input.bind.textures2D.textures2d_sampled_num = include_flat ? 1 : 0;
	input.bind.textures2D.textures2d_array_sampled_num = include_array ? 1 : 0;
	input.bind.textures2D.textures3d_sampled_num = include_volume ? 1 : 0;
	input.bind.samplers.samplers_num = 1;
	input.bind.samplers.start_register[0] = 32;

	int descriptor = 0;
	int start_register = 8;
	if (include_flat)
	{
		auto& flat = input.bind.textures2D.desc[descriptor++];
		flat.start_register = start_register;
		flat.usage = ShaderTextureUsage::ReadOnly;
		flat.texture.fields[1] = 1u << 20u;
		flat.texture.fields[3] = (9u << 28u) | DstSel(4, 4, 4, 4);
		start_register += 8;
	}
	if (include_array)
	{
		auto& array = input.bind.textures2D.desc[descriptor++];
		array.start_register = start_register;
		array.usage = ShaderTextureUsage::ReadOnly;
		array.texture.fields[1] = 1u << 20u;
		array.texture.fields[3] = (13u << 28u) | DstSel(4, 4, 4, 4);
		start_register += 8;
	}
	if (include_volume)
	{
		auto& volume = input.bind.textures2D.desc[descriptor];
		volume.start_register = start_register;
		volume.usage = ShaderTextureUsage::ReadOnly;
		volume.texture.fields[1] = 1u << 20u;
		volume.texture.fields[3] = (10u << 28u) | DstSel(4, 4, 4, 4);
	}
	ShaderCalcBindingIndices(&input.bind);
	return input;
}

ShaderInstruction ImplicitSample(const ImplicitSampleFormat& sample_format, uint32_t dimension, int texture_register)
{
	ShaderInstruction sample {};
	sample.type = ShaderInstructionType::ImageSample;
	sample.format = sample_format.format;
	sample.dst = Operand(ShaderOperandType::Vgpr, 20, static_cast<int>(sample_format.destination_num));
	sample.src[0] = Operand(ShaderOperandType::Vgpr, 4, 3);
	sample.src[1] = Operand(ShaderOperandType::Sgpr, texture_register, 8);
	sample.src[2] = Operand(ShaderOperandType::Sgpr, 32, 4);
	sample.src_num = 3;
	sample.mimg_dimension = static_cast<uint8_t>(dimension);
	sample.mimg_dmask = sample_format.format == ShaderInstructionFormat::VdataVaddr3StSsMimgDmask ? sample_format.dmask : 0;
	// Model the captured NSA order (v4, v7, v6), where v6 supplies z.
	sample.mimg_address_num = 3;
	sample.mimg_address[0] = Operand(ShaderOperandType::Vgpr, 4);
	sample.mimg_address[1] = Operand(ShaderOperandType::Vgpr, 7);
	sample.mimg_address[2] = Operand(ShaderOperandType::Vgpr, 6);
	return sample;
}

String8 GenerateImplicitSample(const ImplicitSampleFormat& sample_format, bool include_flat, bool include_volume,
	                               bool sample_volume, uint32_t dimension, bool next_gen = true, bool include_array = false,
	                               bool sample_array = false)
{
	InitializeEmitterTest();
	Config::SetNextGen(next_gen);
	const int texture_register = sample_volume ? 8 + (include_flat ? 8 : 0) + (include_array ? 8 : 0) :
	                             (sample_array ? 8 + (include_flat ? 8 : 0) : 8);
	auto input = ImplicitSampleInput(include_flat, include_array, include_volume);

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	code.GetInstructions().Add(ImplicitSample(sample_format, dimension, texture_register));
	code.GetInstructions().Add(EndProgram());
	return SpirvGenerateSource(code, nullptr, &input, nullptr);
}

bool HasPackedImageSampleContract(const String8& source, const ImplicitSampleFormat& sample_format)
{
	uint32_t destination_offset = 0;
	for (uint32_t component = 0; component < 4u; ++component)
	{
		if ((sample_format.dmask & (1u << component)) == 0u)
		{
			continue;
		}
		const auto extract = String8::FromPrintf(
		    "%%image_sample_component_0_%u = OpCompositeExtract %%float %%image_sample_value_0 %u", component,
		    component);
		const auto select = String8::FromPrintf(
		    "%%image_exec_value_0_%u = OpSelect %%float %%image_exec_active_0 %%image_sample_component_0_%u %%image_exec_old_0_%u",
		    destination_offset, component, destination_offset);
		const auto store = String8::FromPrintf("OpStore %%v%u %%image_exec_value_0_%u", 20u + destination_offset,
		                                       destination_offset);
		if (source.FindIndex(extract) == Core::STRING8_INVALID_INDEX || source.FindIndex(select) == Core::STRING8_INVALID_INDEX ||
		    source.FindIndex(store) == Core::STRING8_INVALID_INDEX)
		{
			return false;
		}
		++destination_offset;
	}
	return destination_offset == sample_format.destination_num;
}

bool HasVolumeSampleContract(const String8& source, const ImplicitSampleFormat& sample_format)
{
	return source.FindIndex("%image_sample_image_ptr_0 = OpAccessChain %_ptr_UniformConstant_ImageS3D %textures3D_S") !=
	           Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("%image_sample_x_0 = OpLoad %float %v4") != Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("%image_sample_y_0 = OpLoad %float %v7") != Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("%image_sample_layer_0 = OpLoad %float %v6") != Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("%image_sample_coord_0 = OpCompositeConstruct %v3float %image_sample_x_0 %image_sample_y_0 "
	                        "%image_sample_layer_0") != Core::STRING8_INVALID_INDEX &&
	       HasPackedImageSampleContract(source, sample_format);
}

bool HasArraySampleContract(const String8& source, const ImplicitSampleFormat& sample_format)
{
	return source.FindIndex("%image_sample_image_ptr_0 = OpAccessChain %_ptr_UniformConstant_ImageSA %textures2DA_S") !=
	           Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("%image_sample_x_0 = OpLoad %float %v4") != Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("%image_sample_y_0 = OpLoad %float %v7") != Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("%image_sample_layer_0 = OpLoad %float %v6") != Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("%image_sample_coord_0 = OpCompositeConstruct %v3float %image_sample_x_0 %image_sample_y_0 "
	                        "%image_sample_layer_0") != Core::STRING8_INVALID_INDEX &&
	       HasPackedImageSampleContract(source, sample_format);
}

bool HasFlatSampleContract(const String8& source)
{
	return source.FindIndex("OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S") != Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("OpCompositeConstruct %v2float") != Core::STRING8_INVALID_INDEX &&
	       source.FindIndex("OpAccessChain %_ptr_UniformConstant_ImageS3D %textures3D_S") == Core::STRING8_INVALID_INDEX;
}

[[noreturn]] void RunVolumeOnlyImplicitSampleAndExit()
{
	const auto& sample_format = kImplicitSampleFormats[6]; // RGB dmask 0x7, the captured form.
	const auto source = GenerateImplicitSample(sample_format, false, true, true, 2u);
	if (!HasVolumeSampleContract(source, sample_format))
	{
		std::_Exit(2);
	}
	Vector<uint32_t> binary;
	String8 error;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		std::_Exit(3);
	}
	std::_Exit(0);
}

[[noreturn]] void RunGenericVolumeSampleAndExit(uint8_t dmask, uint32_t destination_num)
{
	const ImplicitSampleFormat sample_format {ShaderInstructionFormat::VdataVaddr3StSsMimgDmask, dmask, destination_num};
	const auto source = GenerateImplicitSample(sample_format, true, true, true, 2u);
	if (!HasVolumeSampleContract(source, sample_format))
	{
		std::_Exit(2);
	}
	Vector<uint32_t> binary;
	String8 error;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		std::_Exit(3);
	}
	std::_Exit(0);
}

ShaderInstruction Ldexp()
{
	ShaderInstruction inst {};
	inst.type    = ShaderInstructionType::VLdexpF32;
	inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	inst.dst     = Operand(ShaderOperandType::Vgpr, 1);
	inst.src[0]  = Operand(ShaderOperandType::Vgpr, 2);
	inst.src[1]  = Operand(ShaderOperandType::Vgpr, 3);
	inst.src_num = 2;
	return inst;
}

// v_movrels_b32 v5, v4 after s_mov_b32 m0, 10: parsed, then copied so a case
// can corrupt one operand without hand-encoding the whole instruction.
void ParseMovrels(ShaderInstruction* mov_m0, ShaderInstruction* movrels)
{
	const uint32_t words[] = {0xbefc038au, (0x3fu << 25u) | (5u << 17u) | (0x43u << 9u) | 260u, kSEndpgm};
	ShaderCode     parsed;
	parsed.SetType(ShaderType::Pixel);
	ShaderParse(words, &parsed);
	if (parsed.GetInstructions().Size() != 3u || parsed.GetInstructions().At(1).type != ShaderInstructionType::VMovrelsB32)
	{
		std::_Exit(2);
	}
	*mov_m0 = parsed.GetInstructions().At(0);
	*movrels = parsed.GetInstructions().At(1);
}

[[noreturn]] void GenerateAndExit(ShaderType type, const ShaderInstruction& first, const ShaderInstruction* second,
                                  const ShaderPixelInputInfo& input)
{
	ShaderCode code;
	code.SetType(type);
	code.GetInstructions().Add(first);
	if (second != nullptr)
	{
		code.GetInstructions().Add(*second);
	}
	code.GetInstructions().Add(EndProgram());
	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
	std::_Exit(source.IsEmpty() ? 3 : 0);
}

[[noreturn]] void RunLdexp(ShaderOperand dst)
{
	InitializeEmitterTest();
	auto inst = Ldexp();
	inst.dst  = dst;
	GenerateAndExit(ShaderType::Pixel, inst, nullptr, PixelInput());
}

[[noreturn]] void RunMovrels(ShaderOperand dst)
{
	InitializeEmitterTest();
	ShaderInstruction mov_m0 {};
	ShaderInstruction movrels {};
	ParseMovrels(&mov_m0, &movrels);
	movrels.dst = dst;
	GenerateAndExit(ShaderType::Pixel, mov_m0, &movrels, PixelInput());
}

enum class SampleOperand
{
	None,
	Dst,
	Address,
	Texture,
	Sampler,
};

[[noreturn]] void RunSample(SampleOperand corrupt)
{
	InitializeEmitterTest();
	auto sample = SampleLzDmaskB();
	switch (corrupt)
	{
		case SampleOperand::Dst: sample.dst = Operand(ShaderOperandType::Sgpr, 3, 3); break;
		case SampleOperand::Address: sample.src[0] = Operand(ShaderOperandType::Sgpr, 0, 3); break;
		case SampleOperand::Texture: sample.src[1] = Operand(ShaderOperandType::Vgpr, 8, 8); break;
		case SampleOperand::Sampler: sample.src[2] = Operand(ShaderOperandType::Vgpr, 16, 4); break;
		case SampleOperand::None: break;
	}
	GenerateAndExit(ShaderType::Pixel, sample, nullptr, SampleInput());
}

} // namespace

TEST(EmulatorShaderEmitterPreconditions, VNopCarriesNoOperandsInEitherEncoding)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	// v_nop (VOP1 opcode 0) and its VOP3 form (opcode 0x180), as a shipped
	// pixel shader encodes them.
	const uint32_t words[] = {0x7e000000u, 0xd5800000u, 0x00000000u, kSEndpgm};
	ShaderCode     parsed;
	parsed.SetType(ShaderType::Pixel);
	ShaderParse(words, &parsed);
	ASSERT_EQ(parsed.GetInstructions().Size(), 3u);
	for (uint32_t i = 0; i < 2u; i++)
	{
		const auto& inst = parsed.GetInstructions().At(i);
		EXPECT_EQ(inst.type, ShaderInstructionType::VNop);
		EXPECT_TRUE(ShaderInstructionLoweringPreconditions(inst)) << i;
	}
}

TEST(EmulatorShaderEmitterPreconditions, ControlsEmitValidOperandKinds)
{
	// Positive controls: the same programs with well-formed operands generate.
	ASSERT_EXIT(RunLdexp(Operand(ShaderOperandType::Vgpr, 1)), ::testing::ExitedWithCode(0), "");
	ASSERT_EXIT(RunMovrels(Operand(ShaderOperandType::Vgpr, 5)), ::testing::ExitedWithCode(0), "");
	ASSERT_EXIT(RunSample(SampleOperand::None), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderEmitterPreconditions, ImplicitImageSampleUsesBoundShapeAndPacksEnabledComponents)
{
	for (const auto& sample_format: kImplicitSampleFormats)
	{
		if (sample_format.format == ShaderInstructionFormat::VdataVaddr3StSsMimgDmask)
		{
			continue;
		}
		const auto source = GenerateImplicitSample(sample_format, true, true, true, 2u);
		EXPECT_TRUE(HasVolumeSampleContract(source, sample_format)) << String8::FromPrintf("dmask=0x%x", sample_format.dmask).c_str();

		if (sample_format.dmask == 0x7u)
		{
			Vector<uint32_t> binary;
			String8          error;
			EXPECT_TRUE(ShaderToolchain::Run(source, &binary, &error)) << error.c_str();
			EXPECT_FALSE(binary.IsEmpty());
		}
	}

	const auto flat_source = GenerateImplicitSample(kImplicitSampleFormats[6], true, false, false, 1u, false);
	EXPECT_TRUE(HasFlatSampleContract(flat_source));
	const auto mixed_flat_source = GenerateImplicitSample(kImplicitSampleFormats[6], true, true, false, 1u);
	EXPECT_TRUE(HasFlatSampleContract(mixed_flat_source));

	const auto array_source = GenerateImplicitSample(kImplicitSampleFormats[4], false, false, false, 5u, true, true, true);
	EXPECT_TRUE(HasArraySampleContract(array_source, kImplicitSampleFormats[4]));

	// Volume-only and generic-dmask cases used to reject before reaching the
	// typed emitter; keep them isolated so the expected failure stays local.
	EXPECT_EXIT(RunVolumeOnlyImplicitSampleAndExit(), ::testing::ExitedWithCode(0), "");
	EXPECT_EXIT(RunGenericVolumeSampleAndExit(0x6u, 2u), ::testing::ExitedWithCode(0), "");
	EXPECT_EXIT(RunGenericVolumeSampleAndExit(0xeu, 3u), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderEmitterPreconditions, LdexpRejectsNonRegisterDestination)
{
	ASSERT_EXIT(RunLdexp(Operand(ShaderOperandType::Null, 0)), ::testing::ExitedWithCode(kRejectedExit), kRejectedMessage);
}

TEST(EmulatorShaderEmitterPreconditions, LdexpRejectsNonFloatDestination)
{
	ASSERT_EXIT(RunLdexp(Operand(ShaderOperandType::Sgpr, 4)), ::testing::ExitedWithCode(kRejectedExit), kRejectedMessage);
}

TEST(EmulatorShaderEmitterPreconditions, MovrelsRejectsNonRegisterDestination)
{
	ASSERT_EXIT(RunMovrels(Operand(ShaderOperandType::Null, 0)), ::testing::ExitedWithCode(kRejectedExit), kRejectedMessage);
}

TEST(EmulatorShaderEmitterPreconditions, MovrelsRejectsNonFloatDestination)
{
	ASSERT_EXIT(RunMovrels(Operand(ShaderOperandType::Sgpr, 5)), ::testing::ExitedWithCode(kRejectedExit), kRejectedMessage);
}

TEST(EmulatorShaderEmitterPreconditions, SampleLzRejectsNonFloatDestination)
{
	ASSERT_EXIT(RunSample(SampleOperand::Dst), ::testing::ExitedWithCode(kRejectedExit), kRejectedMessage);
}

TEST(EmulatorShaderEmitterPreconditions, SampleLzRejectsNonFloatAddress)
{
	ASSERT_EXIT(RunSample(SampleOperand::Address), ::testing::ExitedWithCode(kRejectedExit), kRejectedMessage);
}

TEST(EmulatorShaderEmitterPreconditions, SampleLzRejectsNonScalarTextureDescriptor)
{
	ASSERT_EXIT(RunSample(SampleOperand::Texture), ::testing::ExitedWithCode(kRejectedExit), kRejectedMessage);
}

TEST(EmulatorShaderEmitterPreconditions, SampleLzRejectsNonScalarSamplerDescriptor)
{
	ASSERT_EXIT(RunSample(SampleOperand::Sampler), ::testing::ExitedWithCode(kRejectedExit), kRejectedMessage);
}

UT_END();
