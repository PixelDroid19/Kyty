#include "VulkanComputeProbe.h"
#include "ShaderWaveMaskCases.h"
#include "ShaderWaveLaneCases.h"
#include "ShaderWaveLdsCases.h"
#include "ShaderWaveAluCases.h"
#include "ShaderNativeLdsAtomicCases.h"
#include "ShaderWaveScalarCases.h"
#include "ShaderWaveBranchCases.h"
#include "ShaderWaveHintCases.h"
#include "ShaderWaveResourceCases.h"
#include "ShaderWaveShiftCases.h"
#include "ShaderWaveProbeSource.h"

#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Math/MathAll.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveRuntime.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using Kyty::Libs::Graphics::ShaderComputeWaveDispatchPlan;
using Kyty::Libs::Graphics::ShaderComputeWaveLayout;
using Kyty::Libs::Graphics::ShaderComputeWaveRequest;
using Kyty::Libs::Graphics::ShaderComputeWaveLayoutStatus;
using Kyty::Libs::Graphics::ShaderGuestLaneOrder;
using Kyty::Libs::Graphics::ShaderComputeWaveStrategy;
using Kyty::Libs::Graphics::ShaderToolchain::Run;
using Kyty::Libs::Graphics::VulkanComputeProbe;

[[noreturn]] void Fail(const char* message)
{
	std::fprintf(stderr, "paired wave64 integration failure: %s\n", message);
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void CheckOversizedOutputRejectsBeforeVulkanInitialization()
{
	VulkanComputeProbe probe;
	const ShaderComputeWaveLayout layout {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0};
	const std::array<uint32_t, 3> groups {1, 1, 1};
	const std::vector<uint32_t> initial(4097);
	std::vector<uint32_t> oversized_output(4097);
	const std::array<uint32_t, 5> spirv {0x07230203u, 0x00010000u, 0u, 1u, 0u};
	std::string message;
	const auto result = probe.DispatchWave(spirv.data(), spirv.size(), layout, groups, initial, &oversized_output, &message);
	if (result != VulkanComputeProbe::Result::InvalidArgument)
	{
		Fail("a 4097-word output must be rejected before Vulkan resource creation");
	}
	std::printf("OversizedWaveOutputRejected PASS\n");
}

void InitializeConfig()
{
	char program[] = "kyty_shader_wave64_integration";
	char* argv[] = {program, nullptr};
	Kyty::Core::SubsystemsList* subsystems = Kyty::Core::SubsystemsListSingleton::Instance();
	subsystems->SetArgs(1, argv);
	using Kyty::Config::ConfigSubsystem;
	using Kyty::Core::CoreSubsystem;
	using Kyty::Core::ThreadsSubsystem;
	using Kyty::Log::LogSubsystem;
	using Kyty::Math::MathSubsystem;
	subsystems->Add(CoreSubsystem::Instance(), {});
	subsystems->Add(ConfigSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(MathSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(ThreadsSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(LogSubsystem::Instance(),
	                {CoreSubsystem::Instance(), ConfigSubsystem::Instance(), ThreadsSubsystem::Instance()});
	if (!subsystems->InitAll(false)) { Fail("core/config/log subsystems must initialize"); }
	Kyty::Config::SetNextGen(true);

	class ValidationConfig final: public Kyty::Config::ConfigSource
	{
	public:
		bool Has(const Kyty::Core::String& key) const override
		{
			return key == U"ShaderValidationEnabled" || key == U"ShaderOptimizationType";
		}
		int64_t GetInteger(const Kyty::Core::String&) const override { return 0; }
		bool GetBool(const Kyty::Core::String&) const override { return true; }
		Kyty::Core::String GetString(const Kyty::Core::String&) const override { return {}; }
	} validation;
	Kyty::Config::Load(validation);
}

const char* const kHostLayoutSpirv = R"SPV(
OpCapability Shader
OpCapability GroupNonUniform
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %subgroup_lane %subgroup_id %workgroup_id %output_buffer
OpExecutionMode %main LocalSize 128 1 1
OpDecorate %subgroup_lane BuiltIn SubgroupLocalInvocationId
OpDecorate %subgroup_id BuiltIn SubgroupId
OpDecorate %workgroup_id BuiltIn WorkgroupId
OpDecorate %output_words ArrayStride 4
OpMemberDecorate %output_block 0 Offset 0
OpDecorate %output_block Block
OpDecorate %output_buffer DescriptorSet 0
OpDecorate %output_buffer Binding 0
%void = OpTypeVoid
%function_type = OpTypeFunction %void
%uint = OpTypeInt 32 0
%v3uint = OpTypeVector %uint 3
%uint_zero = OpConstant %uint 0
%uint_32 = OpConstant %uint 32
%uint_64 = OpConstant %uint 64
%uint_256 = OpConstant %uint 256
%output_words = OpTypeRuntimeArray %uint
%output_block = OpTypeStruct %output_words
%output_block_pointer = OpTypePointer StorageBuffer %output_block
%output_uint_pointer = OpTypePointer StorageBuffer %uint
%input_uint_pointer = OpTypePointer Input %uint
%input_v3uint_pointer = OpTypePointer Input %v3uint
%output_buffer = OpVariable %output_block_pointer StorageBuffer
%subgroup_lane = OpVariable %input_uint_pointer Input
%subgroup_id = OpVariable %input_uint_pointer Input
%workgroup_id = OpVariable %input_v3uint_pointer Input
%main = OpFunction %void None %function_type
%entry = OpLabel
%lane = OpLoad %uint %subgroup_lane
%subgroup = OpLoad %uint %subgroup_id
%workgroup = OpLoad %v3uint %workgroup_id
%workgroup_x = OpCompositeExtract %uint %workgroup 0
%workgroup_base = OpIMul %uint %workgroup_x %uint_256
%subgroup_base = OpIMul %uint %subgroup %uint_64
%wave_base = OpIAdd %uint %workgroup_base %subgroup_base
%bank_zero_index = OpIAdd %uint %wave_base %lane
%bank_one_lane = OpIAdd %uint %lane %uint_32
%bank_one_index = OpIAdd %uint %wave_base %bank_one_lane
%bank_zero_pointer = OpAccessChain %output_uint_pointer %output_buffer %uint_zero %bank_zero_index
%bank_one_pointer = OpAccessChain %output_uint_pointer %output_buffer %uint_zero %bank_one_index
OpStore %bank_zero_pointer %bank_zero_index
OpStore %bank_one_pointer %bank_one_index
OpReturn
OpFunctionEnd
)SPV";

const char* const kNativeWave32MaskSpirv = R"SPV(
OpCapability Shader
OpCapability GroupNonUniform
OpCapability GroupNonUniformBallot
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %subgroup_lane %subgroup_id %output_buffer
OpExecutionMode %main LocalSize 64 1 1
OpDecorate %subgroup_lane BuiltIn SubgroupLocalInvocationId
OpDecorate %subgroup_id BuiltIn SubgroupId
OpDecorate %output_words ArrayStride 4
OpMemberDecorate %output_block 0 Offset 0
OpDecorate %output_block Block
OpDecorate %output_buffer DescriptorSet 0
OpDecorate %output_buffer Binding 0
%void = OpTypeVoid
%bool = OpTypeBool
%function_type = OpTypeFunction %void
%uint = OpTypeInt 32 0
%v4uint = OpTypeVector %uint 4
%uint_zero = OpConstant %uint 0
%uint_one = OpConstant %uint 1
%uint_eight = OpConstant %uint 8
%uint_32 = OpConstant %uint 32
%uint_subgroup_scope = OpConstant %uint 3
%output_words = OpTypeRuntimeArray %uint
%output_block = OpTypeStruct %output_words
%output_block_pointer = OpTypePointer StorageBuffer %output_block
%output_uint_pointer = OpTypePointer StorageBuffer %uint
%input_uint_pointer = OpTypePointer Input %uint
%output_buffer = OpVariable %output_block_pointer StorageBuffer
%subgroup_lane = OpVariable %input_uint_pointer Input
%subgroup_id = OpVariable %input_uint_pointer Input
%main = OpFunction %void None %function_type
%entry = OpLabel
%lane = OpLoad %uint %subgroup_lane
%subgroup = OpLoad %uint %subgroup_id
%wave_base = OpIMul %uint %subgroup %uint_32
%guest_index = OpIAdd %uint %wave_base %lane
%lane_parity = OpBitwiseAnd %uint %lane %uint_one
%even_lane = OpIEqual %bool %lane_parity %uint_zero
%first_wave = OpIEqual %bool %subgroup %uint_zero
%second_wave_lane = OpULessThan %bool %lane %uint_eight
%predicate = OpSelect %bool %first_wave %even_lane %second_wave_lane
%wave_mask = OpGroupNonUniformBallot %v4uint %uint_subgroup_scope %predicate
%mask_low = OpCompositeExtract %uint %wave_mask 0
%output_pointer = OpAccessChain %output_uint_pointer %output_buffer %uint_zero %guest_index
OpStore %output_pointer %mask_low
OpReturn
OpFunctionEnd
)SPV";

void RunNativeLayoutFixture(VulkanComputeProbe* vulkan)
{
	if (vulkan == nullptr) { Fail("wave probe is null"); }
	const ShaderComputeWaveRequest request {{256, 1, 1}, {2, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	ShaderComputeWaveLayout layout {};
	const auto layout_status = Kyty::Libs::Graphics::ShaderBuildPairedComputeWaveLayout(request,
	                                                                                     vulkan->WaveCapabilities(), &layout);
	if (layout_status == ShaderComputeWaveLayoutStatus::MissingHostCapability ||
	    layout_status == ShaderComputeWaveLayoutStatus::HostLimitExceeded)
	{
		std::fprintf(stderr, "paired-wave host layout unavailable: status=%u\n", static_cast<unsigned>(layout_status));
		std::fflush(stderr);
		std::_Exit(77);
	}
	if (layout_status != ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("the bounded size-64 host-layout request was rejected unexpectedly");
	}

	Kyty::Core::String8 assembly = kHostLayoutSpirv;
	Kyty::Vector<uint32_t> spirv;
	Kyty::Core::String8 error;
	if (!Run(assembly, &spirv, &error) || spirv.IsEmpty())
	{
		std::fprintf(stderr, "host-layout SPIR-V assembly/validation failed: %s\n", error.c_str());
		std::fflush(stderr);
		Fail("host-layout SPIR-V did not validate");
	}

	constexpr uint32_t kCanary = 0xdeadbeefu;
	std::vector<uint32_t> initial_words(512, kCanary);
	std::vector<uint32_t> actual(512, kCanary);
	const std::array<uint32_t, 3> groups {2, 1, 1};
	std::string message;
	const auto result = vulkan->DispatchWave(spirv.GetDataConst(), spirv.Size(), layout, groups, initial_words, &actual, &message);
	if (result == VulkanComputeProbe::Result::Unavailable)
	{
		std::fprintf(stderr, "paired-wave host layout unavailable: %s\n", message.c_str());
		std::fflush(stderr);
		std::_Exit(77);
	}
	if (result != VulkanComputeProbe::Result::Success)
	{
		std::fprintf(stderr, "paired-wave host layout dispatch failed: %s\n", message.c_str());
		std::fflush(stderr);
		Fail("host-layout Vulkan dispatch failed");
	}
	for (uint32_t lane = 0; lane < 512; ++lane)
	{
		if (actual[lane] != lane) { Fail("host-layout output did not preserve unique slots across guest waves and workgroups"); }
	}
	std::printf("PairedWave64HostLayout waves_per_group=4 groups=2 output_words=512 PASS\n");
}

void RunNativeWave32MaskFixture(VulkanComputeProbe* vulkan)
{
	if (vulkan == nullptr) { Fail("native wave32 probe is null"); }
	const auto& capabilities = vulkan->WaveCapabilities();
	if ((vulkan->DefaultSubgroupSize() != 32u && !capabilities.size_control_enabled) ||
	    !capabilities.compute_required_size_supported || capabilities.min_subgroup_size > 32u ||
	    capabilities.max_subgroup_size < 32u || capabilities.max_subgroups < 2u)
	{
		std::fprintf(stderr, "native wave32 full-wave fixture unavailable: exact subgroup32 or two full subgroup slots are unsupported\n");
		std::fflush(stderr);
		std::_Exit(77);
	}

	Kyty::Libs::Graphics::ShaderComputeWavePreflightRequest request {};
	request.is_next_gen = true;
	request.dispatch_mode = 0x8041u;
	request.raw_dispatch_count[0] = request.raw_dispatch_count[1] = request.raw_dispatch_count[2] = 1u;
	request.local_size[0] = 64u;
	request.local_size[1] = request.local_size[2] = 1u;
	request.lane_order = ShaderGuestLaneOrder::LinearXFirst;
	ShaderComputeWaveDispatchPlan plan {};
	const auto preflight = Kyty::Libs::Graphics::ShaderBuildComputeWaveDispatchPlan(request, capabilities, &plan);
	if (preflight.status != Kyty::Libs::Graphics::ShaderComputeWavePreflightStatus::Supported)
	{
		std::fprintf(stderr, "native wave32 full-wave fixture unavailable: preflight status=%u reason=%u\n",
		             static_cast<unsigned>(preflight.status), static_cast<unsigned>(preflight.reason));
		std::fflush(stderr);
		std::_Exit(77);
	}
	plan.wave_layout.native_subgroup_size = 32u;

	Kyty::Core::String8 assembly = kNativeWave32MaskSpirv;
	Kyty::Vector<uint32_t> spirv;
	Kyty::Core::String8 error;
	if (!Run(assembly, &spirv, &error) || spirv.IsEmpty())
	{
		std::fprintf(stderr, "native wave32 mask SPIR-V assembly/validation failed: %s\n", error.c_str());
		std::fflush(stderr);
		Fail("native wave32 mask SPIR-V did not validate");
	}

	constexpr uint32_t kCanary = 0xdeadbeefu;
	std::vector<uint32_t> initial(64u, kCanary);
	std::vector<uint32_t> actual(64u, kCanary);
	std::string message;
	const auto result = vulkan->DispatchWave(spirv.GetDataConst(), spirv.Size(), plan.wave_layout, {1u, 1u, 1u},
	                                        initial, &actual, &message);
	if (result == VulkanComputeProbe::Result::Unavailable)
	{
		std::fprintf(stderr, "native wave32 full-wave fixture unavailable: %s\n", message.c_str());
		std::fflush(stderr);
		std::_Exit(77);
	}
	if (result != VulkanComputeProbe::Result::Success)
	{
		std::fprintf(stderr, "native wave32 full-wave dispatch failed: %s\n", message.c_str());
		std::fflush(stderr);
		Fail("native wave32 full-wave Vulkan dispatch failed after capability preflight");
	}
	for (uint32_t lane = 0; lane < 64u; ++lane)
	{
		const uint32_t expected = lane < 32u ? 0x55555555u : 0x000000ffu;
		if (actual[lane] != expected)
		{
			Fail(Kyty::Core::String8::FromPrintf("native wave32 mask lane=%u actual=%08x expected=%08x", lane, actual[lane], expected).c_str());
		}
	}
	std::printf("NativeWave32Masks full_waves=2 wave0=0x55555555 wave1=0x000000ff PASS\n");
}

void RunNativeWave32CoordinateFixture(VulkanComputeProbe* vulkan)
{
	if (vulkan == nullptr) { Fail("native wave32 coordinate probe is null"); }
	const auto& capabilities = vulkan->WaveCapabilities();
	if ((vulkan->DefaultSubgroupSize() != 32u && !capabilities.size_control_enabled) ||
	    !capabilities.compute_required_size_supported || capabilities.min_subgroup_size > 32u ||
	    capabilities.max_subgroup_size < 32u || capabilities.max_subgroups < 2u)
	{
		std::fprintf(stderr, "native wave32 coordinate fixture unavailable: exact subgroup32 or two full subgroup slots are unsupported\n");
		std::fflush(stderr);
		std::_Exit(77);
	}

	Kyty::Libs::Graphics::ShaderComputeWavePreflightRequest request {};
	request.is_next_gen = true;
	request.dispatch_mode = 0x8061u; // Wave32 + USE_THREAD_DIMENSIONS + compute enable.
	request.raw_dispatch_count[0] = 32u;
	request.raw_dispatch_count[1] = 1u;
	request.raw_dispatch_count[2] = 1u;
	request.local_size[0] = 32u;
	request.local_size[1] = 2u;
	request.local_size[2] = 1u;
	request.lane_order = ShaderGuestLaneOrder::LinearXFirst;
	ShaderComputeWaveDispatchPlan plan {};
	const auto preflight = Kyty::Libs::Graphics::ShaderBuildComputeWaveDispatchPlan(request, capabilities, &plan);
	if (preflight.status != Kyty::Libs::Graphics::ShaderComputeWavePreflightStatus::Supported)
	{
		std::fprintf(stderr, "native wave32 coordinate fixture unavailable: preflight status=%u reason=%u\n",
		             static_cast<unsigned>(preflight.status), static_cast<unsigned>(preflight.reason));
		std::fflush(stderr);
		std::_Exit(77);
	}
	plan.wave_layout.native_subgroup_size = 32u;

	const std::array<uint32_t, 2> words {0xbe84037eu, 0xbf810000u}; // s_mov_b32 s4, exec_lo; s_endpgm.
	Kyty::Libs::Graphics::ShaderCode code;
	code.SetType(Kyty::Libs::Graphics::ShaderType::Compute);
	if (!Kyty::Libs::Graphics::ShaderTryParseBounded(words.data(), static_cast<uint32_t>(sizeof(words)), &code) ||
	    !Kyty::Libs::Graphics::ShaderUsesNativeWaveState(code))
	{
		Fail("native coordinate fixture must consume guest EXEC state");
	}

	Kyty::Libs::Graphics::ShaderComputeInputInfo input {};
	input.dispatch_mode = request.dispatch_mode;
	input.threads_num[0] = request.local_size[0];
	input.threads_num[1] = request.local_size[1];
	input.threads_num[2] = request.local_size[2];
	input.thread_ids_num = 2;
	input.wave_layout = plan.wave_layout;
	input.native_wave_sensitive = Kyty::Libs::Graphics::ShaderUsesNativeWaveState(code);
	input.required_subgroup_size = input.native_wave_sensitive ? 32u : 0u;
	input.thread_limits_used = plan.thread_limits_used;
	for (uint32_t axis = 0; axis < 3u; ++axis) { input.thread_limits[axis] = plan.thread_limits[axis]; }

	auto& bind = input.bind;
	bind.extended.used = true;
	bind.extended.slot = 5;
	bind.extended.start_register = 12;
	bind.extended.eud_user_sgpr_num = 14;
	bind.extended.eud_size_dw = 24;
	bind.extended.eud_offset_base = 32;
	bind.extended.data.fields[0] = 1u;
	bind.storage_buffers.buffers_num = 1;
	bind.storage_buffers.dynamic_sload[0] = true;
	bind.storage_buffers.sources[0] = Kyty::Libs::Graphics::ShaderStorageBindingSource::DynamicScalarLoad;
	bind.storage_buffers.start_register[0] = 16;
	bind.thread_limits_used = input.thread_limits_used;
	for (uint32_t axis = 0; axis < 3u; ++axis) { bind.thread_limits[axis] = input.thread_limits[axis]; }
	Kyty::Libs::Graphics::ShaderDynamicSLoadMapping mapping {};
	mapping.kind = Kyty::Libs::Graphics::ShaderDynamicSLoadResourceKind::StorageBuffer;
	mapping.resource_index = 0;
	mapping.destination_register = 16;
	mapping.instruction_pc = 4u;
	mapping.offset_dw = 20;
	mapping.dword_count = 4;
	mapping.last_consumer_pc = 0x14u;
	bind.dynamic_sloads.records.Add(mapping);
	Kyty::Libs::Graphics::ShaderCalcBindingIndices(&bind);
	if (bind.push_constant_size != 32u || bind.thread_limits_offset_dw != 4u)
	{
		Fail("native coordinate fixture metadata layout is not the expected descriptor-plus-thread-limits block");
	}

	const std::vector<Kyty::Libs::Graphics::WaveProbeObservation> observations {
	    {"v0", "v0", true}, {"v1", "v1", true}, {"s4", "s4"}, {"exec_lo", "exec_lo"}};
	Kyty::Core::String8 source, error;
	if (!Kyty::Libs::Graphics::BuildWaveProbeSource(words.data(), sizeof(words), input, observations, {}, &source, &error, 1u))
	{
		Fail(error.c_str());
	}
	Kyty::Vector<uint32_t> spirv;
	if (!Run(source, &spirv, &error) || spirv.IsEmpty())
	{
		std::fprintf(stderr, "native wave32 coordinate emitter assembly failed: %s\n", error.c_str());
		std::fflush(stderr);
		Fail("production-emitter native wave32 coordinate SPIR-V did not validate");
	}

	std::vector<uint32_t> metadata(bind.push_constant_size / sizeof(uint32_t));
	for (uint32_t axis = 0; axis < 3u; ++axis) { metadata[bind.thread_limits_offset_dw + axis] = input.thread_limits[axis]; }
	constexpr uint32_t kCanary = 0xdeadbeefu;
	constexpr uint32_t kObservationStride = 4u;
	std::vector<uint32_t> initial(128u * kObservationStride, kCanary);
	std::vector<uint32_t> actual(initial.size(), kCanary);
	const std::array<uint32_t, 3> groups {plan.group_count[0], plan.group_count[1], plan.group_count[2]};
	std::string message;
	const auto result = vulkan->DispatchWaveWithMetadata(spirv.GetDataConst(), spirv.Size(), plan.wave_layout, groups, initial,
	                                                    bind, metadata, 1u, &actual, &message);
	if (result == VulkanComputeProbe::Result::Unavailable)
	{
		std::fprintf(stderr, "native wave32 coordinate fixture unavailable: %s\n", message.c_str());
		std::fflush(stderr);
		std::_Exit(77);
	}
	if (result != VulkanComputeProbe::Result::Success)
	{
		std::fprintf(stderr, "native wave32 coordinate dispatch failed: %s\n", message.c_str());
		std::fflush(stderr);
		Fail("production-emitter native wave32 coordinate dispatch failed after capability preflight");
	}

	for (uint32_t subgroup = 0; subgroup < 2u; ++subgroup)
	{
		const uint32_t expected_exec = subgroup == 0u ? 0xffffffffu : 0u;
		for (uint32_t lane = 0; lane < 32u; ++lane)
		{
			for (uint32_t bank = 0; bank < 2u; ++bank)
			{
				const uint32_t logical = subgroup * 64u + bank * 32u + lane;
				const uint32_t index = logical * kObservationStride;
				if (actual[index] != lane || actual[index + 1u] != subgroup || actual[index + 2u] != expected_exec ||
				    actual[index + 3u] != expected_exec)
				{
					Fail(Kyty::Core::String8::FromPrintf(
					         "native coordinate subgroup=%u lane=%u bank=%u got=(%u,%u,%08x,%08x) expected=(%u,%u,%08x,%08x)",
					         subgroup, lane, bank, actual[index], actual[index + 1u], actual[index + 2u], actual[index + 3u], lane, subgroup,
					         expected_exec, expected_exec).c_str());
				}
			}
		}
	}
	std::printf("NativeWave32Coordinates XY=32x2 thread_limits_y=1 seeded=(x,y) masks=(0xffffffff,0x00000000) PASS\n");
}

} // namespace

int main()
{
	CheckOversizedOutputRejectsBeforeVulkanInitialization();
	InitializeConfig();
	VulkanComputeProbe vulkan;
	std::string message;
	const auto initialized = vulkan.InitializeWave(&message);
	if (initialized == VulkanComputeProbe::Result::Unavailable)
	{
		std::fprintf(stderr, "paired-wave compute capability unavailable: %s\n", message.c_str());
		std::fflush(stderr);
		return 77;
	}
	if (initialized != VulkanComputeProbe::Result::Success)
	{
		std::fprintf(stderr, "paired-wave compute initialization failed: %s\n", message.c_str());
		std::fflush(stderr);
		Fail("optional Vulkan wave probe initialization failed");
	}
	RunNativeLayoutFixture(&vulkan);
	RunNativeWave32MaskFixture(&vulkan);
	RunNativeWave32CoordinateFixture(&vulkan);
	Kyty::Libs::Graphics::RunWaveMaskCases(vulkan);
	Kyty::Libs::Graphics::RunWaveBranchCases(vulkan);
	Kyty::Libs::Graphics::RunWaveLaneCases(vulkan);
	Kyty::Libs::Graphics::RunWaveLdsCases(vulkan);
	Kyty::Libs::Graphics::RunWaveAluCases(vulkan);
	Kyty::Libs::Graphics::RunNativeLdsAtomicCases(vulkan);
	Kyty::Libs::Graphics::RunWaveScalarCases(vulkan);
	Kyty::Libs::Graphics::RunWaveShiftCases(vulkan);
	Kyty::Libs::Graphics::RunWaveHintCases(vulkan);
	Kyty::Libs::Graphics::RunWaveResourceCases(vulkan);
	return 0;
}
