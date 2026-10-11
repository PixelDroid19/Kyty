#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderImageGradientProof.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

UT_BEGIN(EmulatorShaderMimg);

using namespace Libs::Graphics;

static void InitMimgParser()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

static ShaderOperand TileVgpr(int reg)
{
	return {.type = ShaderOperandType::Vgpr, .register_id = reg, .size = 1};
}

static ShaderOperand TileSgpr(int reg, int size = 1)
{
	return {.type = ShaderOperandType::Sgpr, .register_id = reg, .size = size};
}

static ShaderOperand TileConstant(uint32_t value)
{
	ShaderOperand operand {.type = ShaderOperandType::IntegerInlineConstant, .size = 1};
	operand.constant.u = value;
	return operand;
}

static ShaderInstruction* FirstWaitcntInstruction(ShaderCode& code)
{
	for (auto& instruction: code.GetInstructions())
	{
		if (instruction.type == ShaderInstructionType::SWaitcnt) { return &instruction; }
	}
	return nullptr;
}

static void ExpectExactWaitcntTuple(const ShaderInstruction& instruction, uint32_t immediate)
{
	EXPECT_EQ(instruction.type, ShaderInstructionType::SWaitcnt);
	EXPECT_EQ(instruction.format, ShaderInstructionFormat::Imm);
	EXPECT_EQ(instruction.sopp_opcode, 0x0cu);
	EXPECT_EQ(instruction.src_num, 1);
	EXPECT_EQ(instruction.dst.type, ShaderOperandType::Unknown);
	EXPECT_EQ(instruction.dst.size, 0);
	EXPECT_EQ(instruction.dst2.type, ShaderOperandType::Unknown);
	EXPECT_EQ(instruction.dst2.size, 0);
	EXPECT_EQ(instruction.src[0].type, ShaderOperandType::LiteralConstant);
	EXPECT_EQ(instruction.src[0].size, 0);
	EXPECT_EQ(instruction.src[0].constant.u, immediate);
	for (int source = 1; source < 4; ++source)
	{
		EXPECT_EQ(instruction.src[source].type, ShaderOperandType::Unknown);
		EXPECT_EQ(instruction.src[source].size, 0);
	}
}

static ShaderCode MakeFourQuadrantTileShader(bool omit_last_store = false, bool read_destination = false)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	auto add = [&code](ShaderInstructionType type, int dst, std::initializer_list<ShaderOperand> sources)
	{
		ShaderInstruction inst {};
		inst.pc      = static_cast<uint32_t>((code.GetInstructions().Size() + 1) * 4);
		inst.type    = type;
		inst.dst     = TileVgpr(dst);
		inst.src_num = static_cast<int>(sources.size());
		int index = 0;
		for (const auto source: sources)
		{
			inst.src[index++] = source;
		}
		code.GetInstructions().Add(inst);
	};
	add(ShaderInstructionType::VLshrrevB32, 1, {TileConstant(3), TileVgpr(0)});
	add(ShaderInstructionType::VBfeU32, 2, {TileVgpr(0), TileConstant(1), TileConstant(3)});
	add(ShaderInstructionType::VAndB32, 1, {TileConstant(6), TileVgpr(1)});
	add(ShaderInstructionType::VAndOrB32, 1, {TileConstant(1), TileVgpr(0), TileVgpr(1)});
	add(ShaderInstructionType::VLshlAddU32, 39, {TileSgpr(14), TileConstant(4), TileVgpr(2)});
	add(ShaderInstructionType::VLshlAddU32, 40, {TileSgpr(15), TileConstant(4), TileVgpr(1)});
	add(ShaderInstructionType::VAddI32, 47, {TileConstant(8), TileVgpr(39)});
	add(ShaderInstructionType::VAddI32, 41, {TileConstant(8), TileVgpr(40)});
	if (read_destination)
	{
		ShaderInstruction load {};
		load.pc       = static_cast<uint32_t>((code.GetInstructions().Size() + 1) * 4);
		load.type     = ShaderInstructionType::ImageLoad;
		load.src[0]   = TileVgpr(39);
		load.src[1]   = TileSgpr(24, 8);
		load.src_num  = 2;
		code.GetInstructions().Add(load);
	}
	const int x[4] = {39, 47, 47, 39};
	const int y[4] = {40, 40, 41, 41};
	for (int index = 0; index < (omit_last_store ? 3 : 4); ++index)
	{
		ShaderInstruction store {};
		store.pc               = static_cast<uint32_t>((code.GetInstructions().Size() + 1) * 4);
		store.type             = ShaderInstructionType::ImageStore;
		store.dst              = TileVgpr(100);
		store.dst.size         = 4;
		store.src[0]           = TileVgpr(x[index]);
		store.src[0].size      = 3;
		store.src[1]           = TileSgpr(24, 8);
		store.src_num          = 2;
		store.mimg_dimension   = 1;
		store.mimg_dmask       = 0xf;
		store.mimg_address_num = 5;
		store.mimg_address[0]  = TileVgpr(x[index]);
		store.mimg_address[1]  = TileVgpr(y[index]);
		code.GetInstructions().Add(store);
	}
	ShaderInstruction end {};
	end.pc   = static_cast<uint32_t>((code.GetInstructions().Size() + 1) * 4);
	end.type = ShaderInstructionType::SEndpgm;
	code.GetInstructions().Add(end);
	return code;
}

static ShaderBindResources FourQuadrantTileBinding(const ShaderCode& code)
{
	ShaderBindResources bind {};
	bind.textures2D.textures_num = 1;
	bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadWrite;
	bind.textures2D.desc[0].textures2d_without_sampler = true;
	// A metadata descriptor can also be consumed through a mapped S_LOAD.
	bind.textures2D.desc[0].dynamic_sload = false;
	bind.textures2D.desc[0].start_register = 16;
	ShaderDynamicSLoadMapping record {};
	record.kind                 = ShaderDynamicSLoadResourceKind::Texture;
	record.resource_index       = 0;
	record.destination_register = 24;
	record.instruction_pc       = 0;
	record.last_consumer_pc     = code.GetInstructions().At(code.GetInstructions().Size() - 2).pc;
	record.dword_count          = 8;
	bind.dynamic_sloads.records.Add(record);
	return bind;
}

TEST(EmulatorShaderMimg, ProvesFourQuadrantStorageOverwriteFromDynamicDescriptor)
{
	const auto code = MakeFourQuadrantTileShader();
	const auto bind = FourQuadrantTileBinding(code);
	const uint32_t threads[3] = {64, 1, 1};
	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 14, threads);
	EXPECT_EQ(coverage.width, 16u);
	EXPECT_EQ(coverage.height, 16u);
}

TEST(EmulatorShaderMimg, RejectsIncompleteOrReadBeforeWriteStorageOverwrite)
{
	const uint32_t threads[3] = {64, 1, 1};
	for (const bool read_destination: {false, true})
	{
		const auto code = MakeFourQuadrantTileShader(!read_destination, read_destination);
		const auto bind = FourQuadrantTileBinding(code);
		const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 14, threads);
		EXPECT_EQ(coverage.width, 0u);
		EXPECT_EQ(coverage.height, 0u);
	}
}

static ShaderCode MakeQuadReductionShader(bool restore_exec = true, bool read_destination = false, bool full_channel_store = true,
                                          bool skipped_scalar_write = false)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	auto add = [&code](ShaderInstructionType type, ShaderOperand dst, std::initializer_list<ShaderOperand> sources)
	{
		ShaderInstruction inst {};
		inst.pc      = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		inst.type    = type;
		inst.dst     = dst;
		inst.src_num = static_cast<int>(sources.size());
		int source_index = 0;
		for (const auto source: sources)
		{
			inst.src[source_index++] = source;
		}
		code.GetInstructions().Add(inst);
	};
	add(ShaderInstructionType::VLshlAddU32, TileVgpr(3), {TileSgpr(15), TileConstant(4), TileVgpr(1)});
	add(ShaderInstructionType::VLshlAddU32, TileVgpr(2), {TileSgpr(14), TileConstant(4), TileVgpr(0)});
	add(ShaderInstructionType::VCmpxGtU32, {}, {TileSgpr(17), TileVgpr(3)});
	const int early_branch_index = code.GetInstructions().Size();
	add(ShaderInstructionType::SCbranchExecz, {}, {TileConstant(0)});
	add(ShaderInstructionType::ImageLoad, TileVgpr(4), {TileVgpr(2), TileSgpr(0, 8)});
	if (skipped_scalar_write)
	{
		add(ShaderInstructionType::SLoadDwordx8, TileSgpr(20, 8), {TileSgpr(12, 2), TileConstant(0)});
	}
	if (restore_exec)
	{
		ShaderOperand exec {.type = ShaderOperandType::ExecLo, .size = 2};
		const uint32_t reset_pc = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		code.GetInstructions()[early_branch_index].src[0].constant.u =
		    reset_pc - (code.GetInstructions().At(early_branch_index).pc + 4);
		add(ShaderInstructionType::SMovB64, exec, {TileConstant(UINT32_MAX)});
	}
	add(ShaderInstructionType::VAndB32, TileVgpr(4), {TileConstant(1), TileVgpr(1)});
	add(ShaderInstructionType::VAndB32, TileVgpr(5), {TileConstant(1), TileVgpr(0)});
	add(ShaderInstructionType::VCmpxEqU32, {}, {TileConstant(0), TileVgpr(4)});
	add(ShaderInstructionType::VCmpxEqU32, {}, {TileConstant(0), TileVgpr(5)});
	const int branch_index = code.GetInstructions().Size();
	add(ShaderInstructionType::SCbranchExecz, {}, {TileConstant(0)});
	add(ShaderInstructionType::VLshrrevB32, TileVgpr(0), {TileConstant(1), TileVgpr(2)});
	add(ShaderInstructionType::VLshrrevB32, TileVgpr(1), {TileConstant(1), TileVgpr(3)});
	add(ShaderInstructionType::SLoadDwordx8, TileSgpr(0, 8), {TileSgpr(12, 2), TileConstant(0)});
	if (read_destination)
	{
		add(ShaderInstructionType::ImageLoad, TileVgpr(6), {TileVgpr(0), TileSgpr(0, 8)});
	}
	ShaderInstruction store {};
	store.pc               = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
	store.type             = ShaderInstructionType::ImageStore;
	store.src[0]           = TileVgpr(0);
	store.src[0].size      = 3;
	store.src[1]           = TileSgpr(0, 8);
	store.src_num          = 2;
	store.mimg_dimension   = 1;
	store.mimg_dmask       = full_channel_store ? 1 : 2;
	code.GetInstructions().Add(store);
	add(ShaderInstructionType::SEndpgm, {}, {});
	code.GetInstructions()[branch_index].src[0].constant.u =
	    code.GetInstructions().At(code.GetInstructions().Size() - 1).pc -
	    (code.GetInstructions().At(branch_index).pc + 4);
	return code;
}

static ShaderBindResources QuadReductionBinding(const ShaderCode& code)
{
	ShaderBindResources bind {};
	bind.textures2D.textures_num = 2;
	bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadOnly;
	bind.textures2D.desc[0].start_register = 0;
	bind.textures2D.desc[1].usage = ShaderTextureUsage::ReadWrite;
	bind.textures2D.desc[1].textures2d_without_sampler = true;
	bind.textures2D.desc[1].texture.fields[1] = 22u << 20u;
	ShaderDynamicSLoadMapping record {};
	record.kind                 = ShaderDynamicSLoadResourceKind::Texture;
	record.resource_index       = 1;
	record.destination_register = 0;
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::SLoadDwordx8 && inst.dst.register_id == 0)
		{
			record.instruction_pc = inst.pc;
			break;
		}
	}
	record.last_consumer_pc     = code.GetInstructions().At(code.GetInstructions().Size() - 2).pc;
	record.dword_count          = 8;
	bind.dynamic_sloads.records.Add(record);
	return bind;
}

TEST(EmulatorShaderMimg, ProvesQuadReductionStorageOverwrite)
{
	const auto code = MakeQuadReductionShader();
	const auto bind = QuadReductionBinding(code);
	const uint32_t threads[3] = {16, 16, 1};
	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, true);
	EXPECT_EQ(coverage.width, 8u);
	EXPECT_EQ(coverage.height, 8u);
}

TEST(EmulatorShaderMimg, RejectsUnprovenQuadReductionStorageOverwrite)
{
	const uint32_t threads[3] = {16, 16, 1};
	for (const auto& code: {MakeQuadReductionShader(false), MakeQuadReductionShader(true, true),
	                       MakeQuadReductionShader(true, false, false), MakeQuadReductionShader(true, false, true, true)})
	{
		const auto bind = QuadReductionBinding(code);
		const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, true);
		EXPECT_EQ(coverage.width, 0u);
	}
}

