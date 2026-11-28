#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderScalarLiveness.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "../../../emulator/src/Graphics/ShaderStorageAnalysis.h"

#include <array>
#include <cstdlib>
#include <vector>

UT_BEGIN(EmulatorShaderResourcePointers);

using namespace Libs::Graphics;

static ShaderBindResources ParseVertexFetchPointers(ShaderParsedUsage* usage, HW::UserSgprInfo* user_sgpr)
{
	std::array<uint16_t, 11> offsets;
	offsets.fill(0xffffu);
	offsets[8]  = 4; // Descriptor-table pointer, not a four-word V#.
	offsets[10] = 6; // Attribute-table pointer.
	ShaderUserData data {};
	data.direct_resource_offset = offsets.data();
	data.direct_resource_count  = offsets.size();
	for (uint32_t word = 0; word < 8u; ++word)
	{
		user_sgpr->value[word] = 0x10203040u + word;
	}
	ShaderBindResources bind {};
	ShaderParseUsage2(&data, usage, &bind, *user_sgpr, 8, nullptr, 8);
	return bind;
}

TEST(EmulatorShaderResourcePointers, NggFetchTablePointersRemainDirectApiWords)
{
	ShaderParsedUsage usage {};
	HW::UserSgprInfo user_sgpr {};
	const auto bind = ParseVertexFetchPointers(&usage, &user_sgpr);
	EXPECT_TRUE(usage.vertex_buffer);
	EXPECT_TRUE(usage.vertex_attrib);
	EXPECT_EQ(usage.vertex_buffer_reg, 4);
	EXPECT_EQ(usage.vertex_attrib_reg, 6);
	EXPECT_EQ(bind.storage_buffers.buffers_num, 0);
	ASSERT_EQ(bind.direct_sgprs.sgprs_num, 8);
	for (int word = 0; word < 8; ++word)
	{
		EXPECT_EQ(bind.direct_sgprs.start_register[word], word);
		EXPECT_EQ(bind.direct_sgprs.sgprs[word].field, user_sgpr.value[word]);
		EXPECT_FALSE(bind.direct_sgprs.absolute_register[word]);
	}
}

TEST(EmulatorShaderResourcePointers, NggNativeFetchTableLoadsHaveInitializedShaderPointers)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderParsedUsage usage {};
	HW::UserSgprInfo user_sgpr {};
	ShaderVertexInputInfo input {};
	input.bind                     = ParseVertexFetchPointers(&usage, &user_sgpr);
	input.bind.device_address_used = true;
	input.gs_prolog                = true;
	input.fetch_embedded           = true;
	input.fetch_attrib_reg         = usage.vertex_attrib_reg;
	input.fetch_buffer_reg         = usage.vertex_buffer_reg;
	ShaderCalcBindingIndices(&input.bind);

	ShaderInstruction attr {};
	attr.type               = ShaderInstructionType::SLoadDwordx2;
	attr.format             = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
	attr.dst                = {.type = ShaderOperandType::Sgpr, .register_id = 32, .size = 2};
	attr.src[0]             = {.type = ShaderOperandType::Sgpr, .register_id = 14, .size = 2};
	attr.src[1].type        = ShaderOperandType::IntegerInlineConstant;
	attr.src_num            = 2;
	ShaderInstruction descriptor = attr;
	descriptor.pc                 = 8;
	descriptor.type               = ShaderInstructionType::SLoadDwordx4;
	descriptor.format             = ShaderInstructionFormat::Sdst4SbaseSoffset;
	descriptor.dst                = {.type = ShaderOperandType::Sgpr, .register_id = 36, .size = 4};
	descriptor.src[0].register_id = 12;
	ShaderInstruction end {};
	end.pc     = 16;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	code.GetInstructions().Add(attr);
	code.GetInstructions().Add(descriptor);
	code.GetInstructions().Add(end);
	const auto source = SpirvGenerateSource(code, &input, nullptr, nullptr);
	for (int reg = 12; reg < 16; ++reg)
	{
		const auto store = source.FindIndex(String8::FromPrintf("OpStore %%s%d %%vsharp_value_s%d_", reg, reg));
		EXPECT_NE(store, Core::STRING8_INVALID_INDEX) << reg;
		EXPECT_LT(store, source.FindIndex("%sg_0_blo = OpLoad %uint %s14")) << reg;
	}
	EXPECT_NE(source.FindIndex("%sg_1_blo = OpLoad %uint %s12"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s36 %sg_1_d0"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%sload_attr_"), Core::STRING8_INVALID_INDEX);
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		explicit ValidationConfig(bool enabled): m_enabled(enabled) {}
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return m_enabled; }
		Core::String GetString(const Core::String&) const override { return {}; }

	private:
		bool m_enabled;
	};
	const ValidationConfig restore(Config::ShaderValidationEnabled());
	Config::Load(ValidationConfig(true));
	Vector<uint32_t> binary;
	String8 error;
	const bool valid = ShaderToolchain::Run(source, &binary, &error);
	Config::Load(restore);
	EXPECT_TRUE(valid) << error.c_str();
	EXPECT_FALSE(binary.IsEmpty());
}

static void ParseLiveDirectStorageAcrossPartialSrtSpan()
{
	std::array<uint16_t, 2> offsets = {0xffffu, 0u};
	ShaderUserData data {};
	data.direct_resource_offset = offsets.data();
	data.direct_resource_count  = offsets.size();
	data.srt_size_dw            = 2;

	HW::UserSgprInfo user_sgpr {};
	user_sgpr.value[0] = 0x1000u;
	user_sgpr.value[2] = 64u;

	ShaderInstruction load {};
	load.pc               = 0;
	load.type             = ShaderInstructionType::BufferLoadDword;
	load.src_num          = 2;
	load.src[0].type      = ShaderOperandType::Vgpr;
	load.src[0].size      = 1;
	load.src[1].type      = ShaderOperandType::Sgpr;
	load.src[1].size      = 4;
	ShaderInstruction end {};
	end.pc   = 4;
	end.type = ShaderInstructionType::SEndpgm;
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(load);
	code.GetInstructions().Add(end);

	ShaderParsedUsage   usage {};
	ShaderBindResources bind {};
	ShaderParseUsage2(&data, &usage, &bind, user_sgpr, 4, &code, 0, false);
}

