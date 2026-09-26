#include "ShaderWaveResourceCases.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveProbeSource.h"
#include "VulkanComputeProbe.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kLogicalLanes = 128u;
constexpr uint32_t kSentinel     = 0xa5c37e19u;

[[noreturn]] void Fail(const char* reason)
{
	std::fprintf(stderr, "paired EUD S_LOAD mapped-lowering failure: %s\n", reason);
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

// Two guest waves with one EUD S_LOAD s[16:19] at pc 4 mapped to storage
// resource 0. This hand-supplied mapping isolates SPIR-V lowering;
// collector-produced mappings and actual descriptor consumers have separate
// validation.
ShaderComputeInputInfo MappedInput(VulkanComputeProbe& probe, const std::array<uint32_t, 4>& descriptor, uint32_t last_consumer_pc)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = kLogicalLanes;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num                        = 1u;
	const ShaderComputeWaveRequest request {{kLogicalLanes, 1u, 1u}, {1u, 1u, 1u}, 0x41u, 0u, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("two-wave paired layout unavailable");
	}

	auto& bind = input.bind;
	bind.extended.used                       = true;
	bind.extended.slot                       = 5;
	bind.extended.start_register             = 12;
	bind.extended.eud_user_sgpr_num          = 14;
	bind.extended.eud_size_dw                = 24;
	bind.extended.eud_offset_base            = 32;
	bind.extended.data.fields[0]             = 1u;
	bind.storage_buffers.buffers_num         = 1;
	bind.storage_buffers.dynamic_sload[0]    = true;
	bind.storage_buffers.sources[0]         = ShaderStorageBindingSource::DynamicScalarLoad;
	bind.storage_buffers.start_register[0]  = 16;
	for (size_t field = 0; field < descriptor.size(); ++field)
	{
		bind.storage_buffers.buffers[0].fields[field] = descriptor[field];
	}
	ShaderDynamicSLoadMapping mapping {};
	mapping.kind                 = ShaderDynamicSLoadResourceKind::StorageBuffer;
	mapping.resource_index       = 0;
	mapping.destination_register = 16;
	mapping.instruction_pc       = 4u;
	mapping.offset_dw            = 20;
	mapping.dword_count           = 4;
	mapping.last_consumer_pc      = last_consumer_pc;
	bind.dynamic_sloads.records.Add(mapping);
	ShaderCalcBindingIndices(&bind);
	if (bind.vsharp_uniform_buffer || bind.push_constant_size != descriptor.size() * sizeof(uint32_t))
	{
		Fail("synthetic descriptor metadata is not a portable push constant");
	}
	return input;
}

// The fixture binds the same bounded buffer as storage resource 0 and as the
// observation output, so reads must target words that no lane writes.
std::vector<uint32_t> DispatchMapped(VulkanComputeProbe& probe, const uint32_t* words, uint32_t bytes, const ShaderComputeInputInfo& input,
                                     const std::array<uint32_t, 4>& metadata_words,
                                     const std::vector<WaveProbeObservation>& observations, const std::vector<WaveProbeSeed>& seeds,
                                     const std::vector<uint32_t>& initial)
{
	String8 source, error;
	if (!BuildWaveProbeSource(words, bytes, input, observations, seeds, &source, &error, 1u))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		Fail(error.c_str());
	}
	std::vector<uint32_t>       actual(initial.size());
	std::string                 message;
	const std::vector<uint32_t> metadata(metadata_words.begin(), metadata_words.end());
	if (probe.DispatchWaveWithMetadata(binary.GetDataConst(), binary.Size(), input.wave_layout, {1u, 1u, 1u}, initial, input.bind,
	                                   metadata, 1u, &actual, &message) != VulkanComputeProbe::Result::Success)
	{
		Fail(message.c_str());
	}
	return actual;
}

