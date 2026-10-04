#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveScalarBuffer.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

UT_BEGIN(EmulatorComputeWaveScalarBuffer);

using namespace Libs::Graphics;

// Synthetic RDNA2 words; each decodes identically with the LLVM gfx1030 disassembler.
static constexpr uint32_t kPrefetch3  = 0xbfa00003u; // s_inst_prefetch 0x3
static constexpr uint32_t kLoadWord0  = 0xf4080406u; // s_load_dwordx4 s[16:19], s[12:13], 0x50
static constexpr uint32_t kLoadWord1  = 0xfa000050u;
static constexpr uint32_t kWaitLgkm0  = 0xbf8cc07fu; // s_waitcnt lgkmcnt(0)
static constexpr uint32_t kBufferVcc  = 0xf4201a88u; // s_buffer_load_dword vcc_lo, s[16:19], ...
static constexpr uint32_t kBufferSgpr = 0xf4200508u; // s_buffer_load_dword s20, s[16:19], ...
static constexpr uint32_t kNullOffset = 0xfa000000u; // null SOFFSET, immediate 0
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

// One mapped EUD S_LOAD at pc 4 whose V# is a 16-byte-record buffer. The
// mapping is fixture-supplied; production mappings come from the collector.
static ShaderComputeInputInfo MappedInput(uint32_t last_consumer_pc = 0x14u)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num                    = 1;
	input.wave_layout                       = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0};
	auto& bind                              = input.bind;
	bind.extended.used                      = true;
	bind.extended.slot                      = 5;
	bind.extended.start_register            = 12;
	bind.extended.eud_user_sgpr_num         = 14;
	bind.extended.eud_size_dw               = 24;
	bind.extended.eud_offset_base           = 32;
	bind.extended.data.fields[0]            = 1u;
	bind.storage_buffers.buffers_num        = 1;
	bind.storage_buffers.dynamic_sload[0]   = true;
	bind.storage_buffers.sources[0]         = ShaderStorageBindingSource::DynamicScalarLoad;
	bind.storage_buffers.start_register[0]  = 16;
	bind.storage_buffers.buffers[0].fields[0] = 0x00001000u;
	bind.storage_buffers.buffers[0].fields[1] = 16u << 16u;
	bind.storage_buffers.buffers[0].fields[2] = 1u;
	ShaderDynamicSLoadMapping mapping {};
	mapping.kind                 = ShaderDynamicSLoadResourceKind::StorageBuffer;
	mapping.resource_index       = 0;
	mapping.destination_register = 16;
	mapping.instruction_pc       = 4u;
	mapping.offset_dw            = 20;
	mapping.dword_count          = 4;
	mapping.last_consumer_pc     = last_consumer_pc;
	bind.dynamic_sloads.records.Add(mapping);
	return input;
}

static void ExpectFirstUnsupportedPc(const std::vector<uint32_t>& words, const ShaderComputeInputInfo& input, uint32_t pc,
                                     const char* reason)
{
	EXPECT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, input);
		    std::_Exit(!result.supported && result.unsupported_pc == pc && result.reason.ContainsStr(reason) ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

static uint32_t FindInstructionIndex(const ShaderCode& code, uint32_t pc)
{
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		if (code.GetInstructions().At(index).pc == pc)
		{
			return index;
		}
	}
	return UINT32_MAX;
}