TEST(EmulatorShaderResourcePointers, MixedSrtAndEudRetainsExplicitEudPointer)
{
	std::array<uint16_t, 6> offsets;
	offsets.fill(0xffffu);
	offsets[5] = 12;
	ShaderUserData data {};
	data.direct_resource_offset = offsets.data();
	data.direct_resource_count  = offsets.size();
	data.eud_size_dw            = 48;
	data.srt_size_dw            = 2;
	EXPECT_TRUE(Gen5HasEudPointer(&data));
	data.srt_size_dw = 0;
	EXPECT_TRUE(Gen5HasEudPointer(&data));
	data.eud_size_dw = 0;
	EXPECT_FALSE(Gen5HasEudPointer(&data));
	data.eud_size_dw = 48;
	offsets[5] = 0xffffu;
	EXPECT_FALSE(Gen5HasEudPointer(&data));
	data.direct_resource_offset = nullptr;
	EXPECT_FALSE(Gen5HasEudPointer(&data));
	EXPECT_FALSE(Gen5HasEudPointer(nullptr));
}

TEST(EmulatorShaderResourcePointers, SparseSharpSlotsDoNotConsumeDescriptorStorage)
{
	std::array<ShaderSharp, 96> sharps {};
	for (auto& sharp: sharps) { sharp.offset_dw = 0x7fffu; }
	sharps[0].offset_dw = 32;
	sharps.back().offset_dw = 32; // Two API slots can refer to the same words.
	ShaderUserData data {};
	data.eud_size_dw = 48;
	data.sharp_resource_count[0] = sharps.size();
	data.sharp_resource_offset[0] = sharps.data();
	uint32_t end = 0;
	EXPECT_TRUE(ShaderGen5EudRequiredEndDwords(&data, 16, 14, nullptr, 0, &end));
	EXPECT_EQ(end, 48u);
	std::array<uint32_t, 48> snapshot {};
	end = snapshot.size();
	EXPECT_TRUE(ShaderGen5EudExpandEndDwordsForSharpImages(&data, 16, snapshot.data(), snapshot.size(), &end));
	EXPECT_EQ(end, snapshot.size());
	// Offset 30 lies between the 16-word user-SGPR window and EUD base 32.
	// A descriptor starting in that gap is unsupported.
	sharps.back().offset_dw = 30;
	EXPECT_FALSE(ShaderGen5EudRequiredEndDwords(&data, 16, 14, nullptr, 0, &end));
	EXPECT_FALSE(ShaderGen5EudExpandEndDwordsForSharpImages(&data, 16, snapshot.data(), snapshot.size(), &end));
}

TEST(EmulatorShaderResourcePointers, PartialSrtDirectResourceSpanRemainsRawForScalarLoadPointer)
{
	std::array<uint16_t, 6> offsets;
	offsets.fill(0xffffu);
	offsets[1] = 0;
	offsets[5] = 14;
	ShaderUserData data {};
	data.direct_resource_offset = offsets.data();
	data.direct_resource_count  = offsets.size();
	data.eud_size_dw            = 64;
	data.srt_size_dw            = 2;

	HW::UserSgprInfo user_sgpr {};
	// S0:S3 look enough like a non-null V# to trigger the existing generic
	// direct-resource fallback, but the metadata-declared SRT span is only S0:S1.
	// The code's first use treats S0:S1 as the base for a scalar load.
	user_sgpr.value[0] = 0x1000u;
	user_sgpr.value[2] = 64u;

	ShaderInstruction load {};
	load.pc                  = 0;
	load.type                = ShaderInstructionType::SLoadDwordx2;
	load.format              = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
	load.src_num             = 2;
	load.src[0].type         = ShaderOperandType::Sgpr;
	load.src[0].register_id  = 0;
	load.src[0].size         = 2;
	load.src[1].type         = ShaderOperandType::IntegerInlineConstant;
	load.src[1].size         = 0;
	load.dst.type            = ShaderOperandType::Sgpr;
	load.dst.register_id     = 16;
	load.dst.size            = 2;
	ShaderInstruction end {};
	end.pc     = 4;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(load);
	code.GetInstructions().Add(end);

	ShaderParsedUsage   usage {};
	ShaderBindResources bind {};
	ShaderParseUsage2(&data, &usage, &bind, user_sgpr, 16, &code, 0, false);

	EXPECT_EQ(bind.storage_buffers.buffers_num, 0);
	EXPECT_EQ(usage.storage_buffers_readonly, 0);
	EXPECT_TRUE(bind.extended.used);
	EXPECT_EQ(bind.extended.start_register, 14);
	ASSERT_EQ(bind.direct_sgprs.sgprs_num, 14);
	EXPECT_EQ(bind.direct_sgprs.start_register[0], 0);
	EXPECT_EQ(bind.direct_sgprs.start_register[1], 1);
	EXPECT_EQ(bind.direct_sgprs.start_register[13], 13);
}

