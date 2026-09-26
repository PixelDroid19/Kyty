#include "Kyty/UnitTest.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveLdsSafety.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>
#include <vector>

UT_BEGIN(EmulatorComputeWaveLdsSafety);
using namespace Libs::Graphics;

static ShaderComputeInputInfo PairedInput()
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 128;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.thread_ids_num = 1;
	input.lds_dwords = 1;
	input.wave_layout = {ShaderComputeWaveStrategy::Paired64On32, {128, 1, 1}, {64, 1, 1}, 64, 32, 2, 2, 1};
	return input;
}

TEST(EmulatorComputeWaveLdsSafety, RejectsSharedWritesAndUnsynchronizedEffectPhases)
{
	struct Case { std::vector<uint32_t> words; uint32_t pc; bool accepted = false; };
	const Case cases[] = {
	    {{0x87fe807eu, 0x7e020280u, 0xd8d80000u, 0x04000001u, 0xbf810000u}, 8u}, // AND clears EXEC; move no longer initializes all lanes
	    {{0xbe842480u, 0x7e020280u, 0xd8d80000u, 0x04000001u, 0xbf810000u}, 8u}, // SAVEEXEC implicitly clears EXEC
	    {{0x7e020280u, 0xd4c2006au, 256u | (128u << 9u), 0xbe84046au, 0x8884c104u,
	      0xbefe0404u, 0xd8340000u, 0x00000101u, 0xbf810000u}, 24u}, // OR invalidates the saved singleton pair
	    {{0x7e020280u, 0xd4c2006au, 256u | (128u << 9u), 0xbefe046au, 0x88fec17eu,
	      0xd8340000u, 0x00000101u, 0xbf810000u}, 20u}, // OR invalidates singleton EXEC directly
	    {{0x7e020280u, 0xd8340000u, 0x00000001u, 0xbf810000u}, 4u}, // all lanes write their distinct v0 to address0
	    {{0x7e020280u, 0xbefe0481u, 0xd8340000u, 0x00000101u, 0xbf810000u}, 8u}, // one writer PER wave, not per group
	    {{0x7e020280u, 0xd4c2006au, 256u | (128u << 9u), 0xbefe046au,
	      0xd8340000u, 0x00000101u, 0xbefe04c1u, 0xd8d80000u, 0x04000001u, 0xbf810000u}, 28u}, // missing producer/consumer barrier
	    {{0x7e020280u, 0xd4c2006au, 256u | (128u << 9u), 0xbe84046au, 0xbe850381u,
	      0xbefe0404u, 0xd8340000u, 0x00000101u, 0xbf810000u}, 24u}, // single-word write invalidates pair provenance
	    {{0x7e020280u, 0xd4c2006au, 256u | (128u << 9u), 0xbe84046au, 0xd5820005u, 256u,
	      0xbefe0404u, 0xd8340000u, 0x00000101u, 0xbf810000u}, 28u}, // readfirstlane also overwrites a mask half
	    {{0x7e020280u, 0xd4c2006au, 256u | (128u << 9u), 0x8f6b8214u, 0xbefe046au,
	      0xd8340000u, 0x00000101u, 0xbf810000u}, 20u}, // s_lshl_b32 vcc_hi replaces the VCC high word
	    {{0x7e020280u, 0x7e000280u, 0xd4c2006au, 256u | (128u << 9u),
	      0xbefe046au, 0xd8340000u, 0x00000101u, 0xbf810000u}, 20u}, // v0 is no longer a unique initial X
	    {{0x7e020280u, 0xd4c2006au, 256u | (128u << 9u), 0xbe84046au, 0xbe840404u,
	      0xbefe0404u, 0xd8340000u, 0x00000101u, 0xbf810000u}, 0u, true}, // pair self-copy preserves a proven single writer
	};
	for (const auto& test: cases)
	{
		SCOPED_TRACE(test.pc);
		EXPECT_EXIT(
		    {
			    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
			    Config::SetNextGen(true);
			    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(test.words.data(), test.words.size() * sizeof(uint32_t), &code)) { std::_Exit(2); }
			    const auto result = ShaderAnalyzeComputeWaveCode(code, PairedInput());
			    std::_Exit(result.supported == test.accepted && (result.supported || result.unsupported_pc == test.pc) ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveLdsSafety, MultiwordScalarLoadInvalidatesSingletonMaskProvenance)
{
	const std::vector<uint32_t> words {0x7e020280u, 0xd4c2006au, 256u | (128u << 9u), 0xbe84046au,
	                                   0xf4080106u, 0xfa000050u, 0xbefe0404u, 0xd8340000u,
	                                   0x00000101u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words.data(), words.size() * sizeof(uint32_t), &code)) { std::_Exit(2); }
		    const auto result = ShaderAnalyzeComputeWaveLdsSafety(code, PairedInput());
		    std::_Exit(!result.supported && result.unsupported_pc == 28u ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
