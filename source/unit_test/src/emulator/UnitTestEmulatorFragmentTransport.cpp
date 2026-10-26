#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/FragmentTransportResolve.h"
#include "Emulator/Graphics/FragmentTransportShade.h"
#include "Emulator/Graphics/Shader.h"
#include "NggPassthroughFixture.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdint>

UT_BEGIN(EmulatorFragmentTransport);

using namespace Libs::Graphics;

namespace {

using namespace NggFixture;

// The production path assembles these modules with the guest program's real
// lane packing. Force the same validation here so an incomplete interface or a
// packed offset that does not match the record fails the suite instead of the
// driver.
class ScopedShaderValidation final: public Config::ConfigSource
{
public:
	ScopedShaderValidation(): m_previous(Config::ShaderValidationEnabled()) { Config::Load(*this); }
	~ScopedShaderValidation() override
	{
		m_enabled = m_previous;
		Config::Load(*this);
	}
	bool         Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
	int64_t      GetInteger(const Core::String&) const override { return 0; }
	bool         GetBool(const Core::String&) const override { return m_enabled; }
	Core::String GetString(const Core::String&) const override { return {}; }

private:
	bool m_previous;
	bool m_enabled = true;
};

ShaderInstruction Interpolation(ShaderInstructionType type, uint32_t destination, uint32_t attribute, uint32_t channel)
{
	ShaderInstruction instruction {};
	instruction.type               = type;
	instruction.format             = ShaderInstructionFormat::VdstVsrcAttrChan;
	instruction.dst.type           = ShaderOperandType::Vgpr;
	instruction.dst.register_id    = static_cast<int>(destination);
	instruction.dst.size           = 1;
	instruction.src[0].type        = ShaderOperandType::Vgpr;
	instruction.src[0].register_id = 0;
	instruction.src[0].size        = 1;
	instruction.src[1].type        = ShaderOperandType::IntegerInlineConstant;
	instruction.src[1].constant.u  = attribute;
	instruction.src[2].type        = ShaderOperandType::IntegerInlineConstant;
	instruction.src[2].constant.u  = channel;
	instruction.src_num            = 3;
	return instruction;
}

void SplitShadeModuleValidates()
{
	ScopedShaderValidation validation;

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	code.GetInstructions().Add(Interpolation(ShaderInstructionType::VInterpP1F32, 0, 0, 0));
	code.GetInstructions().Add(Interpolation(ShaderInstructionType::VInterpP2F32, 1, 0, 0));
	ShaderInstruction end {};
	end.type = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);

	ShaderPixelInputInfo pixel {};
	pixel.input_num = 1;

	Vector<uint32_t> binary;
	String8          error;
	Check(FragmentTransportBuildShadeModule(code, pixel, 0u, 0u, &binary, &error), error.c_str());
	Check(binary.Size() > 8u, "split shade module has code");
}

void ResolveModuleValidates()
{
	ScopedShaderValidation validation;

	const auto source = FragmentTransport::GenerateResolveSource(2u, 0x00000033u);
	Check(!source.StartsWith("OpKyty"), "resolve source is generated");

	Vector<uint32_t> binary;
	String8          error;
	Check(ShaderToolchain::Run(source, &binary, &error), error.c_str());
	Check(binary.Size() > 8u, "resolve module has code");
}

void LaneWordsFollowInterpolatorSource()
{
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	code.GetInstructions().Add(Interpolation(ShaderInstructionType::VInterpP1F32, 0, 1, 0));
	code.GetInstructions().Add(Interpolation(ShaderInstructionType::VInterpP2F32, 1, 1, 0));
	ShaderInstruction move = Interpolation(ShaderInstructionType::VInterpMovF32, 2, 1, 3);
	move.src[0].type       = ShaderOperandType::IntegerInlineConstant;
	move.src[0].constant.u = 0;
	move.src[0].size       = 0;
	code.GetInstructions().Add(move);

	ShaderPixelInputInfo pixel {};
	pixel.input_num = 2;
	const uint32_t base = 1u + FragmentTransportInitialVgprs(pixel);
	Check(FragmentTransportLaneWords(code, pixel) == base + 4u, "a parameter v_interp_mov shares the interpolated slot");

	pixel.custom_interpolation.per_vertex_inputs = 1u << 1u;
	Check(FragmentTransportLaneWords(code, pixel) == base + 16u, "a per-vertex v_interp_mov stores three vertices");
}

void LaneWordsPackOnlyObservedForms()
{
	ScopedShaderValidation validation;

	ShaderCode parameter_code;
	parameter_code.SetType(ShaderType::Pixel);
	parameter_code.GetInstructions().Add(Interpolation(ShaderInstructionType::VInterpP1F32, 0, 0, 0));

	ShaderPixelInputInfo pixel {};
	pixel.input_num = 1;
	const auto parameter_words = FragmentTransportLaneWords(parameter_code, pixel);
	Check(parameter_words == 1u + FragmentTransportInitialVgprs(pixel) + 4u, "one interpolated parameter costs four words");

	ShaderCode raw_code;
	raw_code.SetType(ShaderType::Pixel);
	ShaderInstruction move = Interpolation(ShaderInstructionType::VInterpMovF32, 0, 0, 0);
	move.src[0].type       = ShaderOperandType::IntegerInlineConstant;
	move.src[0].constant.u = 0;
	move.src[0].size       = 0;
	raw_code.GetInstructions().Add(move);
	pixel.custom_interpolation.per_vertex_inputs = 1u << 0u;
	const auto raw_words = FragmentTransportLaneWords(raw_code, pixel);
	Check(raw_words == 1u + FragmentTransportInitialVgprs(pixel) + 12u, "one per-vertex v_interp_mov source costs twelve words");
}

} // namespace

TEST(EmulatorFragmentTransport, SplitShadeModuleValidates)
{
	ASSERT_EXIT(([] { Initialize(); SplitShadeModuleValidates(); std::_Exit(0); })(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentTransport, ResolveModuleValidates)
{
	ASSERT_EXIT(([] { Initialize(); ResolveModuleValidates(); std::_Exit(0); })(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentTransport, LaneWordsPackOnlyObservedForms)
{
	ASSERT_EXIT(([] { Initialize(); LaneWordsPackOnlyObservedForms(); std::_Exit(0); })(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentTransport, LaneWordsFollowInterpolatorSource)
{
	ASSERT_EXIT(([] { Initialize(); LaneWordsFollowInterpolatorSource(); std::_Exit(0); })(), ::testing::ExitedWithCode(0), "");
}

UT_END();