TEST(EmulatorShaderResourcePointers, DirectStorageConsumerAcrossSrtBoundaryFailsClosed)
{
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	ASSERT_EXIT(
	    {
		    ParseLiveDirectStorageAcrossPartialSrtSpan();
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(rejected_exit), "");
}

TEST(EmulatorShaderResourcePointers, LaterDescriptorConsumerAfterSrtSgprOverwriteDoesNotProveLiveInVsharp)
{
	std::array<uint16_t, 6> offsets;
	offsets.fill(0xffffu);
	offsets[1] = 0;
	offsets[5] = 14;
	ShaderUserData data {};
	data.direct_resource_offset = offsets.data();
	data.direct_resource_count  = offsets.size();
	data.eud_size_dw            = 64;
	data.srt_size_dw            = 2;

	HW::UserSgprInfo user_sgpr {};
	user_sgpr.value[0] = 0x1000u;
	user_sgpr.value[2] = 64u;

	ShaderInstruction pointer_load {};
	pointer_load.pc                 = 0;
	pointer_load.type               = ShaderInstructionType::SLoadDwordx2;
	pointer_load.format             = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
	pointer_load.src_num            = 2;
	pointer_load.src[0].type        = ShaderOperandType::Sgpr;
	pointer_load.src[0].register_id = 0;
	pointer_load.src[0].size        = 2;
	pointer_load.src[1].type        = ShaderOperandType::IntegerInlineConstant;
	pointer_load.src[1].size        = 0;
	pointer_load.dst.type           = ShaderOperandType::Sgpr;
	pointer_load.dst.register_id    = 16;
	pointer_load.dst.size           = 2;

	ShaderInstruction descriptor_load {};
	descriptor_load.pc                 = 4;
	descriptor_load.type               = ShaderInstructionType::SLoadDwordx4;
	descriptor_load.format             = ShaderInstructionFormat::Sdst4SbaseSoffset;
	descriptor_load.src_num            = 2;
	descriptor_load.src[0].type        = ShaderOperandType::Sgpr;
	descriptor_load.src[0].register_id = 14;
	descriptor_load.src[0].size        = 2;
	descriptor_load.src[1].type        = ShaderOperandType::IntegerInlineConstant;
	descriptor_load.src[1].size        = 0;
	descriptor_load.dst.type           = ShaderOperandType::Sgpr;
	descriptor_load.dst.register_id    = 0;
	descriptor_load.dst.size           = 4;

	ShaderInstruction buffer_load {};
	buffer_load.pc                 = 8;
	buffer_load.type               = ShaderInstructionType::BufferLoadDword;
	buffer_load.src_num            = 2;
	buffer_load.src[0].type        = ShaderOperandType::Vgpr;
	buffer_load.src[0].size        = 1;
	buffer_load.src[1].type        = ShaderOperandType::Sgpr;
	buffer_load.src[1].register_id = 0;
	buffer_load.src[1].size        = 4;
	ShaderInstruction end {};
	end.pc   = 12;
	end.type = ShaderInstructionType::SEndpgm;

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(pointer_load);
	code.GetInstructions().Add(descriptor_load);
	code.GetInstructions().Add(buffer_load);
	code.GetInstructions().Add(end);

	EXPECT_EQ(AnalyzeShaderStorageUse(code, 0).access, ShaderStorageAccess::Unknown);
	ShaderParsedUsage   usage {};
	ShaderBindResources bind {};
	ShaderParseUsage2(&data, &usage, &bind, user_sgpr, 16, &code, 0, false);

	EXPECT_EQ(bind.storage_buffers.buffers_num, 0);
	EXPECT_EQ(usage.storage_buffers_readonly, 0);
	EXPECT_TRUE(bind.extended.used);
	ASSERT_GE(bind.direct_sgprs.sgprs_num, 2);
	EXPECT_EQ(bind.direct_sgprs.start_register[0], 0);
	EXPECT_EQ(bind.direct_sgprs.start_register[1], 1);
}

TEST(EmulatorShaderResourcePointers, DirectStorageSpanContainedBySrtRemainsBindable)
{
	std::array<uint16_t, 2> offsets = {0xffffu, 0u};
	ShaderUserData data {};
	data.direct_resource_offset = offsets.data();
	data.direct_resource_count  = offsets.size();
	data.srt_size_dw            = 4;

	HW::UserSgprInfo user_sgpr {};
	user_sgpr.value[0] = 0x1000u;
	user_sgpr.value[2] = 64u;

	ShaderInstruction end {};
	end.pc     = 8;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	// A descriptor is live only if the instruction stream actually consumes it.
	ShaderInstruction load {};
	load.type               = ShaderInstructionType::BufferLoadDword;
	load.format             = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
	load.dst                = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	load.src[0]             = {.type = ShaderOperandType::Vgpr, .register_id = 1, .size = 1};
	load.src[1]             = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 4};
	load.src[2].type        = ShaderOperandType::IntegerInlineConstant;
	load.src_num            = 3;
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(load);
	code.GetInstructions().Add(end);

	ShaderParsedUsage   usage {};
	ShaderBindResources bind {};
	ShaderParseUsage2(&data, &usage, &bind, user_sgpr, 8, &code, 0, false);

	ASSERT_EQ(bind.storage_buffers.buffers_num, 1);
	EXPECT_EQ(bind.storage_buffers.start_register[0], 0);
	EXPECT_EQ(usage.storage_buffers_readonly, 1);
}

