#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderImageGradientProof.h"

#include <array>
#include <cstdint>
#include <vector>

UT_BEGIN(EmulatorImageGradientProof);

using namespace Libs::Graphics;

namespace {

using Program = std::vector<ShaderInstruction>;

ShaderOperand Vgpr(int reg, int size = 1)
{
	ShaderOperand operand {};
	operand.type        = ShaderOperandType::Vgpr;
	operand.register_id = reg;
	operand.size        = size;
	return operand;
}

ShaderOperand Sgpr(int reg, int size = 1)
{
	ShaderOperand operand {};
	operand.type        = ShaderOperandType::Sgpr;
	operand.register_id = reg;
	operand.size        = size;
	return operand;
}

ShaderOperand Literal(int value)
{
	ShaderOperand operand {};
	operand.type       = ShaderOperandType::LiteralConstant;
	operand.constant.i = value;
	return operand;
}

ShaderInstruction Wqm()
{
	ShaderInstruction instruction {};
	instruction.type        = ShaderInstructionType::SWqmB64;
	instruction.format      = ShaderInstructionFormat::Sdst2Ssrc02;
	instruction.dst.type    = ShaderOperandType::ExecLo;
	instruction.dst.size    = 2;
	instruction.src[0].type = ShaderOperandType::ExecLo;
	instruction.src[0].size = 2;
	instruction.src_num     = 1;
	return instruction;
}

ShaderInstruction Move(int destination, ShaderOperand source)
{
	ShaderInstruction instruction {};
	instruction.type        = ShaderInstructionType::VMovB32;
	instruction.format      = ShaderInstructionFormat::SVdstSVsrc0;
	instruction.dst         = Vgpr(destination);
	instruction.src[0]      = source;
	instruction.src_num     = 1;
	return instruction;
}

ShaderInstruction DppMove(int destination, int source, uint16_t control = 0x00u, uint8_t row_mask = 0x0fu,
	                       uint8_t bank_mask = 0x0fu)
{
	auto instruction                  = Move(destination, Vgpr(source));
	instruction.src[0].dpp            = true;
	instruction.src[0].dpp_ctrl       = control;
	instruction.src[0].dpp_row_mask   = row_mask;
	instruction.src[0].dpp_bank_mask  = bank_mask;
	instruction.src[0].dpp_bound_ctrl = true;
	return instruction;
}

ShaderInstruction Subtract(int destination, int base, int offset, uint16_t control)
{
	ShaderInstruction instruction {};
	instruction.type                      = ShaderInstructionType::VSubF32;
	instruction.format                    = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	instruction.dst                       = Vgpr(destination);
	instruction.src[0]                    = Vgpr(base);
	instruction.src[0].dpp                = true;
	instruction.src[0].dpp_ctrl           = control;
	instruction.src[0].dpp_row_mask       = 0x0fu;
	instruction.src[0].dpp_bank_mask      = 0x0fu;
	instruction.src[0].dpp_bound_ctrl     = true;
	instruction.src[1]                    = Vgpr(offset);
	instruction.src_num                   = 2;
	return instruction;
}

ShaderInstruction Mad(int destination, ShaderOperand source0, ShaderOperand source1, ShaderOperand source2)
{
	ShaderInstruction instruction {};
	instruction.type    = ShaderInstructionType::VMadF32;
	instruction.format  = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
	instruction.dst     = Vgpr(destination);
	instruction.src[0]  = source0;
	instruction.src[1]  = source1;
	instruction.src[2]  = source2;
	instruction.src_num = 3;
	return instruction;
}

ShaderInstruction Mac(int destination, ShaderOperand source0, ShaderOperand source1)
{
	ShaderInstruction instruction {};
	instruction.type    = ShaderInstructionType::VMacF32;
	instruction.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	instruction.dst     = Vgpr(destination);
	instruction.src[0]  = source0;
	instruction.src[1]  = source1;
	instruction.src_num = 2;
	return instruction;
}

ShaderInstruction Sample(bool use_nsa = false, bool high_vaddr = false)
{
	const int vaddr = high_vaddr ? 255 : 30;
	ShaderInstruction instruction {};
	instruction.type           = ShaderInstructionType::ImageSampleCd;
	instruction.format         = ShaderInstructionFormat::VdataVaddr6StSsMimgDmask;
	instruction.dst            = Vgpr(60);
	instruction.src[0]         = Vgpr(vaddr, use_nsa ? 1 : 6);
	instruction.src[1]         = Sgpr(8, 8);
	instruction.src[2]         = Sgpr(24, 4);
	instruction.src_num        = 3;
	instruction.mimg_dimension = 1;
	instruction.mimg_dmask     = 1;
	if (use_nsa)
	{
		instruction.mimg_address_num = 9;
		for (int index = 0; index < instruction.mimg_address_num; ++index)
		{
			instruction.mimg_address[index] = Vgpr(high_vaddr && index == 0 ? 255 : 30 + index);
		}
	}
	return instruction;
}

ShaderInstruction ConditionalBranch(int displacement_bytes)
{
	ShaderInstruction instruction {};
	instruction.type        = ShaderInstructionType::SCbranchScc0;
	instruction.format      = ShaderInstructionFormat::Label;
	instruction.src[0]      = Literal(displacement_bytes);
	instruction.src_num     = 1;
	return instruction;
}

ShaderInstruction End()
{
	ShaderInstruction instruction {};
	instruction.type   = ShaderInstructionType::SEndpgm;
	instruction.format = ShaderInstructionFormat::Empty;
	return instruction;
}

ShaderCode Build(const Program& program)
{
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	for (auto instruction: program)
	{
		instruction.pc = code.GetInstructions().Size() * 4u;
		code.GetInstructions().Add(instruction);
	}
	for (const auto& instruction: code.GetInstructions())
	{
		if (instruction.type == ShaderInstructionType::SBranch || instruction.type == ShaderInstructionType::SCbranchScc0 ||
		    instruction.type == ShaderInstructionType::SCbranchScc1 || instruction.type == ShaderInstructionType::SCbranchVccz ||
		    instruction.type == ShaderInstructionType::SCbranchVccnz || instruction.type == ShaderInstructionType::SCbranchExecz ||
		    instruction.type == ShaderInstructionType::SCbranchExecnz)
		{
			const uint32_t target = instruction.pc + 4u + static_cast<uint32_t>(instruction.src[0].constant.i);
			code.GetLabels().Add(ShaderLabel(target, instruction.pc));
			if (instruction.type != ShaderInstructionType::SBranch)
			{
				code.GetIndirectLabels().Add(ShaderLabel(instruction.pc + 4u, instruction.pc));
			}
		}
	}
	return code;
}

Program Producers(bool high_vaddr = false)
{
	return {Wqm(), Move(7, Sgpr(16)), Move(42, Sgpr(17)), DppMove(20, 7), DppMove(24, 42, 0xffu),
	        Subtract(high_vaddr ? 255 : 30, 7, 20, 0x55u),
	        Subtract(31, 7, 20, 0xaau), Subtract(32, 42, 24, 0x55u), Subtract(33, 42, 24, 0xaau)};
}

Program NativeProducers()
{
	auto v7  = Mad(7, Sgpr(78), Vgpr(39), Vgpr(21));
	v7.src[2].negate = true;
	auto v42 = Mad(42, Sgpr(64), Vgpr(20), Sgpr(76));
	v42.src[0].negate = true;
	return {Wqm(), v42, v7, Mac(42, Sgpr(79), Vgpr(16)), DppMove(20, 7), DppMove(24, 42, 0xffu),
	        Subtract(30, 7, 20, 0x55u), Subtract(31, 7, 20, 0xaau), Subtract(32, 42, 24, 0x55u),
	        Subtract(33, 42, 24, 0xaau)};
}

bool Admitted(const Program& program, bool use_nsa = false, bool high_vaddr = false)
{
	auto complete = program;
	complete.push_back(Sample(use_nsa, high_vaddr));
	complete.push_back(End());
	auto code = Build(complete);
	return ShaderImageSampleCdHasQuadUniformGradients(code, static_cast<uint32_t>(complete.size() - 2u));
}

} // namespace

