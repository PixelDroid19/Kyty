#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveSdwa.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdlib>
#include <initializer_list>

UT_BEGIN(EmulatorComputeWaveSdwa);

using namespace Libs::Graphics;

static constexpr uint32_t kEnd = 0xbf810000u; // s_endpgm

static void InitializeConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

// The whole-dword lowering preconditions always refuse a partial destination select; the per-lane emitter
// admits exactly the selects it merges.
static bool PartialDestinationRowHolds(uint32_t word0, uint32_t ctrl, bool merged)
{
	InitializeConfig();
	const uint32_t words[] = {word0, ctrl, kEnd};
	ShaderCode     code;
	code.SetType(ShaderType::Pixel);
	if (!ShaderTryParseBounded(words, sizeof(words), &code))
	{
		return false;
	}
	const auto& inst = code.GetInstructions().At(0);
	return ShaderSdwaPartialDestinationSupported(inst) == merged && ShaderInstructionPerLaneLoweringPreconditions(inst) == merged &&
	       !ShaderInstructionLoweringPreconditions(inst);
}

TEST(EmulatorComputeWaveSdwa, MergesOnlyPartialDestinationSelectsPerLane)
{
	// v_cvt_i32_f32_sdwa v13, v13 (VOP1 0x08) with src0_sel=DWORD; the observed control 0x0006140d is
	// dst_sel=WORD_0, dst_u=PRESERVE. Each row flips the destination fields or adds clamp/omod.
	static constexpr uint32_t kCvtSdwaW0 = 0x7e1a10f9u;
	static constexpr uint32_t kMacSdwaW0 = 0x3e1a1af9u; // v_mac_f32_sdwa v13, v13, v13 (VOP2 0x1f)
	struct Row
	{
		uint32_t word0;
		uint32_t ctrl;
		bool     merged;
	};
	const Row rows[] = {
	    {kCvtSdwaW0, 0x0006140du, true},  // WORD_0, PRESERVE
	    {kCvtSdwaW0, 0x0006040du, true},  // WORD_0, PAD
	    {kCvtSdwaW0, 0x00060c0du, true},  // WORD_0, SEXT
	    {kCvtSdwaW0, 0x0006100du, true},  // BYTE_0, PRESERVE
	    {kCvtSdwaW0, 0x0006150du, true},  // WORD_1, PRESERVE
	    {kCvtSdwaW0, 0x0006110du, true},  // BYTE_1, PRESERVE
	    {kCvtSdwaW0, 0x0006120du, true},  // BYTE_2, PRESERVE (an RGBA8 channel pack)
	    {kCvtSdwaW0, 0x00060b0du, true},  // BYTE_3, SEXT
	    {kCvtSdwaW0, 0x0006070du, false}, // dst_sel reserved
	    {kCvtSdwaW0, 0x00061c0du, false}, // dst_u reserved
	    {kCvtSdwaW0, 0x0006340du, false}, // clamp
	    {kCvtSdwaW0, 0x0006540du, false}, // omod
	    {kMacSdwaW0, 0x0606140du, false}, // reads its destination
	};
	for (const auto& row: rows)
	{
		ASSERT_EXIT({ std::_Exit(PartialDestinationRowHolds(row.word0, row.ctrl, row.merged) ? 0 : 3); }, ::testing::ExitedWithCode(0), "");
	}
}
// Every admitted select assembles and validates: the merge's field masks and shifts are declared constants.
[[noreturn]] static void RunPartialDestinationMerge(uint32_t ctrl)
{
	static constexpr uint32_t kCvtSdwaW0 = 0x7e1a10f9u; // v_cvt_i32_f32_sdwa v13, v13
	InitializeConfig();
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool         Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t      GetInteger(const Core::String&) const override { return 0; }
		bool         GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation;
	Config::Load(validation);
	const uint32_t words[] = {kCvtSdwaW0, ctrl, kEnd};
	ShaderCode     code;
	code.SetType(ShaderType::Pixel);
	if (!ShaderTryParseBounded(words, sizeof(words), &code) || !ShaderSdwaPartialDestinationSupported(code.GetInstructions().At(0)))
	{
		std::_Exit(2);
	}
	ShaderPixelInputInfo input {};
	const auto           source = SpirvGenerateSource(code, nullptr, &input, nullptr);
	Vector<uint32_t>     binary;
	String8              error;
	std::_Exit(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty() ? 0 : 3);
}

TEST(EmulatorComputeWaveSdwa, PartialDestinationMergeValidatesForEverySelect)
{
	// BYTE_0-3 and WORD_0-1 with PRESERVE, BYTE_2 with PAD and BYTE_3 with SEXT.
	for (const uint32_t ctrl: {0x0006100du, 0x0006110du, 0x0006120du, 0x0006130du, 0x0006140du, 0x0006150du, 0x0006020du, 0x00060b0du})
	{
		ASSERT_EXIT(RunPartialDestinationMerge(ctrl), ::testing::ExitedWithCode(0), "");
	}
}
UT_END();