TEST(EmulatorShaderResourcePointers, ZeroSBufferLoadToVccHiEmitsValidStoreBeforeScalarMove)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;
	input.bind.zero_sbuffer_resources.start_register[0] = 20;
	input.bind.zero_sbuffer_resources.buffers_num       = 1;

	ShaderInstruction load {};
	load.pc                  = 0;
	load.type                = ShaderInstructionType::SBufferLoadDword;
	load.format              = ShaderInstructionFormat::SdstSvSoffset;
	load.dst                 = {.type = ShaderOperandType::VccHi, .register_id = 0, .size = 1};
	load.src[0]              = {.type = ShaderOperandType::Sgpr, .register_id = 20, .size = 4};
	load.src[1].type         = ShaderOperandType::IntegerInlineConstant;
	load.src[1].size         = 0;
	load.src_num             = 2;
	load.smem_imm_offset      = 64;
	ShaderInstruction move {};
	move.pc                  = 8;
	move.type                = ShaderInstructionType::SMovB32;
	move.format              = ShaderInstructionFormat::SVdstSVsrc0;
	move.dst                 = {.type = ShaderOperandType::Sgpr, .register_id = 18, .size = 1};
	move.src[0].type         = ShaderOperandType::FloatInlineConstant;
	move.src[0].constant.f   = 0.3333f;
	move.src[0].size         = 0;
	move.src_num             = 1;
	ShaderInstruction end {};
	end.pc     = 12;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	code.GetInstructions().Add(load);
	code.GetInstructions().Add(move);
	code.GetInstructions().Add(end);

	const auto source    = SpirvGenerateSource(code, nullptr, &input, nullptr);
	const auto vcc_store = source.FindIndex("OpStore %vcc_hi %uint_0");
	const auto move_store = source.FindIndex("OpStore %s18");
	EXPECT_NE(vcc_store, Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("OpStore % %uint_0"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(move_store, Core::STRING8_INVALID_INDEX);
	EXPECT_LT(vcc_store, move_store);

	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		explicit ValidationConfig(bool enabled): m_enabled(enabled) {}
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return m_enabled; }
		Core::String GetString(const Core::String&) const override { return {}; }

	private:
		bool m_enabled;
	};
	const ValidationConfig restore(Config::ShaderValidationEnabled());
	Config::Load(ValidationConfig(true));
	Vector<uint32_t> binary;
	String8 error;
	const bool valid = ShaderToolchain::Run(source, &binary, &error);
	Config::Load(restore);
	EXPECT_TRUE(valid) << error.c_str();
	EXPECT_FALSE(binary.IsEmpty());
}

static ShaderInstruction GsUserDataLoad(uint32_t pc, ShaderInstructionType type, int dst, int dwords, uint32_t offset_bytes)
{
	ShaderInstruction load {};
	load.pc                   = pc;
	load.type                 = type;
	load.dst                  = {.type = ShaderOperandType::Sgpr, .register_id = dst, .size = dwords};
	load.src[0]               = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 2};
	load.src[1].type          = ShaderOperandType::LiteralConstant;
	load.src[1].constant.u    = offset_bytes;
	load.src_num              = 2;
	return load;
}

static ShaderInstruction GsUserDataBranch(uint32_t pc, ShaderInstructionType type, uint32_t target)
{
	ShaderInstruction branch {};
	branch.pc                 = pc;
	branch.type               = type;
	branch.format             = ShaderInstructionFormat::Label;
	branch.src[0].type        = ShaderOperandType::LiteralConstant;
	branch.src[0].constant.i  = static_cast<int32_t>(target - pc - 4u);
	branch.src_num            = 1;
	return branch;
}

static ShaderInstruction GsUserDataSample(uint32_t pc)
{
	ShaderInstruction sample {};
	sample.pc     = pc;
	sample.type   = ShaderInstructionType::ImageSampleLz;
	sample.dst    = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 4};
	sample.src[0] = {.type = ShaderOperandType::Vgpr, .register_id = 4, .size = 2};
	sample.src[1] = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 8};
	sample.src[2] = {.type = ShaderOperandType::Sgpr, .register_id = 32, .size = 4};
	sample.src_num = 3;
	return sample;
}

static ShaderCode GsUserDataProgram(const std::vector<ShaderInstruction>& instructions)
{
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	for (const auto& inst: instructions)
	{
		code.GetInstructions().Add(inst);
		if (ShaderInstructionHasStaticBranchTarget(inst.type))
		{
			code.GetLabels().Add(ShaderLabel(inst));
		}
	}
	return code;
}

// A T# and S# loaded through the s[0:1] user-data address bind for every
// sample the load reaches, including one past an unconditional branch.
TEST(EmulatorShaderResourcePointers, GsUserDataDescriptorsReachConsumersPastBranches)
{
	ShaderInstruction end {};
	end.pc   = 32;
	end.type = ShaderInstructionType::SEndpgm;
	const auto code  = GsUserDataProgram({GsUserDataLoad(0, ShaderInstructionType::SLoadDwordx8, 8, 8, 32),
	                                      GsUserDataLoad(8, ShaderInstructionType::SLoadDwordx4, 32, 4, 64),
	                                      GsUserDataBranch(16, ShaderInstructionType::SCbranchScc0, 28), GsUserDataSample(20),
	                                      GsUserDataBranch(24, ShaderInstructionType::SBranch, 32), GsUserDataSample(28), end});
	const auto loads = ShaderFindGsUserDataDescriptorLoads(code);
	ASSERT_EQ(loads.size(), 2u);
	EXPECT_EQ(loads[0].use.kind, ShaderDynamicSLoadResourceKind::Texture);
	EXPECT_EQ(loads[0].offset_dw, 8);
	EXPECT_EQ(loads[0].use.last_consumer_pc, 28u);
	EXPECT_EQ(loads[1].use.kind, ShaderDynamicSLoadResourceKind::Sampler);
	EXPECT_EQ(loads[1].offset_dw, 16);
	EXPECT_EQ(loads[1].use.last_consumer_pc, 28u);
}

// A sample that sees the loaded T# on one path only binds nothing.
TEST(EmulatorShaderResourcePointers, GsUserDataDescriptorClobberedOnOnePathIsNotBound)
{
	ShaderInstruction clobber {};
	clobber.pc          = 20;
	clobber.type        = ShaderInstructionType::SMovB32;
	clobber.dst         = {.type = ShaderOperandType::Sgpr, .register_id = 9, .size = 1};
	clobber.src[0].type = ShaderOperandType::IntegerInlineConstant;
	clobber.src_num     = 1;
	ShaderInstruction end {};
	end.pc   = 28;
	end.type = ShaderInstructionType::SEndpgm;
	const auto code  = GsUserDataProgram({GsUserDataLoad(0, ShaderInstructionType::SLoadDwordx8, 8, 8, 0),
	                                      GsUserDataLoad(8, ShaderInstructionType::SLoadDwordx4, 32, 4, 32),
	                                      GsUserDataBranch(16, ShaderInstructionType::SCbranchScc0, 24), clobber, GsUserDataSample(24),
	                                      end});
	const auto loads = ShaderFindGsUserDataDescriptorLoads(code);
	ASSERT_EQ(loads.size(), 1u);
	EXPECT_EQ(loads[0].use.kind, ShaderDynamicSLoadResourceKind::Sampler);
}

