#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "../../../emulator/src/Graphics/ShaderStorageAnalysis.h"

#include <array>
#include <cstdlib>

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
	sharps.back().offset_dw = 32 + SHADER_GEN5_EUD_MAX_DWORDS;
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

UT_END();