static void ExpectAdmitted(const std::vector<uint32_t>& words, const ShaderComputeInputInfo& input)
{
	EXPECT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code))
		    {
			    std::_Exit(2);
		    }
		    std::_Exit(ShaderAnalyzeComputeWaveCode(code, input).supported ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// The descriptor-provenance proof of the specialized synchronous S_BUFFER_LOAD route
// must refuse the load at `pc`.
static void ExpectProofRefuses(const std::vector<uint32_t>& words, const ShaderComputeInputInfo& input, uint32_t pc, const char* reason)
{
	EXPECT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code))
		    {
			    std::_Exit(2);
		    }
		    const uint32_t index = FindInstructionIndex(code, pc);
		    if (index == UINT32_MAX)
		    {
			    std::_Exit(4);
		    }
		    const auto result = ShaderAnalyzeComputeWaveScalarBufferLoad(code, index, input.bind);
		    std::_Exit(!result.supported && result.unsupported_pc == pc && result.reason.ContainsStr(reason) ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// Since 704f4ad3 an unproven load is not rejected by the analysis: it is
// lowered per lane by the generic emitter, so the first unsupported
// instruction is the sentinel (or the program is admitted when it has none).
static void ExpectGenericFallback(const std::vector<uint32_t>& words, const ShaderComputeInputInfo& input)
{
	const auto sentinel = std::find(words.begin(), words.end(), kUnsupportedSentinel);
	if (sentinel == words.end())
	{
		ExpectAdmitted(words, input);
		return;
	}
	ExpectFirstUnsupportedPc(words, input, static_cast<uint32_t>(sentinel - words.begin()) * 4u, "VInterpP1F32");
}

// The specialized proof refuses the load at `pc` and the analysis falls back.
static void ExpectUnprovenLoad(const std::vector<uint32_t>& words, const ShaderComputeInputInfo& input, uint32_t pc, const char* reason)
{
	ExpectProofRefuses(words, input, pc, reason);
	ExpectGenericFallback(words, input);
}

TEST(EmulatorComputeWaveScalarBuffer, AdmitsDrainedMappedDescriptorLoadIntoVccOrSgpr)
{
	for (const uint32_t load: {kBufferVcc, kBufferSgpr})
	{
		ExpectFirstUnsupportedPc({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, load, kNullOffset, kUnsupportedSentinel, kEnd}, MappedInput(),
		                         0x18u, "VInterpP1F32");
	}
}

TEST(EmulatorComputeWaveScalarBuffer, UnprovenDescriptorProvenanceFallsBackToTheGenericLoad)
{
	// Descriptor still in flight: no lgkmcnt(0) between producer and consumer.
	ExpectUnprovenLoad({kPrefetch3, kLoadWord0, kLoadWord1, kBufferVcc, kNullOffset, kUnsupportedSentinel, kEnd}, MappedInput(), 0xcu,
	                         "paired SBufferLoadDword");
	// V# from user SGPRs s[8:11], not a mapped EUD S_LOAD.
	ExpectUnprovenLoad({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xf4201a84u, kNullOffset, kUnsupportedSentinel, kEnd},
	                         MappedInput(), 0x10u, "paired SBufferLoadDword");
	// s_mov_b32 s17, 0 redefines part of the drained V#.
	ExpectUnprovenLoad({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xbe910380u, kBufferVcc, kNullOffset, kUnsupportedSentinel, kEnd},
	                         MappedInput(), 0x14u, "paired SBufferLoadDword");
	// The collector's mapping ends before this consumer.
	ExpectUnprovenLoad({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kBufferVcc, kNullOffset, kUnsupportedSentinel, kEnd},
	                         MappedInput(0x0cu), 0x10u, "paired SBufferLoadDword");
	// s_branch at 0x18 targets the consumer, adding a second path.
	ExpectUnprovenLoad({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kBufferVcc, kNullOffset, 0xbf82fffdu, kEnd},
	                         MappedInput(), 0x10u, "paired SBufferLoadDword");
}

TEST(EmulatorComputeWaveScalarBuffer, UnsupportedTupleControlsFallBackToTheGenericLoad)
{
	const std::vector<std::vector<uint32_t>> loads = {
	    {kBufferVcc, 0xfa000010u},          // offset 0x10 is past the 16-byte descriptor
	    {kBufferVcc, 0x2a000000u},          // dynamic SOFFSET s21
	    {kBufferVcc | (1u << 16u), kNullOffset}, // GLC
	    {kBufferVcc | (1u << 13u), kNullOffset}, // undefined encoding bit
	    {0xf4201f88u, kNullOffset},         // exec_lo destination
	    {0xf4201f08u, kNullOffset},         // m0 destination
	};
	for (const auto& load: loads)
	{
		ExpectUnprovenLoad({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, load[0], load[1], kUnsupportedSentinel, kEnd}, MappedInput(),
		                         0x10u, "paired SBufferLoadDword");
	}
}

TEST(EmulatorComputeWaveScalarBuffer, DescriptorsOutsideTheSynchronousReadContractFallBackToTheGenericLoad)
{
	const std::vector<uint32_t> words = {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kBufferVcc, kNullOffset, kUnsupportedSentinel, kEnd};
	auto empty                                   = MappedInput();
	empty.bind.storage_buffers.buffers[0].fields[2] = 0u;
	ExpectUnprovenLoad(words, empty, 0x10u, "paired SBufferLoadDword");
	auto swizzled = MappedInput();
	swizzled.bind.storage_buffers.buffers[0].fields[1] |= 1u << 31u;
	ExpectUnprovenLoad(words, swizzled, 0x10u, "paired SBufferLoadDword");
	auto zero_policy                                         = MappedInput();
	zero_policy.bind.zero_sbuffer_resources.start_register[0] = 16;
	zero_policy.bind.zero_sbuffer_resources.buffers_num       = 1;
	ExpectUnprovenLoad(words, zero_policy, 0x10u, "paired SBufferLoadDword");
}

UT_END();