static ShaderCode MakeBoundedGridStoreShader(bool clobber_coordinate = false, bool read_destination = false)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	auto add = [&code](ShaderInstructionType type, ShaderOperand dst, std::initializer_list<ShaderOperand> sources)
	{
		ShaderInstruction inst {};
		inst.pc      = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		inst.type    = type;
		inst.dst     = dst;
		inst.src_num = static_cast<int>(sources.size());
		int source_index = 0;
		for (const auto source: sources)
		{
			inst.src[source_index++] = source;
		}
		code.GetInstructions().Add(inst);
	};
	ShaderOperand vcc {.type = ShaderOperandType::VccLo, .size = 2};
	ShaderOperand exec {.type = ShaderOperandType::ExecLo, .size = 2};
	add(ShaderInstructionType::SInstPrefetch, {}, {TileConstant(3)});
	add(ShaderInstructionType::VLshlAddU32, TileVgpr(3), {TileSgpr(14), TileConstant(3), TileVgpr(0)});
	add(ShaderInstructionType::VLshlAddU32, TileVgpr(4), {TileSgpr(15), TileConstant(3), TileVgpr(1)});
	add(ShaderInstructionType::SLoadDwordx4, TileSgpr(16, 4), {TileSgpr(12, 2), TileConstant(96)});
	add(ShaderInstructionType::SWaitcnt, {}, {TileConstant(0)});
	add(ShaderInstructionType::SBufferLoadDwordx2, TileSgpr(14, 2), {TileSgpr(16, 4), TileConstant(0)});
	add(ShaderInstructionType::SWaitcnt, {}, {TileConstant(0)});
	add(ShaderInstructionType::VCmpLeU32, TileSgpr(16, 2), {TileSgpr(14), TileVgpr(3)});
	add(ShaderInstructionType::VCmpLeU32, vcc, {TileSgpr(15), TileVgpr(4)});
	add(ShaderInstructionType::SNorB64, vcc, {TileSgpr(16, 2), vcc});
	add(ShaderInstructionType::SMovB64, exec, {vcc});
	const int branch_index = code.GetInstructions().Size();
	add(ShaderInstructionType::SCbranchExecz, {}, {TileConstant(0)});
	add(ShaderInstructionType::SLoadDwordx8, TileSgpr(24, 8), {TileSgpr(12, 2), TileConstant(32)});
	if (clobber_coordinate)
	{
		add(ShaderInstructionType::VMovB32, TileVgpr(3), {TileConstant(0)});
	}
	if (read_destination)
	{
		add(ShaderInstructionType::ImageLoad, TileVgpr(5), {TileVgpr(3), TileSgpr(24, 8)});
	}
	ShaderInstruction store {};
	store.pc               = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
	store.type             = ShaderInstructionType::ImageStore;
	store.src[0]           = TileVgpr(0);
	store.src[0].size      = 2;
	store.src[1]           = TileSgpr(24, 8);
	store.src_num          = 2;
	store.mimg_dimension   = 1;
	store.mimg_dmask       = 3;
	store.mimg_address_num = 3;
	store.mimg_address[0]  = TileVgpr(3);
	store.mimg_address[1]  = TileVgpr(4);
	store.mimg_address[2]  = TileVgpr(5);
	code.GetInstructions().Add(store);
	add(ShaderInstructionType::SEndpgm, {}, {});
	code.GetInstructions()[branch_index].src[0].constant.u =
	    code.GetInstructions().At(code.GetInstructions().Size() - 1).pc - code.GetInstructions().At(branch_index).pc - 4u;
	return code;
}

static ShaderBindResources BoundedGridStoreBinding(const ShaderCode& code)
{
	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num = 1;
	bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadOnly;
	bind.storage_buffers.accesses[0] = ShaderStorageAccess::Raw;
	bind.storage_buffers.sources[0] = ShaderStorageBindingSource::DynamicScalarLoad;
	bind.storage_buffers.code_available[0] = true;
	bind.storage_buffers.exact_matches[0] = true;
	bind.storage_buffers.start_register[0] = 16;
	bind.textures2D.textures_num = 1;
	bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadWrite;
	bind.textures2D.desc[0].textures2d_without_sampler = true;
	bind.textures2D.desc[0].texture.fields[1] = 29u << 20u;
	ShaderDynamicSLoadMapping bounds {};
	bounds.kind = ShaderDynamicSLoadResourceKind::StorageBuffer;
	bounds.resource_index = 0;
	bounds.destination_register = 16;
	bounds.instruction_pc = code.GetInstructions().At(3).pc;
	bounds.last_consumer_pc = code.GetInstructions().At(5).pc;
	bounds.dword_count = 4;
	bind.dynamic_sloads.records.Add(bounds);
	ShaderDynamicSLoadMapping image {};
	image.kind = ShaderDynamicSLoadResourceKind::Texture;
	image.resource_index = 0;
	image.destination_register = 24;
	image.instruction_pc = code.GetInstructions().At(12).pc;
	image.last_consumer_pc = code.GetInstructions().At(code.GetInstructions().Size() - 2).pc;
	image.dword_count = 8;
	bind.dynamic_sloads.records.Add(image);
	return bind;
}

TEST(EmulatorShaderMimg, ProvesBoundedGridStorageOverwriteWithRuntimeBounds)
{
	auto code = MakeBoundedGridStoreShader();
	code.GetInstructions()[1].src[1].size = 0;
	code.GetInstructions()[2].src[1].size = 0;
	code.GetInstructions()[5].src[1].size = 0;
	code.GetInstructions()[13].mimg_address_num = 0;
	code.GetInstructions()[13].src[0] = TileVgpr(3);
	code.GetInstructions()[13].src[0].size = 3;
	const auto bind = BoundedGridStoreBinding(code);
	const uint32_t threads[3] = {8, 8, 1};
	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 14, threads, true, true);
	EXPECT_EQ(coverage.width, 8u);
	EXPECT_EQ(coverage.height, 8u);
	EXPECT_EQ(coverage.bounds_storage_buffer_index, 0);
}

TEST(EmulatorShaderMimg, RejectsUnprovenBoundedGridStorageOverwrite)
{
	const uint32_t threads[3] = {8, 8, 1};
	const auto rejects = [&](const ShaderCode& code, const ShaderBindResources& bind)
	{
		const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 14, threads, true, true);
		EXPECT_EQ(coverage.width, 0u);
	};
	auto code = MakeBoundedGridStoreShader();
	auto bind = BoundedGridStoreBinding(code);
	code.GetInstructions()[7].type = ShaderInstructionType::VCmpLtU32;
	rejects(code, bind);
	code = MakeBoundedGridStoreShader();
	code.GetInstructions()[11].src[0].constant.u -= 4u;
	rejects(code, bind);
	code = MakeBoundedGridStoreShader(true);
	rejects(code, BoundedGridStoreBinding(code));
	code = MakeBoundedGridStoreShader(false, true);
	rejects(code, BoundedGridStoreBinding(code));
	code = MakeBoundedGridStoreShader();
	code.GetInstructions()[13].mimg_dmask = 1;
	rejects(code, bind);
	code = MakeBoundedGridStoreShader();
	code.GetInstructions()[1].src[2].dpp = true;
	rejects(code, bind);
	code = MakeBoundedGridStoreShader();
	bind.storage_buffers.sources[0] = ShaderStorageBindingSource::DirectResource;
	rejects(code, bind);
	bind.storage_buffers.sources[0] = ShaderStorageBindingSource::DynamicScalarLoad;
	bind.dynamic_sloads.records.Clear();
	rejects(MakeBoundedGridStoreShader(), bind);
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, BoundedGridStoreBinding(code), 0, 14, threads, true, false).width,
	          0u);
}

// Sanitized offset-grid shape: origin and extent come from a static raw
// metadata V#, while the write-only R8 target is a separate static T#.
static ShaderCode MakeMetadataOffsetBoundedGridStoreShader(bool overwrite_parameters = false,
                                                           bool overwrite_target = false,
                                                           bool clobber_coordinate = false,
                                                           bool read_target = false,
                                                           bool y_inplace_prelude = false,
                                                           bool restore_exec_before_store = false,
                                                           int optional_coefficient_byte_offset = -1,
                                                           uint32_t tile_shift = 2)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	auto add = [&code](ShaderInstructionType type, ShaderOperand dst, std::initializer_list<ShaderOperand> sources)
	{
		ShaderInstruction inst {};
		inst.pc      = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		inst.type    = type;
		inst.dst     = dst;
		inst.src_num = static_cast<int>(sources.size());
		int source_index = 0;
		for (const auto source: sources) { inst.src[source_index++] = source; }
		code.GetInstructions().Add(inst);
	};
	const ShaderOperand vcc {.type = ShaderOperandType::VccLo, .size = 2};
	auto add_waitcnt = [&code](uint32_t immediate)
	{
		ShaderInstruction wait {};
		wait.pc          = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		wait.type        = ShaderInstructionType::SWaitcnt;
		wait.format      = ShaderInstructionFormat::Imm;
		wait.sopp_opcode = 0x0cu;
		wait.src_num     = 1;
		wait.src[0].type = ShaderOperandType::LiteralConstant;
		wait.src[0].constant.u = immediate;
		code.GetInstructions().Add(wait);
	};
	const int x_register = y_inplace_prelude ? 3 : 10;
	const int y_register = y_inplace_prelude ? 1 : 11;
	if (y_inplace_prelude)
	{
		// Match the observed ordering shape: form Y in place, load the metadata
		// parameters, then form X in a different VGPR.
		add(ShaderInstructionType::VLshlAddU32, TileVgpr(1), {TileSgpr(7), TileConstant(tile_shift), TileVgpr(1)});
	} else
	{
		add(ShaderInstructionType::VLshlAddU32, TileVgpr(x_register), {TileSgpr(6), TileConstant(tile_shift), TileVgpr(0)});
		add(ShaderInstructionType::VLshlAddU32, TileVgpr(y_register), {TileSgpr(7), TileConstant(tile_shift), TileVgpr(1)});
	}
	if (overwrite_parameters) { add(ShaderInstructionType::SMovB32, TileSgpr(12), {TileConstant(0)}); }
	add(ShaderInstructionType::SBufferLoadDwordx4, TileSgpr(20, 4), {TileSgpr(12, 4), TileConstant(16)});
	if (y_inplace_prelude)
	{
		add(ShaderInstructionType::VLshlAddU32, TileVgpr(x_register), {TileSgpr(6), TileConstant(tile_shift), TileVgpr(0)});
	}
	add_waitcnt(0xc07fu);
	add(ShaderInstructionType::VCmpxGtU32, vcc, {TileSgpr(22), TileVgpr(x_register)});
	add(ShaderInstructionType::VCmpxGtU32, vcc, {TileSgpr(23), TileVgpr(y_register)});
	const int branch_index = code.GetInstructions().Size();
	add(ShaderInstructionType::SCbranchExecz, {}, {TileConstant(0)});
	if (restore_exec_before_store)
	{
		const ShaderOperand exec {.type = ShaderOperandType::ExecLo, .size = 2};
		add(ShaderInstructionType::SMovB64, exec, {TileConstant(UINT32_MAX)});
	}
	if (optional_coefficient_byte_offset >= 0)
	{
		add(ShaderInstructionType::SBufferLoadDwordx2, vcc,
		    {TileSgpr(12, 4), TileConstant(static_cast<uint32_t>(optional_coefficient_byte_offset))});
		add_waitcnt(0xc07fu);
	}
	if (overwrite_target) { add(ShaderInstructionType::SMovB32, TileSgpr(40), {TileConstant(0)}); }
	add(ShaderInstructionType::VAddI32, TileVgpr(12), {TileSgpr(20), TileVgpr(x_register)});
	add(ShaderInstructionType::VAddI32, TileVgpr(13), {TileSgpr(21), TileVgpr(y_register)});
	if (clobber_coordinate) { add(ShaderInstructionType::VMovB32, TileVgpr(12), {TileConstant(0)}); }
	if (read_target)
	{
		ShaderInstruction load {};
		load.pc               = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		load.type             = ShaderInstructionType::ImageLoad;
		load.dst              = TileVgpr(14);
		load.src[0]           = TileVgpr(12);
		load.src[0].size      = 2;
		load.src[1]           = TileSgpr(40, 8);
		load.src_num          = 2;
		load.mimg_dimension   = 1;
		load.mimg_dmask       = 1;
		load.mimg_address_num = 2;
		load.mimg_address[0]  = TileVgpr(12);
		load.mimg_address[1]  = TileVgpr(13);
		code.GetInstructions().Add(load);
	}
	// Keep one unrelated typed buffer read in the sanitized data body. The
	// coverage proof must distinguish it from a read of the target image.
	ShaderInstruction typed_read {};
	typed_read.pc                   = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
	typed_read.type                 = ShaderInstructionType::BufferLoadFormatX;
	typed_read.format               = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
	typed_read.dst                  = TileVgpr(18);
	typed_read.src[0]               = TileVgpr(0);
	typed_read.src[1]               = TileSgpr(28, 4);
	typed_read.src[2]               = TileConstant(0);
	typed_read.src_num              = 3;
	typed_read.buffer_idxen         = true;
	typed_read.buffer_imm_offset    = 0;
	code.GetInstructions().Add(typed_read);
	ShaderInstruction store {};
	store.pc               = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
	store.type             = ShaderInstructionType::ImageStore;
	store.src[0]           = TileVgpr(12);
	store.src[0].size      = 2;
	store.src[1]           = TileSgpr(40, 8);
	store.src_num          = 2;
	store.mimg_dimension   = 1;
	store.mimg_dmask       = 0xf;
	store.mimg_address_num = 2;
	store.mimg_address[0]  = TileVgpr(12);
	store.mimg_address[1]  = TileVgpr(13);
	code.GetInstructions().Add(store);
	ShaderInstruction end {};
	end.pc   = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
	end.type = ShaderInstructionType::SEndpgm;
	code.GetInstructions().Add(end);
	code.GetInstructions()[branch_index].src[0].constant.u =
	    code.GetInstructions().At(code.GetInstructions().Size() - 1).pc -
	    code.GetInstructions().At(branch_index).pc - 4u;
	return code;
}

