#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveWaitcnt.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>

UT_BEGIN(EmulatorComputeWaveWaitcnt);

using namespace Libs::Graphics;

// Synthetic RDNA2 words; each decodes identically with the LLVM gfx1030
// disassembler. Wave-width-independent scalar encodings only.
static constexpr uint32_t kPrefetch3  = 0xbfa00003u; // s_inst_prefetch 0x3
static constexpr uint32_t kLoadWord0  = 0xf4080406u; // s_load_dwordx4 s[16:19], s[12:13], 0x50
static constexpr uint32_t kLoadWord1  = 0xfa000050u;
static constexpr uint32_t kShiftVccHi = 0x8f6b8214u; // s_lshl_b32 vcc_hi, s20, 2
static constexpr uint32_t kWaitLgkm0  = 0xbf8cc07fu; // s_waitcnt lgkmcnt(0)
static constexpr uint32_t kGetpc      = 0xbe941f00u; // s_getpc_b64 s[20:21]; outside the paired set
static constexpr uint32_t kEnd        = 0xbf810000u; // s_endpgm

static void InitializeConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

static bool ParseCompute(const uint32_t* words, uint32_t bytes, ShaderCode* code)
{
	code->SetType(ShaderType::Compute);
	return ShaderTryParseBounded(words, bytes, code);
}

static ShaderComputeInputInfo PairedInput(uint32_t lds_dwords)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num = 1;
	input.lds_dwords     = lds_dwords;
	input.wave_layout    = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, lds_dwords};
	return input;
}

// Type-5 EUD table at s[12:13]. The per-PC mapping is fixture-supplied here;
// production mappings come from the resource collector, not from this test.
static ShaderComputeInputInfo EudInput(bool mapped, uint32_t load_pc)
{
	auto input                             = PairedInput(0);
	input.bind.extended.used               = true;
	input.bind.extended.slot               = 5;
	input.bind.extended.start_register     = 12;
	input.bind.extended.eud_user_sgpr_num  = 14;
	input.bind.extended.eud_size_dw        = 24;
	input.bind.extended.eud_offset_base    = 32;
	input.bind.extended.data.fields[0]     = 1u;
	if (mapped)
	{
		input.bind.storage_buffers.buffers_num       = 1;
		input.bind.storage_buffers.dynamic_sload[0]  = true;
		input.bind.storage_buffers.sources[0]        = ShaderStorageBindingSource::DynamicScalarLoad;
		input.bind.storage_buffers.start_register[0] = 16;
		// One 16-byte record: the scalar-buffer dword at offset zero is in range.
		input.bind.storage_buffers.buffers[0].fields[1] = 16u << 16u;
		input.bind.storage_buffers.buffers[0].fields[2] = 1u;
		ShaderDynamicSLoadMapping mapping {};
		mapping.kind                 = ShaderDynamicSLoadResourceKind::StorageBuffer;
		mapping.resource_index       = 0;
		mapping.destination_register = 16;
		mapping.instruction_pc       = load_pc;
		mapping.offset_dw            = 20;
		mapping.dword_count          = 4;
		mapping.last_consumer_pc     = load_pc + 0x10u;
		input.bind.dynamic_sloads.records.Add(mapping);
	}
	return input;
}

