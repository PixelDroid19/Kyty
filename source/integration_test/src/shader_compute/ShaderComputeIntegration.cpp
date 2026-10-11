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
	std::string                     name;
	std::vector<uint32_t>           shader_words;
	std::vector<ScalarRegisterSeed> register_seeds;
	int                             result_register = -1;
	std::array<uint32_t, 4>         expected {};
	Kyty::Vector<uint32_t>          spirv;
	bool                            nonempty_exec = false;
	int                             result_vgpr   = -1;
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

uint32_t Sop2Word(uint32_t opcode, uint32_t destination, uint32_t source0, uint32_t source1)
{
	return 0x80000000u | (opcode << 23u) | (destination << 16u) | (source1 << 8u) | source0;
}

uint32_t GetPcB64Word(uint32_t destination)
{
	return (0x17du << 23u) | (destination << 16u) | (0x1fu << 8u);
}

uint32_t Compare64NotEqualWord(uint32_t source0, uint32_t source1)
{
	return 0xbf000000u | (0x13u << 16u) | (source1 << 8u) | source0;
}

uint32_t Vop1Word(uint32_t destination, uint32_t opcode, uint32_t source)
{
	return (0x3fu << 25u) | (destination << 17u) | (opcode << 9u) | source;
}

void PrepareCase(ProbeCase* test_case)
{
	Kyty::Core::String8                          source;
	Kyty::Core::String8                          error;
	Kyty::Libs::Graphics::ShaderComputeInputInfo input_info {};
	input_info.threads_num[0] = 1;
	input_info.threads_num[1] = 1;
	input_info.threads_num[2] = 1;
	const int result_register = test_case->result_vgpr >= 0 ? -1 : test_case->result_register;
	if (!Kyty::Libs::Graphics::BuildScalarProbeSource(test_case->shader_words.data(), test_case->shader_words.size(),
	                                                  test_case->register_seeds, result_register, -1, input_info, &source, &error,
	                                                  test_case->nonempty_exec))
	{
		Fail(std::string(test_case->name) + ": source generation failed: " + error.c_str());
	}
	if (test_case->result_vgpr >= 0)
	{
		std::string       text(source.GetDataConst(), source.Size());
		const std::string anchor   = "OpStore %probe_ptr_0 %uint_0";
		const size_t      position = text.find(anchor);
		if (position == std::string::npos || text.find(anchor, position + anchor.size()) != std::string::npos)
		{
			Fail(std::string(test_case->name) + ": generated source has no unique VGPR probe output");
		}
		const std::string result = Kyty::Core::String8::FromPrintf("%%probe_result_float = OpLoad %%float %%v%d\n"
		                                                           "%%probe_result_bits = OpBitcast %%uint %%probe_result_float\n"
		                                                           "OpStore %%probe_ptr_0 %%probe_result_bits",
		                                                           test_case->result_vgpr)
		                               .GetDataConst();
		text.replace(position, anchor.size(), result);
		source = text.c_str();
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
		constexpr int kDirectSgprs = 32;
		static_assert(kDirectSgprs <= Kyty::Libs::Graphics::ShaderDirectSgprsResources::SGPRS_MAX);
		bind.direct_sgprs.sgprs_num = kDirectSgprs;
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
	std::printf("%s PASS\n", test_case.name.c_str());
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
	cases.reserve(84);
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

	// The first subtraction supplies the borrowed-in state consumed by S_SUBB_U32.
	cases.push_back({"SubbBorrowClearedBySub",
	                 {Sop2Word(0x01u, 9u, 10u, 11u), Sop2Word(0x05u, 8u, 12u, 13u), endpgm},
	                 {{10u, 5u}, {11u, 5u}, {12u, 9u}, {13u, 4u}}, 8,
	                 {5u, 0u, kSeedExecLo, kSeedExecHi}, {}});
	cases.push_back({"SubbBorrowFromSubWithMaxSource",
	                 {Sop2Word(0x01u, 9u, 10u, 11u), Sop2Word(0x05u, 8u, 12u, 13u), endpgm},
	                 {{10u, 2u}, {11u, 3u}, {12u, UINT32_MAX}, {13u, UINT32_MAX}}, 8,
	                 {UINT32_MAX, 1u, kSeedExecLo, kSeedExecHi}, {}});

	// VOP1's VGPR source encoding is 256 + register. A literal V_MOV_B32
	// supplies exact input bits; the test-only SSBO export observes the VGPR.
	const auto add_conversion = [&](const char* name, uint32_t opcode, uint32_t input_bits, uint32_t expected)
	{
		cases.push_back({name,
		                 {Vop1Word(0, 0x01u, 0xffu), input_bits, Vop1Word(1, opcode, 256u), endpgm},
		                 {},
		                 -1,
		                 {expected, kSeedScc, 1u, 0u},
		                 {},
		                 true,
		                 1});
	};
	const auto add_unsigned = [&](const char* name, uint32_t input_bits, uint32_t expected)
	{
		add_conversion(name, 0x07u, input_bits, expected); // v_cvt_u32_f32
	};
	const auto add_signed = [&](const char* name, uint32_t input_bits, uint32_t expected)
	{
		add_conversion(name, 0x08u, input_bits, expected); // v_cvt_i32_f32
	};
	add_unsigned("CvtU32QuietNaN", 0x7fc12345u, 0u);
	add_unsigned("CvtU32SignalingNaN", 0x7f800001u, 0u);
	add_unsigned("CvtU32PositiveInfinity", 0x7f800000u, 0xffffffffu);
	add_unsigned("CvtU32NegativeInfinity", 0xff800000u, 0u);
	add_unsigned("CvtU32LargestInRange", 0x4f7fffffu, 0xffffff00u);
	add_unsigned("CvtU32PositiveOverflow", 0x4f800000u, 0xffffffffu);
	add_unsigned("CvtU32PositiveFraction", 0x40700000u, 3u);
	add_unsigned("CvtU32NegativeFraction", 0xc0700000u, 0u);
	add_signed("CvtI32QuietNaN", 0x7fc12345u, 0u);
	add_signed("CvtI32SignalingNaN", 0x7f800001u, 0u);
	add_signed("CvtI32PositiveInfinity", 0x7f800000u, 0x7fffffffu);
	add_signed("CvtI32NegativeInfinity", 0xff800000u, 0x80000000u);
	add_signed("CvtI32LargestInRange", 0x4effffffu, 0x7fffff80u);
	add_signed("CvtI32PositiveOverflow", 0x4f000000u, 0x7fffffffu);
	add_signed("CvtI32SmallestInRange", 0xcf000000u, 0x80000000u);
	add_signed("CvtI32NegativeOverflow", 0xcf000001u, 0x80000000u);
	add_signed("CvtI32PositiveFraction", 0x40700000u, 3u);
	add_signed("CvtI32NegativeFraction", 0xc0700000u, 0xfffffffdu);

	const auto arithmetic_right32 = [](uint32_t value, uint32_t shift) {
		if (shift == 0u) { return value; }
		const uint32_t extension = (value & 0x80000000u) != 0u ? UINT32_MAX << (32u - shift) : 0u;
		return (value >> shift) | extension;
	};
	const auto arithmetic_right64_words = [&](uint32_t low, uint32_t high, uint32_t shift) {
		shift &= 63u;
		if (shift == 0u) { return std::array<uint32_t, 2> {low, high}; }
		if (shift < 32u)
		{
			return std::array<uint32_t, 2> {(low >> shift) | (high << (32u - shift)), arithmetic_right32(high, shift)};
		}
		return std::array<uint32_t, 2> {arithmetic_right32(high, shift - 32u), arithmetic_right32(high, 31u)};
	};
	const auto add_ashr_probe = [&](const std::string& name, uint32_t destination, uint32_t result_register,
	                                uint32_t low, uint32_t high, uint32_t shift) {
		const auto expected_words = arithmetic_right64_words(low, high, shift);
		const uint32_t expected_scc = (expected_words[0] | expected_words[1]) != 0u ? 1u : 0u;
		cases.push_back({name,
		                 {Sop2Word(0x23u, destination, 20u, 22u), endpgm},
		                 {{20u, low}, {21u, high}, {22u, shift}}, static_cast<int>(result_register),
		                 {result_register == destination ? expected_words[0] : expected_words[1], expected_scc, 0u, 0u}, {}});
	};
	constexpr std::array<uint32_t, 6> shift_counts {0u, 1u, 31u, 32u, 33u, 63u};
	for (const bool negative: {false, true})
	{
		constexpr uint32_t low = 0x89abcdefu;
		const uint32_t high = negative ? 0x81234567u : 0x01234567u;
		const char* sign = negative ? "Negative" : "Positive";
		for (const uint32_t shift: shift_counts)
		{
			const std::string base = std::string("AshrI64") + sign + std::to_string(shift);
			add_ashr_probe(base + "ExactLow", 20u, 20u, low, high, shift);
			add_ashr_probe(base + "ExactHigh", 20u, 21u, low, high, shift);
			if (shift == 1u || shift == 32u || shift == 63u)
			{
				// The even-aligned destination pair s22:s23 overlaps the scalar shift operand s22.
				add_ashr_probe(base + "ShiftSourceAliasLow", 22u, 22u, low, high, shift);
				add_ashr_probe(base + "ShiftSourceAliasHigh", 22u, 23u, low, high, shift);
			}
		}
	}
	// Beyond-end widths use the admitted bit-vector clamp model; this oracle
	// does not establish how PS5 hardware defines those cases.
	const auto signed_bfe_i64_words = [](uint32_t low, uint32_t high, uint32_t control)
	{
		const uint32_t offset          = control & 0x3fu;
		const uint32_t requested_width = (control >> 16u) & 0x7fu;
		const uint32_t remaining_width = 64u - offset;
		const uint32_t width           = requested_width < remaining_width ? requested_width : remaining_width;
		if (width == 0u)
		{
			return std::array<uint32_t, 2> {0u, 0u};
		}

		const uint64_t source     = (static_cast<uint64_t>(high) << 32u) | low;
		const uint64_t shifted    = source >> offset;
		const uint64_t field_mask = width == 64u ? ~uint64_t {0} : (uint64_t {1} << width) - 1u;
		uint64_t       result     = shifted & field_mask;
		if (width < 64u && (result & (uint64_t {1} << (width - 1u))) != 0u)
		{
			result |= ~field_mask;
		}
		return std::array<uint32_t, 2> {static_cast<uint32_t>(result), static_cast<uint32_t>(result >> 32u)};
	};
	const auto add_bfe_i64_probe = [&](const char* name, uint32_t destination, uint32_t source_register, uint32_t result_word,
	                                   uint32_t source_low, uint32_t source_high, uint32_t offset, uint32_t width)
	{
		const uint32_t control      = offset | (width << 16u);
		const auto     result_words = signed_bfe_i64_words(source_low, source_high, control);
		const uint32_t expected_scc = (result_words[0] | result_words[1]) != 0u ? 1u : 0u;
		cases.push_back({name,
		                 {Sop2Word(0x2au, destination, source_register, 24u), endpgm},
		                 {{source_register, source_low}, {source_register + 1u, source_high}, {24u, control}},
		                 static_cast<int>(destination + result_word),
		                 {result_words[result_word], expected_scc, kSeedExecLo, kSeedExecHi},
		                 {}});
	};
	constexpr uint32_t kBfeSourceLow  = 0x89abcdefu;
	constexpr uint32_t kBfeSourceHigh = 0xf1234567u;
	add_bfe_i64_probe("BfeI64WidthZeroLow", 20u, 22u, 0u, kBfeSourceLow, kBfeSourceHigh, 0u, 0u);
	add_bfe_i64_probe("BfeI64WidthZeroHigh", 20u, 22u, 1u, kBfeSourceLow, kBfeSourceHigh, 0u, 0u);
	add_bfe_i64_probe("BfeI64Width64Low", 20u, 22u, 0u, kBfeSourceLow, kBfeSourceHigh, 0u, 64u);
	add_bfe_i64_probe("BfeI64Width64High", 20u, 22u, 1u, kBfeSourceLow, kBfeSourceHigh, 0u, 64u);
	add_bfe_i64_probe("BfeI64Offset31ClampsTo33Low", 20u, 22u, 0u, kBfeSourceLow, kBfeSourceHigh, 31u, 64u);
	add_bfe_i64_probe("BfeI64Offset31ClampsTo33High", 20u, 22u, 1u, kBfeSourceLow, kBfeSourceHigh, 31u, 64u);
	add_bfe_i64_probe("BfeI64Offset32ClampsTo32High", 20u, 22u, 1u, kBfeSourceLow, kBfeSourceHigh, 32u, 64u);
	add_bfe_i64_probe("BfeI64Offset63ClampsToOneLow", 20u, 22u, 0u, kBfeSourceLow, kBfeSourceHigh, 63u, 64u);
	add_bfe_i64_probe("BfeI64Offset63ClampsToOneHigh", 20u, 22u, 1u, kBfeSourceLow, kBfeSourceHigh, 63u, 64u);
	add_bfe_i64_probe("BfeI64HighOnlyResultSetsScc", 20u, 22u, 0u, 0u, 1u, 0u, 64u);
	add_bfe_i64_probe("BfeI64AliasedSourceDestinationLow", 22u, 22u, 0u, kBfeSourceLow, kBfeSourceHigh, 32u, 64u);
	add_bfe_i64_probe("BfeI64AliasedSourceDestinationHigh", 22u, 22u, 1u, kBfeSourceLow, kBfeSourceHigh, 32u, 64u);
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