// Read-only raw dword vector buffers loaded through s[0:1] bind by device
// address, while writable or format-loaded V# blocks stay unbound.
TEST(EmulatorShaderResourcePointers, GsUserDataReadOnlyDwordVectorBuffersBindByAddress)
{
	auto make_buf = [](uint32_t pc, ShaderInstructionType type, int sgpr)
	{
		ShaderInstruction inst {};
		inst.pc           = pc;
		inst.type         = type;
		inst.dst          = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 4};
		inst.src[0]       = {.type = ShaderOperandType::Vgpr, .register_id = 4, .size = 1};
		inst.src[1]       = {.type = ShaderOperandType::Sgpr, .register_id = sgpr, .size = 4};
		inst.src[2].type  = ShaderOperandType::IntegerInlineConstant;
		inst.src[2].constant.u = 0;
		inst.src_num      = 3;
		inst.buffer_idxen = true;
		return inst;
	};
	ShaderInstruction sbuf {};
	sbuf.pc               = 20;
	sbuf.type             = ShaderInstructionType::SBufferLoadDwordx4;
	sbuf.dst              = {.type = ShaderOperandType::Sgpr, .register_id = 28, .size = 4};
	sbuf.src[0]           = {.type = ShaderOperandType::Sgpr, .register_id = 24, .size = 4};
	sbuf.src[1].type      = ShaderOperandType::IntegerInlineConstant;
	sbuf.src[1].constant.u = 0;
	sbuf.src_num          = 2;
	sbuf.smem_flags       = 0;
	ShaderInstruction end {};
	end.pc   = 44;
	end.type = ShaderInstructionType::SEndpgm;
	const auto code = GsUserDataProgram({
	    GsUserDataLoad(0, ShaderInstructionType::SLoadDwordx8, 12, 8, 64),
	    GsUserDataLoad(8, ShaderInstructionType::SLoadDwordx8, 20, 8, 96),
	    make_buf(16, ShaderInstructionType::BufferLoadDwordx4, 12),
	    sbuf,
	    make_buf(28, ShaderInstructionType::BufferStoreDword, 16),
	    make_buf(36, ShaderInstructionType::BufferLoadFormatXyzw, 20),
	    end,
	});
	const auto loads = ShaderFindGsUserDataDescriptorLoads(code);
	ASSERT_EQ(loads.size(), 2u);
	EXPECT_EQ(loads[0].use.kind, ShaderDynamicSLoadResourceKind::StorageBuffer);
	EXPECT_EQ(loads[0].offset_dw, 16);
	EXPECT_EQ(loads[0].block.dst.register_id, 12);
	EXPECT_TRUE(loads[0].use.dword_loads_only);
	EXPECT_TRUE(loads[0].use.raw_only);
	EXPECT_EQ(loads[1].use.kind, ShaderDynamicSLoadResourceKind::StorageBuffer);
	EXPECT_EQ(loads[1].offset_dw, 28);
	EXPECT_EQ(loads[1].block.dst.register_id, 24);
	EXPECT_TRUE(loads[1].use.dword_loads_only);
	EXPECT_TRUE(loads[1].use.raw_smem_use);

	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	const bool prev_next_gen = Config::IsNextGen();
	Config::SetNextGen(true);
	ShaderSetStorageAddressReads(true);

	struct TableReader final : ShaderGuestDescriptorReader
	{
		std::array<uint32_t, 32> table {};
		bool                     Read(uint64_t addr, uint32_t dwords, uint32_t* out) override
		{
			if (addr != 0x20000000ull || dwords > table.size()) { return false; }
			std::copy_n(table.data(), dwords, out);
			return true;
		}
	};
	TableReader reader;
	reader.table[16] = 0x10000000u;
	reader.table[17] = (16u << 16u) | 0x0001u;
	reader.table[18] = 1024u;
	reader.table[19] = 0u;
	reader.table[28] = 0x12000000u;
	reader.table[29] = 0x0001u;
	reader.table[30] = 2048u;
	reader.table[31] = 0u;
	ScopedShaderGuestDescriptorReader scope(&reader);

	ShaderBindResources bind {};
	bind.descriptor_flow_proven = true;
	ShaderParsedUsage usage {};
	HW::UserSgprInfo  user_sgpr {};
	ShaderBindGsUserDataDescriptors(code, loads, 0x20000000ull, user_sgpr, &bind, &usage, nullptr);
	ShaderCalcBindingIndices(&bind);

	Config::SetNextGen(prev_next_gen);

	ASSERT_EQ(bind.storage_buffers.buffers_num, 2);
	EXPECT_TRUE(bind.storage_buffers.by_address[0]);
	EXPECT_TRUE(bind.storage_buffers.by_address[1]);
	EXPECT_TRUE(bind.storage_address_used);
	EXPECT_TRUE(bind.storage_buffers.dword_loads_only[0]);
	EXPECT_TRUE(bind.storage_buffers.dword_loads_only[1]);
	ASSERT_EQ(bind.dynamic_sloads.records.Size(), 2u);
	EXPECT_TRUE(bind.dynamic_sloads.records.At(0).guest_load);
	EXPECT_TRUE(bind.dynamic_sloads.records.At(1).guest_load);
	const ShaderOperand aliased_sgpr {.type = ShaderOperandType::Sgpr, .register_id = 20, .size = 4};
	EXPECT_EQ(ShaderStorageBufferBoundIndex(bind, aliased_sgpr, 8), -1);
}

