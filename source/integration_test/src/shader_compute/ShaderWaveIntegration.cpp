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

#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Math/MathAll.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

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
