#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveNativeEquivalence.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdio>
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
			    if (!result.supported) { std::fprintf(stderr, "refused at pc=0x%x: %s\n", result.unsupported_pc, result.reason.c_str()); }
			    std::_Exit(result.supported ? 0 : 3);
		    }
		    const bool matches = !result.supported && result.unsupported_pc == pc && result.reason.ContainsStr(reason_part);
		    if (!matches) { std::fprintf(stderr, "got supported=%d pc=0x%x reason=%s\n", result.supported ? 1 : 0, result.unsupported_pc, result.reason.c_str()); }
		    std::_Exit(matches ? 0 : 4);
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

// A loop whose back edge tests VCC runs the same iterations on every lane when VCC is provably
// wave-uniform: the counter starts from a constant, advances by a constant and is compared with
// a constant, all under a full EXEC. Then no lane leaves before another and no scalar can diverge.
static constexpr uint32_t kExecFull    = 0xbefe04c1u; // s_mov_b64 exec, -1
static constexpr uint32_t kAddV1One    = 0x4a020281u; // v_add_nc_u32 v1, 1, v1
static constexpr uint32_t kCmpV1Below8 = 0x7d880288u; // v_cmp_gt_u32 vcc, 8, v1
static constexpr uint32_t kCmpV0V1     = 0x7d880300u; // v_cmp_gt_u32 vcc, v0, v1 (v0 is a per-lane input)
static constexpr uint32_t kBackVccnz3  = 0xbf87fffdu; // s_cbranch_vccnz -3 dwords
static constexpr uint32_t kBackExecz3  = 0xbf88fffdu; // s_cbranch_execz -3 dwords

TEST(EmulatorComputeWaveNativeEquivalence, AdmitsALoopOnAWaveUniformCounter)
{
	ExpectNativeEquivalence({kExecFull, kMovV1Zero, kAddV1One, kCmpV1Below8, kBackVccnz3, kEnd}, true, 0, "");
}

TEST(EmulatorComputeWaveNativeEquivalence, RejectsALoopCounterStartedUnderAnUnknownExec)
{
	// Lanes that are inactive when the counter is initialized keep whatever they held.
	ExpectNativeEquivalence({kMovV1Zero, kAddV1One, kCmpV1Below8, kBackVccnz3, kEnd}, false, 0xcu, "wave-uniform");
}

TEST(EmulatorComputeWaveNativeEquivalence, RejectsALoopOnAPerLaneCondition)
{
	ExpectNativeEquivalence({kExecFull, kMovV1Zero, kAddV1One, kCmpV0V1, kBackVccnz3, kEnd}, false, 0x10u, "wave-uniform");
}

TEST(EmulatorComputeWaveNativeEquivalence, RejectsALoopCounterAdvancedUnderANarrowedExec)
{
	// The second increment runs only for the lanes selected by a per-lane compare, so the counter
	// differs between lanes by the time the uniform-looking test reads it.
	ExpectNativeEquivalence({kExecFull, kMovV1Zero, kAddV1One, kCmpNeVcc, kSaveexecVcc, kExeczSkip1, kAddV1One, kRestoreExec,
	                         kExecFull, kCmpV1Below8, 0xbf87fff7u, kEnd},
	                        false, 0x28u, "wave-uniform");
}

TEST(EmulatorComputeWaveNativeEquivalence, StillRejectsALoopExitedThroughExec)
{
	// A lane that has left the loop is only kept idle by an EXEC narrowing this proof does not model.
	ExpectNativeEquivalence({kExecFull, kMovV1Zero, kAddV1One, kCmpV1Below8, kBackExecz3, kEnd}, false, 0x10u, "not a forward region");
}

UT_END();
