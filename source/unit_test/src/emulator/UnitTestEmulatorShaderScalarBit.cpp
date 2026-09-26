#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdlib>

UT_BEGIN(EmulatorShaderScalarBit);

using namespace Libs::Graphics;

TEST(EmulatorShaderScalarBit, ParsesAndLowersBitset1B32AsReadModifyWrite)
{
	// Synthetic SOP1 encodings for s_bitset1_b32 with indices 19 and 31,
	// followed by a register-sourced case where S0 aliases SDST. The final case
	// proves the tied old destination and index are both read before the store.
	constexpr uint32_t s_bitset1_s9_19  = (0x17du << 23u) | (9u << 16u) | (0x1du << 8u) | 147u;
	constexpr uint32_t s_bitset1_s17_31 = (0x17du << 23u) | (17u << 16u) | (0x1du << 8u) | 159u;
	constexpr uint32_t s_bitset1_s17_s17 = (0x17du << 23u) | (17u << 16u) | (0x1du << 8u) | 17u;
	constexpr uint32_t s_endpgm          = (0xbf800000u | (0x01u << 16u));
	const uint32_t     shader[]          = {s_bitset1_s9_19, s_bitset1_s17_31, s_bitset1_s17_s17, s_endpgm};

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

		    if (code.GetInstructions().Size() != 4u) { std::_Exit(2); }
		    const auto& index19 = code.GetInstructions().At(0);
		    const auto& index31 = code.GetInstructions().At(1);
		    const auto& alias  = code.GetInstructions().At(2);
		    const auto parsed_bitset = [](const ShaderInstruction& inst, int dst, ShaderOperandType src_type, int src_reg,
		                                  int inline_index)
		    {
			    return inst.type == ShaderInstructionType::SBitset1B32 &&
			           inst.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1 && inst.src_num == 2 &&
			           inst.dst.type == ShaderOperandType::Sgpr && inst.dst.register_id == dst && inst.dst.size == 1 &&
			           inst.src[0].type == src_type &&
			           (src_type == ShaderOperandType::IntegerInlineConstant ? inst.src[0].constant.i == inline_index
			                                                               : inst.src[0].register_id == src_reg) &&
			           inst.src[1] == inst.dst;
		    };
		    if (!parsed_bitset(index19, 9, ShaderOperandType::IntegerInlineConstant, 0, 19) ||
		        !parsed_bitset(index31, 17, ShaderOperandType::IntegerInlineConstant, 0, 31) ||
		        !parsed_bitset(alias, 17, ShaderOperandType::Sgpr, 17, 0))
		    {
			    std::_Exit(3);
		    }

		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    const auto source    = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    const uint32_t alias_block_begin = source.FindIndex("%bitset_index_2 = OpBitwiseAnd");
		    const uint32_t alias_index_load  = source.FindIndex("%t0_2 = OpLoad %uint %s17");
		    const uint32_t alias_old_dst_load = source.FindIndex("%t1_2 = OpLoad %uint %s17");
		    const uint32_t alias_store = source.FindIndex("OpStore %s17 %bitset_result_2");
		    const uint32_t alias_scc_store = source.FindIndex("OpStore %scc", alias_block_begin);
		    const uint32_t alias_exec_store = source.FindIndex("OpStore %exec", alias_block_begin);
		    const bool source_contract =
			    source.FindIndex("%bitset_index_0 = OpBitwiseAnd %uint %t0_0 %uint_31") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%bitset_mask_0 = OpShiftLeftLogical %uint %uint_1 %bitset_index_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%bitset_result_0 = OpBitwiseOr %uint %t1_0 %bitset_mask_0") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("OpStore %s9 %bitset_result_0") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%bitset_index_1 = OpBitwiseAnd %uint %t0_1 %uint_31") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%bitset_result_1 = OpBitwiseOr %uint %t1_1 %bitset_mask_1") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("OpStore %s17 %bitset_result_1") != Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%bitset_index_2 = OpBitwiseAnd %uint %t0_2 %uint_31") !=
			        Core::STRING8_INVALID_INDEX &&
			    source.FindIndex("%bitset_result_2 = OpBitwiseOr %uint %t1_2 %bitset_mask_2") !=
			        Core::STRING8_INVALID_INDEX &&
			    alias_block_begin != Core::STRING8_INVALID_INDEX && alias_index_load != Core::STRING8_INVALID_INDEX &&
			    alias_old_dst_load != Core::STRING8_INVALID_INDEX && alias_store != Core::STRING8_INVALID_INDEX &&
			    alias_index_load < alias_store && alias_old_dst_load < alias_store &&
			    alias_scc_store == Core::STRING8_INVALID_INDEX && alias_exec_store == Core::STRING8_INVALID_INDEX;

		    Vector<uint32_t> binary;
		    Core::String8    error;
		    const bool       toolchain_contract = ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty();
		    std::_Exit(source_contract && toolchain_contract ? 0 : 4);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
