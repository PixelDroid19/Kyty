#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdlib>

UT_BEGIN(EmulatorShaderVectorPack);

using namespace Libs::Graphics;

TEST(EmulatorShaderVectorPack, DecodesAndLowersGen5Unorm16Pair)
{
	// Synthetic VOP3A fields from the RDNA2 ISA: two VGPR float sources,
	// one VGPR packed destination, and no modifiers. The first source aliases
	// the destination to exercise read-before-write behavior.
	constexpr uint32_t word0 = (0x35u << 26u) | (0x369u << 16u) | 5u;
	constexpr uint32_t word1 = ((256u + 6u) << 9u) | (256u + 5u);
	constexpr uint32_t words[] = {word0, word1, 0xbf810000u};

	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    class ValidationConfig final: public Config::ConfigSource
		    {
		    public:
			    bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t GetInteger(const Core::String&) const override { return 0; }
			    bool GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation;
		    Config::Load(validation);
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().Size() != 2u) { std::_Exit(2); }
		    const auto& pack = code.GetInstructions().At(0);
		    if (pack.type != ShaderInstructionType::VCvtPknormU16F32 ||
		        pack.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || pack.src_num != 2u ||
		        pack.dst.type != ShaderOperandType::Vgpr || pack.dst.register_id != 5 ||
		        pack.src[0].type != ShaderOperandType::Vgpr || pack.src[0].register_id != 5 ||
		        pack.src[1].type != ShaderOperandType::Vgpr || pack.src[1].register_id != 6 ||
		        pack.vop3_op_sel != 0u || pack.vop3_omod != 0u || pack.dst.clamp)
		    {
			    std::_Exit(3);
		    }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    const auto first_load = source.FindIndex("%t0_0 = OpLoad %float %v5");
		    const auto second_load = source.FindIndex("%t1_0 = OpLoad %float %v6");
		    const auto pack_op = source.FindIndex("OpExtInst %uint %GLSL_std_450 PackUnorm2x16");
		    const auto bitcast = source.FindIndex("OpBitcast %float %packed_bits_0");
		    const auto store = source.FindIndex("OpStore %v5 %tval_0");
		    if (first_load == Core::STRING8_INVALID_INDEX || second_load == Core::STRING8_INVALID_INDEX ||
		        pack_op == Core::STRING8_INVALID_INDEX || bitcast == Core::STRING8_INVALID_INDEX ||
		        store == Core::STRING8_INVALID_INDEX || first_load >= pack_op || second_load >= pack_op || pack_op >= store)
		    {
			    std::_Exit(4);
		    }
		    Vector<uint32_t> binary;
		    Core::String8 error;
		    std::_Exit(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty() ? 0 : 5);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
