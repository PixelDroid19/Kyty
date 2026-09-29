#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include <cstdlib>

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

UT_END();
