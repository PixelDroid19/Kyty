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
static constexpr uint32_t kGetpc     = 0xbe941f00u; // s_getpc_b64 s[20:21]; outside the paired set
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
		ExpectFirstUnsupportedPc({kMovSdwaW0, ctrl, kGetpc, kEnd}, 0x8u, "SGetpcB64");
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

TEST(EmulatorComputeWaveSdwa, RejectsEveryNonContractControlField)
{
	// Each dword flips exactly one field off the admitted BYTE_0 tuple:
	// src0_sext, src0_neg, src0_abs, CLMP, OMOD=1, DST_U=PRESERVE,
	// DST_SEL=BYTE_0, SGPR source (S0=1), reserved select 7, reserved bit 22,
	// reserved bit 24.
	for (const uint32_t ctrl:
	     {0x00080616u, 0x00100616u, 0x00200616u, 0x00002616u, 0x00004616u, 0x00001616u, 0x00000016u, 0x00800616u, 0x00070616u,
	      0x00400616u, 0x01000616u})
	{
		ExpectFirstUnsupportedPc({kMovSdwaW0, ctrl, kGetpc, kEnd}, 0x0u, "SDWA");
	}
}

TEST(EmulatorComputeWaveSdwa, RejectsSdwaOnOtherVop1Opcodes)
{
	// v_not_b32_sdwa keeps the same operand tuple but is a different operation.
	const uint32_t not_sdwa = (kMovSdwaW0 & ~0x0001fe00u) | (0x37u << 9u);
	ExpectFirstUnsupportedPc({not_sdwa, kCtrlBase, kGetpc, kEnd}, 0x0u, "SDWA");
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
		ExpectFirstUnsupportedPc({kCmpEqSdwaW0, ctrl, kGetpc, kEnd}, 0x8u, "SGetpcB64");
	}
	ExpectFirstUnsupportedPc({kCmpNeSdwaW0, 0x06868e81u, kGetpc, kEnd}, 0x8u, "SGetpcB64");
}

TEST(EmulatorComputeWaveSdwa, RejectsNonContractCompareControls)
{
	// src0_sel=BYTE_0, src1_sel=BYTE_0, src0_sext, src0_neg, src0_abs,
	// src1_sext, src1_neg, src1_abs, reserved bit 22, reserved bit 30.
	for (const uint32_t ctrl:
	     {0x06808e81u, 0x00808e81u, 0x068e8e81u, 0x06968e81u, 0x06a68e81u, 0x0e868e81u, 0x16868e81u, 0x26868e81u, 0x06c68e81u,
	      0x46868e81u})
	{
		ExpectFirstUnsupportedPc({kCmpEqSdwaW0, ctrl, kGetpc, kEnd}, 0x0u, "SDWA");
	}
	// Destination pair aliasing an SGPR source stays outside the compare set.
	ExpectFirstUnsupportedPc({kCmpEqSdwaW0, 0x06868e0eu /*s14 vs s14*/, kGetpc, kEnd}, 0x0u, "SDWA");
	// The encoding reserved for a NULL mask destination is not a mask pair.
	ExpectFirstUnsupportedPc({kCmpEqSdwaW0, 0x0686fe81u /*sdst=0x7f null*/, kGetpc, kEnd}, 0x0u, "SDWA");
}

UT_END();