// S_LSHL_B32 vcc_lo, s4, 4; S_LOAD_DWORDX4 s[0:3], s[0:1]; S_BUFFER_LOAD_DWORDX4
// s[0:3], s[0:3], vcc_lo offset:16; IMAGE_SAMPLE with S# s[0:3]. An optional
// S_MOV_B32 s4 first replaces the user-data index.
static ShaderCode TableSamplerProgram(bool overwrite_index)
{
	std::vector<ShaderInstruction> program;
	uint32_t                       pc = 0;
	if (overwrite_index)
	{
		ShaderInstruction move {};
		move.type        = ShaderInstructionType::SMovB32;
		move.dst         = {.type = ShaderOperandType::Sgpr, .register_id = 4, .size = 1};
		move.src[0].type = ShaderOperandType::IntegerInlineConstant;
		move.src_num     = 1;
		program.push_back(move);
	}
	ShaderInstruction shift {};
	shift.type               = ShaderInstructionType::SLshlB32;
	shift.dst                = {.type = ShaderOperandType::VccLo, .register_id = 0, .size = 1};
	shift.src[0]             = {.type = ShaderOperandType::Sgpr, .register_id = 4, .size = 1};
	shift.src[1].type        = ShaderOperandType::IntegerInlineConstant;
	shift.src[1].constant.u  = 4;
	shift.src_num            = 2;
	program.push_back(shift);
	auto vsharp = GsUserDataLoad(0, ShaderInstructionType::SLoadDwordx4, 0, 4, 0);
	program.push_back(vsharp);
	ShaderInstruction record {};
	record.type            = ShaderInstructionType::SBufferLoadDwordx4;
	record.dst             = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 4};
	record.src[0]          = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 4};
	record.src[1]          = {.type = ShaderOperandType::VccLo, .register_id = 0, .size = 1};
	record.src_num         = 2;
	record.smem_imm_offset = 16;
	program.push_back(record);
	auto sample   = GsUserDataSample(0);
	sample.type   = ShaderInstructionType::ImageSample;
	sample.src[2] = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 4};
	program.push_back(sample);
	ShaderInstruction end {};
	end.type = ShaderInstructionType::SEndpgm;
	program.push_back(end);
	for (auto& inst: program)
	{
		inst.pc = pc;
		pc += 8;
	}
	auto code = GsUserDataProgram(program);
	code.SetType(ShaderType::Pixel);
	return code;
}

struct SamplerTableReader final : ShaderGuestDescriptorReader
{
	std::array<uint32_t, 4>  vsharp {0x30000000u, 0u, 64u, 0u}; // stride 0: 64 bytes, four records
	std::array<uint32_t, 16> records {};
	bool                     Read(uint64_t addr, uint32_t dwords, uint32_t* out) override
	{
		if (addr == 0x20000000ull && dwords == vsharp.size())
		{
			std::copy_n(vsharp.data(), dwords, out);
			return true;
		}
		const uint64_t first = (addr - 0x30000000ull) / sizeof(uint32_t);
		if (addr < 0x30000000ull || first + dwords > records.size()) { return false; }
		std::copy_n(records.data() + first, dwords, out);
		return true;
	}
};

static ShaderBindResources CollectTableSamplers(const ShaderCode& code, uint32_t index)
{
	SamplerTableReader reader;
	for (uint32_t word = 0; word < reader.records.size(); ++word) { reader.records[word] = 0xa0000000u + word; }
	ScopedShaderGuestDescriptorReader scope(&reader);
	HW::UserSgprInfo user_sgpr {};
	user_sgpr.value[0] = 0x20000000u;
	user_sgpr.value[4] = index;
	ShaderBindResources bind {};
	ShaderParsedUsage   usage {};
	ShaderCollectTableSamplers(code, &bind, &usage, user_sgpr, 8, 0, *ShaderScalarFlowOf(code), nullptr);
	return bind;
}

// Record 1 plus the 16-byte immediate selects record 2: dwords 8 to 11.
TEST(EmulatorShaderResourcePointers, TableSamplerBindsTheRecordUserDataSelects)
{
	const auto code = TableSamplerProgram(false);
	ASSERT_EQ(ShaderFindTableSamplerReads(code).size(), 1u);
	const auto bind = CollectTableSamplers(code, 1);
	ASSERT_EQ(bind.samplers.samplers_num, 1);
	for (int field = 0; field < 4; ++field)
	{
		EXPECT_EQ(bind.samplers.samplers[0].fields[field], 0xa0000008u + static_cast<uint32_t>(field));
	}
	ASSERT_EQ(bind.constant_samplers.Size(), 1u);
	EXPECT_EQ(bind.constant_samplers.At(0).consumer_pc, 24u);
	EXPECT_EQ(ShaderFindImageSamplerDescriptor(code.GetInstructions().At(3), bind, 0), 0);
}

// An index the program writes before the shift is not the draw's user data.
TEST(EmulatorShaderResourcePointers, TableSamplerIndexMustHoldUserData)
{
	const auto code = TableSamplerProgram(true);
	ASSERT_EQ(ShaderFindTableSamplerReads(code).size(), 1u);
	const auto bind = CollectTableSamplers(code, 1);
	EXPECT_EQ(bind.samplers.samplers_num, 0);
	EXPECT_EQ(bind.constant_samplers.Size(), 0u);
}

static ShaderOperand HeapConstant(uint32_t value)
{
	ShaderOperand operand {};
	operand.type       = ShaderOperandType::LiteralConstant;
	operand.constant.u = value;
	operand.size       = 1;
	return operand;
}

static ShaderInstruction HeapScalar(ShaderInstructionType type, ShaderOperand dst, ShaderOperand src0, ShaderOperand src1)
{
	ShaderInstruction inst {};
	inst.type    = type;
	inst.dst     = dst;
	inst.src[0]  = src0;
	inst.src[1]  = src1;
	inst.src_num = 2;
	return inst;
}

static ShaderInstruction HeapBufferLoad(ShaderInstructionType type, ShaderOperand dst, int vsharp, ShaderOperand offset, int imm)
{
	auto load            = HeapScalar(type, dst, {.type = ShaderOperandType::Sgpr, .register_id = vsharp, .size = 4}, offset);
	load.smem_imm_offset = imm;
	return load;
}