template <size_t N>
void RunMappedFourDwords(VulkanComputeProbe& probe, const std::array<uint32_t, N>& words, const char* case_name)
{
	constexpr std::array<uint32_t, 4> descriptor {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u};
	const auto                        input = MappedInput(probe, descriptor, 0x14u);
	const std::vector<WaveProbeObservation> observations {{"s16", "s16"}, {"s17", "s17"},
	                                                      {"s18", "s18"}, {"s19", "s19"}};
	const std::vector<uint32_t> initial(kLogicalLanes * descriptor.size() + 64u, kSentinel);
	const auto actual = DispatchMapped(probe, words.data(), sizeof(words), input, descriptor, observations, {}, initial);
	for (uint32_t lane = 0; lane < kLogicalLanes; ++lane)
	{
		for (size_t field = 0; field < descriptor.size(); ++field)
		{
			if (actual[lane * descriptor.size() + field] != descriptor[field])
			{
				Fail("descriptor field differs across paired waves or banks");
			}
		}
	}
	for (size_t index = kLogicalLanes * descriptor.size(); index < actual.size(); ++index)
	{
		if (actual[index] != kSentinel) { Fail("output canary overwritten"); }
	}
	std::printf("%s PASS\n", case_name);
}

// s_buffer_load_dword vcc_lo reads storage word 130 through the drained V#,
// optionally followed by its own lgkmcnt(0). Dword 0 of the V# carries binding
// index 0, as PrepareStorageBuffers publishes it.
void RunMappedScalarBufferLoad(VulkanComputeProbe& probe, bool trailing_wait, const char* case_name)
{
	constexpr uint32_t                word  = kLogicalLanes + 2u;
	constexpr uint32_t                value = 0x5eed1234u;
	constexpr std::array<uint32_t, 4> descriptor {0u, 0u, (kLogicalLanes + 64u) * 4u, 0u};
	const auto                        input = MappedInput(probe, descriptor, 0x10u);
	const std::array<uint32_t, 8>     words {0xbfa00003u, 0xf4080406u, 0xfa000050u, 0xbf8cc07fu, 0xf4201a88u, 0xfa000000u | (word * 4u),
                                         trailing_wait ? 0xbf8cc07fu : 0xbf810000u, 0xbf810000u};
	std::vector<uint32_t>             initial(kLogicalLanes + 64u, kSentinel);
	initial[word]     = value;
	const uint32_t bytes = static_cast<uint32_t>((trailing_wait ? words.size() : words.size() - 1u) * sizeof(uint32_t));
	// Seeding a different value proves that the destination is overwritten.
	const auto actual = DispatchMapped(probe, words.data(), bytes, input, descriptor, {{"vcc_lo", "vcc_lo"}}, {{"vcc_lo", 0x0badf00du}},
	                                       initial);
	for (uint32_t lane = 0; lane < kLogicalLanes; ++lane)
	{
		if (actual[lane] != value) { Fail("scalar buffer word differs across paired waves or banks"); }
	}
	for (size_t index = kLogicalLanes; index < actual.size(); ++index)
	{
		if (actual[index] != initial[index]) { Fail("scalar buffer source or output canary overwritten"); }
	}
	std::printf("%s PASS\n", case_name);
}

} // namespace

void RunWaveResourceCases(VulkanComputeProbe& probe)
{
	RunMappedFourDwords(probe, std::array<uint32_t, 4> {0xbfa00003u, 0xf4080406u, 0xfa000050u, 0xbf810000u},
	                    "PairedWaveEudMappedSLoadLoweringFourDwords");
	// s_waitcnt lgkmcnt(0) covering the mapped load must not change its result.
	RunMappedFourDwords(probe, std::array<uint32_t, 5> {0xbfa00003u, 0xf4080406u, 0xfa000050u, 0xbf8cc07fu, 0xbf810000u},
	                    "PairedWaveEudMappedSLoadCoveredByLgkmWait");
	RunMappedScalarBufferLoad(probe, false, "PairedWaveMappedScalarBufferLoadReadsDrainedDescriptor");
	RunMappedScalarBufferLoad(probe, true, "PairedWaveMappedScalarBufferLoadCoveredByLgkmWait");
}

} // namespace Kyty::Libs::Graphics
