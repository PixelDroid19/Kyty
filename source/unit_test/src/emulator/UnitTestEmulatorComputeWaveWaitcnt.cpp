#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveWaitcnt.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>
#include <vector>

UT_BEGIN(EmulatorComputeWaveWaitcnt);

using namespace Libs::Graphics;

// Synthetic RDNA2 words; each decodes identically with the LLVM gfx1030
// disassembler. Wave-width-independent scalar encodings only.
static constexpr uint32_t kPrefetch3  = 0xbfa00003u; // s_inst_prefetch 0x3
static constexpr uint32_t kLoadWord0  = 0xf4080406u; // s_load_dwordx4 s[16:19], s[12:13], 0x50
static constexpr uint32_t kLoadWord1  = 0xfa000050u;
static constexpr uint32_t kShiftVccHi = 0x8f6b8214u; // s_lshl_b32 vcc_hi, s20, 2
static constexpr uint32_t kWaitLgkm0  = 0xbf8cc07fu; // s_waitcnt lgkmcnt(0)
static constexpr uint32_t kUnsupportedSentinel = 0xc8000001u; // v_interp_p1_f32 v0, v1, attr0.x; pixel-only, outside the paired compute set
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
	const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kShiftVccHi, kWaitLgkm0, kUnsupportedSentinel, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code) || code.GetInstructions().Size() != 6u)
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
		    std::_Exit(!result.supported && result.unsupported_pc == 0x14u && result.reason.ContainsStr("VInterpP1F32") ? 0 : 3);
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

// Every admitted memory operation completes at its own PC, so a wait carries no
// window to validate: any exact SOPP s_waitcnt is satisfied, whatever its counters.
TEST(EmulatorComputeWaveWaitcnt, AdmitsEveryExactWaitImmediate)
{
	// vmcnt(62)+lgkmcnt(0), undefined bit 7, lgkmcnt(1), all-zero drain and
	// expcnt(0) with pending VMEM.
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
			    std::_Exit(ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u)).supported ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveWaitcnt, SynchronousLoadsLeaveNoPendingWindowToReject)
{
	struct Program
	{
		std::vector<uint32_t> words;
		bool                  mapped;
		uint32_t              lds_dwords;
	};
	const std::vector<Program> programs = {
	    // v_mov_b32 v1, 0; ds_read_b32 v4, v1; s_waitcnt lgkmcnt(0): LDS before the wait.
	    {{0x7e020280u, 0xd8d80000u, 0x04000001u, kWaitLgkm0, kEnd}, false, 1u},
	    // The mapped descriptor is read (s_mov_b32 s20, s17) and rewritten (s_mov_b32 s17, 0) before its wait.
	    {{kPrefetch3, kLoadWord0, kLoadWord1, 0xbe940311u, kWaitLgkm0, kEnd}, true, 0u},
	    {{kPrefetch3, kLoadWord0, kLoadWord1, 0xbe910380u, kWaitLgkm0, kEnd}, true, 0u},
	    // s_mov_b64 s[20:21], vcc / s_mov_b64 vcc, s[20:21] around an in-flight s_buffer_load_dword vcc_lo.
	    {{kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xf4201a88u, 0xfa000000u, 0xbe94046au, kWaitLgkm0, kEnd}, true, 0u},
	    {{kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xf4201a88u, 0xfa000000u, 0xbeea0414u, kWaitLgkm0, kEnd}, true, 0u},
	    // A wait reached through a branch target, with an unmapped scalar load before it.
	    {{kLoadWord0, kLoadWord1, kWaitLgkm0, 0xbf82fffeu, kEnd}, true, 0u},
	    {{kPrefetch3, kLoadWord0, kLoadWord1, kShiftVccHi, kWaitLgkm0, kEnd}, false, 0u},
	    // SOPK s_waitcnt_vscnt null, 0x0 counts stores, which are synchronous too.
	    {{kPrefetch3, kLoadWord0, kLoadWord1, 0xbbfd0000u, kEnd}, true, 0u},
	};
	for (const auto& program: programs)
	{
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    if (!ParseCompute(program.words.data(), static_cast<uint32_t>(program.words.size() * sizeof(uint32_t)), &code))
			    {
				    std::_Exit(2);
			    }
			    const auto input = program.lds_dwords != 0u ? PairedInput(program.lds_dwords) : EudInput(program.mapped, 4u);
			    std::_Exit(ShaderAnalyzeComputeWaveCode(code, input).supported ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveWaitcnt, RejectsAnInexactWaitEncoding)
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
		    const bool exact = ShaderComputeWaveIsExactWait(code.GetInstructions().At(0));
		    code.GetInstructions()[0].sopp_opcode = 0x0du;
		    const auto alias = ShaderAnalyzeComputeWaveCode(code, PairedInput(0));
		    std::_Exit(exact && !alias.supported && alias.unsupported_pc == 0u && alias.reason.ContainsStr("exact SOPP") ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveWaitcnt, AdmitsPureVmcntWaitsAfterSynchronousLoads)
{
	// Every vmcnt value is admitted: all paired VMEM ops complete at their PC.
	// vmcnt(0), vmcnt(4), vmcnt(5), vmcnt(15), vmcnt(20) via the [15:14] bits.
	for (const uint32_t immediate: {0x3f70u, 0x3f74u, 0x3f75u, 0x3f7fu, 0x7f74u})
	{
		const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xbf8c0000u | immediate, kUnsupportedSentinel, kEnd};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    if (!ParseCompute(words, sizeof(words), &code))
			    {
				    std::_Exit(2);
			    }
			    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
			    std::_Exit(!result.supported && result.unsupported_pc == 0x14u && result.reason.ContainsStr("VInterpP1F32") ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveWaitcnt, AdvancesPastLgkmZeroCoveringAdmittedScalarBufferLoad)
{
	// s_load at 4; wait at 0xc; s_buffer_load_dword vcc_lo at 0x10; wait at 0x18.
	const uint32_t words[] = {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xf4201a88u, 0xfa000000u, kWaitLgkm0, kUnsupportedSentinel, kEnd};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    if (!ParseCompute(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, EudInput(true, 4u));
		    std::_Exit(!result.supported && result.unsupported_pc == 0x1cu && result.reason.ContainsStr("VInterpP1F32") ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
