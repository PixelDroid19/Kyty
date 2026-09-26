#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdio>
#include <cstdlib>

UT_BEGIN(EmulatorShaderScalarPack);

using namespace Libs::Graphics;

TEST(EmulatorShaderScalarPack, PacksLowHalvesAndWritesVccHiAfterReadingAliasedSource)
{
	constexpr uint32_t s_mov_b32_s9_literal      = (0x17du << 23u) | (9u << 16u) | (0x03u << 8u) | 255u;
	constexpr uint32_t s_mov_b32_s21_literal     = (0x17du << 23u) | (21u << 16u) | (0x03u << 8u) | 255u;
	constexpr uint32_t sop2_prefix               = 0x80000000u | (0x32u << 23u);
	constexpr uint32_t s_pack_vcc_hi_vcc_hi_zero = sop2_prefix | (107u << 16u) | (128u << 8u) | 107u;
	constexpr uint32_t s_pack_s17_s9_s21         = sop2_prefix | (17u << 16u) | (21u << 8u) | 9u;
	constexpr uint32_t s_endpgm                  = 0xbf810000u;
	// SOP2 has one literal payload, so initialize both distinct source halves with SOP1 moves.
	const uint32_t shader[] = {s_mov_b32_s9_literal, 0x12345678u, s_mov_b32_s21_literal, 0x9abcdef0u, s_pack_vcc_hi_vcc_hi_zero,
	                           s_pack_s17_s9_s21,    s_endpgm};

	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized())
		    {
			    Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    class ValidationConfig final: public Config::ConfigSource
		    {
			public:
			    bool         Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t      GetInteger(const Core::String&) const override { return 0; }
			    bool         GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation;
		    Config::Load(validation);

		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    if (code.GetInstructions().Size() != 5u)
		    {
			    std::_Exit(2);
		    }

		    const auto& vcc_alias = code.GetInstructions().At(2);
		    if (vcc_alias.type != ShaderInstructionType::SPackLlB32B16 || vcc_alias.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 ||
		        vcc_alias.src_num != 2 || vcc_alias.dst.type != ShaderOperandType::VccHi || vcc_alias.dst.size != 1 ||
		        vcc_alias.src[0].type != ShaderOperandType::VccHi || vcc_alias.src[1].type != ShaderOperandType::IntegerInlineConstant ||
		        vcc_alias.src[1].constant.i != 0)
		    {
			    std::_Exit(3);
		    }

		    const auto& nonzero = code.GetInstructions().At(3);
		    if (nonzero.type != ShaderInstructionType::SPackLlB32B16 || nonzero.dst.type != ShaderOperandType::Sgpr ||
		        nonzero.dst.register_id != 17 || nonzero.src_num != 2 || nonzero.src[0].type != ShaderOperandType::Sgpr ||
		        nonzero.src[0].register_id != 9 || nonzero.src[1].type != ShaderOperandType::Sgpr || nonzero.src[1].register_id != 21)
		    {
			    std::_Exit(4);
		    }

		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    const auto source                                                  = SpirvGenerateSource(code, nullptr, nullptr, &input);

		    const uint32_t vcc_load           = source.FindIndex("%pack_s0_2 = OpLoad %uint %vcc_hi");
		    const uint32_t vcc_store          = source.FindIndex("OpStore %vcc_hi %pack_result_2");
		    const bool     vcc_alias_contract = vcc_load != Core::STRING8_INVALID_INDEX && vcc_store != Core::STRING8_INVALID_INDEX &&
		                                        vcc_load < vcc_store &&
		                                        source.FindIndex("%pack_s1_2 = OpBitcast %uint %int_0") != Core::STRING8_INVALID_INDEX;
		    const auto     vcc_pack_source    = vcc_alias_contract ? source.Mid(vcc_load, vcc_store - vcc_load) : String8();
		    const bool     vcc_no_scc_or_exec_write = vcc_pack_source.FindIndex("OpStore %scc") == Core::STRING8_INVALID_INDEX &&
		                                              vcc_pack_source.FindIndex("OpStore %exec_lo") == Core::STRING8_INVALID_INDEX &&
		                                              vcc_pack_source.FindIndex("OpStore %exec_hi") == Core::STRING8_INVALID_INDEX;
		    const uint32_t pack_load                = source.FindIndex("%pack_s0_3 = OpLoad %uint %s9");
		    const uint32_t pack_store               = source.FindIndex("OpStore %s17 %pack_result_3");
		    const bool     pack_ordered =
		        pack_load != Core::STRING8_INVALID_INDEX && pack_store != Core::STRING8_INVALID_INDEX && pack_load < pack_store;
		    const auto pack_source = pack_ordered ? source.Mid(pack_load, pack_store - pack_load) : String8();
		    const bool low_half_contract =
		        pack_load != Core::STRING8_INVALID_INDEX &&
		        pack_source.FindIndex("%pack_s0_shift_3 = OpShiftLeftLogical %uint %pack_s0_3 %uint_16") != Core::STRING8_INVALID_INDEX &&
		        pack_source.FindIndex("%pack_low_3 = OpShiftRightLogical %uint %pack_s0_shift_3 %uint_16") != Core::STRING8_INVALID_INDEX;
		    const bool high_half_contract =
		        pack_source.FindIndex("%pack_s1_3 = OpLoad %uint %s21") != Core::STRING8_INVALID_INDEX &&
		        pack_source.FindIndex("%pack_high_3 = OpShiftLeftLogical %uint %pack_s1_3 %uint_16") != Core::STRING8_INVALID_INDEX &&
		        pack_source.FindIndex("%pack_result_3 = OpBitwiseOr %uint %pack_low_3 %pack_high_3") != Core::STRING8_INVALID_INDEX &&
		        pack_store != Core::STRING8_INVALID_INDEX;
		    const bool pack_no_scc_or_exec_write = pack_source.FindIndex("OpStore %scc") == Core::STRING8_INVALID_INDEX &&
		                                           pack_source.FindIndex("OpStore %exec_lo") == Core::STRING8_INVALID_INDEX &&
		                                           pack_source.FindIndex("OpStore %exec_hi") == Core::STRING8_INVALID_INDEX;
		    if (!vcc_alias_contract || !vcc_no_scc_or_exec_write || !low_half_contract || !high_half_contract || !pack_no_scc_or_exec_write)
		    {
			    std::fprintf(stderr, "pack contracts: alias=%d vcc_flags=%d low=%d high=%d flags=%d\n",
			                 vcc_alias_contract, vcc_no_scc_or_exec_write, low_half_contract, high_half_contract,
			                 pack_no_scc_or_exec_write);
			    std::_Exit(5);
		    }

		    Vector<uint32_t> binary;
		    String8          error;
		    if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
		    {
			    std::fprintf(stderr, "%s\n", error.c_str());
			    std::_Exit(6);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderScalarPack, RejectsUnimplementedOperandModifiers)
{
	const uint32_t words[] = {0x80000000u | (0x32u << 23u) | (17u << 16u) | (9u << 8u) | 9u, 0xbf810000u};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	for (int modifier = 0; modifier < 4; ++modifier)
	{
		ASSERT_EXIT(
		    {
			    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
			    Config::SetNextGen(true);
			    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    ShaderParse(words, &code);
			    auto& inst = code.GetInstructions()[0];
			    auto& operand = modifier < 2 ? inst.dst : inst.src[0];
			    if ((modifier & 1) == 0) { operand.clamp = true; }
			    else { operand.multiplier = 2.0f; }
			    ShaderComputeInputInfo input {};
			    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
			    (void)SpirvGenerateSource(code, nullptr, nullptr, &input);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(rejected_exit), "");
	}
}

UT_END();
