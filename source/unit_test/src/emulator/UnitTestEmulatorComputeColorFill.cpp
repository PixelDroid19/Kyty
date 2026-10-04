#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ComputeColorFill.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Utils.h"

#include <array>

UT_BEGIN(EmulatorComputeColorFill);

using namespace Libs::Graphics;

namespace {

ShaderOperand UniformBufferFillVgpr(int register_id, int size = 1)
{
	return {.type = ShaderOperandType::Vgpr, .register_id = register_id, .size = size};
}

ShaderOperand UniformBufferFillSgpr(int register_id, int size = 1)
{
	return {.type = ShaderOperandType::Sgpr, .register_id = register_id, .size = size};
}

ShaderOperand UniformBufferFillInline(uint32_t value)
{
	ShaderOperand operand {};
	operand.type       = ShaderOperandType::IntegerInlineConstant;
	operand.constant.u = value;
	return operand;
}

struct UniformBufferFillPattern
{
	int      workgroup_register         = 0;
	int      index_register             = 0;
	int      value_start_register       = 0;
	int      destination_start_register = 0;
	int      value_registers[4]         = {};
	uint32_t workgroup_shift            = 0;
	bool     include_padding            = false;
	bool     moves_before_index         = false;
};

ShaderCode UniformBufferFillCode(const UniformBufferFillPattern& pattern)
{
	ShaderCode code {};
	code.SetType(ShaderType::Compute);

	uint32_t pc = 0;
	auto add = [&code, &pc](ShaderInstruction instruction)
	{
		instruction.pc = pc;
		pc += 4u;
		code.GetInstructions().Add(instruction);
	};

	ShaderInstruction index {};
	index.type    = ShaderInstructionType::VLshlAddU32;
	index.format  = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
	index.dst     = UniformBufferFillVgpr(pattern.index_register);
	index.src[0]  = UniformBufferFillSgpr(pattern.workgroup_register);
	index.src[1]  = UniformBufferFillInline(pattern.workgroup_shift);
	index.src[2]  = UniformBufferFillVgpr(0);
	index.src_num = 3;
	auto add_moves = [&]
	{
		for (int component = 0; component < 4; ++component)
		{
			ShaderInstruction move {};
			move.type    = ShaderInstructionType::VMovB32;
			move.format  = ShaderInstructionFormat::SVdstSVsrc0;
			move.dst     = UniformBufferFillVgpr(pattern.value_start_register + component);
			move.src[0]  = UniformBufferFillSgpr(pattern.value_registers[component]);
			move.src_num = 1;
			add(move);
		}
	};
	auto add_wait = [&]
	{
		ShaderInstruction wait {};
		wait.type              = ShaderInstructionType::SWaitcnt;
		wait.format            = ShaderInstructionFormat::Imm;
		wait.src[0].type       = ShaderOperandType::LiteralConstant;
		wait.src[0].constant.u = 0u;
		wait.src_num           = 1;
		add(wait);
	};

	if (pattern.moves_before_index)
	{
		add_moves();
		if (pattern.include_padding)
		{
			add_wait();
		}
		add(index);
	} else
	{
		add(index);
		if (pattern.include_padding)
		{
			add_wait();
		}
		add_moves();
	}

	if (pattern.include_padding)
	{
		// The current shader IR represents s_nop as SInstPrefetch.
		ShaderInstruction nop {};
		nop.type              = ShaderInstructionType::SInstPrefetch;
		nop.format            = ShaderInstructionFormat::Imm;
		nop.src[0].type       = ShaderOperandType::LiteralConstant;
		nop.src[0].constant.u = 0u;
		nop.src_num           = 1;
		add(nop);
	}

	ShaderInstruction store {};
	store.type                    = ShaderInstructionType::BufferStoreFormatXyzw;
	store.format                  = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
	store.dst                     = UniformBufferFillVgpr(pattern.value_start_register, 4);
	store.src[0]                  = UniformBufferFillVgpr(pattern.index_register);
	store.src[1]                  = UniformBufferFillSgpr(pattern.destination_start_register, 4);
	store.src[2]                  = UniformBufferFillInline(0u);
	store.src_num                 = 3;
	store.buffer_idxen            = true;
	store.buffer_offen            = false;
	store.buffer_imm_offset       = 0u;
	store.buffer_return_old_value = false;
	add(store);

	ShaderInstruction end {};
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	add(end);

	return code;
}

UniformBufferFillPattern UniformBufferFillTestPattern()
{
	return {
		.workgroup_register         = 17,
		.index_register             = 33,
		.value_start_register       = 72,
		.destination_start_register = 48,
		.value_registers            = {91, 7, 62, 19},
		.workgroup_shift            = 5u,
	};
}

ComputeColorFillIdentity FillIdentity(uint64_t address, uint64_t content = 7)
{
	return {address, 0x1000u, 3u, 5u, content};
}

SubmissionId Submission(uint32_t queue, uint64_t sequence)
{
	SubmissionId id;
	id.queue    = GpuQueueId {queue};
	id.sequence = sequence;
	return id;
}

} // namespace