TEST(EmulatorComputeWaveWaitcnt, AdvancesPastLgkmZeroCoveringMappedScalarLoad)
{
	const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kShiftVccHi, kWaitLgkm0, kGetpc, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code) || code.GetInstructions().Size() != 6u)
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
		    std::_Exit(!result.supported && result.unsupported_pc == 0x14u && result.reason.ContainsStr("SGetpcB64") ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, AdmitsCompleteMappedLoadWaitProgram)
{
	const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kShiftVccHi, kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    std::_Exit(ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u)).supported ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, RejectsEveryOtherWaitImmediateAtItsPc)
{
	// vmcnt(62)+lgkmcnt(0), undefined bit 7, lgkmcnt(1), all-zero drain,
	// expcnt(0) with pending VMEM — none are the pure vmcnt(N) form.
	for (const uint32_t immediate: {0xc07eu, 0xc0ffu, 0xc17fu, 0x0000u, 0x3f04u})
	{
		const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kShiftVccHi, 0xbf8c0000u | immediate, kEnd};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    if (!ParseCompute(words, sizeof(words), &code))
			    {
				    std::_Exit(2);
			    }
			    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
			    std::_Exit(!result.supported && result.unsupported_pc == 0x10u && result.reason.ContainsStr("immediate") ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveWaitcnt, AdmitsPureVmcntWaitsAfterSynchronousLoads)
{
	// Every vmcnt value is admitted: all paired VMEM ops complete at their PC.
	// vmcnt(0), vmcnt(4), vmcnt(5), vmcnt(15), vmcnt(20) via the [15:14] bits.
	for (const uint32_t immediate: {0x3f70u, 0x3f74u, 0x3f75u, 0x3f7fu, 0x7f74u})
	{
		const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xbf8c0000u | immediate, kGetpc, kEnd};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    if (!ParseCompute(words, sizeof(words), &code))
			    {
				    std::_Exit(2);
			    }
			    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
			    std::_Exit(!result.supported && result.unsupported_pc == 0x14u && result.reason.ContainsStr("SGetpcB64") ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveWaitcnt, RejectsPendingLdsBeforeLgkmWait)
{
	// v_mov_b32 v1, 0; ds_read_b32 v4, v1; s_waitcnt lgkmcnt(0); s_endpgm.
	const uint32_t words[] = {0x7e020280u, 0xd8d80000u, 0x04000001u, kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, PairedInput(1));
		    std::_Exit(!result.supported && result.unsupported_pc == 0xcu && result.reason.ContainsStr("LDS") ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, KeepsUnmappedScalarLoadAsFirstUnsupportedPc)
{
	const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kShiftVccHi, kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(false, 4u));
		    std::_Exit(!result.supported && result.unsupported_pc == 4u ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, RejectsPendingDestinationAccessBeforeItsWait)
{
	// s_mov_b32 s20, s17 reads and s_mov_b32 s17, 0 writes an in-flight destination.
	for (const uint32_t access: {0xbe940311u, 0xbe910380u})
	{
		const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, access, kWaitLgkm0, kEnd};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    if (!ParseCompute(words, sizeof(words), &code))
			    {
				    std::_Exit(2);
			    }
			    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
			    std::_Exit(!result.supported && result.unsupported_pc == 0xcu && result.reason.ContainsStr("pending") ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

// The window-level entry point under test: a cbranch_execz inside the pending
// LGKM window whose taken edge leaves it entirely, while the fall-through runs
// the mapped S_LOAD and its drain in order. CFG shape is validated separately
// by ShaderAnalyzeComputeWaveControlFlow, so these call the wait analysis
// directly.
static constexpr uint32_t kCbranchTo0x24 = 0xbf880006u; // s_cbranch_execz +0x18
static constexpr uint32_t kCbranchTo0x1c = 0xbf880004u; // s_cbranch_execz +0x10
static constexpr uint32_t kMovLit0       = 0x7e1002ffu; // v_mov_b32 v8, <lit>
static constexpr uint32_t kMovLit1       = 0x27bc86aau;
static constexpr uint32_t kMovV9Zero     = 0x7e120280u; // v_mov_b32 v9, 0

TEST(EmulatorComputeWaveWaitcnt, AdmitsConditionalBranchInsideTheWindow)
{
	const uint32_t words[] = {kPrefetch3, kWaitLgkm0, kCbranchTo0x24, kMovLit0, kMovLit1,
	                          kLoadWord0, kLoadWord1, kMovV9Zero, kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto bind    = EudInput(true, 0x14u).bind;
		    const auto result  = ShaderAnalyzeComputeWaveWaitcnt(code, 6u, bind);
		    std::_Exit(result.supported ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, AdmitsVmcntWaitsInsideTheLgkmWindow)
{
	// Pure vmcnt waits are transparent to LGKM accounting: they issue no LGKM
	// operation and never drain it. The lgkmcnt(0) wait still owns the window.
	const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xbf8c3f74u /*vmcnt(4)*/,
	                          0x7e020280u /*v_mov_b32 v1,0*/, kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto bind   = EudInput(true, 4u).bind;
		    const auto result = ShaderAnalyzeComputeWaveWaitcnt(code, 5u, bind);
		    std::_Exit(result.supported ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, RejectsRealBranchTargetInsideTheWindow)
{
	// A real target landing inside the window (0x1c) is still a mid-window
	// entry; the fall-through relaxation only covers sequential edges.
	const uint32_t words[] = {kPrefetch3, kWaitLgkm0, kCbranchTo0x1c, kMovLit0, kMovLit1,
	                          kLoadWord0, kLoadWord1, kMovV9Zero, kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code) || code.GetLabels().Size() != 1u ||
		        code.GetLabels().At(0).GetDst() != 0x1cu)
		    {
			    std::_Exit(2);
		    }
		    const auto bind   = EudInput(true, 0x14u).bind;
		    const auto result = ShaderAnalyzeComputeWaveWaitcnt(code, 6u, bind);
		    std::_Exit(!result.supported && result.unsupported_pc == 0x20u && result.reason.ContainsStr("branch target") ? 0
		                                                                                                        : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, AdmitsBranchTargetBeforeThePendingProducer)
{
	// A real target landing before the earliest pending producer still runs it
	// on the taken path, so the window is not bypassed. The workload shader
	// uses this shape: cbranch at 0xc0 enters at 0x144 while the covered
	// S_LOAD sits later at 0x164.
	// 0x00 prefetch; 0x04 lgkmcnt(0); 0x08 cbranch ->0x14; 0x0c v_mov lit;
	// 0x14 v_mov (target); 0x18 mapped s_load; 0x20 lgkmcnt(0); 0x24 end.
	const uint32_t words[] = {kPrefetch3, kWaitLgkm0, 0xbf880002u /*+8 -> 0x14*/, kMovLit0, kMovLit1,
	                          kMovV9Zero, kLoadWord0, kLoadWord1, kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code) || code.GetLabels().Size() != 1u ||
		        code.GetLabels().At(0).GetDst() != 0x14u)
		    {
			    std::_Exit(2);
		    }
		    const auto bind   = EudInput(true, 0x18u).bind;
		    const auto result = ShaderAnalyzeComputeWaveWaitcnt(code, 6u, bind);
		    std::_Exit(result.supported ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, RejectsBranchTargetAfterThePendingProducer)
{
	// A target landing strictly after the earliest pending producer can reach
	// its consumers without ever running the load: still a mid-window entry.
	// 0x08 cbranch ->0x18; 0x0c v_mov; 0x10 mapped s_load; 0x18 v_mov (target);
	// 0x1c lgkmcnt(0).
	const uint32_t words[] = {kPrefetch3, kWaitLgkm0, 0xbf880003u /*+12 -> 0x18*/, kMovV9Zero,
	                          kLoadWord0, kLoadWord1, kMovV9Zero, kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code) || code.GetLabels().Size() != 1u ||
		        code.GetLabels().At(0).GetDst() != 0x18u)
		    {
			    std::_Exit(2);
		    }
		    const auto bind   = EudInput(true, 0x10u).bind;
		    const auto result = ShaderAnalyzeComputeWaveWaitcnt(code, 6u, bind);
		    std::_Exit(!result.supported && result.unsupported_pc == 0x1cu && result.reason.ContainsStr("branch target") ? 0
		                                                                                                          : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, RejectsWaitReachedThroughBranchTarget)
{
	// s_load at 0; s_waitcnt at 8; s_branch at 0xc targets the wait.
	const uint32_t words[] = {kLoadWord0, kLoadWord1, kWaitLgkm0, 0xbf82fffeu, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code) || code.GetLabels().Size() != 1u ||
		        code.GetLabels().At(0).GetDst() != 8u)
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 0u));
		    std::_Exit(!result.supported && result.unsupported_pc == 8u && result.reason.ContainsStr("branch target") ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, AdvancesPastLgkmZeroCoveringAdmittedScalarBufferLoad)
{
	// s_load at 4; wait at 0xc; s_buffer_load_dword vcc_lo at 0x10; wait at 0x18.
	const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xf4201a88u, 0xfa000000u, kWaitLgkm0, kGetpc, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
		    std::_Exit(!result.supported && result.unsupported_pc == 0x1cu && result.reason.ContainsStr("SGetpcB64") ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, RejectsPendingVccAccessBeforeItsWait)
{
	// s_mov_b64 s[20:21], vcc reads and s_mov_b64 vcc, s[20:21] writes the in-flight VCC_LO.
	for (const uint32_t access: {0xbe94046au, 0xbeea0414u})
	{
		const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xf4201a88u, 0xfa000000u, access, kWaitLgkm0, kEnd};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    if (!ParseCompute(words, sizeof(words), &code))
			    {
				    std::_Exit(2);
			    }
			    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
			    std::_Exit(!result.supported && result.unsupported_pc == 0x18u && result.reason.ContainsStr("pending") ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveWaitcnt, WindowRejectsUnmappedLoadAndEndsAtPreviousDrain)
{
	// s_load (unmapped) at 0; s_waitcnt at 8 and at 0xc; s_endpgm.
	const uint32_t words[] = {kLoadWord0, kLoadWord1, kWaitLgkm0, kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code) || code.GetInstructions().Size() != 4u)
		    {
			    std::_Exit(2);
		    }
		    const auto bind   = EudInput(false, 0u).bind;
		    const auto first  = ShaderAnalyzeComputeWaveWaitcnt(code, 1u, bind);
		    const auto second = ShaderAnalyzeComputeWaveWaitcnt(code, 2u, bind);
		    const bool first_rejected =
		        !first.supported && first.unsupported_pc == 8u && first.reason.ContainsStr("unmodeled instruction SLoadDwordx4");
		    std::_Exit(first_rejected && second.supported ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, WindowRejectsNonWaitOpcodeTuple)
{
	const uint32_t words[] = {kWaitLgkm0, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto bind  = PairedInput(0).bind;
		    const bool exact = ShaderAnalyzeComputeWaveWaitcnt(code, 0u, bind).supported;
		    code.GetInstructions()[0].sopp_opcode = 0x0du;
		    const auto alias = ShaderAnalyzeComputeWaveWaitcnt(code, 0u, bind);
		    std::_Exit(exact && !alias.supported && alias.reason.ContainsStr("opcode") ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, KeepsStoreCountWaitRejected)
{
	// SOPK s_waitcnt_vscnt null, 0x0 is a different counter and stays outside the set.
	const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, 0xbbfd0000u, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
		    std::_Exit(!result.supported && result.unsupported_pc == 0xcu ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
