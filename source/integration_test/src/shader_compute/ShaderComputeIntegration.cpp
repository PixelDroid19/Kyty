#include "ShaderProbeSource.h"
#include "VulkanComputeProbe.h"

#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Math/MathAll.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using Kyty::Libs::Graphics::ScalarRegisterSeed;
using Kyty::Libs::Graphics::ShaderToolchain::Run;
using Kyty::Libs::Graphics::VulkanComputeProbe;

constexpr uint32_t kSeedScc     = 1u;
constexpr uint32_t kSeedExecLo  = 0u;
constexpr uint32_t kSeedExecHi  = 0u;
constexpr uint32_t kEndProgram  = 0xbf810000u;

struct ProbeCase
{
	const char*               name = nullptr;
	std::vector<uint32_t>     shader_words;
	std::vector<ScalarRegisterSeed> register_seeds;
	int                       result_register = -1;
	std::array<uint32_t, 4>   expected {};
	Kyty::Vector<uint32_t>    spirv;
};

struct ProgramBaseProbe
{
	const char*                    name = nullptr;
	bool                           nonempty_exec = false;
	Kyty::Libs::Graphics::ShaderComputeInputInfo input_info;
	Kyty::Vector<uint32_t>         spirv;
};

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "shader compute integration failure: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void InitializeConfig()
{
	char program[] = "kyty_shader_compute_integration";
	char* argv[]   = {program, nullptr};
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
	subsystems->Add(LogSubsystem::Instance(), {CoreSubsystem::Instance(), ConfigSubsystem::Instance(), ThreadsSubsystem::Instance()});
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

uint32_t Bitset1Word(uint32_t destination, uint32_t source)
{
	return (0x17du << 23u) | (destination << 16u) | (0x1du << 8u) | source;
}

uint32_t GetPcB64Word(uint32_t destination)
{
	return (0x17du << 23u) | (destination << 16u) | (0x1fu << 8u);
}

uint32_t Compare64NotEqualWord(uint32_t source0, uint32_t source1)
{
	return 0xbf000000u | (0x13u << 16u) | (source1 << 8u) | source0;
}

void PrepareCase(ProbeCase* test_case)
{
	Kyty::Core::String8 source;
	Kyty::Core::String8 error;
	if (!Kyty::Libs::Graphics::BuildScalarProbeSource(test_case->shader_words.data(), test_case->shader_words.size(),
	                                                  test_case->register_seeds, test_case->result_register, &source, &error))
	{
		Fail(std::string(test_case->name) + ": source generation failed: " + error.c_str());
	}
	if (!Run(source, &test_case->spirv, &error) || test_case->spirv.IsEmpty())
	{
		Fail(std::string(test_case->name) + ": ShaderToolchain failed validation/assembly: " + error.c_str());
	}
}

ProgramBaseProbe MakeProgramBaseProbe(const char* name, bool use_uniform_buffer)
{
	ProgramBaseProbe probe {};
	probe.name = name;
	probe.input_info.threads_num[0] = 1;
	probe.input_info.threads_num[1] = 1;
	probe.input_info.threads_num[2] = 1;
	auto& bind = probe.input_info.bind;
	bind.program_base_used = true;
	if (use_uniform_buffer)
	{
		// Model the real 32 direct SGPR entries used by the 144-byte metadata
		// layout; the test SSBO is allocated at the next descriptor binding.
		bind.direct_sgprs.sgprs_num = Kyty::Libs::Graphics::ShaderDirectSgprsResources::SGPRS_MAX;
		for (int i = 0; i < bind.direct_sgprs.sgprs_num; ++i)
		{
			bind.direct_sgprs.start_register[i] = i;
			bind.direct_sgprs.sgprs[i].field = 0;
		}
	}
	ShaderCalcBindingIndices(&bind);
	if (bind.push_constant_size != (use_uniform_buffer ? 144u : 16u) ||
	    bind.program_base_offset_dw != (use_uniform_buffer ? 32u : 0u) ||
	    bind.vsharp_uniform_buffer != use_uniform_buffer ||
	    bind.vsharp_binding_index != (use_uniform_buffer ? 0 : -1))
	{
		Fail(std::string(name) + ": unexpected production program-base binding layout");
	}
	return probe;
}

void PrepareProgramBaseProbe(ProgramBaseProbe* probe)
{
	const std::array<uint32_t, 2> shader_words {GetPcB64Word(8), kEndProgram};
	const std::vector<ScalarRegisterSeed> register_seeds;
	Kyty::Core::String8 source;
	Kyty::Core::String8 error;
	if (!Kyty::Libs::Graphics::BuildScalarProbeSource(shader_words.data(), shader_words.size(), register_seeds, 8, 9,
	                                                  probe->input_info, &source, &error, probe->nonempty_exec))
	{
		Fail(std::string(probe->name) + ": source generation failed: " + error.c_str());
	}
	if (!Run(source, &probe->spirv, &error) || probe->spirv.IsEmpty())
	{
		Fail(std::string(probe->name) + ": ShaderToolchain failed validation/assembly: " + error.c_str());
	}
}

int RunCase(const ProbeCase& test_case, const VulkanComputeProbe& vulkan)
{
	const std::array<uint32_t, 4> initial {};
	std::array<uint32_t, 4> actual {};
	std::string message;
	const auto result = vulkan.Dispatch(test_case.spirv.GetDataConst(), test_case.spirv.Size(), initial, &actual, &message);
	if (result == VulkanComputeProbe::Result::Unavailable)
	{
		std::fprintf(stderr, "Vulkan compute capability unavailable: %s\n", message.c_str());
		std::fflush(stderr);
		return 77;
	}
	if (result != VulkanComputeProbe::Result::Success)
	{
		Fail(std::string(test_case.name) + ": Vulkan dispatch failed: " + message);
	}
	if (actual != test_case.expected)
	{
		Fail(std::string(test_case.name) + ": numeric mismatch; got {" + std::to_string(actual[0]) + ", " +
		     std::to_string(actual[1]) + ", " + std::to_string(actual[2]) + ", " + std::to_string(actual[3]) + "}");
	}
	std::printf("%s PASS\n", test_case.name);
	return 0;
}

int RunProgramBaseProbe(const ProgramBaseProbe& probe, const VulkanComputeProbe& vulkan)
{
	constexpr uint64_t kFirstBase  = 0x00000001fffffffcull;
	constexpr uint64_t kSecondBase = 0x1234567800000010ull;
	const std::array<uint32_t, 8> initial {};
	const uint32_t expected_exec_lo = probe.nonempty_exec ? 1u : 0u;
	const std::array<uint32_t, 8> expected_first {0, 2, kSeedScc, expected_exec_lo, kSeedExecHi, 0, 0, 0};
	const std::array<uint32_t, 8> expected_second {0x14, 0x12345678, kSeedScc, expected_exec_lo, kSeedExecHi, 0, 0, 0};
	std::array<uint32_t, 8> first_actual {};
	std::array<uint32_t, 8> second_actual {};
	std::string message;
	const auto result = vulkan.DispatchProgramBasePair(probe.spirv.GetDataConst(), probe.spirv.Size(), initial,
	                                                   probe.input_info, kFirstBase, kSecondBase,
	                                                   &first_actual, &second_actual, &message);
	if (result == VulkanComputeProbe::Result::Unavailable)
	{
		std::fprintf(stderr, "Vulkan compute capability unavailable: %s\n", message.c_str());
		std::fflush(stderr);
		return 77;
	}
	if (result != VulkanComputeProbe::Result::Success)
	{
		Fail(std::string(probe.name) + ": Vulkan dispatch failed: " + message);
	}
	if (first_actual != expected_first || second_actual != expected_second)
	{
		Fail(std::string(probe.name) + ": relocated S_GETPC_B64 numeric results do not match both runtime bases");
	}
	std::printf("%s PASS\n", probe.name);
	return 0;
}

std::vector<ProbeCase> MakeCases()
{
	std::vector<ProbeCase> cases;
	cases.reserve(10);
	const auto bitset_expectation = [](uint32_t result) { return std::array<uint32_t, 4> {result, kSeedScc, kSeedExecLo, kSeedExecHi}; };
	const uint32_t endpgm = kEndProgram;

	cases.push_back({"BitsetInlineZero", {Bitset1Word(9, 128), endpgm}, {{9, 4}}, 9, bitset_expectation(5), {}});
	cases.push_back({"BitsetInline31", {Bitset1Word(17, 159), endpgm}, {{17, 0}}, 17, bitset_expectation(0x80000000u), {}});
	cases.push_back({"BitsetWrapsIndex32", {Bitset1Word(9, 160), endpgm}, {{9, 8}}, 9, bitset_expectation(9), {}});
	cases.push_back({"BitsetAliasedIndexDestination", {Bitset1Word(17, 17), endpgm}, {{17, 31}}, 17,
	                 bitset_expectation(0x8000001fu), {}});

	const auto compare_expectation = [](uint32_t scc) { return std::array<uint32_t, 4> {0, scc, kSeedExecLo, kSeedExecHi}; };
	auto compare_words = [](uint32_t source0, uint32_t source1) {
		return std::vector<uint32_t> {Compare64NotEqualWord(source0, source1), kEndProgram};
	};
	cases.push_back({"Compare64Equal", compare_words(8, 12), {{8, 0x12345678u}, {9, 1}, {12, 0x12345678u}, {13, 1}}, -1,
	                 compare_expectation(0), {}});
	cases.push_back({"Compare64LowWordDiffers", compare_words(8, 12), {{8, 0x12345678u}, {9, 1}, {12, 0x12345679u}, {13, 1}}, -1,
	                 compare_expectation(1), {}});
	cases.push_back({"Compare64HighWordDiffers", compare_words(8, 12), {{8, 0x12345678u}, {9, 1}, {12, 0x12345678u}, {13, 2}}, -1,
	                 compare_expectation(1), {}});
	const auto pack = [](uint32_t dst, uint32_t src0, uint32_t src1) {
		return 0x80000000u | (0x32u << 23u) | (dst << 16u) | (src1 << 8u) | src0;
	};
	const auto move = [](uint32_t dst, uint32_t src) { return (0x17du << 23u) | (dst << 16u) | (3u << 8u) | src; };
	cases.push_back({"PackDistinctLowHalves", {pack(17, 9, 21), endpgm}, {{9, 0x12345678u}, {21, 0x9abcdef0u}}, 17,
	                 bitset_expectation(0xdef05678u), {}});
	cases.push_back({"PackAliasedSourcesAndDestination", {pack(17, 17, 17), endpgm}, {{17, 0x12345678u}}, 17,
	                 bitset_expectation(0x56785678u), {}});
	cases.push_back({"PackVccHiAliasAndZero", {move(107, 9), pack(107, 107, 128), move(17, 107), endpgm}, {{9, 0x12345678u}}, 17,
	                 bitset_expectation(0x5678u), {}});
	return cases;
}

} // namespace