// A light loop: each pass takes its record's T# and S# heap indexes, scales
// them to heap offsets in VCC_LO and VCC_HI, and samples through the heap
// descriptors. The record index s38 is written before the loop and on the back
// edge, so only the S_MUL_I32 that scales it reaches the record loads.
static ShaderCode HeapLightLoopProgram()
{
	const ShaderOperand vcc_lo {.type = ShaderOperandType::VccLo, .register_id = 0, .size = 1};
	const ShaderOperand vcc_hi {.type = ShaderOperandType::VccHi, .register_id = 0, .size = 1};
	const ShaderOperand index {.type = ShaderOperandType::Sgpr, .register_id = 38, .size = 1};
	std::vector<ShaderInstruction> program;
	program.push_back(GsUserDataLoad(0, ShaderInstructionType::SLoadDwordx8, 16, 8, 0)); // sampler heap s[16:19], texture heap s[20:23]
	auto lights          = GsUserDataLoad(0, ShaderInstructionType::SLoadDwordx4, 32, 4, 0);
	lights.src[0].register_id = 4;
	program.push_back(lights);
	auto first   = HeapScalar(ShaderInstructionType::SMovB32, index, HeapConstant(0), {});
	first.src_num = 1;
	program.push_back(first);
	program.push_back(HeapScalar(ShaderInstructionType::SMulI32, vcc_hi, index, HeapConstant(16))); // loop head, pc 24
	program.push_back(HeapBufferLoad(ShaderInstructionType::SBufferLoadDword, vcc_lo, 32, vcc_hi, 4));
	program.push_back(HeapBufferLoad(ShaderInstructionType::SBufferLoadDword, vcc_hi, 32, vcc_hi, 8));
	program.push_back(HeapScalar(ShaderInstructionType::SLshlB32, vcc_lo, vcc_lo, HeapConstant(5)));
	program.push_back(HeapScalar(ShaderInstructionType::SLshlB32, vcc_hi, vcc_hi, HeapConstant(4)));
	program.push_back(HeapBufferLoad(ShaderInstructionType::SBufferLoadDwordx4, {.type = ShaderOperandType::Sgpr, .register_id = 24, .size = 4},
	                                 16, vcc_hi, 0));
	program.push_back(HeapBufferLoad(ShaderInstructionType::SBufferLoadDwordx8, {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 8},
	                                 20, vcc_lo, 0));
	auto sample   = GsUserDataSample(0);
	sample.type   = ShaderInstructionType::ImageSample;
	sample.src[2] = {.type = ShaderOperandType::Sgpr, .register_id = 24, .size = 4};
	program.push_back(sample);
	program.push_back(HeapScalar(ShaderInstructionType::SAddU32, index, index, HeapConstant(1)));
	auto compare = HeapScalar(ShaderInstructionType::SCmpLtU32, {}, index, HeapConstant(3));
	program.push_back(compare);
	program.push_back(GsUserDataBranch(0, ShaderInstructionType::SCbranchScc1, 24));
	ShaderInstruction end {};
	end.type = ShaderInstructionType::SEndpgm;
	program.push_back(end);
	uint32_t pc = 0;
	for (auto& inst: program)
	{
		inst.pc = pc;
		pc += 8;
	}
	program[13].src[0].constant.i = static_cast<int32_t>(24u - program[13].pc - 4u);
	auto code = GsUserDataProgram(program);
	code.SetType(ShaderType::Pixel);
	return code;
}

// Guest memory of the light loop: a heap table, a light V# of three 16-byte
// records holding a T# and an S# heap index, and the two heaps.
struct HeapLightReader final : ShaderGuestDescriptorReader
{
	static constexpr uint64_t kHeaps       = 0x10000000ull;
	static constexpr uint64_t kLights      = 0x18000000ull;
	static constexpr uint64_t kRecords     = 0x40000000ull;
	static constexpr uint64_t kSamplerHeap = 0x20000000ull;
	static constexpr uint64_t kTextureHeap = 0x30000000ull;
	static constexpr std::array<uint32_t, 3> kTextureIndex {5, 2, 7};
	static constexpr std::array<uint32_t, 3> kSamplerIndex {3, 0, 1};

	bool Read(uint64_t addr, uint32_t dwords, uint32_t* out) override
	{
		for (uint32_t word = 0; word < dwords; ++word)
		{
			if (!Word(addr + word * 4ull, &out[word]))
			{
				return false;
			}
		}
		return true;
	}

	static bool Word(uint64_t addr, uint32_t* out)
	{
		const std::array<uint32_t, 8> heaps {static_cast<uint32_t>(kSamplerHeap), 0u, 16u * 4u, 0u, static_cast<uint32_t>(kTextureHeap), 0u,
		                                     32u * 8u, 0u};
		const std::array<uint32_t, 4> lights {static_cast<uint32_t>(kRecords), 16u << 16u, 3u, 0u};
		if (addr >= kHeaps && addr < kHeaps + 32)
		{
			*out = heaps[(addr - kHeaps) / 4];
			return true;
		}
		if (addr >= kLights && addr < kLights + 16)
		{
			*out = lights[(addr - kLights) / 4];
			return true;
		}
		if (addr >= kRecords && addr < kRecords + 48)
		{
			const uint64_t record = (addr - kRecords) / 16;
			const uint64_t field  = (addr - kRecords) % 16 / 4;
			*out = field == 1 ? kTextureIndex[record] : (field == 2 ? kSamplerIndex[record] : 0u);
			return true;
		}
		if (addr >= kTextureHeap && addr < kTextureHeap + 32 * 8)
		{
			*out = 0xc0000000u + static_cast<uint32_t>((addr - kTextureHeap) / 4);
			return true;
		}
		if (addr >= kSamplerHeap && addr < kSamplerHeap + 16 * 4)
		{
			*out = 0xd0000000u + static_cast<uint32_t>((addr - kSamplerHeap) / 4);
			return true;
		}
		return false;
	}
};

