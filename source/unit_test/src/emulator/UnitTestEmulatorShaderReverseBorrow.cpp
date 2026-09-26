#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdlib>

UT_BEGIN(EmulatorShaderReverseBorrow);

using namespace Libs::Graphics;

TEST(EmulatorShaderReverseBorrow, ParsesAndLowersGen5VSubrevCoCiU32)
{
	// Synthetic VOP2/VOP3B encodings, deliberately assembled from instruction
	// fields rather than copied workload words. Both forms compute
	// src1 - src0 - borrow_in; VOP2 uses the implicit VCC pair and VOP3B uses
	// the explicit scalar pairs below. VOP3B's S4:S5 borrow input is produced
	// by the preceding full-pair V_CMP_LT_U32.
	constexpr uint32_t vop3_cmp_lt_u32 = (0xd4u << 24u) | (0xc1u << 16u) | 4u;
	constexpr uint32_t vop3_cmp_sources = 256u | ((256u + 1u) << 9u);
	constexpr uint32_t vop2_subrev_co_ci = (0x2au << 25u) | (5u << 17u) | (17u << 9u) | 128u;
	constexpr uint32_t vop3_subrev_co_ci = (0xd4u << 24u) | (0x12au << 16u) | (8u << 8u) | 6u;
	constexpr uint32_t vop3_sources      = (129u << 0u) | ((256u + 18u) << 9u) | (4u << 18u);
	constexpr uint32_t s_endpgm          = (0xbf800000u | (0x01u << 16u));
	const uint32_t     shader[]          = {vop2_subrev_co_ci, vop3_cmp_lt_u32, vop3_cmp_sources,
	                                        vop3_subrev_co_ci, vop3_sources, s_endpgm};

	// The missing parser exits before it can emit an instruction on the old
	// behavior. A subprocess keeps that expected RED failure contained while
	// the successful path proves parser shape, generated SPIR-V, and the real
	// toolchain assembly/optimization route together.
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
			    bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t GetInteger(const Core::String&) const override { return 0; }
			    bool GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation_config;
		    Config::Load(validation_config);

		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);

		    if (code.GetInstructions().Size() != 4u)
		    {
			    std::_Exit(2);
		    }
		    const auto& vop2 = code.GetInstructions().At(0);
		    const auto& cmp  = code.GetInstructions().At(1);
		    const auto& vop3 = code.GetInstructions().At(2);
		    const bool cmp_contract = cmp.type == ShaderInstructionType::VCmpLtU32 && cmp.dst.type == ShaderOperandType::Sgpr &&
		                             cmp.dst.register_id == 4 && cmp.dst.size == 2;
		    const bool vop2_contract =
			    vop2.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2 && vop2.src_num == 3 &&
			    vop2.dst.type == ShaderOperandType::Vgpr && vop2.dst.register_id == 5 &&
			    vop2.src[0].type == ShaderOperandType::IntegerInlineConstant && vop2.src[0].constant.i == 0 &&
			    vop2.src[1].type == ShaderOperandType::Vgpr && vop2.src[1].register_id == 17 &&
			    vop2.src[2].type == ShaderOperandType::VccLo && vop2.src[2].size == 2 &&
			    vop2.dst2.type == ShaderOperandType::VccLo && vop2.dst2.size == 2;
		    const bool vop3_contract =
			    vop3.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2 && vop3.src_num == 3 &&
			    vop3.dst.type == ShaderOperandType::Vgpr && vop3.dst.register_id == 6 &&
			    vop3.src[0].type == ShaderOperandType::IntegerInlineConstant && vop3.src[0].constant.i == 1 &&
			    vop3.src[1].type == ShaderOperandType::Vgpr && vop3.src[1].register_id == 18 &&
			    vop3.src[2].type == ShaderOperandType::Sgpr && vop3.src[2].register_id == 4 && vop3.src[2].size == 2 &&
			    vop3.dst2.type == ShaderOperandType::Sgpr && vop3.dst2.register_id == 8 && vop3.dst2.size == 2;
		    if (!vop2_contract || !cmp_contract || !vop3_contract)
		    {
			    std::_Exit(3);
		    }

		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = 1;
		    input.threads_num[1] = 1;
		    input.threads_num[2] = 1;
		    const auto source    = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    const bool source_contract =
			    source.FindIndex("%t2_0 = OpLoad %uint %vcc_lo") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%t2_2 = OpLoad %uint %s4") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_carry_nonzero_0 = OpINotEqual %bool %t2_0 %uint_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_carry_nonzero_2 = OpINotEqual %bool %t2_2 %uint_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_base_0 = OpISubBorrow %ResTypeU %t1_0 %t0_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_base_2 = OpISubBorrow %ResTypeU %t1_2 %t0_2") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_carry_0 = OpSelect %uint %subrev_carry_nonzero_0 %uint_1 %uint_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_carry_2 = OpSelect %uint %subrev_carry_nonzero_2 %uint_1 %uint_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_adjusted_0 = OpISubBorrow %ResTypeU %subrev_value_0 %subrev_carry_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_adjusted_2 = OpISubBorrow %ResTypeU %subrev_value_2 %subrev_carry_2") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex(
			        "%subrev_borrow_0 = OpBitwiseOr %uint %subrev_base_borrow_0 %subrev_adjusted_borrow_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex(
			        "%subrev_borrow_2 = OpBitwiseOr %uint %subrev_base_borrow_2 %subrev_adjusted_borrow_2") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("OpStore %vcc_lo %t213_0") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_old_dst_0 = OpLoad %float %v5") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_old_dst_2 = OpLoad %float %v6") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_dst_0 = OpSelect %float %exec_lo_b_0 %t210_0 %subrev_old_dst_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%subrev_dst_2 = OpSelect %float %exec_lo_b_2 %t210_2 %subrev_old_dst_2") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("OpStore %v5 %subrev_dst_0") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("OpStore %v6 %subrev_dst_2") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("OpStore %vcc_hi %uint_0") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("OpStore %s8 %t213_2") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("OpStore %s9 %uint_0") != Core::STRING8_INVALID_INDEX;

		    Vector<uint32_t>       binary;
		    Core::String8          error;
		    const bool toolchain_contract = ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty();
		    std::_Exit(source_contract && toolchain_contract ? 0 : 4);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderReverseBorrow, RejectsUnprovenScalarBorrowMask)
{
	// Raw s_mov_b64 does not prove that a scalar pair contains normalized
	// per-invocation comparison bits, even when reverse-borrow consumes it.
	constexpr uint32_t s_mov_b64_s4_s2 = (0x17du << 23u) | (4u << 16u) | (0x04u << 8u) | 2u;
	constexpr uint32_t vop3_subrev_co_ci = (0xd4u << 24u) | (0x12au << 16u) | (8u << 8u) | 6u;
	constexpr uint32_t vop3_sources      = (129u << 0u) | ((256u + 18u) << 9u) | (4u << 18u);
	constexpr uint32_t s_endpgm          = (0xbf800000u | (0x01u << 16u));
	const uint32_t     shader[]          = {s_mov_b64_s4_s2, vop3_subrev_co_ci, vop3_sources, s_endpgm};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized())
		    {
			    Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    if (code.GetInstructions().Size() != 3u || code.GetInstructions().At(0).type != ShaderInstructionType::SMovB64 ||
		        code.GetInstructions().At(1).type != ShaderInstructionType::VSubrevCoCiU32 ||
		        code.GetInstructions().At(1).src[2].type != ShaderOperandType::Sgpr ||
		        code.GetInstructions().At(1).src[2].register_id != 4 || code.GetInstructions().At(1).src[2].size != 2)
		    {
			    std::_Exit(2);
		    }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    (void)SpirvGenerateSource(code, nullptr, nullptr, &input);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(rejected_exit), "");
}