TEST(EmulatorImageGradientProof, AcceptsFourFixedBroadcastAndSubtractionGradients)
{
	EXPECT_TRUE(Admitted(Producers()));
	EXPECT_TRUE(Admitted(Producers(), true));
	EXPECT_TRUE(Admitted(Producers(true), true, true));
	EXPECT_TRUE(Admitted(NativeProducers()));

	auto branched = Producers();
	branched.push_back(ConditionalBranch(4)); // The taken edge skips the sample, not its producers.
	EXPECT_TRUE(Admitted(branched));

	auto narrowed = Producers();
	ShaderInstruction exec_narrow {};
	exec_narrow.type = ShaderInstructionType::VCmpxLtU32;
	narrowed.push_back(exec_narrow);
	EXPECT_TRUE(Admitted(narrowed));
}

TEST(EmulatorImageGradientProof, RejectsUnprovenOrIncompleteGradientValues)
{
	auto clobbered = Producers();
	clobbered.push_back(Move(32, Vgpr(8)));
	EXPECT_FALSE(Admitted(clobbered));

	auto undefined_source = Producers();
	undefined_source[3]    = DppMove(20, 8);
	EXPECT_FALSE(Admitted(undefined_source));

	auto partial_row = Producers();
	partial_row[5]     = Subtract(30, 7, 20, 0x55u);
	partial_row[5].src[0].dpp_row_mask = 0x07u;
	EXPECT_FALSE(Admitted(partial_row));

	auto partial_bank = Producers();
	partial_bank[7].src[0].dpp_bank_mask = 0x0eu;
	EXPECT_FALSE(Admitted(partial_bank));

	auto partial_dpp_write = Producers();
	partial_dpp_write[3].src[0].dpp_row_mask = 0x07u;
	EXPECT_FALSE(Admitted(partial_dpp_write));

	auto sdwa_write = Producers();
	sdwa_write[1].vop_sdwa      = true;
	sdwa_write[1].vop_sdwa_ctrl = 0xffffffffu;
	EXPECT_FALSE(Admitted(sdwa_write));

	auto unmodeled_dpp_bits = Producers();
	unmodeled_dpp_bits[3].src[0].dpp_unmodeled_bits = 1u << 17u;
	EXPECT_FALSE(Admitted(unmodeled_dpp_bits));

	auto missing_mac_accumulator = NativeProducers();
	missing_mac_accumulator.erase(missing_mac_accumulator.begin() + 1);
	EXPECT_FALSE(Admitted(missing_mac_accumulator));

	auto no_wqm = Producers();
	no_wqm.erase(no_wqm.begin());
	EXPECT_FALSE(Admitted(no_wqm));

	auto incomplete_nsa = Producers();
	incomplete_nsa.push_back(Sample(true));
	incomplete_nsa.push_back(End());
	auto incomplete_nsa_code = Build(incomplete_nsa);
	incomplete_nsa_code.GetInstructions()[static_cast<uint32_t>(incomplete_nsa.size() - 2u)].mimg_address_num = 5;
	EXPECT_FALSE(ShaderImageSampleCdHasQuadUniformGradients(incomplete_nsa_code,
	                                                       static_cast<uint32_t>(incomplete_nsa.size() - 2u)));

	const std::array<ShaderInstructionType, 3> source_only_destination_types {
	    ShaderInstructionType::BufferStoreDword, ShaderInstructionType::BufferAtomicAdd, ShaderInstructionType::ImageStore};
	for (const auto type: source_only_destination_types)
	{
		auto source_only_destination = Producers();
		ShaderInstruction source_only {};
		source_only.type         = type;
		source_only.dst          = Vgpr(7);
		source_only.buffer_flags = 0u;
		source_only_destination[1] = source_only;
		EXPECT_FALSE(Admitted(source_only_destination));
	}

	auto lds_buffer_load = Producers();
	ShaderInstruction lds_load {};
	lds_load.type         = ShaderInstructionType::BufferLoadDword;
	lds_load.dst          = Vgpr(7);
	lds_load.buffer_flags = 0x01u;
	lds_buffer_load[1]    = lds_load;
	EXPECT_FALSE(Admitted(lds_buffer_load));

	auto tfe_status_overlap = Producers();
	ShaderInstruction buffer_load {};
	buffer_load.type         = ShaderInstructionType::BufferLoadDword;
	buffer_load.dst          = Vgpr(32);
	buffer_load.buffer_flags = 0x04u;
	tfe_status_overlap.push_back(buffer_load);
	EXPECT_FALSE(Admitted(tfe_status_overlap));

	auto unmodeled_buffer_flags = Producers();
	buffer_load.buffer_flags = 0x80u;
	unmodeled_buffer_flags.push_back(buffer_load);
	EXPECT_FALSE(Admitted(unmodeled_buffer_flags));
}

TEST(EmulatorImageGradientProof, RejectsBranchBypassAndIndirectControlFlow)
{
	auto bypass   = Producers();
	Program slopes(bypass.begin() + 5, bypass.end());
	bypass.resize(5);
	bypass.push_back(ConditionalBranch(16)); // Taken edge skips the four gradient writes.
	bypass.insert(bypass.end(), slopes.begin(), slopes.end());
	EXPECT_FALSE(Admitted(bypass));

	auto indirect = Producers();
	ShaderInstruction setpc {};
	setpc.type = ShaderInstructionType::SSetpcB64;
	indirect.push_back(setpc);
	EXPECT_FALSE(Admitted(indirect));

	auto loop_kill = Producers();
	const auto branch_index = static_cast<int>(loop_kill.size());
	loop_kill.push_back(ConditionalBranch(-branch_index * 4));
	loop_kill.push_back(Move(30, Vgpr(7)));
	EXPECT_FALSE(Admitted(loop_kill));
}

UT_END();