int main()
{
	InitializeConfig();
	std::vector<ProbeCase> cases = MakeCases();
	for (auto& test_case: cases) { PrepareCase(&test_case); }
	ProgramBaseProbe push_constant_probe = MakeProgramBaseProbe("GetPcRelocatedPushConstants", false);
	ProgramBaseProbe uniform_buffer_probe = MakeProgramBaseProbe("GetPcRelocatedUbo", true);
	ProgramBaseProbe active_exec_probe = MakeProgramBaseProbe("GetPcPreservesActiveExec", false);
	active_exec_probe.nonempty_exec = true;
	PrepareProgramBaseProbe(&push_constant_probe);
	PrepareProgramBaseProbe(&uniform_buffer_probe);
	PrepareProgramBaseProbe(&active_exec_probe);

	VulkanComputeProbe vulkan;
	std::string message;
	const auto initialized = vulkan.Initialize(&message);
	if (initialized == VulkanComputeProbe::Result::Unavailable)
	{
		std::fprintf(stderr, "Vulkan compute capability unavailable: %s\n", message.c_str());
		return 77;
	}
	if (initialized != VulkanComputeProbe::Result::Success) { Fail("Vulkan initialization failed: " + message); }

	for (const auto& test_case: cases)
	{
		const int result = RunCase(test_case, vulkan);
		if (result != 0) { return result; }
	}
	for (const ProgramBaseProbe* probe: {&push_constant_probe, &uniform_buffer_probe, &active_exec_probe})
	{
		const int result = RunProgramBaseProbe(*probe, vulkan);
		if (result != 0) { return result; }
	}
	std::fflush(stdout);
	return 0;
}