// Each record of the light V# selects its own T# and S#; past the last record
// the record load leaves the V# and the read has no descriptor.
TEST(EmulatorShaderResourcePointers, HeapDescriptorReadsFollowTheLightRecordIndex)
{
	const auto code  = HeapLightLoopProgram();
	const auto reads = ShaderFindHeapDescriptorReads(code);
	ASSERT_EQ(reads.size(), 2u);
	HeapLightReader                  reader;
	ScopedShaderGuestDescriptorReader scope(&reader);
	HW::UserSgprInfo                 user_sgpr {};
	user_sgpr.value[0]      = static_cast<uint32_t>(HeapLightReader::kHeaps);
	user_sgpr.value[4]      = static_cast<uint32_t>(HeapLightReader::kLights);
	const auto& entry       = ShaderScalarFlowOf(code)->holding_entry_value;
	for (const auto& read: reads)
	{
		EXPECT_EQ(read.instruction, 10u);
		EXPECT_EQ(read.slot, 3u);
		const int root = static_cast<int>(read.nodes.size()) - 1;
		for (uint32_t record = 0; record < 3; ++record)
		{
			std::array<uint32_t, 8> words {};
			ASSERT_TRUE(ShaderEvalScalarChain(read.nodes, root, record, user_sgpr, 8, 0, entry, words.data()));
			const uint32_t first = read.sampler ? 0xd0000000u + HeapLightReader::kSamplerIndex[record] * 4u
			                                    : 0xc0000000u + HeapLightReader::kTextureIndex[record] * 8u;
			for (int word = 0; word < (read.sampler ? 4 : 8); ++word)
			{
				EXPECT_EQ(words[word], first + static_cast<uint32_t>(word)) << "record " << record << " sampler " << read.sampler;
			}
		}
		std::array<uint32_t, 8> words {};
		EXPECT_FALSE(ShaderEvalScalarChain(read.nodes, root, 3, user_sgpr, 8, 0, entry, words.data()));
	}
}

// The light loop's heaps holding 2D image T#s and, unless the S#s differ per
// heap index, one S# for every index.
struct HeapImageReader final : ShaderGuestDescriptorReader
{
	bool distinct_samplers = false;

	bool Read(uint64_t addr, uint32_t dwords, uint32_t* out) override
	{
		for (uint32_t word = 0; word < dwords; ++word)
		{
			const uint64_t at = addr + word * 4ull;
			if (at >= HeapLightReader::kTextureHeap && at < HeapLightReader::kTextureHeap + 32 * 8)
			{
				const auto index = static_cast<uint32_t>((at - HeapLightReader::kTextureHeap) / 32);
				const auto field = (at - HeapLightReader::kTextureHeap) % 32 / 4;
				out[word]        = field == 0 ? 0x1000u * (index + 1u) : (field == 1 ? 20u << 20u : (field == 3 ? 9u << 28u : 0u));
			} else if (at >= HeapLightReader::kSamplerHeap && at < HeapLightReader::kSamplerHeap + 16 * 4)
			{
				const auto index = static_cast<uint32_t>((at - HeapLightReader::kSamplerHeap) / 16);
				out[word]        = 0x50u + static_cast<uint32_t>((at - HeapLightReader::kSamplerHeap) % 16 / 4) + (distinct_samplers ? index << 8u : 0u);
			} else if (!HeapLightReader::Word(at, &out[word]))
			{
				return false;
			}
		}
		return true;
	}
};

// Each record binds its own texture, so the module depends on the record count
// and not on which records share a T#; the S# is baked, so records must share it.
TEST(EmulatorShaderResourcePointers, HeapReadsBindATexturePerRecordAndOneSharedSampler)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	const bool prev_next_gen = Config::IsNextGen();
	Config::SetNextGen(true);
	const auto code   = HeapLightLoopProgram();
	const auto sample = code.GetInstructions().At(10);
	for (const bool distinct: {false, true})
	{
		HeapImageReader reader;
		reader.distinct_samplers = distinct;
		ScopedShaderGuestDescriptorReader scope(&reader);
		HW::UserSgprInfo                  user_sgpr {};
		user_sgpr.value[0] = static_cast<uint32_t>(HeapLightReader::kHeaps);
		user_sgpr.value[4] = static_cast<uint32_t>(HeapLightReader::kLights);
		ShaderBindResources bind {};
		ShaderParsedUsage   info {};
		ShaderCollectHeapDescriptors(code, &bind, &info, user_sgpr, 8, 0, *ShaderScalarFlowOf(code), nullptr);
		ASSERT_EQ(bind.heap_textures.Size(), 3u);
		EXPECT_EQ(bind.textures2D.textures_num, 3);
		for (uint32_t record = 0; record < 3; ++record)
		{
			const auto& heap = bind.heap_textures.At(record);
			EXPECT_EQ(heap.consumer_pc, sample.pc);
			EXPECT_EQ(heap.load_pc, code.GetInstructions().At(9).pc);
			EXPECT_EQ(heap.slot_pc, code.GetInstructions().At(3).pc);
			EXPECT_EQ(heap.slot_index.register_id, 38);
			EXPECT_EQ(heap.record, record);
			EXPECT_EQ(bind.textures2D.desc[heap.texture_index].texture.fields[0], 0x1000u * (HeapLightReader::kTextureIndex[record] + 1u));
		}
		EXPECT_EQ(ShaderFindImageSampledTextureDescriptor(sample, bind, 0), bind.heap_textures.At(0).texture_index);
		EXPECT_EQ(ShaderFindImageSamplerDescriptor(sample, bind, 0) >= 0, !distinct);
		EXPECT_EQ(bind.samplers.samplers_num, distinct ? 0 : 1);
	}
	Config::SetNextGen(prev_next_gen);
}

UT_END();
