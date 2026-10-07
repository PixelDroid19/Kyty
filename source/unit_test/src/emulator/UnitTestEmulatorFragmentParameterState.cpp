#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderSpirv.h"

UT_BEGIN(EmulatorFragmentParameterState);

using namespace Libs::Graphics;

static ShaderInstruction Move(ShaderOperandType destination, int reg, ShaderOperandType source, int source_reg)
{
	ShaderInstruction instruction {};
	instruction.type               = ShaderInstructionType::SMovB32;
	instruction.format             = ShaderInstructionFormat::SVdstSVsrc0;
	instruction.dst.type           = destination;
	instruction.dst.register_id    = reg;
	instruction.dst.size           = 1;
	instruction.src[0].type        = source;
	instruction.src[0].register_id = source_reg;
	instruction.src[0].size        = source == ShaderOperandType::Sgpr || source == ShaderOperandType::M0 ? 1 : 0;
	instruction.src[0].constant.u  = 7;
	instruction.src_num            = 1;
	return instruction;
}

static void Append(ShaderCode* code, ShaderInstruction instruction)
{
	instruction.pc = code->GetInstructions().Size() * 4u;
	code->GetInstructions().Add(instruction);
}

static ShaderCode InitialCopy()
{
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	Append(&code, Move(ShaderOperandType::M0, 0, ShaderOperandType::Sgpr, 30));
	return code;
}

TEST(EmulatorFragmentParameterState, RemovesOnlyTheUnobservableInitialParameterHeader)
{
	auto code = InitialCopy();
	Append(&code, Move(ShaderOperandType::Sgpr, 30, ShaderOperandType::LiteralConstant, 0));
	Append(&code, Move(ShaderOperandType::Sgpr, 1, ShaderOperandType::Sgpr, 30));
	EXPECT_TRUE(ShaderAnalyzeFragmentVirtualParameterState(code, 30).supported);
	ShaderFragmentComputeInfo transport {1, 0, 1, 4, 30};
	EXPECT_EQ(transport.HeaderWords(), 1u);
	transport.parameter_state = ShaderFragmentParameterState::Virtualized;
	EXPECT_EQ(transport.HeaderWords(), 0u);
}

TEST(EmulatorFragmentParameterState, RejectsInitialScalarAndM0DataReads)
{
	for (auto source: {ShaderOperandType::Sgpr, ShaderOperandType::M0})
	{
		auto code = InitialCopy();
		Append(&code, Move(ShaderOperandType::Sgpr, 1, source, source == ShaderOperandType::Sgpr ? 30 : 0));
		EXPECT_FALSE(ShaderAnalyzeFragmentVirtualParameterState(code, 30).supported);
	}
}

TEST(EmulatorFragmentParameterState, RejectsOverwriteAfterControlTransfer)
{
	for (auto type: {ShaderInstructionType::SBranch, ShaderInstructionType::SSetpcB64, ShaderInstructionType::SSwappcB64})
	{
		auto              code = InitialCopy();
		ShaderInstruction transfer {};
		transfer.type = type;
		Append(&code, transfer);
		Append(&code, Move(ShaderOperandType::Sgpr, 30, ShaderOperandType::LiteralConstant, 0));
		Append(&code, Move(ShaderOperandType::Sgpr, 1, ShaderOperandType::Sgpr, 30));
		EXPECT_FALSE(ShaderAnalyzeFragmentVirtualParameterState(code, 30).supported);
	}
}

TEST(EmulatorFragmentParameterState, RejectsPartialOverwriteAndLaterM0Replacement)
{
	auto code = InitialCopy();
	Append(&code, Move(ShaderOperandType::Sgpr, 31, ShaderOperandType::LiteralConstant, 0));
	Append(&code, Move(ShaderOperandType::Sgpr, 1, ShaderOperandType::Sgpr, 30));
	EXPECT_FALSE(ShaderAnalyzeFragmentVirtualParameterState(code, 30).supported);
	code = InitialCopy();
	Append(&code, Move(ShaderOperandType::M0, 0, ShaderOperandType::LiteralConstant, 0));
	EXPECT_FALSE(ShaderAnalyzeFragmentVirtualParameterState(code, 30).supported);
	EXPECT_FALSE(ShaderAnalyzeFragmentVirtualParameterState(code, UINT32_MAX).supported);
}

UT_END();