static ShaderBindResources MetadataOffsetBoundedGridStoreBinding()
{
	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num                    = 2;
	bind.storage_buffers.usages[0]                     = ShaderStorageUsage::ReadOnly;
	bind.storage_buffers.accesses[0]                   = ShaderStorageAccess::Typed;
	bind.storage_buffers.sources[0]                    = ShaderStorageBindingSource::DirectResource;
	bind.storage_buffers.code_available[0]             = true;
	bind.storage_buffers.exact_matches[0]              = true;
	bind.storage_buffers.start_register[0]              = 28;
	bind.storage_buffers.buffers[0].fields[0]           = 0x2000u;
	bind.storage_buffers.buffers[0].fields[1]           = 4u << 16u;
	bind.storage_buffers.buffers[0].fields[2]           = 4u;
	bind.storage_buffers.buffers[0].fields[3]           = DstSel(4, 0, 0, 1) | (5u << 12u);
	bind.storage_buffers.usages[1]                     = ShaderStorageUsage::Constant;
	bind.storage_buffers.accesses[1]                   = ShaderStorageAccess::Raw;
	bind.storage_buffers.sources[1]                    = ShaderStorageBindingSource::MetadataSharp;
	bind.storage_buffers.code_available[1]             = true;
	bind.storage_buffers.exact_matches[1]              = true;
	bind.storage_buffers.start_register[1]              = 12;
	bind.storage_buffers.buffers[1].fields[0]           = 0x1000u;
	bind.storage_buffers.buffers[1].fields[1]           = 16u << 16u;
	bind.storage_buffers.buffers[1].fields[2]           = 2u;
	bind.storage_buffers.raw_smem_use[1]                = true;
	bind.storage_buffers.raw_smem_required_bytes[1]      = 32u;
	bind.textures2D.textures_num                        = 1;
	bind.textures2D.desc[0].usage                       = ShaderTextureUsage::ReadWrite;
	bind.textures2D.desc[0].textures2d_without_sampler  = true;
	bind.textures2D.desc[0].start_register              = 40;
	bind.textures2D.desc[0].texture.fields[1]            = (5u << 20u) | ((31u & 3u) << 30u);
	bind.textures2D.desc[0].texture.fields[2]            = (17u << 14u) | (31u >> 2u);
	bind.textures2D.desc[0].texture.fields[3]            = 9u << 28u;
	return bind;
}

static bool ParseSanitizedMetadataOffsetImageStore(uint32_t nsa, uint32_t explicit_y_register, uint32_t dimension,
                                                  ShaderInstruction* store)
{
	if (store == nullptr || nsa > 1u) { return false; }

	// Exercise coordinate representation within the parser's admitted flag
	// subset; this fixture does not establish complete ISA flag conformance.
	std::array<uint32_t, 4> words {};
	words[0] = (0x3cu << 26u) | (0x08u << 18u) | (0xfu << 8u) | (dimension << 3u) | (nsa << 1u);
	words[1] = (10u << 16u) | (8u << 8u) | 4u; // T#40, VDATA v8, VADDR v4.
	uint32_t word_count = 2u;
	if (nsa != 0u)
	{
		words[word_count++] = explicit_y_register | (7u << 8u) | (8u << 16u) | (9u << 24u);
	}
	words[word_count++] = 0xbf810000u; // s_endpgm

	ShaderCode parsed;
	parsed.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words.data(), static_cast<uint32_t>(word_count * sizeof(words[0])), &parsed) ||
	    parsed.GetInstructions().Size() != 2u)
	{
		return false;
	}
	const auto& instruction = parsed.GetInstructions().At(0);
	if (instruction.type != ShaderInstructionType::ImageStore ||
	    parsed.GetInstructions().At(1).type != ShaderInstructionType::SEndpgm ||
	    !ShaderInstructionLoweringPreconditions(instruction))
	{
		return false;
	}
	*store = instruction;
	return true;
}

static bool ReplaceMetadataOffsetGridStoreWithParsedAddress(ShaderCode* code, uint32_t nsa = 0u,
                                                            uint32_t explicit_y_register = 5u,
                                                            uint32_t sequential_y_register = 5u, uint32_t dimension = 1u)
{
	if (code == nullptr || code->GetInstructions().Size() < 2u) { return false; }

	uint32_t x_add_count = 0;
	uint32_t y_add_count = 0;
	for (auto& instruction: code->GetInstructions())
	{
		if (instruction.type != ShaderInstructionType::VAddI32 || instruction.src_num != 2) { continue; }
		if (instruction.src[0].type == ShaderOperandType::Sgpr && instruction.src[0].register_id == 20 &&
		    instruction.src[1].type == ShaderOperandType::Vgpr && instruction.src[1].register_id == 10)
		{
			instruction.dst = TileVgpr(4);
			x_add_count++;
		} else if (instruction.src[0].type == ShaderOperandType::Sgpr && instruction.src[0].register_id == 21 &&
		           instruction.src[1].type == ShaderOperandType::Vgpr && instruction.src[1].register_id == 11)
		{
			instruction.dst = TileVgpr(sequential_y_register);
			y_add_count++;
		}
	}
	if (x_add_count != 1u || y_add_count != 1u) { return false; }

	const uint32_t store_index = code->GetInstructions().Size() - 2u;
	auto& store = code->GetInstructions()[store_index];
	if (store.type != ShaderInstructionType::ImageStore) { return false; }
	const uint32_t store_pc = store.pc;
	ShaderInstruction parsed_store {};
	if (!ParseSanitizedMetadataOffsetImageStore(nsa, explicit_y_register, dimension, &parsed_store)) { return false; }
	parsed_store.pc = store_pc;
	store            = parsed_store;
	return true;
}

TEST(EmulatorShaderMimg, ProvesMetadataOffsetBoundedR8StorageImageCoverage)
{
	auto code = MakeMetadataOffsetBoundedGridStoreShader();
	const auto bind = MetadataOffsetBoundedGridStoreBinding();
	const uint32_t threads[3] = {4, 4, 1};
	const auto* metadata_wait = FirstWaitcntInstruction(code);
	ASSERT_NE(metadata_wait, nullptr);
	ExpectExactWaitcntTuple(*metadata_wait, 0xc07fu);
	const uint32_t store_index = code.GetInstructions().Size() - 2u;
	ASSERT_EQ(bind.textures2D.desc[0].texture.Format(), 5u);
	ASSERT_EQ(bind.textures2D.desc[0].texture.Width5() + 1u, 32u);
	ASSERT_EQ(bind.textures2D.desc[0].texture.Height5() + 1u, 18u);
	ASSERT_EQ(ShaderFindImageStorageTextureDescriptor(code, store_index, bind, 0), 0);
	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
	EXPECT_EQ(coverage.width, 4u);
	EXPECT_EQ(coverage.height, 4u);
	EXPECT_EQ(coverage.bounds_storage_buffer_index, 1);
	EXPECT_EQ(coverage.bounds_byte_offset, 24u);
	EXPECT_EQ(coverage.origin_byte_offset, 16);
}

TEST(EmulatorShaderMimg, ProvesMetadataOffsetCoverageForParsedSequentialImageAddress)
{
	InitMimgParser();
	auto code = MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, false, false, -1, 3);
	ASSERT_TRUE(ReplaceMetadataOffsetGridStoreWithParsedAddress(&code));
	const auto bind = MetadataOffsetBoundedGridStoreBinding();
	const uint32_t threads[3] = {8, 8, 1};
	const uint32_t store_index = code.GetInstructions().Size() - 2u;
	const auto& store = code.GetInstructions().At(store_index);
	ASSERT_EQ(store.type, ShaderInstructionType::ImageStore);
	ASSERT_EQ(store.src_num, 2);
	EXPECT_EQ(store.mimg_address_num, 0);
	EXPECT_EQ(store.mimg_dimension, 1u);
	EXPECT_EQ(store.mimg_dmask, 0xfu);
	ASSERT_EQ(store.src[0].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(store.src[0].register_id, 4);
	EXPECT_EQ(store.src[0].size, 3);
	EXPECT_EQ(store.src[1].type, ShaderOperandType::Sgpr);
	EXPECT_EQ(store.src[1].register_id, 40);
	EXPECT_EQ(store.src[1].size, 8);
	ASSERT_TRUE(ShaderInstructionLoweringPreconditions(store));
	ASSERT_EQ(bind.textures2D.desc[0].texture.Format(), 5u);
	ASSERT_EQ(bind.textures2D.desc[0].texture.Width5() + 1u, 32u);
	ASSERT_EQ(bind.textures2D.desc[0].texture.Height5() + 1u, 18u);
	ASSERT_EQ(ShaderFindImageStorageTextureDescriptor(code, store_index, bind, 0), 0);

	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
	EXPECT_EQ(coverage.width, 8u);
	EXPECT_EQ(coverage.height, 8u);
	EXPECT_EQ(coverage.bounds_storage_buffer_index, 1);
	EXPECT_EQ(coverage.bounds_byte_offset, 24u);
	EXPECT_EQ(coverage.origin_byte_offset, 16);
}

TEST(EmulatorShaderMimg, RejectsMalformedMetadataOffsetParsedImageAddress)
{
	InitMimgParser();
	const auto bind = MetadataOffsetBoundedGridStoreBinding();
	const uint32_t threads[3] = {8, 8, 1};
	const auto rejects = [&](const ShaderCode& code)
	{
		const uint32_t store_index = code.GetInstructions().Size() - 2u;
		ASSERT_TRUE(ShaderInstructionLoweringPreconditions(code.GetInstructions().At(store_index)));
		ASSERT_EQ(ShaderFindImageStorageTextureDescriptor(code, store_index, bind, 0), 0);
		const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
		EXPECT_EQ(coverage.width, 0u);
		EXPECT_EQ(coverage.height, 0u);
	};
	{
		auto code = MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, false, false, -1, 3);
		ASSERT_TRUE(ReplaceMetadataOffsetGridStoreWithParsedAddress(&code));
		const uint32_t store_index = code.GetInstructions().Size() - 2u;
		auto& store = code.GetInstructions()[store_index];
		ASSERT_EQ(store.mimg_address_num, 0);
		ASSERT_EQ(store.src[0].size, 3);
		// An IR-only short span provides X but not the implicit adjacent Y VGPR.
		store.src[0].size = 1;
		ASSERT_TRUE(ShaderInstructionLoweringPreconditions(store));
		rejects(code);
	}
	{
		auto code = MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, false, false, -1, 3);
		ASSERT_TRUE(ReplaceMetadataOffsetGridStoreWithParsedAddress(&code, 1u, 6u));
		const uint32_t store_index = code.GetInstructions().Size() - 2u;
		const auto& store = code.GetInstructions().At(store_index);
		ASSERT_EQ(store.mimg_address_num, 5);
		ASSERT_EQ(store.mimg_address[0].register_id, 4);
		ASSERT_EQ(store.mimg_address[1].register_id, 6);
		rejects(code);
	}
	{
		auto code = MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, false, false, -1, 3);
		ASSERT_TRUE(ReplaceMetadataOffsetGridStoreWithParsedAddress(&code, 0u, 5u, 6u));
		const uint32_t store_index = code.GetInstructions().Size() - 2u;
		const auto& store = code.GetInstructions().At(store_index);
		ASSERT_EQ(store.mimg_address_num, 0);
		ASSERT_EQ(store.src[0].register_id, 4);
		rejects(code);
	}
	{
		auto code = MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, false, false, -1, 3);
		ASSERT_TRUE(ReplaceMetadataOffsetGridStoreWithParsedAddress(&code, 0u, 5u, 5u, 2u));
		const uint32_t store_index = code.GetInstructions().Size() - 2u;
		EXPECT_EQ(code.GetInstructions().At(store_index).mimg_dimension, 2u);
		rejects(code);
	}
	{
		auto code = MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, false, false, -1, 3);
		ASSERT_TRUE(ReplaceMetadataOffsetGridStoreWithParsedAddress(&code));
		const uint32_t store_index = code.GetInstructions().Size() - 2u;
		// MIMG VADDR has no encoded modifier; this models malformed internal IR.
		code.GetInstructions()[store_index].src[0].negate = true;
		rejects(code);
	}
}

TEST(EmulatorShaderMimg, ProvesMetadataOffsetCoverageWithYInplacePreludeOrdering)
{
	const auto code = MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, true);
	const auto bind = MetadataOffsetBoundedGridStoreBinding();
	const uint32_t threads[3] = {4, 4, 1};
	const uint32_t store_index = code.GetInstructions().Size() - 2u;
	ASSERT_EQ(ShaderFindImageStorageTextureDescriptor(code, store_index, bind, 0), 0);
	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
	EXPECT_EQ(coverage.width, 4u);
	EXPECT_EQ(coverage.height, 4u);
	EXPECT_EQ(coverage.bounds_storage_buffer_index, 1);
	EXPECT_EQ(coverage.bounds_byte_offset, 24u);
	EXPECT_EQ(coverage.origin_byte_offset, 16);
}

TEST(EmulatorShaderMimg, ProvesMetadataOffsetCoverageWithOptionalCoefficientLoad)
{
	const auto code = MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, false, false, 0);
	const auto bind = MetadataOffsetBoundedGridStoreBinding();
	const uint32_t threads[3] = {4, 4, 1};
	uint32_t wait_count = 0;
	for (const auto& instruction: code.GetInstructions())
	{
		if (instruction.type == ShaderInstructionType::SWaitcnt)
		{
			ExpectExactWaitcntTuple(instruction, 0xc07fu);
			wait_count++;
		}
	}
	EXPECT_EQ(wait_count, 2u);
	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
	EXPECT_EQ(coverage.width, 4u);
	EXPECT_EQ(coverage.height, 4u);
	EXPECT_EQ(coverage.bounds_storage_buffer_index, 1);
	EXPECT_EQ(coverage.bounds_byte_offset, 24u);
	EXPECT_EQ(coverage.origin_byte_offset, 16);
}

