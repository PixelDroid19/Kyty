#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveVectorBuffer.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

UT_BEGIN(EmulatorComputeWaveVectorBuffer);

using namespace Libs::Graphics;

// Synthetic RDNA2 words; each decodes identically with the LLVM gfx1030
// assembler. The base MUBUF pair is the real guest encoding for
// "buffer_load_dwordx2 v[5:6], v43, s[24:27], 0 idxen" shifted to s[16:19].
static constexpr uint32_t kPrefetch3   = 0xbfa00003u; // s_inst_prefetch 0x3
static constexpr uint32_t kLoadWord0   = 0xf4080406u; // s_load_dwordx4 s[16:19], s[12:13], 0x50
static constexpr uint32_t kLoadWord1   = 0xfa000050u;
static constexpr uint32_t kWaitLgkm0   = 0xbf8cc07fu; // s_waitcnt lgkmcnt(0)
static constexpr uint32_t kMubufX2W0   = 0xe0342000u; // buffer_load_dwordx2 v[5:6], v43, s[16:19], 0 idxen
static constexpr uint32_t kMubufX2W1   = 0x8004052bu;
static constexpr uint32_t kMubufX1W0   = 0xe0302000u; // buffer_load_dword v5, v43, s[16:19], 0 idxen
static constexpr uint32_t kMubufX3W0   = 0xe03c2000u; // buffer_load_dwordx3 v[5:7], v43, s[16:19], 0 idxen
static constexpr uint32_t kMubufX4W0   = 0xe0382000u; // buffer_load_dwordx4 v[5:8], v43, s[16:19], 0 idxen
static constexpr uint32_t kMubufImm8W0 = 0xe0342008u; // same x2 with immediate offset 8
static constexpr uint32_t kMovS8Zero   = 0xbe880380u; // s_mov_b32 s8, 0
static constexpr uint32_t kUnsupportedSentinel = 0xc8000001u; // v_interp_p1_f32 v0, v1, attr0.x; pixel-only, outside the paired compute set
static constexpr uint32_t kEnd         = 0xbf810000u; // s_endpgm

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
static ShaderComputeInputInfo MappedInput(uint32_t last_consumer_pc = 0x10u)
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
	mapping.raw_vmem_oob_guarded = true;
	bind.dynamic_sloads.records.Add(mapping);
	return input;
}

// A direct user-SGPR V# at s[8:11]: no producer instruction is involved.
static ShaderComputeInputInfo DirectInput()
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num                    = 1;
	input.wave_layout                       = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0};
	auto& bind                              = input.bind;
	bind.storage_buffers.buffers_num        = 1;
	bind.storage_buffers.dynamic_sload[0]   = false;
	bind.storage_buffers.sources[0]         = ShaderStorageBindingSource::DirectResource;
	bind.storage_buffers.start_register[0]  = 8;
	bind.storage_buffers.buffers[0].fields[0] = 0x00001000u;
	bind.storage_buffers.buffers[0].fields[1] = 16u << 16u;
	bind.storage_buffers.buffers[0].fields[2] = 1u;
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
		    const bool expected = !result.supported && result.unsupported_pc == pc && result.reason.ContainsStr(reason);
		    if (!expected)
		    {
			    std::fprintf(stderr, "supported=%d pc=0x%x expected_pc=0x%x reason=%s expected_reason=%s\n",
			                 result.supported, result.unsupported_pc, pc, result.reason.c_str(), reason);
		    }
		    std::_Exit(expected ? 0 : 3);
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

// The descriptor-provenance proof of the specialized synchronous MUBUF load route
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
		    const auto result = ShaderAnalyzeComputeWaveVectorBufferLoad(code, index, input.bind);
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

TEST(EmulatorComputeWaveVectorBuffer, AdmitsDrainedMappedRawLoads)
{
	for (const uint32_t word0: {kMubufX1W0, kMubufX2W0, kMubufX3W0, kMubufX4W0, kMubufImm8W0})
	{
		ExpectFirstUnsupportedPc({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, word0, kMubufX2W1, kUnsupportedSentinel, kEnd},
		                         MappedInput(), 0x18u, "VInterpP1F32");
	}
}