TEST(EmulatorComputeColorFill, ComputeUniformBufferFillRecognizesRenamedRegistersAcrossWorkgroupWidths)
{
	UniformBufferFillPattern patterns[] = {
		UniformBufferFillTestPattern(),
		{
			.workgroup_register         = 3,
			.index_register             = 118,
			.value_start_register       = 24,
			.destination_start_register = 84,
			.value_registers            = {43, 12, 97, 31},
			.workgroup_shift            = 6u,
			.include_padding            = true,
			.moves_before_index         = true,
		},
	};

	for (const auto& pattern: patterns)
	{
		const auto evidence = AnalyzeShaderComputeUniformBufferFill(UniformBufferFillCode(pattern));
		ASSERT_TRUE(evidence.valid);
		EXPECT_EQ(evidence.destination_start_register, pattern.destination_start_register);
		EXPECT_EQ(evidence.workgroup_register, pattern.workgroup_register);
		EXPECT_EQ(evidence.workgroup_shift, pattern.workgroup_shift);
		for (int component = 0; component < 4; ++component)
		{
			EXPECT_EQ(evidence.value_registers[component], pattern.value_registers[component]);
		}
	}
}

TEST(EmulatorComputeColorFill, ComputeUniformBufferFillRejectsClobberedIndexAndNonScalarValues)
{
	const auto pattern = UniformBufferFillTestPattern();

	auto clobbered_index = UniformBufferFillCode(pattern);
	clobbered_index.GetInstructions()[1].dst = UniformBufferFillVgpr(pattern.index_register);
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(clobbered_index).valid);

	auto vector_value = UniformBufferFillCode(pattern);
	vector_value.GetInstructions()[1].src[0] = UniformBufferFillVgpr(9);
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(vector_value).valid);

	auto wrong_vaddr = UniformBufferFillCode(pattern);
	wrong_vaddr.GetInstructions()[5].src[0] = UniformBufferFillVgpr(pattern.value_start_register);
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(wrong_vaddr).valid);
}

TEST(EmulatorComputeColorFill, ComputeUniformBufferFillRejectsMoveFirstClobberOfInvocationId)
{
	auto moves_first = UniformBufferFillTestPattern();
	moves_first.moves_before_index    = true;
	moves_first.value_start_register = 0;
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(UniformBufferFillCode(moves_first)).valid);

	moves_first.moves_before_index = false;
	EXPECT_TRUE(AnalyzeShaderComputeUniformBufferFill(UniformBufferFillCode(moves_first)).valid);
}

TEST(EmulatorComputeColorFill, ComputeUniformBufferFillRejectsScalarAndBufferOffsets)
{
	const auto pattern = UniformBufferFillTestPattern();

	auto scalar_offset = UniformBufferFillCode(pattern);
	scalar_offset.GetInstructions()[5].src[2] = UniformBufferFillSgpr(4);
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(scalar_offset).valid);

	auto immediate_offset = UniformBufferFillCode(pattern);
	immediate_offset.GetInstructions()[5].buffer_imm_offset = 16u;
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(immediate_offset).valid);

	auto vector_offset = UniformBufferFillCode(pattern);
	vector_offset.GetInstructions()[5].buffer_offen = true;
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(vector_offset).valid);

	auto missing_index = UniformBufferFillCode(pattern);
	missing_index.GetInstructions()[5].buffer_idxen = false;
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(missing_index).valid);
}

