#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveNativeEquivalence.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>
#include <initializer_list>

UT_BEGIN(EmulatorComputeWaveNativeEquivalence);

using namespace Libs::Graphics;

static void InitializeShaderConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

// Exit 0 when the analysis outcome matches: supported, or rejected at pc with
// a reason containing reason_part.
static void ExpectNativeEquivalence(std::initializer_list<uint32_t> words, bool supported, uint32_t pc, const char* reason_part)
{
	ASSERT_EXIT(
	    {
		    InitializeShaderConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words.begin(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveNativeEquivalence(code);
		    if (supported)
		    {
			    std::_Exit(result.supported ? 0 : 3);
		    }
		    std::_Exit(!result.supported && result.unsupported_pc == pc && result.reason.ContainsStr(reason_part) ? 0 : 4);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// RDNA2 encodings.
static constexpr uint32_t kCmpNeVcc     = 0x7d8a0a80u; // v_cmp_ne_u32 vcc, 0, v5
static constexpr uint32_t kSaveexecVcc  = 0xbe90246au; // s_and_saveexec_b64 s[16:17], vcc
static constexpr uint32_t kExeczSkip1   = 0xbf880001u; // s_cbranch_execz +1 dword
static constexpr uint32_t kMovV1Zero    = 0x7e020280u; // v_mov_b32 v1, 0
static constexpr uint32_t kRestoreExec  = 0xbefe0410u; // s_mov_b64 exec, s[16:17]
static constexpr uint32_t kMovS20Seven  = 0xbe940387u; // s_mov_b32 s20, 7
static constexpr uint32_t kMovV1S20     = 0x7e020214u; // v_mov_b32 v1, s20
static constexpr uint32_t kMovV1VccLo   = 0x7e02026au; // v_mov_b32 v1, vcc_lo
static constexpr uint32_t kMovV1ExecLo  = 0x7e02027eu; // v_mov_b32 v1, exec_lo
static constexpr uint32_t kFirstlane    = 0x7e000501u; // v_readfirstlane_b32 s0, v1
static constexpr uint32_t kExecFromData = 0xbefe040eu; // s_mov_b64 exec, s[14:15]
static constexpr uint32_t kEnd          = 0xbf810000u; // s_endpgm

TEST(EmulatorComputeWaveNativeEquivalence, AdmitsLaneLocalDivergentRegion)
{
	ExpectNativeEquivalence({kCmpNeVcc, kSaveexecVcc, kExeczSkip1, kMovV1Zero, kRestoreExec, kEnd}, true, 0, "");
}

TEST(EmulatorComputeWaveNativeEquivalence, RejectsScalarWriteLiveAtJoin)
{
	// A host subgroup that skips the region would keep a stale s20.
	ExpectNativeEquivalence({kCmpNeVcc, kSaveexecVcc, kExeczSkip1, kMovS20Seven, kRestoreExec, kMovV1S20, kEnd}, false, 0x8u,
	                        "live at its join");
}

TEST(EmulatorComputeWaveNativeEquivalence, AdmitsScalarWriteDeadAtJoin)
{
	ExpectNativeEquivalence({kCmpNeVcc, kSaveexecVcc, kExeczSkip1, kMovS20Seven, kRestoreExec, kEnd}, true, 0, "");
}

TEST(EmulatorComputeWaveNativeEquivalence, RejectsMaskReadAsData)
{
	ExpectNativeEquivalence({kCmpNeVcc, kMovV1VccLo, kEnd}, false, 0x4u, "read as uniform data");
}

TEST(EmulatorComputeWaveNativeEquivalence, RejectsExecReadAsData)
{
	ExpectNativeEquivalence({kMovV1ExecLo, kEnd}, false, 0x0u, "not representable per lane");
}

TEST(EmulatorComputeWaveNativeEquivalence, RejectsLaneCrossing)
{
	ExpectNativeEquivalence({kFirstlane, kEnd}, false, 0x0u, "another lane");
}

TEST(EmulatorComputeWaveNativeEquivalence, RejectsMaskFromUniformData)
{
	ExpectNativeEquivalence({kExecFromData, kEnd}, false, 0x0u, "may hold uniform data");
}

UT_END();