TEST(EmulatorShaderMimg, RejectsOutOfRangeOptionalMetadataCoefficientLoad)
{
	const auto code = MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, false, false, 32);
	const auto bind = MetadataOffsetBoundedGridStoreBinding();
	const uint32_t threads[3] = {4, 4, 1};
	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
	EXPECT_EQ(coverage.width, 0u);
	EXPECT_EQ(coverage.height, 0u);
}

TEST(EmulatorShaderMimg, RejectsMetadataWaitWithLgkmcntOne)
{
	auto code = MakeMetadataOffsetBoundedGridStoreShader();
	const auto bind = MetadataOffsetBoundedGridStoreBinding();
	const uint32_t threads[3] = {4, 4, 1};
	auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
	ASSERT_EQ(coverage.width, 4u);
	ASSERT_EQ(coverage.height, 4u);
	auto* metadata_wait = FirstWaitcntInstruction(code);
	ASSERT_NE(metadata_wait, nullptr);
	ExpectExactWaitcntTuple(*metadata_wait, 0xc07fu);
	metadata_wait->src[0].constant.u = 0xc17fu;
	ExpectExactWaitcntTuple(*metadata_wait, 0xc17fu);
	coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
	EXPECT_EQ(coverage.width, 0u);
	EXPECT_EQ(coverage.height, 0u);
}

TEST(EmulatorShaderMimg, RejectsMetadataWaitWithLgkmcntFifteen)
{
	auto code = MakeMetadataOffsetBoundedGridStoreShader();
	const auto bind = MetadataOffsetBoundedGridStoreBinding();
	const uint32_t threads[3] = {4, 4, 1};
	auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
	ASSERT_EQ(coverage.width, 4u);
	ASSERT_EQ(coverage.height, 4u);
	auto* metadata_wait = FirstWaitcntInstruction(code);
	ASSERT_NE(metadata_wait, nullptr);
	ExpectExactWaitcntTuple(*metadata_wait, 0xc07fu);
	metadata_wait->src[0].constant.u = 0xcf70u;
	ExpectExactWaitcntTuple(*metadata_wait, 0xcf70u);
	coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
	EXPECT_EQ(coverage.width, 0u);
	EXPECT_EQ(coverage.height, 0u);
}

TEST(EmulatorShaderMimg, RejectsUnprovenMetadataOffsetBoundedR8Coverage)
{
	const uint32_t threads[3] = {4, 4, 1};
	const auto rejects = [&](const ShaderCode& code, const ShaderBindResources& bind)
	{
		const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 0, 6, threads, true, true);
		EXPECT_EQ(coverage.width, 0u);
		EXPECT_EQ(coverage.height, 0u);
	};
	{
		auto code = MakeMetadataOffsetBoundedGridStoreShader();
		for (auto& inst: code.GetInstructions())
		{
			if (inst.type == ShaderInstructionType::VCmpxGtU32) { inst.type = ShaderInstructionType::VCmpGtU32; }
		}
		rejects(code, MetadataOffsetBoundedGridStoreBinding());
	}
	rejects(MakeMetadataOffsetBoundedGridStoreShader(false, false, true), MetadataOffsetBoundedGridStoreBinding());
	rejects(MakeMetadataOffsetBoundedGridStoreShader(true), MetadataOffsetBoundedGridStoreBinding());
	{
		auto code = MakeMetadataOffsetBoundedGridStoreShader(false, true);
		const auto bind = MetadataOffsetBoundedGridStoreBinding();
		EXPECT_EQ(ShaderFindImageStorageTextureDescriptor(code, code.GetInstructions().Size() - 2u, bind, 0), -1);
		rejects(code, bind);
	}
	rejects(MakeMetadataOffsetBoundedGridStoreShader(false, false, false, true), MetadataOffsetBoundedGridStoreBinding());
	{
		auto code = MakeMetadataOffsetBoundedGridStoreShader();
		code.GetInstructions()[code.GetInstructions().Size() - 2].mimg_dmask = 0u;
		rejects(code, MetadataOffsetBoundedGridStoreBinding());
	}
	// The metadata V# must still be the resource bound at the shader's load
	// register; matching descriptor bytes alone do not establish provenance.
	{
		auto bind = MetadataOffsetBoundedGridStoreBinding();
		bind.storage_buffers.start_register[1]++;
		rejects(MakeMetadataOffsetBoundedGridStoreShader(), bind);
	}
	// Restoring EXEC after the bounds guard re-enables lanes that the guard
	// excluded, so the store no longer proves the analyzed tile coverage.
	rejects(MakeMetadataOffsetBoundedGridStoreShader(false, false, false, false, false, true),
	        MetadataOffsetBoundedGridStoreBinding());
}

static ShaderCode MakePairedSplitStoreShader(bool omit_first_store = false, bool clobber_coordinate = false,
                                            bool read_destination = false, bool partial_store = false)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	auto add = [&code](ShaderInstructionType type, ShaderOperand dst, std::initializer_list<ShaderOperand> sources)
	{
		ShaderInstruction inst {};
		inst.pc      = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		inst.type    = type;
		inst.dst     = dst;
		inst.src_num = static_cast<int>(sources.size());
		int source_index = 0;
		for (const auto source: sources) { inst.src[source_index++] = source; }
		code.GetInstructions().Add(inst);
	};
	ShaderOperand vcc {.type = ShaderOperandType::VccLo, .size = 2};
	ShaderOperand exec {.type = ShaderOperandType::ExecLo, .size = 2};
	add(ShaderInstructionType::VAndB32, TileVgpr(5), {TileConstant(4), TileVgpr(0)});
	add(ShaderInstructionType::VLshlrevB32, TileVgpr(2), {TileConstant(2), TileVgpr(1)});
	add(ShaderInstructionType::VBfeU32, TileVgpr(4), {TileVgpr(0), TileConstant(1), TileConstant(1)});
	add(ShaderInstructionType::VAndB32, TileVgpr(3), {TileConstant(1), TileVgpr(0)});
	add(ShaderInstructionType::VAndB32, TileVgpr(1), {TileConstant(6), TileVgpr(1)});
	add(ShaderInstructionType::VLshrrevB32, TileVgpr(5), {TileConstant(1), TileVgpr(5)});
	add(ShaderInstructionType::VAndB32, TileVgpr(2), {TileConstant(4), TileVgpr(2)});
	add(ShaderInstructionType::VLshlAddU32, TileVgpr(6), {TileSgpr(15), TileConstant(3), TileVgpr(4)});
	add(ShaderInstructionType::VLshlAddU32, TileVgpr(0), {TileSgpr(14), TileConstant(3), TileVgpr(5)});
	add(ShaderInstructionType::VAddI32, TileVgpr(26), {TileVgpr(6), TileVgpr(1)});
	add(ShaderInstructionType::VAdd3U32, TileVgpr(25), {TileVgpr(0), TileVgpr(2), TileVgpr(3)});
	add(ShaderInstructionType::VMovB32, TileVgpr(27), {TileConstant(1)});
	add(ShaderInstructionType::VCmpLtF32, vcc, {TileSgpr(24), TileConstant(1)});
	const int early_branch = code.GetInstructions().Size();
	add(ShaderInstructionType::SCbranchVccz, {}, {TileConstant(0)});
	add(ShaderInstructionType::ImageSampleLz, TileVgpr(0), {TileVgpr(6), TileSgpr(0, 8), TileSgpr(8, 4)});
	const int split = code.GetInstructions().Size();
	add(ShaderInstructionType::VCmpxNeqF32, vcc, {TileConstant(0), TileVgpr(27)});
	const int first_branch = code.GetInstructions().Size();
	add(ShaderInstructionType::SCbranchExecz, {}, {TileConstant(0)});
	add(ShaderInstructionType::SLoadDwordx8, TileSgpr(0, 8), {TileSgpr(12, 2), TileConstant(64)});
	if (clobber_coordinate) { add(ShaderInstructionType::VMovB32, TileVgpr(25), {TileConstant(0)}); }
	if (read_destination) { add(ShaderInstructionType::ImageLoad, TileVgpr(4), {TileVgpr(25), TileSgpr(0, 8)}); }
	auto store = [&code, &partial_store](bool first)
	{
		ShaderInstruction inst {};
		inst.pc             = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		inst.type           = ShaderInstructionType::ImageStore;
		inst.src[0]         = TileVgpr(25);
		inst.src[0].size    = 3;
		inst.src[1]         = TileSgpr(0, 8);
		inst.src_num        = 2;
		inst.mimg_dimension = 1;
		inst.mimg_dmask     = first && partial_store ? 0x3 : 0xf;
		code.GetInstructions().Add(inst);
	};
	if (!omit_first_store) { store(true); }
	const int invert = code.GetInstructions().Size();
	add(ShaderInstructionType::SNotB64, exec, {exec});
	const int second_branch = code.GetInstructions().Size();
	add(ShaderInstructionType::SCbranchExecz, {}, {TileConstant(0)});
	add(ShaderInstructionType::SLoadDwordx8, TileSgpr(0, 8), {TileSgpr(12, 2), TileConstant(64)});
	store(false);
	const int end = code.GetInstructions().Size();
	add(ShaderInstructionType::SEndpgm, {}, {});
	for (const auto [from, to]: {std::pair {early_branch, split}, std::pair {first_branch, invert},
	                            std::pair {second_branch, end}})
	{
		code.GetInstructions()[from].src[0].constant.u =
		    code.GetInstructions().At(to).pc - code.GetInstructions().At(from).pc - 4u;
	}
	return code;
}

static ShaderBindResources PairedSplitStoreBinding(const ShaderCode& code)
{
	ShaderBindResources bind {};
	bind.textures2D.textures_num = 2;
	bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadOnly;
	bind.textures2D.desc[0].start_register = 0;
	bind.textures2D.desc[1].usage = ShaderTextureUsage::ReadWrite;
	bind.textures2D.desc[1].textures2d_without_sampler = true;
	bind.textures2D.desc[1].start_register = 32;
	bind.textures2D.desc[1].texture.fields[1] = 36u << 20u;
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		if (code.GetInstructions().At(index).type != ShaderInstructionType::SLoadDwordx8) { continue; }
		ShaderDynamicSLoadMapping record {};
		record.kind = ShaderDynamicSLoadResourceKind::Texture;
		record.resource_index = 1;
		record.destination_register = 0;
		record.instruction_pc = code.GetInstructions().At(index).pc;
		record.dword_count = 8;
		for (uint32_t next = index + 1; next < code.GetInstructions().Size(); ++next)
		{
			if (code.GetInstructions().At(next).type == ShaderInstructionType::ImageStore)
			{
				record.last_consumer_pc = code.GetInstructions().At(next).pc;
				break;
			}
		}
		if (record.last_consumer_pc != 0u) { bind.dynamic_sloads.records.Add(record); }
	}
	return bind;
}

TEST(EmulatorShaderMimg, ProvesPairedSplitStorageOverwrite)
{
	const auto code = MakePairedSplitStoreShader();
	const auto bind = PairedSplitStoreBinding(code);
	const uint32_t threads[3] = {8, 8, 1};
	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, true);
	EXPECT_EQ(coverage.width, 8u);
	EXPECT_EQ(coverage.height, 8u);
}

TEST(EmulatorShaderMimg, RejectsUnprovenPairedSplitStorageOverwrite)
{
	const uint32_t threads[3] = {8, 8, 1};
	for (const auto& code: {MakePairedSplitStoreShader(true), MakePairedSplitStoreShader(false, true),
	                       MakePairedSplitStoreShader(false, false, true), MakePairedSplitStoreShader(false, false, false, true)})
	{
		EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, PairedSplitStoreBinding(code), 1, 14, threads,
		                                               false, true, true).width, 0u);
	}
	auto code = MakePairedSplitStoreShader();
	auto bind = PairedSplitStoreBinding(code);
	for (auto& inst: code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::SCbranchVccz) { inst.src[0].constant.u -= 4u; }
	}
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, true).width, 0u);
	code = MakePairedSplitStoreShader();
	bind = PairedSplitStoreBinding(code);
	code.GetInstructions()[6].src[0] = TileConstant(0);
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, true).width, 0u);
	code = MakePairedSplitStoreShader();
	bind = PairedSplitStoreBinding(code);
	for (auto& inst: code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::SNotB64) { inst.type = ShaderInstructionType::SMovB64; }
	}
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, true).width, 0u);
	code = MakePairedSplitStoreShader();
	bind = PairedSplitStoreBinding(code);
	bind.textures2D.desc[0].start_register = 16;
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, true).width, 0u);
	code = MakePairedSplitStoreShader();
	bind = PairedSplitStoreBinding(code);
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, false).width, 0u);
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, false, true).width, 0u);
}