TEST(EmulatorComputeColorFill, ComputeUniformBufferFillRejectsWorkgroupDependentValues)
{
	auto pattern = UniformBufferFillTestPattern();
	pattern.value_registers[0] = pattern.workgroup_register;
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(UniformBufferFillCode(pattern)).valid);
}

TEST(EmulatorComputeColorFill, ComputeUniformBufferFillRejectsModifiersExtraInstructionsAndControlFlow)
{
	const auto pattern = UniformBufferFillTestPattern();

	auto modified_move = UniformBufferFillCode(pattern);
	modified_move.GetInstructions()[1].src[0].negate = true;
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(modified_move).valid);

	auto extra_instruction = UniformBufferFillCode(pattern);
	ShaderInstruction extra {};
	extra.type    = ShaderInstructionType::VAddI32;
	extra.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	extra.dst     = UniformBufferFillVgpr(99);
	extra.src[0]  = UniformBufferFillVgpr(1);
	extra.src[1]  = UniformBufferFillVgpr(2);
	extra.src_num = 2;
	extra_instruction.GetInstructions().Add(extra);
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(extra_instruction).valid);

	auto atomic = UniformBufferFillCode(pattern);
	atomic.GetInstructions()[5].type = ShaderInstructionType::BufferAtomicAdd;
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(atomic).valid);

	auto branch = UniformBufferFillCode(pattern);
	branch.GetInstructions()[2].type    = ShaderInstructionType::SBranch;
	branch.GetInstructions()[2].format  = ShaderInstructionFormat::Label;
	branch.GetInstructions()[2].src[0].type = ShaderOperandType::LiteralConstant;
	branch.GetInstructions()[2].src[0].constant.i = 0;
	branch.GetInstructions()[2].src_num = 1;
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(branch).valid);
}

TEST(EmulatorComputeColorFill, ComputeUniformBufferFillRejectsOutOfRangeShiftAndNonComputeStage)
{
	const auto pattern = UniformBufferFillTestPattern();

	auto shift = UniformBufferFillCode(pattern);
	shift.GetInstructions()[0].src[1] = UniformBufferFillInline(11u);
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(shift).valid);

	auto pixel = UniformBufferFillCode(pattern);
	pixel.SetType(ShaderType::Pixel);
	EXPECT_FALSE(AnalyzeShaderComputeUniformBufferFill(pixel).valid);
}

TEST(EmulatorComputeColorFill, AFillEventIsConsumedOnceByItsExactLaterConsumer)
{
	ComputeColorFillEvents events;
	const std::array<uint32_t, 4> words {0u, 0x3c000000u, 0u, 0x3c000000u};
	ASSERT_TRUE(events.Publish(FillIdentity(0x10000u), Submission(8, 4), words));
	ComputeColorFillEvent event {};
	EXPECT_TRUE(events.Consume(FillIdentity(0x10000u), Submission(8, 5), &event));
	EXPECT_EQ(event.words, words);
	EXPECT_FALSE(events.Consume(FillIdentity(0x10000u), Submission(8, 6), &event));
	EXPECT_TRUE(events.Empty());
}

TEST(EmulatorComputeColorFill, AMismatchedConsumerDiscardsTheEventWithoutApplyingIt)
{
	const std::array<uint32_t, 4> words {1u, 2u, 1u, 2u};
	ComputeColorFillEvent event {};
	// Content changed after the fill: the image must not receive a stale clear.
	ComputeColorFillEvents content;
	ASSERT_TRUE(content.Publish(FillIdentity(0x20000u, 7), Submission(8, 4), words));
	EXPECT_FALSE(content.Consume(FillIdentity(0x20000u, 8), Submission(8, 5), &event));
	EXPECT_TRUE(content.Empty());
	// A different virtual queue or an earlier submission is not ordered after the producer.
	ComputeColorFillEvents queue;
	ASSERT_TRUE(queue.Publish(FillIdentity(0x20000u), Submission(8, 4), words));
	EXPECT_FALSE(queue.Consume(FillIdentity(0x20000u), Submission(2, 9), &event));
	ComputeColorFillEvents order;
	ASSERT_TRUE(order.Publish(FillIdentity(0x20000u), Submission(8, 4), words));
	EXPECT_FALSE(order.Consume(FillIdentity(0x20000u), Submission(8, 3), &event));
}

