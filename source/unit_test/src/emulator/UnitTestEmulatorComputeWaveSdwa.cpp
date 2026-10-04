#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>
#include <initializer_list>

UT_BEGIN(EmulatorComputeWaveSdwa);

using namespace Libs::Graphics;

// Synthetic RDNA2 words; each decodes identically with the LLVM gfx1030
// assembler. VOP1 v_mov_b32 with the SDWA literal (src0 encoding 249) and a
// second control dword whose BYTE/WORD/DWORD select drives a zero-extend
// extract.
static constexpr uint32_t kMovSdwaW0 = 0x7e0c02f9u; // v_mov_b32_sdwa v6, <src0>
static constexpr uint32_t kCtrlBase  = 0x00000616u; // src0=v22, dst_sel=DWORD, dst_u=PAD, sel=BYTE_0
static constexpr uint32_t kUnsupportedSentinel = 0xc8000001u; // v_interp_p1_f32 v0, v1, attr0.x; pixel-only, outside the paired compute set
static constexpr uint32_t kEnd       = 0xbf810000u; // s_endpgm

static void InitializeConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

static ShaderComputeInputInfo PairedInput()
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num = 1;
	input.wave_layout    = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0};
	return input;
}

static void ExpectFirstUnsupportedPc(std::initializer_list<uint32_t> words, uint32_t pc, const char* reason_part)
{
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words.begin(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, PairedInput());
		    std::_Exit(!result.supported && result.unsupported_pc == pc && result.reason.ContainsStr(reason_part) ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveSdwa, AdmitsZeroExtendExtractMov)
{
	// BYTE_0..3, WORD_0..1 and DWORD selects on the observed VGPR-source tuple;
	// every select keeps dst_sel=DWORD, dst_u=PAD, no clamp/omod/sext/neg/abs.
	for (const uint32_t sel: {0u, 1u, 2u, 3u, 4u, 5u, 6u})
	{
		const uint32_t ctrl = kCtrlBase | (sel << 16u);
		ExpectFirstUnsupportedPc({kMovSdwaW0, ctrl, kUnsupportedSentinel, kEnd}, 0x8u, "VInterpP1F32");
	}
}

TEST(EmulatorComputeWaveSdwa, AdmitsCompleteProgramWithExtractMov)
{
	const uint32_t words[] = {kMovSdwaW0, kCtrlBase, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    std::_Exit(ShaderAnalyzeComputeWaveCode(code, PairedInput()).supported ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveSdwa, RejectsUndefinedOrUnrepresentableControlFields)
{
	// Each dword flips exactly one field off the admitted BYTE_0 tuple. A sign
	// extension, a partial destination select and the reserved encodings (source
	// select 7, bits 22 and 24) have no lowering and are refused at the SDWA mov.
	for (const uint32_t ctrl: {0x00080616u, 0x00000016u, 0x00070616u, 0x00400616u, 0x01000616u})
	{
		ExpectFirstUnsupportedPc({kMovSdwaW0, ctrl, kUnsupportedSentinel, kEnd}, 0x0u, "SDWA");
	}
}

TEST(EmulatorComputeWaveSdwa, LowersFoldedModifiersThroughTheGenericVectorPath)
{
	// src0_neg, src0_abs, CLMP, OMOD=1, DST_U=PRESERVE (inert with a DWORD
	// destination) and an SGPR source are folded into operands by the decoder, so
	// the generic lowering admits them and the first unsupported pc is the sentinel.
	for (const uint32_t ctrl: {0x00100616u, 0x00200616u, 0x00002616u, 0x00004616u, 0x00001616u, 0x00800616u})
	{
		ExpectFirstUnsupportedPc({kMovSdwaW0, ctrl, kUnsupportedSentinel, kEnd}, 0x8u, "VInterpP1F32");
	}
}

TEST(EmulatorComputeWaveSdwa, LowersSdwaOnOtherVop1OpcodesThroughTheGenericVectorPath)
{
	// v_not_b32_sdwa keeps the operand tuple of the extract mov but is another
	// operation; only the extract tuple is claimed by the SDWA extract path.
	const uint32_t not_sdwa = (kMovSdwaW0 & ~0x0001fe00u) | (0x37u << 9u);
	ExpectFirstUnsupportedPc({not_sdwa, kCtrlBase, kUnsupportedSentinel, kEnd}, 0x8u, "VInterpP1F32");
}

// VOPC SDWAB words verified against the LLVM gfx1030 assembler.
static constexpr uint32_t kCmpEqSdwaW0 = 0x7d840cf9u; // v_cmp_eq_u32_sdwa s14, 1, v6
static constexpr uint32_t kCmpEqCtrl   = 0x06868e81u; // sdst=14, sd=1, s0=1, DWORD selects
static constexpr uint32_t kCmpNeSdwaW0 = 0x7d8a0cf9u; // v_cmp_ne_u32_sdwa s14, 1, v6

TEST(EmulatorComputeWaveSdwa, AdmitsDwordSelectCompareMasks)
{
	// The observed tuple plus the SGPR-source and VGPR-source variants that
	// keep every control field inside the compare contract. SGPR mask pairs
	// are legal at any alignment: s[15:16] is a valid SDST destination.
	for (const uint32_t ctrl: {0x06868e81u /*const src0*/, 0x06869080u /*s16, const 0*/, 0x06068e14u /*v20 src0*/,
	                           0x86868e81u /*sgpr src1*/, 0x06860081u /*VCC destination*/, 0x06868f81u /*s15 pair*/})
	{
		ExpectFirstUnsupportedPc({kCmpEqSdwaW0, ctrl, kUnsupportedSentinel, kEnd}, 0x8u, "VInterpP1F32");
	}
	ExpectFirstUnsupportedPc({kCmpNeSdwaW0, 0x06868e81u, kUnsupportedSentinel, kEnd}, 0x8u, "VInterpP1F32");
}

TEST(EmulatorComputeWaveSdwa, RejectsUndefinedOrSignExtendedCompareControls)
{
	// src0_sext, src1_sext, reserved bit 22, reserved bit 30 and the NULL mask
	// destination have no compare lowering.
	for (const uint32_t ctrl: {0x068e8e81u, 0x0e868e81u, 0x06c68e81u, 0x46868e81u})
	{
		ExpectFirstUnsupportedPc({kCmpEqSdwaW0, ctrl, kUnsupportedSentinel, kEnd}, 0x0u, "SDWA");
	}
	// The encoding reserved for a NULL mask destination is not a mask pair.
	ExpectFirstUnsupportedPc({kCmpEqSdwaW0, 0x0686fe81u /*sdst=0x7f null*/, kUnsupportedSentinel, kEnd}, 0x0u, "SDWA");
}

TEST(EmulatorComputeWaveSdwa, LowersSelectsAndFoldedModifiersOfCompareThroughTheGenericPath)
{
	// src0/src1 BYTE_0 selects, source neg/abs, and a destination pair that
	// aliases an SGPR source (operands are read before the mask is written).
	for (const uint32_t ctrl: {0x06808e81u, 0x00808e81u, 0x06968e81u, 0x06a68e81u, 0x16868e81u, 0x26868e81u, 0x06868e0eu /*s14 vs s14*/})
	{
		ExpectFirstUnsupportedPc({kCmpEqSdwaW0, ctrl, kUnsupportedSentinel, kEnd}, 0x8u, "VInterpP1F32");
	}
}
UT_END();