static ShaderCode MakePairedLinearStoreShader(bool omit_store = false, bool read_destination = false, bool partial_store = false)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	auto add = [&code](ShaderInstructionType type, ShaderOperand dst, std::initializer_list<ShaderOperand> sources)
	{
		ShaderInstruction inst {};
		inst.pc      = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		inst.type    = type;
		inst.dst     = dst;
		inst.src_num = static_cast<int>(sources.size());
		int source_index = 0;
		for (const auto source: sources) { inst.src[source_index++] = source; }
		code.GetInstructions().Add(inst);
	};
	add(ShaderInstructionType::VAndB32, TileVgpr(2), {TileConstant(4), TileVgpr(0)});
	add(ShaderInstructionType::VBfeU32, TileVgpr(5), {TileVgpr(0), TileConstant(1), TileConstant(1)});
	add(ShaderInstructionType::VLshlrevB32, TileVgpr(4), {TileConstant(2), TileVgpr(1)});
	add(ShaderInstructionType::VAndB32, TileVgpr(1), {TileConstant(6), TileVgpr(1)});
	add(ShaderInstructionType::VAndB32, TileVgpr(3), {TileConstant(1), TileVgpr(0)});
	add(ShaderInstructionType::VLshrrevB32, TileVgpr(2), {TileConstant(1), TileVgpr(2)});
	add(ShaderInstructionType::VLshlAddU32, TileVgpr(6), {TileSgpr(15), TileConstant(3), TileVgpr(5)});
	add(ShaderInstructionType::VAndB32, TileVgpr(5), {TileConstant(4), TileVgpr(4)});
	add(ShaderInstructionType::VAddI32, TileVgpr(19), {TileVgpr(6), TileVgpr(1)});
	add(ShaderInstructionType::VLshlAddU32, TileVgpr(6), {TileSgpr(14), TileConstant(3), TileVgpr(2)});
	add(ShaderInstructionType::SLoadDwordx4, TileSgpr(16, 4), {TileSgpr(12, 2), TileConstant(32)});
	add(ShaderInstructionType::SBufferLoadDwordx2, TileSgpr(14, 2), {TileSgpr(16, 4), TileConstant(0)});
	add(ShaderInstructionType::VAdd3U32, TileVgpr(20), {TileVgpr(6), TileVgpr(5), TileVgpr(3)});
	add(ShaderInstructionType::ImageSampleLz, TileVgpr(0), {TileVgpr(6), TileSgpr(0, 8), TileSgpr(8, 4)});
	add(ShaderInstructionType::SLoadDwordx8, TileSgpr(28, 8), {TileSgpr(12, 2), TileConstant(0)});
	if (read_destination) { add(ShaderInstructionType::ImageLoad, TileVgpr(4), {TileVgpr(20), TileSgpr(28, 8)}); }
	if (!omit_store)
	{
		ShaderInstruction store {};
		store.pc               = static_cast<uint32_t>(code.GetInstructions().Size() * 4);
		store.type             = ShaderInstructionType::ImageStore;
		store.src[0]           = TileVgpr(20);
		store.src[0].size      = 3;
		store.src[1]           = TileSgpr(28, 8);
		store.src_num          = 2;
		store.mimg_dimension   = 1;
		store.mimg_dmask       = partial_store ? 0x3 : 0xf;
		store.mimg_address_num = 3;
		store.mimg_address[0]  = TileVgpr(20);
		store.mimg_address[1]  = TileVgpr(19);
		code.GetInstructions().Add(store);
	}
	add(ShaderInstructionType::SEndpgm, {}, {});
	return code;
}

static ShaderBindResources PairedLinearStoreBinding(const ShaderCode& code)
{
	ShaderBindResources bind {};
	bind.textures2D.textures_num = 2;
	bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadOnly;
	bind.textures2D.desc[0].start_register = 0;
	bind.textures2D.desc[1].usage = ShaderTextureUsage::ReadWrite;
	bind.textures2D.desc[1].textures2d_without_sampler = true;
	bind.textures2D.desc[1].start_register = 16;
	bind.textures2D.desc[1].texture.fields[1] = 36u << 20u;
	ShaderDynamicSLoadMapping mapping {};
	mapping.kind = ShaderDynamicSLoadResourceKind::Texture;
	mapping.resource_index = 1;
	mapping.destination_register = 28;
	mapping.dword_count = 8;
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::SLoadDwordx8) { mapping.instruction_pc = inst.pc; }
		if (inst.type == ShaderInstructionType::ImageStore) { mapping.last_consumer_pc = inst.pc; }
	}
	if (mapping.last_consumer_pc != 0u) { bind.dynamic_sloads.records.Add(mapping); }
	return bind;
}

TEST(EmulatorShaderMimg, ProvesPairedLinearStorageOverwrite)
{
	const auto code = MakePairedLinearStoreShader();
	const auto bind = PairedLinearStoreBinding(code);
	const uint32_t threads[3] = {8, 8, 1};
	const auto coverage = AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, true);
	EXPECT_EQ(coverage.width, 8u);
	EXPECT_EQ(coverage.height, 8u);
}

TEST(EmulatorShaderMimg, RejectsUnprovenPairedLinearStorageOverwrite)
{
	const uint32_t threads[3] = {8, 8, 1};
	for (const auto& code: {MakePairedLinearStoreShader(true), MakePairedLinearStoreShader(false, true),
	                       MakePairedLinearStoreShader(false, false, true)})
	{
		EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, PairedLinearStoreBinding(code), 1, 14, threads,
		                                               false, true, true).width, 0u);
	}
	auto code = MakePairedLinearStoreShader();
	auto bind = PairedLinearStoreBinding(code);
	code.GetInstructions()[0].src[0] = TileConstant(0);
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, true).width, 0u);
	code = MakePairedLinearStoreShader();
	bind = PairedLinearStoreBinding(code);
	code.GetInstructions()[code.GetInstructions().Size() - 2].mimg_address[1] = TileVgpr(20);
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, true).width, 0u);
	code = MakePairedLinearStoreShader();
	bind = PairedLinearStoreBinding(code);
	code.GetInstructions()[code.GetInstructions().Size() - 2].mimg_dmask = 1;
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(code, bind, 1, 14, threads, false, true, true).width, 0u);
	EXPECT_EQ(AnalyzeShaderStorageImageTileCoverage(MakePairedLinearStoreShader(), PairedLinearStoreBinding(code), 1, 14,
	                                               threads, false, true, false).width, 0u);
}

static ShaderCode MakeUniformZeroStoreGate()
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderOperand zero {.type = ShaderOperandType::IntegerInlineConstant};
	zero.constant.u = 0u;
	ShaderOperand offset {.type = ShaderOperandType::IntegerInlineConstant};
	offset.constant.u = 64u;
	ShaderOperand vcc {.type = ShaderOperandType::VccLo, .size = 2};
	ShaderOperand exec {.type = ShaderOperandType::ExecLo, .size = 2};
	ShaderInstruction load {};
	load.pc      = 0u;
	load.type    = ShaderInstructionType::SBufferLoadDword;
	load.format  = ShaderInstructionFormat::SdstSvSoffset;
	load.smem_flags = 0u;
	load.dst     = TileSgpr(22);
	load.src[0]  = TileSgpr(16, 4);
	load.src[1]  = offset;
	load.src_num = 2;
	code.GetInstructions().Add(load);
	ShaderInstruction compare {};
	compare.pc      = 4u;
	compare.type    = ShaderInstructionType::VCmpxGtU32;
	compare.format  = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
	compare.dst     = vcc;
	compare.src[0]  = TileSgpr(22);
	compare.src[1]  = zero;
	compare.src_num = 2;
	code.GetInstructions().Add(compare);
	ShaderInstruction branch {};
	branch.pc      = 8u;
	branch.type    = ShaderInstructionType::SCbranchExecz;
	branch.src[0]  = zero;
	branch.src_num = 1;
	code.GetInstructions().Add(branch);
	ShaderInstruction restore {};
	restore.pc      = 12u;
	restore.type    = ShaderInstructionType::SMovB64;
	restore.dst     = exec;
	restore.src[0]  = vcc;
	restore.src_num = 1;
	code.GetInstructions().Add(restore);
	ShaderInstruction store {};
	store.pc   = 16u;
	store.type = ShaderInstructionType::ImageStore;
	code.GetInstructions().Add(store);
	ShaderInstruction end {};
	end.pc   = 20u;
	end.type = ShaderInstructionType::SEndpgm;
	code.GetInstructions().Add(end);
	return code;
}

static ShaderBindResources UniformZeroStoreGateBinding()
{
	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num       = 1;
	bind.storage_buffers.start_register[0] = 16;
	bind.storage_buffers.sources[0]        = ShaderStorageBindingSource::MetadataSharp;
	bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadOnly;
	bind.storage_buffers.code_available[0] = true;
	bind.storage_buffers.exact_matches[0]  = true;
	return bind;
}

TEST(EmulatorShaderMimg, ProvesUniformZeroGateSuppressesEveryStore)
{
	const auto code = MakeUniformZeroStoreGate();
	const auto bind = UniformZeroStoreGateBinding();
	const auto gate = AnalyzeShaderComputeEmptyGate(code, bind);
	EXPECT_EQ(gate.storage_buffer_index, 0);
	EXPECT_EQ(gate.byte_offset, 64u);
	auto dynamic_bind = bind;
	dynamic_bind.storage_buffers.sources[0] = ShaderStorageBindingSource::DynamicScalarLoad;
	EXPECT_EQ(AnalyzeShaderComputeEmptyGate(code, dynamic_bind).storage_buffer_index, 0);
}

TEST(EmulatorShaderMimg, RejectsStorePathsOutsideUniformZeroGate)
{
	const auto bind = UniformZeroStoreGateBinding();
	{
		const auto original = MakeUniformZeroStoreGate();
		ShaderCode code;
		code.SetType(ShaderType::Compute);
		ShaderInstruction bypass {};
		bypass.pc      = 0u;
		bypass.type    = ShaderInstructionType::SCbranchExecz;
		bypass.src[0].type = ShaderOperandType::IntegerInlineConstant;
		bypass.src[0].constant.u = 16u; // branch to the shifted ImageStore
		bypass.src_num = 1;
		code.GetInstructions().Add(bypass);
		for (auto inst: original.GetInstructions())
		{
			inst.pc += 4u;
			code.GetInstructions().Add(inst);
		}
		EXPECT_EQ(AnalyzeShaderComputeEmptyGate(code, bind).storage_buffer_index, -1);
	}
	{
		auto code = MakeUniformZeroStoreGate();
		code.GetInstructions()[3].src[0] = TileConstant(UINT32_MAX);
		EXPECT_EQ(AnalyzeShaderComputeEmptyGate(code, bind).storage_buffer_index, -1);
	}
	{
		auto code = MakeUniformZeroStoreGate();
		code.GetInstructions()[3].dst = TileSgpr(20, 2);
		EXPECT_EQ(AnalyzeShaderComputeEmptyGate(code, bind).storage_buffer_index, -1);
	}
	{
		auto code = MakeUniformZeroStoreGate();
		code.GetInstructions()[4].type = ShaderInstructionType::BufferStoreDword;
		EXPECT_EQ(AnalyzeShaderComputeEmptyGate(code, bind).storage_buffer_index, -1);
	}
	{
		auto code = MakeUniformZeroStoreGate();
		code.GetInstructions()[1].src[1].constant.u = 1u;
		EXPECT_EQ(AnalyzeShaderComputeEmptyGate(code, bind).storage_buffer_index, -1);
	}
	{
		auto code = MakeUniformZeroStoreGate();
		auto bad_bind = bind;
		bad_bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadWrite;
		EXPECT_EQ(AnalyzeShaderComputeEmptyGate(code, bad_bind).storage_buffer_index, -1);
	}
	{
		const auto code = MakeUniformZeroStoreGate();
		auto bad_bind = bind;
		bad_bind.storage_buffers.exact_matches[0] = false;
		EXPECT_EQ(AnalyzeShaderComputeEmptyGate(code, bad_bind).storage_buffer_index, -1);
	}
}

TEST(EmulatorShaderMimg, RejectsGen5MimgExtendedOpcodeInsteadOfAliasingImageLoad)
{
	// GFX10 MIMG encodes OP[7] in word zero bit zero, not beside OP[6:0].
	const uint32_t shader[] = {(0x3cu << 26u) | (0xfu << 8u) | 1u, 0u, 0xbf810000u};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(rejected_exit), "");
}

