#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>
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
static constexpr uint32_t kGetpc       = 0xbe941f00u; // s_getpc_b64 s[20:21]; outside the paired set
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
		    std::_Exit(!result.supported && result.unsupported_pc == pc && result.reason.ContainsStr(reason) ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveVectorBuffer, AdmitsDrainedMappedRawLoads)
{
	for (const uint32_t word0: {kMubufX1W0, kMubufX2W0, kMubufX3W0, kMubufX4W0, kMubufImm8W0})
	{
		ExpectFirstUnsupportedPc({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, word0, kMubufX2W1, kGetpc, kEnd},
		                         MappedInput(), 0x18u, "SGetpcB64");
	}
}

TEST(EmulatorComputeWaveVectorBuffer, AdmitsDirectUserSgprDescriptorLoad)
{
	// buffer_load_dwordx2 v[5:6], v43, s[8:11], 0 idxen bound directly.
	const uint32_t direct_w1 = 0x8002052bu; // srsrc=2 -> s[8:11]
	ExpectFirstUnsupportedPc({kPrefetch3, kMubufX2W0, direct_w1, kGetpc, kEnd}, DirectInput(), 0xcu, "SGetpcB64");
}

TEST(EmulatorComputeWaveVectorBuffer, AdmitsVmcntWaitCoveringTheLoad)
{
	// The real guest shape: mapped S_LOAD, lgkmcnt(0) drain, the dwordx2 load,
	// then the vmcnt(4) wait that covers it before its VGPR consumer.
	const uint32_t vmcnt4 = 0xbf8c3f74u;
	ExpectFirstUnsupportedPc({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kMubufX2W0, kMubufX2W1, vmcnt4, kGetpc, kEnd},
	                         MappedInput(), 0x1cu, "SGetpcB64");
}

TEST(EmulatorComputeWaveVectorBuffer, RejectsUnprovenDescriptorProvenance)
{
	// Descriptor still in flight: no lgkmcnt(0) between producer and consumer.
	ExpectFirstUnsupportedPc({kPrefetch3, kLoadWord0, kLoadWord1, kMubufX2W0, kMubufX2W1, kGetpc, kEnd}, MappedInput(), 0xcu,
	                         "paired BufferLoadDwordx2");
	// V# from user SGPRs s[8:11] with no direct binding and no producer.
	ExpectFirstUnsupportedPc({kPrefetch3, kMubufX2W0, 0x8002052bu, kGetpc, kEnd}, MappedInput(), 0x4u,
	                         "paired BufferLoadDwordx2");
	// s_mov_b32 s8, 0 redefines part of the direct V# quad.
	ExpectFirstUnsupportedPc({kPrefetch3, kMovS8Zero, kMubufX2W0, 0x8002052bu, kGetpc, kEnd}, DirectInput(), 0x8u,
	                         "paired BufferLoadDwordx2");
	// The collector's mapping ends before this consumer.
	ExpectFirstUnsupportedPc({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kMubufX2W0, kMubufX2W1, kGetpc, kEnd},
	                         MappedInput(0x0cu), 0x10u, "paired BufferLoadDwordx2");
	// s_branch at 0x18 targets the consumer, adding a second path.
	ExpectFirstUnsupportedPc(
	    {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kMubufX2W0, kMubufX2W1, 0xbf82fffdu, kEnd}, MappedInput(), 0x10u,
	    "paired BufferLoadDwordx2");
}

TEST(EmulatorComputeWaveVectorBuffer, RejectsUnsupportedTupleControls)
{
	const std::vector<std::vector<uint32_t>> loads = {
	    {0xe0343000u, 0x8007052bu}, // OFFEN: vaddr becomes v[43:44]
	    {0xe0346000u, 0x8006052bu}, // GLC
	    {0xe0352000u, 0x8006052bu}, // LDS
	    {0xe0342000u, 0x8046052bu}, // SLC
	    {0xe0342000u, 0x8086052bu}, // TFE
	    {0xe034a000u, 0x8006052bu}, // undefined word0 bit 15
	    {0xe0362000u, 0x8006052bu}, // undefined word0 bit 17
	    {0xe0342000u, 0x8026052bu}, // undefined word1 bit 21
	    {0xe0340000u, 0x8006052bu}, // IDXEN clear
	    {0xe0342000u, 0x0804052bu}, // dynamic SOFFSET s8
	    {0xe0342000u, 0x801f052bu}, // srsrc quad outside the SGPR range (m0)
	    {0xe0342010u, 0x8006052bu}, // immediate 16: 16+8 bytes past a 16-byte descriptor
	};
	for (const auto& load: loads)
	{
		ExpectFirstUnsupportedPc({kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, load[0], load[1], kGetpc, kEnd},
		                         MappedInput(), 0x10u, "paired BufferLoad");
	}
	// Literal SOFFSET adds a third word and is not admitted either.
	ExpectFirstUnsupportedPc(
	    {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, 0xe0342000u, 0xff04052bu, 0x40u, kGetpc, kEnd}, MappedInput(),
	    0x10u, "paired BufferLoad");
}

TEST(EmulatorComputeWaveVectorBuffer, RejectsDescriptorsOutsideTheRawAccessContract)
{
	const std::vector<uint32_t> words = {kPrefetch3, kLoadWord0, kLoadWord1, kWaitLgkm0, kMubufX2W0, kMubufX2W1, kGetpc, kEnd};
	auto empty                                   = MappedInput();
	empty.bind.storage_buffers.buffers[0].fields[2] = 0u;
	ExpectFirstUnsupportedPc(words, empty, 0x10u, "paired BufferLoadDwordx2");
	auto swizzled = MappedInput();
	swizzled.bind.storage_buffers.buffers[0].fields[1] |= 1u << 31u;
	ExpectFirstUnsupportedPc(words, swizzled, 0x10u, "paired BufferLoadDwordx2");
	auto add_tid = MappedInput();
	add_tid.bind.storage_buffers.buffers[0].fields[3] |= 1u << 23u;
	ExpectFirstUnsupportedPc(words, add_tid, 0x10u, "paired BufferLoadDwordx2");
	auto zero_policy                                         = MappedInput();
	zero_policy.bind.zero_sbuffer_resources.start_register[0] = 16;
	zero_policy.bind.zero_sbuffer_resources.buffers_num       = 1;
	ExpectFirstUnsupportedPc(words, zero_policy, 0x10u, "paired BufferLoadDwordx2");
}

UT_END();
