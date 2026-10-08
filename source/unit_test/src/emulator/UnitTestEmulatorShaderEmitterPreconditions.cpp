#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderControlFlowGraph.h"

#include <cstdlib>
#include <limits>

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
	const auto& instructions = code.GetInstructions();
	if (instructions.IsEmpty()) { std::_Exit(4); }
	const uint32_t last_pc = instructions.At(instructions.Size() - 1u).pc;
	if (last_pc > std::numeric_limits<uint32_t>::max() - 4u) { std::_Exit(4); }
	auto end = EndProgram();
	end.pc = last_pc + 4u;
	code.GetInstructions().Add(end);
	if (!ShaderBuildControlFlowGraph(code).Structurable()) { std::_Exit(4); }
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