TEST(EmulatorComputeColorFill, PendingFillsAreBoundedAndOverlappingWritesRevokeThem)
{
	ComputeColorFillEvents events;
	const std::array<uint32_t, 4> words {0u, 0u, 0u, 0u};
	for (uint32_t i = 0; i < ComputeColorFillEvents::CAPACITY + 8u; ++i)
	{
		ASSERT_TRUE(events.Publish(FillIdentity(0x100000u + i * 0x1000u), Submission(8, 1 + i), words));
	}
	ComputeColorFillEvent event {};
	// The oldest events were evicted; the newest one is still pending.
	EXPECT_FALSE(events.Consume(FillIdentity(0x100000u), Submission(8, 1000), &event));
	const uint64_t newest = 0x100000u + (ComputeColorFillEvents::CAPACITY + 7u) * 0x1000u;
	events.DiscardOverlaps(newest + 0x800u, 4u);
	EXPECT_FALSE(events.Consume(FillIdentity(newest), Submission(8, 1000), &event));
}

TEST(EmulatorComputeColorFill, UniformFillTexelsDecodeOnlyWhenTheQuadRepeatsOneTexel)
{
	VkClearColorValue clear {};
	// Two RGBA16F texels (0, 0, 0, 1): the light-buffer reset.
	ASSERT_TRUE(DecodeGuestUniformFillTexel({0u, 0x3c000000u, 0u, 0x3c000000u}, VK_FORMAT_R16G16B16A16_SFLOAT, &clear));
	EXPECT_FLOAT_EQ(clear.float32[0], 0.0f);
	EXPECT_FLOAT_EQ(clear.float32[3], 1.0f);
	EXPECT_FALSE(DecodeGuestUniformFillTexel({0u, 0x3c000000u, 0u, 0u}, VK_FORMAT_R16G16B16A16_SFLOAT, &clear));
	// Four RGBA8 texels: bytes 80 80 00 ff in memory order.
	ASSERT_TRUE(DecodeGuestUniformFillTexel({0xff008080u, 0xff008080u, 0xff008080u, 0xff008080u}, VK_FORMAT_R8G8B8A8_UNORM, &clear));
	EXPECT_FLOAT_EQ(clear.float32[0], 128.0f / 255.0f);
	EXPECT_FLOAT_EQ(clear.float32[1], 128.0f / 255.0f);
	EXPECT_FLOAT_EQ(clear.float32[2], 0.0f);
	EXPECT_FLOAT_EQ(clear.float32[3], 1.0f);
	ASSERT_TRUE(DecodeGuestUniformFillTexel({0xff008080u, 0xff008080u, 0xff008080u, 0xff008080u}, VK_FORMAT_B8G8R8A8_UNORM, &clear));
	EXPECT_FLOAT_EQ(clear.float32[0], 0.0f);
	EXPECT_FLOAT_EQ(clear.float32[2], 128.0f / 255.0f);
	EXPECT_FALSE(DecodeGuestUniformFillTexel({0xff008080u, 0xff008080u, 0xff008080u, 0u}, VK_FORMAT_R8G8B8A8_UNORM, &clear));
	// sRGB images receive linear input that re-encodes to the stored byte.
	ASSERT_TRUE(DecodeGuestUniformFillTexel({0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}, VK_FORMAT_R8G8B8A8_SRGB, &clear));
	EXPECT_FLOAT_EQ(clear.float32[0], 1.0f);
	ASSERT_TRUE(DecodeGuestUniformFillTexel({0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u}, VK_FORMAT_R8G8B8A8_SRGB, &clear));
	EXPECT_NEAR(clear.float32[0], 0.2158605f, 1e-5f);
	EXPECT_FLOAT_EQ(clear.float32[3], 128.0f / 255.0f);
	// Formats without an evidenced packing are not cleared.
	EXPECT_FALSE(DecodeGuestUniformFillTexel({0u, 0u, 0u, 0u}, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &clear));
}

UT_END();