TEST(EmulatorComputeWaveVectorBuffer, AdmitsDirectUserSgprDescriptorLoad)
{
	// buffer_load_dwordx2 v[5:6], v43, s[8:11], 0 idxen bound directly.
	const uint32_t direct_w1 = 0x8002052bu; // srsrc=2 -> s[8:11]
	ExpectFirstUnsupportedPc({kPrefetch3, kMubufX2W0, direct_w1, kUnsupportedSentinel, kEnd}, DirectInput(), 0xcu, "VInterpP1F32");
}

TEST(EmulatorComputeWaveVectorBuffer, AdmitsVmcntWaitCoveringTheLoad)
{
	// The real guest shape: mapped S_LOAD, lgkmcnt(0) drain, the dwordx2 load,
	// then the vmcnt(4) wait that covers it before its VGPR consumer.
	const uint32_t vmcnt4 = 0xbf8c3f74u;
	ExpectFirstUnsupportedPc({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kMubufX2W0, kMubufX2W1, vmcnt4, kUnsupportedSentinel, kEnd},
	                         MappedInput(), 0x1cu, "VInterpP1F32");
}

TEST(EmulatorComputeWaveVectorBuffer, UnprovenDescriptorProvenanceFallsBackToTheGenericLoad)
{
	// Descriptor still in flight: no lgkmcnt(0) between producer and consumer.
	ExpectUnprovenLoad({kPrefetch3, kLoadWord0, kLoadWord1, kMubufX2W0, kMubufX2W1, kUnsupportedSentinel, kEnd}, MappedInput(), 0xcu,
	                         "paired BufferLoadDwordx2");
	// V# from user SGPRs s[8:11] with no direct binding and no producer.
	ExpectUnprovenLoad({kPrefetch3, kMubufX2W0, 0x8002052bu, kUnsupportedSentinel, kEnd}, MappedInput(), 0x4u,
	                         "paired BufferLoadDwordx2");
	// s_mov_b32 s8, 0 redefines part of the direct V# quad.
	ExpectUnprovenLoad({kPrefetch3, kMovS8Zero, kMubufX2W0, 0x8002052bu, kUnsupportedSentinel, kEnd}, DirectInput(), 0x8u,
	                         "paired BufferLoadDwordx2");
	// The collector's mapping ends before this consumer.
	ExpectUnprovenLoad({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kMubufX2W0, kMubufX2W1, kUnsupportedSentinel, kEnd},
	                         MappedInput(0x0cu), 0x10u, "paired BufferLoadDwordx2");
	// s_branch at 0x18 targets the consumer, adding a second path.
	ExpectUnprovenLoad(
	    {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kMubufX2W0, kMubufX2W1, 0xbf82fffdu, kEnd}, MappedInput(), 0x10u,
	    "paired BufferLoadDwordx2");
}