TEST(EmulatorShaderReverseBorrow, RejectsUnimplementedReverseBorrowModifiers)
{
	// Parsing a neutral instruction first isolates emitter modifier rejection
	// from the previously unsupported opcode. Its VCC source is the explicit
	// backend mask contract; each child changes one modifier.
	const uint32_t shader[] = {(0xd4u << 24u) | (0x12au << 16u) | (8u << 8u) | 6u,
	                           2u | ((256u + 18u) << 9u) | (106u << 18u), 0xbf810000u};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	for (uint32_t modifier = 0; modifier < 8; ++modifier)
	{
		ASSERT_EXIT(
		    {
			    if (!Config::IsInitialized())
			    {
				    Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    }
			    Config::SetNextGen(true);
			    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    ShaderParse(shader, &code);
			    auto& inst = code.GetInstructions()[0];
			    if (inst.type != ShaderInstructionType::VSubrevCoCiU32) { std::_Exit(2); }
			    switch (modifier)
			    {
				    case 0: inst.dst.clamp = true; break;
				    case 1: inst.dst.multiplier = 2.0f; break;
				    case 2: inst.src[0].negate = true; break;
				    case 3: inst.src[0].absolute = true; break;
				    case 4: inst.src[1].negate = true; break;
				    case 5: inst.src[1].absolute = true; break;
				    case 6: inst.src[2].negate = true; break;
				    case 7: inst.src[2].absolute = true; break;
			    }
			    ShaderComputeInputInfo input {};
			    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
			    (void)SpirvGenerateSource(code, nullptr, nullptr, &input);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(rejected_exit), "");
	}
}

TEST(EmulatorShaderReverseBorrow, RejectsUnsupportedReverseBorrowSdwaSelections)
{
	constexpr uint32_t instruction = (0x2au << 25u) | (5u << 17u) | (17u << 9u) | 249u;
	constexpr uint32_t neutral = 1u | (6u << 8u) | (6u << 16u) | (6u << 24u);
	const uint32_t controls[] = {neutral & ~(7u << 8u), neutral | (1u << 11u), neutral | (1u << 19u),
	                             neutral | (1u << 27u), neutral | (1u << 16u), neutral | (1u << 24u)};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	for (const auto control: controls)
	{
		const uint32_t shader[] = {instruction, control, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    if (!Config::IsInitialized())
			    {
				    Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    }
			    Config::SetNextGen(true);
			    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    ShaderParse(shader, &code);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(rejected_exit), "");
	}
}

UT_END();