TEST(EmulatorShaderMimg, ParsesBvhNsaOperandsWithoutTextureOrPaddingRegisters)
{
	// Synthetic GFX10 BVH packet: four outputs, eleven independent address
	// registers, one 128-bit descriptor and two unused NSA padding bytes.
	const uint32_t shader[] = {(0x3cu << 26u) | (0x66u << 18u) | 0x9f07u,
	                           (6u << 16u) | (20u << 8u) | 8u,
	                           0x100e0c0au, 0x18161412u, 0xfffe1c1au, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    const auto& inst = code.GetInstructions().At(0);
		    if (inst.dst.register_id != 20 || inst.dst.size != 4 || inst.src_num != 2 ||
		        inst.src[1].register_id != 24 || inst.src[1].size != 4 || inst.mimg_address_num != 11)
		    {
			    std::_Exit(2);
		    }
		    for (int i = 0; i < 11; ++i)
		    {
			    if (inst.mimg_address[i].register_id != 8 + 2 * i) { std::_Exit(3); }
		    }
		    if (code.GetInstructions().At(1).pc != 20) { std::_Exit(4); }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, ParsesContiguousBvhSourcesThroughLastVgpr)
{
	const uint32_t shader[] = {(0x3cu << 26u) | (0x66u << 18u) | 0x9f01u,
	                           (25u << 16u) | (252u << 8u) | 245u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    const auto& inst = code.GetInstructions().At(0);
		    if (inst.type != ShaderInstructionType::ImageBvhIntersectRay || inst.mimg_address_num != 11 ||
		        inst.dst.register_id != 252 || inst.dst.size != 4 || code.GetInstructions().At(1).pc != 8)
		    {
			    std::_Exit(2);
		    }
		    for (int i = 0; i < 11; ++i)
		    {
			    if (inst.mimg_address[i].register_id != 245 + i) { std::_Exit(3); }
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, ParsesTwoDimensionalOffsetSampleWithRedOnlyNsaResult)
{
	const uint32_t shader[] = {(0x3cu << 26u) | (0x37u << 18u) | (1u << 8u) | (1u << 3u) | (1u << 1u),
	                           (5u << 21u) | (2u << 16u) | 4u, 0x08070605u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    ShaderCode code;
		    code.SetType(ShaderType::Pixel);
		    ShaderParse(shader, &code);
		    const auto& inst = code.GetInstructions().At(0);
		    if (inst.type != ShaderInstructionType::ImageSampleLzO ||
		        inst.format != ShaderInstructionFormat::Vdata1Vaddr4StSsDmask1 || inst.dst.size != 1 ||
		        inst.mimg_dmask != 1 || inst.mimg_dimension != 1 || inst.mimg_address_num != 5 ||
		        inst.mimg_address[0].register_id != 4 || inst.mimg_address[1].register_id != 5 ||
		        inst.mimg_address[2].register_id != 6 || inst.mimg_address[3].register_id != 7)
		    {
			    std::_Exit(2);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, ParsesTwoDimensionalOffsetSampleWithGreenOnlyNsaResult)
{
	const uint32_t shader[] = {(0x3cu << 26u) | (0x37u << 18u) | (2u << 8u) | (1u << 3u) | (1u << 1u),
	                           (5u << 21u) | (2u << 16u) | 4u, 0x08070605u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    ShaderCode code;
		    code.SetType(ShaderType::Pixel);
		    ShaderParse(shader, &code);
		    const auto& inst = code.GetInstructions().At(0);
		    if (inst.type != ShaderInstructionType::ImageSampleLzO ||
		        inst.format == ShaderInstructionFormat::Unknown || inst.dst.size != 1 ||
		        inst.mimg_dmask != 2 || inst.mimg_dimension != 1 || inst.mimg_address_num != 5)
		    {
			    std::_Exit(2);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, EmitsOffsetSampleWithOnlyTheSelectedRedDestination)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    ShaderInstruction sample {};
		    sample.type             = ShaderInstructionType::ImageSampleLzO;
		    sample.format           = ShaderInstructionFormat::Vdata1Vaddr4StSsDmask1;
		    sample.dst              = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
		    sample.src[0]           = {.type = ShaderOperandType::Vgpr, .register_id = 4, .size = 4};
		    sample.src[1]           = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 8};
		    sample.src[2]           = {.type = ShaderOperandType::Sgpr, .register_id = 20, .size = 4};
		    sample.src_num          = 3;
		    sample.mimg_dimension   = 1;
		    sample.mimg_dmask       = 1;
		    sample.mimg_address_num = 5;
		    for (int address = 0; address < 5; ++address)
		    {
			    sample.mimg_address[address] = {.type = ShaderOperandType::Vgpr, .register_id = 4 + address, .size = 1};
		    }
		    ShaderInstruction end {};
		    end.type   = ShaderInstructionType::SEndpgm;
		    end.format = ShaderInstructionFormat::Empty;
		    ShaderCode code;
		    code.SetType(ShaderType::Pixel);
		    code.GetInstructions().Add(sample);
		    code.GetInstructions().Add(end);
		    ShaderPixelInputInfo input {};
		    input.bind.push_constant_size                   = 48;
		    input.bind.textures2D.textures_num              = 1;
		    input.bind.textures2D.textures2d_sampled_num    = 1;
		    input.bind.textures2D.desc[0].start_register    = 8;
		    input.bind.textures2D.desc[0].usage             = ShaderTextureUsage::ReadOnly;
		    input.bind.textures2D.desc[0].texture.fields[1] = 22u << 20u;
		    input.bind.textures2D.desc[0].texture.fields[3] = 9u << 28u;
		    input.bind.samplers.samplers_num                = 1;
		    input.bind.samplers.start_register[0]           = 20;
		    ShaderCalcBindingIndices(&input.bind);
		    const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
		    if (source.FindIndex("OpImageSampleExplicitLod") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpBitFieldSExtract") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpStore %v0") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpStore %v1") != Core::STRING8_INVALID_INDEX)
		    {
			    std::_Exit(2);
		    }
		    code.GetInstructions()[0].format     = ShaderInstructionFormat::VdataVaddr4StSsMimgDmask;
		    code.GetInstructions()[0].mimg_dmask = 2;
		    const auto green_source               = SpirvGenerateSource(code, nullptr, &input, nullptr);
		    if (green_source.FindIndex("OpCompositeExtract %float %t43_0 1") == Core::STRING8_INVALID_INDEX ||
		        green_source.FindIndex("OpCompositeExtract %float %t43_0 0") != Core::STRING8_INVALID_INDEX ||
		        green_source.FindIndex("OpStore %v0") == Core::STRING8_INVALID_INDEX ||
		        green_source.FindIndex("OpStore %v1") != Core::STRING8_INVALID_INDEX)
		    {
			    std::_Exit(3);
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

namespace {

#if defined(_WIN32)
constexpr int kMimgRejectedExit = 321;
#else
constexpr int kMimgRejectedExit = 65;
#endif

void RequireMimgTail(bool condition, const char* reason)
{
	if (!condition)
	{
		std::fprintf(stderr, "MIMG operand-tail control: %s\n", reason);
		std::_Exit(2);
	}
}

void CaptureMimgRejectionDiagnostic()
{
	// Core EXIT writes to stdout; gtest matches the death-test child's stderr.
	std::fflush(stdout);
#if defined(_WIN32)
	const int redirected = ::_dup2(::_fileno(stderr), ::_fileno(stdout));
#else
	const int redirected = ::dup2(::fileno(stderr), ::fileno(stdout));
#endif
	RequireMimgTail(redirected >= 0, "redirect parser diagnostic to the death-test matcher");
}

bool EmptyMimgOperand(const ShaderOperand& operand)
{
	return operand == ShaderOperand {} && operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp;
}

ShaderCode ParseMimgTailInstruction(uint32_t opcode, uint32_t ssamp = 0u, uint32_t nsa = 0u, uint32_t dmask = 0xfu,
                                   ShaderType stage = ShaderType::Pixel, uint32_t flags = 0u)
{
	std::array<uint32_t, 6> words = {
	    (0x3cu << 26u) | (opcode << 18u) | (dmask << 8u) | (1u << 3u) | (nsa << 1u) | flags,
	    (ssamp << 21u) | (8u << 16u) | (8u << 8u) | 4u,
	    0xfffd0911u, 0x17130f0bu, 0x27231f1bu, 0u};
	words[2u + nsa] = 0xbf810000u;
	ShaderCode code;
	code.SetType(stage);
	ShaderParse(words.data(), (3u + nsa) * sizeof(uint32_t), &code);
	return code;
}

ShaderCode ParseCdMimgInstruction(uint32_t nsa = 2u, uint32_t dmask = 0xau, uint32_t dimension = 1u,
                                  uint32_t flags = 0u, uint32_t word1_flags = 0u,
                                  ShaderType stage = ShaderType::Pixel, bool next_gen = true, uint32_t ssamp = 5u,
                                  uint32_t vaddr = 30u)
{
	Config::SetNextGen(next_gen);
	std::array<uint32_t, 6> words = {
	    (0x3cu << 26u) | (0x68u << 18u) | (dmask << 8u) | (dimension << 3u) | (nsa << 1u) | flags,
	    (ssamp << 21u) | (8u << 16u) | (64u << 8u) | (vaddr & 0xffu) | word1_flags,
	    32u | (31u << 8u) | (33u << 16u) | (7u << 24u),
	    42u | (60u << 8u) | (61u << 16u) | (62u << 24u),
	    0xbf810000u, 0u};
	words[2u + nsa] = 0xbf810000u;
	ShaderCode code;
	code.SetType(stage);
	ShaderParse(words.data(), (3u + nsa) * sizeof(uint32_t), &code);
	return code;
}

static ShaderInstruction MimgGradientWqm()
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

static ShaderInstruction MimgGradientMove(int destination, ShaderOperand source)
{
	ShaderInstruction instruction {};
	instruction.type        = ShaderInstructionType::VMovB32;
	instruction.format      = ShaderInstructionFormat::SVdstSVsrc0;
	instruction.dst         = TileVgpr(destination);
	instruction.src[0]      = source;
	instruction.src_num     = 1;
	return instruction;
}

static ShaderInstruction MimgGradientDppMove(int destination, int source, uint16_t control)
{
	auto instruction                  = MimgGradientMove(destination, TileVgpr(source));
	instruction.src[0].dpp            = true;
	instruction.src[0].dpp_ctrl       = control;
	instruction.src[0].dpp_row_mask   = 0x0fu;
	instruction.src[0].dpp_bank_mask  = 0x0fu;
	instruction.src[0].dpp_bound_ctrl = true;
	return instruction;
}

static bool ParseSanitizedDppInstruction(bool vop2, uint32_t destination, uint32_t source, uint32_t source1,
                                         uint16_t control, ShaderInstruction* instruction)
{
	if (instruction == nullptr) { return false; }

	constexpr uint32_t unmodeled_bits = (1u << 17u) | (1u << 20u) | (1u << 23u);
	const uint32_t dpp_word = (source & 0xffu) | (static_cast<uint32_t>(control & 0x1ffu) << 8u) | (1u << 19u) |
	                         unmodeled_bits | (0xfu << 24u) | (0xfu << 28u);
	const uint32_t word = vop2 ? ((0x04u << 25u) | ((destination & 0xffu) << 17u) | ((source1 & 0xffu) << 9u) | 250u)
	                           : ((0x3fu << 25u) | ((destination & 0xffu) << 17u) | (0x01u << 9u) | 250u);
	const uint32_t words[] = {word, dpp_word, 0xbf810000u};
	ShaderCode       parsed;
	parsed.SetType(ShaderType::Pixel);
	if (!ShaderTryParseBounded(words, sizeof(words), &parsed) || parsed.GetInstructions().Size() != 2u) { return false; }

	const auto& decoded = parsed.GetInstructions().At(0);
	if (decoded.type != (vop2 ? ShaderInstructionType::VSubF32 : ShaderInstructionType::VMovB32) ||
	    decoded.dst.type != ShaderOperandType::Vgpr || decoded.dst.register_id != static_cast<int>(destination) ||
	    decoded.src[0].type != ShaderOperandType::Vgpr || decoded.src[0].register_id != static_cast<int>(source) ||
	    decoded.src[0].dpp_unmodeled_bits != unmodeled_bits)
	{
		return false;
	}
	*instruction = decoded;
	return true;
}

static ShaderInstruction MimgGradientSubtract(int destination, int base, int offset, uint16_t control)
{
	ShaderInstruction instruction {};
	instruction.type                  = ShaderInstructionType::VSubF32;
	instruction.format                = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	instruction.dst                   = TileVgpr(destination);
	instruction.src[0]                = TileVgpr(base);
	instruction.src[0].dpp            = true;
	instruction.src[0].dpp_ctrl       = control;
	instruction.src[0].dpp_row_mask   = 0x0fu;
	instruction.src[0].dpp_bank_mask  = 0x0fu;
	instruction.src[0].dpp_bound_ctrl = true;
	instruction.src[1]                = TileVgpr(offset);
	instruction.src_num               = 2;
	return instruction;
}

static ShaderCode MakeParsedCdGradientShader()
{
	const auto parsed = ParseCdMimgInstruction(2u, 0xau);
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	auto append = [&code](ShaderInstruction instruction)
	{
		instruction.pc = static_cast<uint32_t>(code.GetInstructions().Size() * 4u);
		code.GetInstructions().Add(instruction);
	};
	append(MimgGradientWqm());
	append(MimgGradientMove(7, TileSgpr(16)));
	append(MimgGradientMove(42, TileSgpr(17)));
	append(MimgGradientDppMove(20, 7, 0x00u));
	append(MimgGradientDppMove(24, 42, 0xffu));
	append(MimgGradientSubtract(30, 7, 20, 0x55u));
	append(MimgGradientSubtract(31, 7, 20, 0xaau));
	append(MimgGradientSubtract(32, 42, 24, 0x55u));
	append(MimgGradientSubtract(33, 42, 24, 0xaau));
	auto sample = parsed.GetInstructions().At(0);
	append(sample);
	ShaderInstruction end {};
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	append(end);
	return code;
}

static bool CdGradientProofRejectsParsedDpp(ShaderInstruction replacement, uint32_t instruction_index)
{
	auto code = MakeParsedCdGradientShader();
	if (instruction_index >= code.GetInstructions().Size() - 2u) { return false; }
	const auto sample_index = code.GetInstructions().Size() - 2u;
	if (!ShaderImageSampleCdHasQuadUniformGradients(code, sample_index)) { return false; }
	replacement.pc = code.GetInstructions().At(instruction_index).pc;
	code.GetInstructions()[instruction_index] = replacement;
	return !ShaderImageSampleCdHasQuadUniformGradients(code, sample_index);
}

void EnableMimgModuleValidation()
{
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation;
	Config::Load(validation);
	RequireMimgTail(Config::ShaderValidationEnabled(), "full-module validation must be enabled");
}

Core::String8 EmitValidatedMimgTailModule(const ShaderCode& code, bool writable, bool sampler = false)
{
	ShaderPixelInputInfo pixel {};
	ShaderComputeInputInfo compute {};
	compute.threads_num[0] = compute.threads_num[1] = compute.threads_num[2] = 1;
	auto& bind = code.GetType() == ShaderType::Pixel ? pixel.bind : compute.bind;
	bind.textures2D.textures_num = 1;
	bind.textures2D.textures2d_sampled_num = writable ? 0 : 1;
	bind.textures2D.textures2d_storage_num = writable ? 1 : 0;
	auto& descriptor = bind.textures2D.desc[0];
	descriptor.start_register = 32;
	descriptor.usage = writable ? ShaderTextureUsage::ReadWrite : ShaderTextureUsage::ReadOnly;
	descriptor.textures2d_without_sampler = writable;
	descriptor.texture.fields[0] = 0x100u;
	descriptor.texture.fields[1] = 22u << 20u; // raw R32_FLOAT, independent of the sampler field
	descriptor.texture.fields[3] = (9u << 28u) | 4u | (5u << 3u) | (6u << 6u) | (7u << 9u);
	if (sampler)
	{
		bind.samplers.samplers_num = 1;
		bind.samplers.start_register[0] = 20;
	}
	ShaderCalcBindingIndices(&bind);
	const auto source = code.GetType() == ShaderType::Pixel ? SpirvGenerateSource(code, nullptr, &pixel, nullptr)
	                                                       : SpirvGenerateSource(code, nullptr, nullptr, &compute);
	RequireMimgTail(!source.IsEmpty(), "parsed production source is empty");
	Vector<uint32_t> binary;
	Core::String8 error;
	RequireMimgTail(Config::ShaderValidationEnabled(), "validation remains enabled at the toolchain boundary");
	const bool compiled = ShaderToolchain::Run(source, &binary, &error);
	RequireMimgTail(compiled, error.c_str());
	RequireMimgTail(!binary.IsEmpty(), "validated binary is empty");
	return source;
}

} // namespace

TEST(EmulatorShaderMimg, ParsesGen5SampleCdWithTwoDimensionalNsaAddressTuple)
{
	// Sanitized DIM=1 (2D), DMASK=R IMAGE_SAMPLE_CD fields. The two NSA dwords
	// encode nine register fields, while this 2D derivative sample consumes the
	// first six in ISA order: dx/dh, dy/dh, dx/dv, dy/dv, x, y.
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    const uint32_t shader[] = {
		        (0x3cu << 26u) | (0x68u << 18u) | (1u << 8u) | (1u << 3u) | (2u << 1u),
		        (5u << 21u) | (8u << 16u) | (16u << 8u) | 4u,
		        7u | (13u << 8u) | (19u << 16u) | (29u << 24u),
		        37u | (41u << 8u) | (43u << 16u) | (47u << 24u),
		        0xbf810000u};
		    ShaderCode code;
		    code.SetType(ShaderType::Pixel);
		    RequireMimgTail(ShaderTryParseBounded(shader, sizeof(shader), &code), "bounded parse accepts IMAGE_SAMPLE_CD");
		    RequireMimgTail(code.GetInstructions().Size() == 2u, "sample and terminal instruction");
		    const auto& sample = code.GetInstructions().At(0);
		    RequireMimgTail(sample.type != ShaderInstructionType::Unknown && sample.mimg_dimension == 1u && sample.mimg_dmask == 1u,
		                    "recognized 2D sample with a red-only result");
		    RequireMimgTail(sample.dst.type == ShaderOperandType::Vgpr && sample.dst.register_id == 16 && sample.dst.size == 1,
		                    "DMASK selects exactly one destination VGPR");
		    RequireMimgTail(sample.src_num == 3 && sample.src[0].type == ShaderOperandType::Vgpr &&
		                    sample.src[0].register_id == 4 && sample.src[0].size == 1,
		                    "the NSA-form encoded base is one VGPR; six logical addresses live in the explicit tuple");
		    RequireMimgTail(sample.src[1].type == ShaderOperandType::Sgpr && sample.src[1].register_id == 32 &&
		                    sample.src[1].size == 8,
		                    "T# resource base and eight-SGPR descriptor are retained");
		    RequireMimgTail(sample.src[2].type == ShaderOperandType::Sgpr && sample.src[2].register_id == 20 &&
		                    sample.src[2].size == 4,
		                    "S# sampler base and four-SGPR descriptor are retained");
		    const int expected_addresses[] = {4, 7, 13, 19, 29, 37};
		    RequireMimgTail(sample.mimg_address_num == 9, "both NSA dwords retain all nine encoded address fields");
		    for (int index = 0; index < 6; ++index)
		    {
			    RequireMimgTail(sample.mimg_address[index].type == ShaderOperandType::Vgpr &&
			                    sample.mimg_address[index].register_id == expected_addresses[index],
			                    "logical derivative and coordinate register order is retained");
		    }
		    RequireMimgTail(code.GetInstructions().At(1).pc == 16u, "terminal PC follows both NSA dwords");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, ParsesGen5SampleCdWithSequentialSixAddressSpan)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    const auto code = ParseCdMimgInstruction(0u, 0xau);
		    RequireMimgTail(code.GetInstructions().Size() == 2u, "sample and terminal instruction");
		    const auto& sample = code.GetInstructions().At(0);
		    RequireMimgTail(sample.type == ShaderInstructionType::ImageSampleCd &&
		                    sample.format == ShaderInstructionFormat::VdataVaddr6StSsMimgDmask && sample.mimg_dimension == 1u &&
		                    sample.mimg_dmask == 0xau && sample.dst.size == 2 && sample.src[0].register_id == 30 &&
		                    sample.src[0].size == 6 && sample.mimg_address_num == 0,
		                    "regular 2D CD retains its sequential six-register address span and packed DMASK");
		    const auto& sampler = sample.src[2];
		    RequireMimgTail(sampler.type == ShaderOperandType::Sgpr && sampler.register_id == 20 && sampler.size == 4,
		                    "ordinary sampled-image S# remains four SGPRs");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, ParsesSampleCdUpperSsampBitWithinOrdinarySgprRange)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    for (const uint32_t ssamp: {16u, 25u})
		    {
			    const auto code = ParseCdMimgInstruction(2u, 0xau, 1u, 0u, 0u, ShaderType::Pixel, true, ssamp);
			    const auto& sample = code.GetInstructions().At(0);
			    RequireMimgTail(sample.type == ShaderInstructionType::ImageSampleCd && sample.src[2].type == ShaderOperandType::Sgpr &&
			                    sample.src[2].register_id == static_cast<int>(ssamp * 4u) && sample.src[2].size == 4,
			                    "SSAMP's upper bit maps within the ordinary four-SGPR sampler range");
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, KeepsSparseNsaTupleValidAtHighestVaddr)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    const auto code = ParseCdMimgInstruction(2u, 0xau, 1u, 0u, 0u, ShaderType::Pixel, true, 5u, 255u);
		    const auto& sample = code.GetInstructions().At(0);
		    const int expected_addresses[] = {255, 32, 31, 33, 7, 42};
		    RequireMimgTail(sample.type == ShaderInstructionType::ImageSampleCd && sample.src[0].type == ShaderOperandType::Vgpr &&
		                    sample.src[0].register_id == 255 && sample.src[0].size == 1 && sample.mimg_address_num == 9,
		                    "NSA's base VGPR at the register-file boundary remains a single valid operand");
		    for (int address = 0; address < 6; ++address)
		    {
			    RequireMimgTail(sample.mimg_address[address].type == ShaderOperandType::Vgpr &&
			                    sample.mimg_address[address].register_id == expected_addresses[address],
					            "sparse NSA registers supply the six logical addresses without a contiguous overflow");
		    }
		    RequireMimgTail(ShaderInstructionLoweringPreconditions(sample),
					            "all encoded and logical operands pass shared lowering preconditions");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, RejectsUnsupportedSampleCdModifiers)
{
	for (const uint32_t flags: {1u << 25u, 1u << 17u, 1u << 16u, 1u << 15u, 1u << 14u, 1u << 13u, 1u << 12u, 1u << 6u,
	                            1u << 7u})
	{
		ASSERT_EXIT(
		    {
			    InitMimgParser();
			    CaptureMimgRejectionDiagnostic();
			    (void)ParseCdMimgInstruction(2u, 0xau, 1u, flags);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(kMimgRejectedExit), "");
	}
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    CaptureMimgRejectionDiagnostic();
		    (void)ParseCdMimgInstruction(2u, 0xau, 1u, 0u, 1u << 31u);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(kMimgRejectedExit), "");
}

TEST(EmulatorShaderMimg, RejectsNonTwoDimensionalSampleCd)
{
	for (const uint32_t dimension: {0u, 2u, 3u, 4u, 5u, 6u, 7u})
	{
		ASSERT_EXIT(
		    {
			    InitMimgParser();
			    CaptureMimgRejectionDiagnostic();
			    (void)ParseCdMimgInstruction(2u, 0xau, dimension);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(kMimgRejectedExit), "");
	}
}

TEST(EmulatorShaderMimg, RejectsLegacySampleCd)
{
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    CaptureMimgRejectionDiagnostic();
		    (void)ParseCdMimgInstruction(0u, 0xau, 1u, 0u, 0u, ShaderType::Pixel, false);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(kMimgRejectedExit), "");
}

TEST(EmulatorShaderMimg, RejectsSampleCdAddressTupleTooShortForTwoDimensionalGradients)
{
	ASSERT_EXIT(
	    {
		    InitMimgParser();
		    CaptureMimgRejectionDiagnostic();
		    (void)ParseCdMimgInstruction(1u, 0xau);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(kMimgRejectedExit), "");
}

TEST(EmulatorShaderMimg, EmitsAndValidatesParsedCdWithOrderedGradientsAndPackedDmask)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    EnableMimgModuleValidation();
		    const auto code = MakeParsedCdGradientShader();
		    const auto sample_index = code.GetInstructions().Size() - 2u;
		    const auto& sample = code.GetInstructions().At(sample_index);
		    RequireMimgTail(sample.type == ShaderInstructionType::ImageSampleCd && sample.mimg_dimension == 1u &&
		                    sample.mimg_dmask == 0xau && sample.dst.register_id == 64 && sample.dst.size == 2,
		                    "parsed CD carries the nontrivial two-component DMASK destination span");
		    const int expected_addresses[] = {30, 32, 31, 33, 7, 42};
		    RequireMimgTail(sample.mimg_address_num == 9 && sample.src[0].size == 1,
		                    "the two NSA dwords retain the CD six-address span plus three unused slots");
		    for (int address = 0; address < 6; ++address)
		    {
			    RequireMimgTail(sample.mimg_address[address].register_id == expected_addresses[address],
			                    "each parsed address register is distinct and in CD operand order");
		    }
		    const auto source = EmitValidatedMimgTailModule(code, false, true);
		    const auto tag = Core::String8::FromPrintf("%u", sample_index);
		    const auto expected_grad_x = Core::String8::FromPrintf(
		        "%%sample_cd_grad_x_%s = OpCompositeConstruct %%v2float %%sample_cd_address_%s_0 %%sample_cd_address_%s_1",
		        tag.c_str(), tag.c_str(), tag.c_str());
		    const auto expected_grad_y = Core::String8::FromPrintf(
		        "%%sample_cd_grad_y_%s = OpCompositeConstruct %%v2float %%sample_cd_address_%s_2 %%sample_cd_address_%s_3",
		        tag.c_str(), tag.c_str(), tag.c_str());
		    const auto expected_coordinates = Core::String8::FromPrintf(
		        "%%sample_cd_coordinate_%s = OpCompositeConstruct %%v2float %%sample_cd_address_%s_4 %%sample_cd_address_%s_5",
		        tag.c_str(), tag.c_str(), tag.c_str());
		    const auto expected_sample = Core::String8::FromPrintf(
		        "%%sample_cd_value_%s = OpImageSampleExplicitLod %%v4float %%sample_cd_sampled_%s %%sample_cd_coordinate_%s "
		        "Grad %%sample_cd_grad_x_%s %%sample_cd_grad_y_%s",
		        tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str());
		    RequireMimgTail(source.ContainsStr(expected_grad_x.c_str()) && source.ContainsStr(expected_grad_y.c_str()) &&
		                    source.ContainsStr(expected_coordinates.c_str()) && source.ContainsStr(expected_sample.c_str()),
		                    "validated SPIR-V source supplies dx/dh, dy/dh, dx/dv, dy/dv in the exact Grad vector order");
		    for (int address = 0; address < 6; ++address)
		    {
			    const auto load = Core::String8::FromPrintf("%%sample_cd_address_%s_%d = OpLoad %%float %%v%d", tag.c_str(), address,
			                                               expected_addresses[address]);
			    RequireMimgTail(source.ContainsStr(load.c_str()), "Grad vectors load the exact parsed NSA VGPR operands");
		    }
		    const auto green_extract = Core::String8::FromPrintf(
		        "%%sample_cd_component_%s_1 = OpCompositeExtract %%float %%sample_cd_value_%s 1", tag.c_str(), tag.c_str());
		    const auto alpha_extract = Core::String8::FromPrintf(
		        "%%sample_cd_component_%s_3 = OpCompositeExtract %%float %%sample_cd_value_%s 3", tag.c_str(), tag.c_str());
		    const auto exec_active = Core::String8::FromPrintf(
		        "%%image_exec_active_%s = OpINotEqual %%bool %%image_exec_value_%s %%uint_0", tag.c_str(), tag.c_str());
		    const auto green_old = Core::String8::FromPrintf("%%image_exec_old_%s_0 = OpLoad %%float %%v64", tag.c_str());
		    const auto alpha_old = Core::String8::FromPrintf("%%image_exec_old_%s_1 = OpLoad %%float %%v65", tag.c_str());
		    const auto green_select = Core::String8::FromPrintf(
		        "%%image_exec_value_%s_0 = OpSelect %%float %%image_exec_active_%s %%sample_cd_component_%s_1 %%image_exec_old_%s_0",
		        tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str());
		    const auto alpha_select = Core::String8::FromPrintf(
		        "%%image_exec_value_%s_1 = OpSelect %%float %%image_exec_active_%s %%sample_cd_component_%s_3 %%image_exec_old_%s_1",
		        tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str());
		    const auto green_store = Core::String8::FromPrintf("OpStore %%v64 %%image_exec_value_%s_0", tag.c_str());
		    const auto alpha_store = Core::String8::FromPrintf("OpStore %%v65 %%image_exec_value_%s_1", tag.c_str());
		    const auto red_component = Core::String8::FromPrintf("%%sample_cd_component_%s_0", tag.c_str());
		    const auto blue_component = Core::String8::FromPrintf("%%sample_cd_component_%s_2", tag.c_str());
		    RequireMimgTail(source.ContainsStr(green_extract.c_str()) && source.ContainsStr(alpha_extract.c_str()) &&
		                    source.ContainsStr(exec_active.c_str()) && source.ContainsStr(green_old.c_str()) &&
		                    source.ContainsStr(alpha_old.c_str()) && source.ContainsStr(green_select.c_str()) &&
		                    source.ContainsStr(alpha_select.c_str()) && source.ContainsStr(green_store.c_str()) &&
		                    source.ContainsStr(alpha_store.c_str()) &&
		                    !source.ContainsStr(red_component.c_str()) && !source.ContainsStr(blue_component.c_str()),
		                    "DMASK G+A packs selected channels into consecutive VGPRs while preserving inactive EXEC lanes");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, RetainsUnmodeledDppBitsAndRejectsCdGradientProof)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    constexpr uint32_t unmodeled_bits = (1u << 17u) | (1u << 20u) | (1u << 23u);
		    ShaderInstruction vop1_dpp {};
		    ShaderInstruction vop2_dpp {};
		    RequireMimgTail(ParseSanitizedDppInstruction(false, 20u, 7u, 0u, 0x00u, &vop1_dpp),
		                    "sanitized VOP1 DPP word parses into its instruction tuple");
		    RequireMimgTail(ParseSanitizedDppInstruction(true, 30u, 7u, 20u, 0x55u, &vop2_dpp),
		                    "sanitized VOP2 DPP word parses into its instruction tuple");
		    RequireMimgTail(vop1_dpp.src[0].dpp && vop1_dpp.src[0].dpp_ctrl == 0x00u &&
		                    vop1_dpp.src[0].dpp_unmodeled_bits == unmodeled_bits,
		                    "VOP1 preserves reserved bits from the second DPP dword");
		    RequireMimgTail(vop2_dpp.src[0].dpp && vop2_dpp.src[0].dpp_ctrl == 0x55u &&
		                    vop2_dpp.src[0].dpp_unmodeled_bits == unmodeled_bits &&
		                    vop2_dpp.src[1].type == ShaderOperandType::Vgpr && vop2_dpp.src[1].register_id == 20,
		                    "VOP2 preserves reserved bits and its ordinary second source");
		    RequireMimgTail(CdGradientProofRejectsParsedDpp(vop1_dpp, 3u),
		                    "the CD proof rejects a parsed VOP1 broadcast with unmodeled controls");
		    RequireMimgTail(CdGradientProofRejectsParsedDpp(vop2_dpp, 5u),
		                    "the CD proof rejects a parsed VOP2 subtract with unmodeled controls");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, ParsesSamplerlessMimgWithExactEmptySourceTail)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    for (uint32_t opcode: {0x00u, 0x01u, 0x08u, 0x09u, 0x0eu})
		    {
			    for (uint32_t nsa = 0u; nsa <= 3u; ++nsa)
			    {
				    const auto code = ParseMimgTailInstruction(opcode, 0u, nsa);
				    RequireMimgTail(code.GetInstructions().Size() == 2u, "MIMG and terminal instruction");
				    const auto& inst = code.GetInstructions().At(0);
				    const auto expected_type = opcode == 0x08u ? ShaderInstructionType::ImageStore :
				                               opcode == 0x09u ? ShaderInstructionType::ImageStoreMip :
				                               opcode == 0x0eu ? ShaderInstructionType::ImageGetResinfo : ShaderInstructionType::ImageLoad;
				    const auto expected_format = opcode == 0x09u ? ShaderInstructionFormat::Vdata4Vaddr4StDmaskF :
				                                 opcode == 0x0eu ? ShaderInstructionFormat::VdataVaddrStDmask :
				                                                   ShaderInstructionFormat::VdataVaddr3StDmask;
				    RequireMimgTail(inst.type == expected_type && inst.format == expected_format &&
				                   inst.mimg_explicit_lod == (opcode == 0x01u), "operation identity and explicit level are retained");
				    RequireMimgTail(inst.dst.type == ShaderOperandType::Vgpr && inst.dst.register_id == 8 && inst.dst.size == 4,
				                   "four-component data tuple is retained");
				    RequireMimgTail(inst.src_num == 2 && EmptyMimgOperand(inst.src[2]) && EmptyMimgOperand(inst.src[3]),
				                   "samplerless op has exactly address/resource sources and a fully empty tail");
				    RequireMimgTail(inst.src[0].type == ShaderOperandType::Vgpr && inst.src[0].register_id == 4 &&
				                   inst.src[0].size == (opcode == 0x0eu ? 1 : opcode == 0x09u ? 4 : 3), "address tuple is retained");
				    RequireMimgTail(inst.src[1].type == ShaderOperandType::Sgpr && inst.src[1].register_id == 32 &&
				                   inst.src[1].size == 8, "resource remains eight SGPRs");
				    RequireMimgTail(inst.mimg_address_num == (nsa == 0u ? 0 : static_cast<int>(1u + 4u * nsa)),
				                   "encoded NSA slots are retained");
				    RequireMimgTail(code.GetInstructions().At(1).pc == 4u * (2u + nsa), "next instruction PC is retained");
				    RequireMimgTail(ShaderInstructionLoweringPreconditions(inst), "parsed samplerless instruction passes the unchanged validator");
				    auto malformed = inst;
				    malformed.src[2] = TileSgpr(0);
				    RequireMimgTail(!ShaderInstructionLoweringPreconditions(malformed), "live source beyond src_num still fails");
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, PreservesSamplerlessNsaSlotsAndFollowingLiteral)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    const uint32_t words[] = {
		        (0x3cu << 26u) | (0x01u << 18u) | (0x03u << 8u) | (1u << 3u) | (3u << 1u),
		        (8u << 16u) | (8u << 8u) | 4u, 0xfffd0911u, 0x17130f0bu, 0x27231f1bu,
		        (0x3fu << 25u) | (12u << 17u) | (1u << 9u) | 255u, 0x89abcdefu, 0xbf810000u};
		    ShaderCode code;
		    code.SetType(ShaderType::Pixel);
		    ShaderParse(words, sizeof(words), &code);
		    RequireMimgTail(code.GetInstructions().Size() == 3u, "NSA, literal move and terminator");
		    const auto& image = code.GetInstructions().At(0);
		    const int addresses[] = {4, 17, 9, 253, 255, 11, 15, 19, 23, 27, 31, 35, 39};
		    RequireMimgTail(image.mimg_address_num == 13 && image.raw_word == words[0], "raw word and NSA length are retained");
		    for (int index = 0; index < 13; ++index)
		    {
			    RequireMimgTail(image.mimg_address[index] == TileVgpr(addresses[index]), "every NSA slot, including padding, is retained");
		    }
		    const auto& move = code.GetInstructions().At(1);
		    RequireMimgTail(move.pc == 20u && move.type == ShaderInstructionType::VMovB32 && move.src_num == 1 &&
		                   move.src[0].type == ShaderOperandType::LiteralConstant && move.src[0].size == 0 &&
		                   move.src[0].constant.u == 0x89abcdefu, "following literal remains in its original operand slot");
		    const auto& end = code.GetInstructions().At(2);
		    RequireMimgTail(end.pc == 28u && end.raw_word == 0xbf810000u && end.src_num == 0 &&
		                   end.src[0].type == ShaderOperandType::LiteralConstant && end.src[0].size == 0 && end.src[0].constant.u == 0u,
		                   "canonical ENDPGM literal padding remains intact");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, PreservesRealSamplerOperandsOnSampleAndGatherOpcodes)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    for (uint32_t opcode: {0x20u, 0x24u, 0x25u, 0x27u, 0x2fu, 0x37u, 0x47u})
		    {
			    for (uint32_t ssamp: {0u, 5u})
			    {
				    for (uint32_t nsa: {0u, 1u})
				    {
					    const auto code = ParseMimgTailInstruction(opcode, ssamp, nsa, 1u);
					    const auto& inst = code.GetInstructions().At(0);
					    RequireMimgTail(inst.src_num == 3 && inst.src[1] == TileSgpr(32, 8) &&
					                   inst.src[2] == TileSgpr(static_cast<int>(ssamp * 4u), 4) && EmptyMimgOperand(inst.src[3]),
					                   "sample/gather retains the actual four-SGPR sampler, including SSAMP zero");
					    RequireMimgTail(ShaderInstructionLoweringPreconditions(inst), "sampled operand tuple remains valid");
				    }
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, PreservesAtomicDataSourceWithoutSamplerOperand)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    for (uint32_t glc: {0u, 1u})
		    {
			    const auto code = ParseMimgTailInstruction(0x11u, 0u, 0u, 1u, ShaderType::Compute, glc << 13u);
			    const auto& inst = code.GetInstructions().At(0);
			    RequireMimgTail(inst.type == ShaderInstructionType::ImageAtomicAdd && inst.src_num == 3 &&
			                   inst.src[1] == TileSgpr(32, 8) && inst.src[2] == TileVgpr(8) && EmptyMimgOperand(inst.src[3]) &&
			                   inst.mimg_return_old_value == (glc != 0u), "atomic's third source is real VGPR data, not SSAMP");
			    RequireMimgTail(ShaderInstructionLoweringPreconditions(inst), "atomic data source passes the unchanged validator");
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, RejectsUnprovenNonzeroSsampOnSamplerlessOpcodes)
{
	for (uint32_t opcode: {0x00u, 0x01u, 0x08u, 0x09u, 0x0eu, 0x11u})
	{
		for (uint32_t ssamp: {1u, 2u, 4u, 8u, 16u, 31u})
		{
			const uint32_t dmask = opcode == 0x11u ? 1u : 0xfu;
			const auto diagnostic = Core::String8::FromPrintf(
			    "unsupported samplerless MIMG SSAMP: opcode=0x%02x ssamp=0x%02x pc=0x00000000 "
			    "word0=0x%08x word1=0x%08x; only SSAMP=0 is admitted",
			    static_cast<unsigned>(opcode), static_cast<unsigned>(ssamp),
			    static_cast<unsigned>(0xf0000008u | (opcode << 18u) | (dmask << 8u)),
			    static_cast<unsigned>(0x00080804u | (ssamp << 21u)));
			ASSERT_EXIT(
			    {
				    InitMimgParser();
				    CaptureMimgRejectionDiagnostic();
				    (void)ParseMimgTailInstruction(opcode, ssamp, 0u, dmask, ShaderType::Compute);
				    std::_Exit(0);
			    },
			    ::testing::ExitedWithCode(kMimgRejectedExit), diagnostic.c_str());
		}
	}
}

TEST(EmulatorShaderMimg, RetainsReservedGen5MimgBit14Rejection)
{
	// RDNA2 Table 100 does not assign bit 14. The existing DA guard rejects
	// it before decoding operands, for both samplerless and sampled opcodes.
	for (uint32_t opcode: {0x00u, 0x01u, 0x08u, 0x09u, 0x0eu, 0x27u})
	{
		ASSERT_EXIT(
		    {
			    InitMimgParser();
			    CaptureMimgRejectionDiagnostic();
			    (void)ParseMimgTailInstruction(opcode, 0u, 0u, 0xfu, ShaderType::Pixel, 1u << 14u);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(kMimgRejectedExit), "Not implemented \\(da == 1\\)");
	}
}

TEST(EmulatorShaderMimg, EmitsAndValidatesParsedSamplerlessImageModules)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    EnableMimgModuleValidation();
		    for (auto stage: {ShaderType::Pixel, ShaderType::Compute})
		    {
			    for (uint32_t opcode: {0x00u, 0x01u, 0x08u, 0x09u, 0x0eu})
			    {
				    const bool writable = opcode == 0x08u || opcode == 0x09u;
				    const auto code = ParseMimgTailInstruction(opcode, 0u, 0u, opcode == 0x01u ? 3u : 0xfu, stage);
				    const auto source = EmitValidatedMimgTailModule(code, writable);
				    RequireMimgTail(!source.ContainsStr("OpTypeSampler") && !source.ContainsStr("OpLoad %Sampler"),
				                   "samplerless parsed instruction does not publish or read a sampler");
				    RequireMimgTail(source.ContainsStr(writable ? "OpImageWrite" : opcode == 0x0eu ? "OpImageQuerySizeLod" : "OpImageFetch %v4float"),
				                   "complete module contains the parsed image operation");
				    if (opcode == 0x01u)
				    {
					    RequireMimgTail(code.GetInstructions().At(0).raw_word == 0xf0040308u &&
					                   source.ContainsStr("%image_load_lod_f_0 = OpLoad %float %v6") &&
					                   source.ContainsStr(" Lod %image_load_lod_0"), "reported parsed mip load retains its explicit level");
				    }
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderMimg, EmitsAndValidatesParsedSampleWithRealSampler)
{
	ASSERT_EXIT(
	    ([] {
		    InitMimgParser();
		    EnableMimgModuleValidation();
		    const auto code = ParseMimgTailInstruction(0x27u, 5u, 0u, 1u);
		    const auto source = EmitValidatedMimgTailModule(code, false, true);
		    RequireMimgTail(source.ContainsStr("OpLoad %Sampler") && source.ContainsStr("OpSampledImage") &&
		                   source.ContainsStr("OpImageSampleExplicitLod"), "parsed sample consumes its real sampler");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