TEST(EmulatorComputeWaveVectorBuffer, UnsupportedTupleControlsFallBackToTheGenericLoadUnlessTheyHaveNoLowering)
{
	// The specialized proof refuses every one of these tuples. The generic
	// emitter lowers the ones whose controls it models (OFFEN, cache hints,
	// IDXEN clear, SGPR or literal S_OFFSET, immediates), so the analysis falls
	// back; LDS, TFE and undefined encoding bits have no lowering and stay
	// rejected at the load, as does a descriptor quad outside the SGPR file.
	const std::vector<std::vector<uint32_t>> generic_loads = {
	    {0xe0343000u, 0x8007052bu}, // OFFEN: vaddr becomes v[43:44]
	    {0xe0346000u, 0x8006052bu}, // GLC
	    {0xe0342000u, 0x8046052bu}, // SLC
	    {0xe034a000u, 0x8006052bu}, // RDNA2 DLC (word0 bit 15), not legacy ADDR64
	    {0xe0340000u, 0x8006052bu}, // IDXEN clear
	    {0xe0342000u, 0x0804052bu}, // dynamic SOFFSET s8
	    {0xe0342010u, 0x8006052bu}, // immediate 16: 16+8 bytes past a 16-byte descriptor
	};
	for (const auto& load: generic_loads)
	{
		ExpectUnprovenLoad({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, load[0], load[1], kUnsupportedSentinel, kEnd}, MappedInput(),
		                   0x10u, "paired BufferLoad");
	}
	// Literal SOFFSET adds a third word.
	ExpectUnprovenLoad(
	    {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xe0342000u, 0xff04052bu, 0x40u, kUnsupportedSentinel, kEnd}, MappedInput(),
	    0x10u, "paired BufferLoad");

	const std::vector<std::vector<uint32_t>> unlowered_loads = {
	    {0xe0352000u, 0x8006052bu}, // LDS: the data goes to LDS, not to VGPRs
	    {0xe0342000u, 0x8086052bu}, // TFE: an extra status VGPR is written
	    {0xe0362000u, 0x8006052bu}, // undefined word0 bit 17
	    {0xe0342000u, 0x8026052bu}, // undefined word1 bit 21
	    {0xe0342000u, 0x801f052bu}, // srsrc quad outside the SGPR range (m0)
	};
	for (const auto& load: unlowered_loads)
	{
		ExpectFirstUnsupportedPc({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, load[0], load[1], kUnsupportedSentinel, kEnd},
		                         MappedInput(), 0x10u,
		                         "instruction BufferLoadDwordx2 has an invalid operand span or unsupported controls before strategy selection");
		ExpectProofRefuses({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, load[0], load[1], kUnsupportedSentinel, kEnd}, MappedInput(),
		                   0x10u, "paired BufferLoad");
	}
}

TEST(EmulatorComputeWaveVectorBuffer, DescriptorsOutsideTheRawAccessContractFallBackToTheGenericLoad)
{
	const std::vector<uint32_t> words = {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kMubufX2W0, kMubufX2W1, kUnsupportedSentinel, kEnd};
	auto empty                                   = MappedInput();
	empty.bind.storage_buffers.buffers[0].fields[2] = 0u;
	ExpectUnprovenLoad(words, empty, 0x10u, "paired BufferLoadDwordx2");
	auto swizzled = MappedInput();
	swizzled.bind.storage_buffers.buffers[0].fields[1] |= 1u << 31u;
	ExpectUnprovenLoad(words, swizzled, 0x10u, "paired BufferLoadDwordx2");
	auto add_tid = MappedInput();
	add_tid.bind.storage_buffers.buffers[0].fields[3] |= 1u << 23u;
	ExpectUnprovenLoad(words, add_tid, 0x10u, "paired BufferLoadDwordx2");
	auto zero_policy                                         = MappedInput();
	zero_policy.bind.zero_sbuffer_resources.start_register[0] = 16;
	zero_policy.bind.zero_sbuffer_resources.buffers_num       = 1;
	ExpectUnprovenLoad(words, zero_policy, 0x10u, "paired BufferLoadDwordx2");
}

TEST(EmulatorComputeWaveVectorBuffer, AdmitsDirectRawUmaxWithoutReturn)
{
	// Synthetic BUFFER_ATOMIC_UMAX v5, v7, s[0:3], 0 offset:4.
	// IDXEN/OFFEN/GLC and the unsupported cache/control bits are clear.
	constexpr uint32_t atomic_w0 = 0xe0e00004u;
	constexpr uint32_t atomic_w1 = 0x80000507u;
	auto input = DirectInput();
	input.bind.storage_buffers.start_register[0]   = 0;
	input.bind.storage_buffers.sources[0]          = ShaderStorageBindingSource::MetadataSharp;
	input.bind.storage_buffers.usages[0]          = ShaderStorageUsage::ReadWrite;
	input.bind.storage_buffers.accesses[0]        = ShaderStorageAccess::Raw;
	input.bind.storage_buffers.raw_vmem_oob_guarded[0] = true;
	input.bind.storage_buffers.code_available[0]   = true;
	input.bind.storage_buffers.exact_matches[0]    = true;
	input.bind.storage_buffers.buffers[0].fields[1] = 1u << 16u;
	input.bind.storage_buffers.buffers[0].fields[2] = 20u;
	input.bind.push_constant_size                   = 16u;
	const uint32_t shader_words[] = {atomic_w0, atomic_w1, kEnd};

	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(shader_words, sizeof(shader_words), &code) || !ShaderAnalyzeComputeWaveCode(code, input).supported)
		    {
			    std::_Exit(3);
		    }
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    const std::string text(source.c_str());
		    const auto first_atomic = text.find("OpAtomicUMax");
		    const auto second_atomic = first_atomic == std::string::npos ? first_atomic : text.find("OpAtomicUMax", first_atomic + 1);
		    if (first_atomic == std::string::npos || second_atomic == std::string::npos ||
		        text.find("OpAtomicUMax", second_atomic + 1) != std::string::npos ||
		        source.FindIndex("%buf_uint = OpVariable") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("unknown_uint_constant") != Core::STRING8_INVALID_INDEX ||
		        ShaderRecompileCS(code, &input).IsEmpty())
		    {
			    std::_Exit(4);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveVectorBuffer, RejectsUmaxReturnAndUnprovenBinding)
{
	InitializeConfig();
	const uint32_t words[] = {0xe0e00004u, 0x80000507u, kEnd};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ASSERT_TRUE(ShaderTryParseBounded(words, sizeof(words), &code));
	auto input = DirectInput();
	input.bind.storage_buffers.start_register[0]   = 0;
	input.bind.storage_buffers.sources[0]          = ShaderStorageBindingSource::MetadataSharp;
	input.bind.storage_buffers.usages[0]           = ShaderStorageUsage::ReadWrite;
	input.bind.storage_buffers.accesses[0]         = ShaderStorageAccess::Raw;
	input.bind.storage_buffers.raw_vmem_oob_guarded[0] = true;
	input.bind.storage_buffers.code_available[0]   = true;
	input.bind.storage_buffers.exact_matches[0]    = true;
	input.bind.storage_buffers.buffers[0].fields[1] = 1u << 16u;
	input.bind.storage_buffers.buffers[0].fields[2] = 20u;
	ASSERT_TRUE(ShaderAnalyzeComputeWaveVectorBufferAtomicUmax(code, 0, input.bind).supported);

	auto modified = code.GetInstructions().At(0);
	modified.buffer_return_old_value = true;
	ShaderCode returning;
	returning.SetType(ShaderType::Compute);
	returning.GetInstructions().Add(modified);
	returning.GetInstructions().Add(code.GetInstructions().At(1));
	EXPECT_FALSE(ShaderAnalyzeComputeWaveVectorBufferAtomicUmax(returning, 0, input.bind).supported);
	modified = code.GetInstructions().At(0);
	modified.buffer_idxen = true;
	ShaderCode indexed;
	indexed.SetType(ShaderType::Compute);
	indexed.GetInstructions().Add(modified);
	indexed.GetInstructions().Add(code.GetInstructions().At(1));
	EXPECT_FALSE(ShaderAnalyzeComputeWaveVectorBufferAtomicUmax(indexed, 0, input.bind).supported);
	auto bind = input.bind;
	bind.storage_buffers.exact_matches[0] = false;
	EXPECT_FALSE(ShaderAnalyzeComputeWaveVectorBufferAtomicUmax(code, 0, bind).supported);
	bind = input.bind;
	bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadOnly;
	EXPECT_FALSE(ShaderAnalyzeComputeWaveVectorBufferAtomicUmax(code, 0, bind).supported);
	bind = input.bind;
	bind.storage_buffers.buffers[0].fields[2] = 4u;
	EXPECT_FALSE(ShaderAnalyzeComputeWaveVectorBufferAtomicUmax(code, 0, bind).supported);
}

TEST(EmulatorComputeWaveVectorBuffer, ClassifiesRawUmaxAsGuardedWrite)
{
	constexpr uint32_t words[] = {0xe0e00004u, 0x80000507u, kEnd};
	InitializeConfig();
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ASSERT_TRUE(ShaderTryParseBounded(words, sizeof(words), &code));
	const auto use = AnalyzeShaderStorageUse(code, 0);
	EXPECT_EQ(use.access, ShaderStorageAccess::Raw);
	EXPECT_TRUE(use.raw_vmem_oob_guarded);
	EXPECT_EQ(ShaderGetDirectStorageUsage(code, 0), ShaderStorageUsage::ReadWrite);
}

UT_END();
