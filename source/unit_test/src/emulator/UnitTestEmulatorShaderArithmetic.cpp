#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderNggFrontBody.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderArithmeticTestInterpreter.h"

#include <cfenv>
#include <cinttypes>
#include <limits>
#include <set>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

UT_BEGIN(EmulatorShaderArithmetic);

using namespace Libs::Graphics;
using namespace ShaderArithmeticTest;

namespace {

constexpr uint32_t kEnd = 0xbf810000u;
#if defined(_WIN32)
constexpr int kRejectedExit = 321;
#else
constexpr int kRejectedExit = 65;
#endif

void Initialize()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation;
	Config::Load(validation);
	std::fesetround(FE_TONEAREST);
}

std::vector<uint32_t> Unary(uint32_t opcode, bool e64, uint32_t source = 256u, uint32_t controls = 0u,
                            uint32_t modifiers = 0u)
{
	if (e64) { return {(0x35u << 26u) | ((opcode + 0x180u) << 16u) | 16u | controls, source | modifiers, kEnd}; }
	return {(0x3fu << 25u) | (16u << 17u) | (opcode << 9u) | source, kEnd};
}

std::vector<uint32_t> Binary(uint32_t opcode, bool e64)
{
	if (e64) { return {(0x35u << 26u) | ((opcode + 0x100u) << 16u) | 16u, 256u | (257u << 9u), kEnd}; }
	return {(opcode << 25u) | (16u << 17u) | (1u << 9u) | 256u, kEnd};
}

uint32_t Sop2Word(uint32_t opcode, uint32_t destination, uint32_t source0, uint32_t source1)
{
	return 0x80000000u | (opcode << 23u) | (destination << 16u) | (source1 << 8u) | source0;
}

std::vector<uint32_t> Ternary(uint32_t opcode, uint32_t op_sel = 0u, uint32_t destination = 16u)
{
	return {(0x35u << 26u) | (opcode << 16u) | (op_sel << 11u) | destination,
	        256u | (257u << 9u) | (258u << 18u), kEnd};
}

ShaderCode Parse(const std::vector<uint32_t>& words, ShaderType stage = ShaderType::Pixel, uint32_t instruction_count = 2u)
{
	ShaderCode code;
	code.SetType(stage);
	if (!ShaderTryParseBounded(words.data(), words.size() * sizeof(uint32_t), &code) ||
	    code.GetInstructions().Size() != instruction_count)
	{
		Fail("production parser rejected synthetic arithmetic");
	}
	return code;
}

enum class Fp16Overflow { Disabled, Enabled, UnknownDisabled, UnknownEnabled };

void CheckDeclaredModuleIds(const ShaderCode& code, const Core::String8& source)
{
	// Check the actual production names before the toolchain's friendly-name
	// disassembly can hide an orphan constant behind a numeric ID. Inspect all
	// operands, including unselected OpSelect arms, without inventing constants
	// for the CPU evaluator. Forward references remain legal.
	std::set<std::string> definitions;
	std::set<std::string> references;
	std::istringstream input(source.c_str());
	std::string line;
	while (std::getline(input, line))
	{
		std::istringstream words(line.substr(0, line.find(';')));
		std::vector<std::string> tokens;
		std::string token;
		while (words >> token) { tokens.push_back(token); }
		const bool definition = tokens.size() > 1u && tokens[1] == "=";
		for (size_t index = 0; index < tokens.size(); ++index)
		{
			if (tokens[index][0] == '%')
			{
				if (definition && index == 0u) { definitions.insert(tokens[index]); }
				else { references.insert(tokens[index]); }
			}
		}
	}
	for (const auto& reference: references)
	{
		if (definitions.count(reference) == 0u)
		{
			const auto& inst = code.GetInstructions().At(0);
			Fail(Core::String8::FromPrintf("undefined module id %s: stage=%u instruction=%u pc=0x%08" PRIx32,
			                              reference.c_str(), static_cast<unsigned>(code.GetType()),
			                              static_cast<unsigned>(inst.type), inst.pc).c_str());
		}
	}
}

Core::String8 Emit(const ShaderCode& code, uint8_t float_mode = 0xc0u, bool ieee = false, bool dx10 = false, bool validate = false,
                   Fp16Overflow overflow = Fp16Overflow::Disabled)
{
	// These are explicit synthetic initial controls, including known-disabled
	// FP16_OVFL for the original fixtures. Production defaults remain unknown.
	const bool overflow_value = overflow == Fp16Overflow::Enabled || overflow == Fp16Overflow::UnknownEnabled;
	const bool overflow_known = overflow == Fp16Overflow::Disabled || overflow == Fp16Overflow::Enabled;
	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;
	input.float_mode = float_mode;
	input.ieee_mode = ieee;
	input.dx10_clamp = dx10;
	input.fp16_overflow = overflow_value;
	input.fp16_overflow_known = overflow_known;
	ShaderComputeInputInfo compute {};
	compute.float_mode = float_mode;
	compute.ieee_mode = ieee;
	compute.dx10_clamp = dx10;
	compute.fp_mode_known = true;
	compute.fp16_overflow = overflow_value;
	compute.fp16_overflow_known = overflow_known;
	compute.threads_num[0] = compute.threads_num[1] = compute.threads_num[2] = 1;
	compute.wave_layout = {ShaderComputeWaveStrategy::Native, {1, 1, 1}, {1, 1, 1}, 32, 32, 1, 1, 0};
	const bool cs = code.GetType() == ShaderType::Compute;
	const auto source = SpirvGenerateSource(code, nullptr, cs ? nullptr : &input, cs ? &compute : nullptr);
	if (source.IsEmpty()) { Fail("empty production shader"); }
	CheckDeclaredModuleIds(code, source);
	if (validate)
	{
		Vector<uint32_t> binary;
		Core::String8 error;
		if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty()) { Fail(error.c_str()); }
	}
	return source;
}

uint32_t Evaluate(const Program& program, uint32_t a, uint32_t b = 0u, uint32_t c = 0u, uint32_t old = 0xa5a51234u,
                  uint32_t exec = 1u, const char* destination = "%v16")
{
	// This is one native lane after strategy adaptation. Architectural EXEC
	// is packed; the resolved implicit ALU predicate is its per-lane bit.
	return program.Run({{"%v0", a}, {"%v1", b}, {"%v2", c}, {"%v16", old},
	                    {"%exec_lo", exec != 0u ? UINT32_MAX : 0u}, {"%exec_hi", 0u},
	                    {"%exec_lane_lo", exec}, {"%exec_lane_hi", 0u}, {"%scc", 0u}},
	                   destination);
}

uint32_t EvaluateScalar(const Program& program, uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3, uint32_t scc_in,
	                       const char* destination, uint32_t* scc_out)
{
	return program.Run({{"%s0", s0}, {"%s1", s1}, {"%s2", s2}, {"%s3", s3}, {"%scc", scc_in},
	                    {"%exec_lo", UINT32_MAX}, {"%exec_hi", 0u}, {"%exec_lane_lo", 1u}, {"%exec_lane_hi", 0u}},
	                   destination, scc_out);
}

uint32_t ArithmeticRight32(uint32_t value, uint32_t shift)
{
	if (shift == 0u) { return value; }
	const uint32_t sign_extension = (value & 0x80000000u) != 0u ? UINT32_MAX << (32u - shift) : 0u;
	return (value >> shift) | sign_extension;
}

std::array<uint32_t, 2> ArithmeticRight64Words(uint32_t low, uint32_t high, uint32_t shift)
{
	shift &= 63u;
	if (shift == 0u) { return {low, high}; }
	if (shift < 32u)
	{
		return {(low >> shift) | (high << (32u - shift)), ArithmeticRight32(high, shift)};
	}
	return {ArithmeticRight32(high, shift - 32u), ArithmeticRight32(high, 31u)};
}

std::array<uint32_t, 2> SignedBfe64Reference(uint32_t low, uint32_t high, uint32_t control)
{
	const uint32_t offset = control & 0x3fu;
	const uint32_t width  = (control >> 16u) & 0x7fu;
	const uint32_t count  = std::min(width, 64u - offset);
	if (count == 0u)
	{
		return {0u, 0u};
	}

	const uint64_t source = (static_cast<uint64_t>(high) << 32u) | low;
	const uint64_t mask   = count == 64u ? UINT64_MAX : ((uint64_t {1} << count) - 1u);
	uint64_t       result = (source >> offset) & mask;
	if ((result & (uint64_t {1} << (count - 1u))) != 0u)
	{
		result |= ~mask;
	}
	return {static_cast<uint32_t>(result), static_cast<uint32_t>(result >> 32u)};
}

[[noreturn]] void RunScalarBfe64Contracts()
{
	Initialize();
	const auto  code        = Parse({Sop2Word(0x2au, 4u, 0u, 2u), kEnd});
	const auto& instruction = code.GetInstructions().At(0);
	if (instruction.type != ShaderInstructionType::SBfeI64 || instruction.format != ShaderInstructionFormat::Sdst2Ssrc02Ssrc1 ||
	    instruction.src_num != 2 || instruction.dst.size != 2 || instruction.src[0].size != 2 || instruction.src[1].size != 1)
	{
		Fail("S_BFE_I64 parser pair contract differs");
	}
	if (!ShaderInstructionLoweringPreconditions(instruction))
	{
		Fail("aligned S_BFE_I64 operands must pass lowering preconditions");
	}

	const auto source = Emit(code, 0xc0u, false, false, true);
	if (source.FindIndex("OpCapability Int64") != Core::STRING8_INVALID_INDEX ||
	    source.FindIndex("OpTypeInt 64") != Core::STRING8_INVALID_INDEX)
	{
		Fail("S_BFE_I64 instruction module must not require SPIR-V Int64");
	}
	const Program program(source.c_str());
	const auto    check_result =
	    [&](uint32_t low, uint32_t high, uint32_t offset, uint32_t width, const char* low_name, const char* high_name, const char* label)
	{
		const uint32_t control      = offset | (width << 16u);
		const auto     expected     = SignedBfe64Reference(low, high, control);
		const uint32_t expected_scc = (expected[0] | expected[1]) != 0u ? 1u : 0u;
		uint32_t       low_scc      = UINT32_MAX;
		uint32_t       high_scc     = UINT32_MAX;
		Equal(EvaluateScalar(program, low, high, control, 0u, 1u, low_name, &low_scc), expected[0], label);
		Equal(EvaluateScalar(program, low, high, control, 0u, 1u, high_name, &high_scc), expected[1], label);
		Equal(low_scc, expected_scc, label);
		Equal(high_scc, expected_scc, label);
	};

	// The offset/width cross product covers zero width and extraction fields that
	// cross each dword boundary, including the 63/64/65-bit overrun edges.
	constexpr std::array<std::array<uint32_t, 2>, 5> sources {{
	    {{0x89abcdefu, 0x01234567u}},
	    {{0x76543210u, 0xfedcba98u}},
	    {{0u, 0x80000001u}},
	    {{0x80000001u, 0u}},
	    {{0u, 0u}},
	}};
	for (const auto& words: sources)
	{
		for (uint32_t offset = 0; offset < 64u; ++offset)
		{
			for (uint32_t width = 0; width < 128u; ++width)
			{
				check_result(words[0], words[1], offset, width, "%s4", "%s5", "S_BFE_I64 offset/width");
			}
		}
	}
	check_result(0u, 1u, 0u, 64u, "%s4", "%s5", "S_BFE_I64 SCC observes nonzero high-only result");

	// Both source pairs are loaded before either aliased destination word is stored.
	for (const uint32_t destination: {0u, 2u})
	{
		const auto alias_code        = Parse({Sop2Word(0x2au, destination, 0u, 2u), kEnd});
		const auto alias_instruction = alias_code.GetInstructions().At(0);
		if (!ShaderInstructionLoweringPreconditions(alias_instruction))
		{
			Fail("even aliased S_BFE_I64 pairs must pass lowering preconditions");
		}
		const Program  alias_program(Emit(alias_code, 0xc0u, false, false, true).c_str());
		const auto     low_name     = Core::String8::FromPrintf("%%s%u", destination);
		const auto     high_name    = Core::String8::FromPrintf("%%s%u", destination + 1u);
		const auto     expected     = SignedBfe64Reference(0x80000001u, 0x80000000u, (17u << 16u) | 57u);
		const uint32_t expected_scc = (expected[0] | expected[1]) != 0u ? 1u : 0u;
		uint32_t       low_scc      = UINT32_MAX;
		uint32_t       high_scc     = UINT32_MAX;
		Equal(EvaluateScalar(alias_program, 0x80000001u, 0x80000000u, (17u << 16u) | 57u, 0u, 1u, low_name.c_str(), &low_scc), expected[0],
		      "S_BFE_I64 source/destination alias low");
		Equal(EvaluateScalar(alias_program, 0x80000001u, 0x80000000u, (17u << 16u) | 57u, 0u, 1u, high_name.c_str(), &high_scc),
		      expected[1], "S_BFE_I64 source/destination alias high");
		Equal(low_scc, expected_scc, "S_BFE_I64 source/destination alias SCC");
		Equal(high_scc, expected_scc, "S_BFE_I64 source/destination alias SCC");
	}

	const auto odd_destination = Parse({Sop2Word(0x2au, 1u, 0u, 2u), kEnd});
	if (ShaderInstructionLoweringPreconditions(odd_destination.GetInstructions().At(0)))
	{
		Fail("S_BFE_I64 odd destination pair must remain rejected");
	}
	const auto odd_source = Parse({Sop2Word(0x2au, 4u, 1u, 2u), kEnd});
	if (ShaderInstructionLoweringPreconditions(odd_source.GetInstructions().At(0)))
	{
		Fail("S_BFE_I64 odd source pair must remain rejected");
	}
	std::_Exit(0);
}

[[noreturn]] void RunScalarSop2Contracts()
{
	Initialize();

	const auto check_subb = [](uint32_t destination, uint32_t source0, uint32_t source1, uint32_t old_scc,
	                           uint32_t expected_result, uint32_t expected_scc, const char* label) {
		auto code = Parse({Sop2Word(0x05u, destination, 0u, 1u), kEnd});
		const auto& instruction = code.GetInstructions().At(0);
		if (instruction.type != ShaderInstructionType::SSubbU32 || instruction.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 ||
		    instruction.src_num != 2 || instruction.dst.size != 1 || instruction.src[0].size != 1 || instruction.src[1].size != 1)
		{
			Fail("S_SUBB_U32 parser contract differs");
		}
		const Program program(Emit(code, 0xc0u, false, false, true).c_str());
		uint32_t actual_scc = UINT32_MAX;
		const uint32_t actual = EvaluateScalar(program, source0, source1, 0u, 0u, old_scc,
		                                      Core::String8::FromPrintf("%%s%u", destination).GetDataConst(), &actual_scc);
		Equal(actual, expected_result, label);
		Equal(actual_scc, expected_scc, label);
	};
	check_subb(3u, 5u, 3u, 0u, 2u, 0u, "S_SUBB_U32 no borrow");
	check_subb(3u, 2u, 3u, 0u, UINT32_MAX, 1u, "S_SUBB_U32 source borrow");
	check_subb(3u, 5u, 5u, 1u, UINT32_MAX, 1u, "S_SUBB_U32 equal sources with borrow-in");
	check_subb(3u, UINT32_MAX, UINT32_MAX, 1u, UINT32_MAX, 1u,
	           "S_SUBB_U32 UINT32_MAX plus borrow-in does not wrap the borrow test");
	{
		auto code = Parse({Sop2Word(0x05u, 0u, 0u, 1u), kEnd});
		const Program program(Emit(code, 0xc0u, false, false, true).c_str());
		uint32_t actual_scc = 0;
		Equal(EvaluateScalar(program, 0u, 0u, 0u, 0u, 1u, "%s0", &actual_scc), UINT32_MAX,
		      "S_SUBB_U32 exact source/destination alias");
		Equal(actual_scc, 1u, "S_SUBB_U32 exact alias SCC");
	}
	{
		auto code = Parse({Sop2Word(0x05u, 1u, 0u, 1u), kEnd});
		const Program program(Emit(code, 0xc0u, false, false, true).c_str());
		uint32_t actual_scc = 0;
		Equal(EvaluateScalar(program, 7u, 5u, 0u, 0u, 0u, "%s1", &actual_scc), 2u,
		      "S_SUBB_U32 source1/destination alias");
		Equal(actual_scc, 0u, "S_SUBB_U32 source1/destination alias SCC");
	}

	constexpr std::array<uint32_t, 6> shifts {0u, 1u, 31u, 32u, 33u, 63u};
	constexpr std::array<uint32_t, 2> high_words {0x01234567u, 0x81234567u};
	for (const uint32_t high: high_words)
	{
		constexpr uint32_t low = 0x89abcdefu;
		for (const uint32_t shift: shifts)
		{
			const uint32_t shift_word = 0xffffffc0u | shift;
			const auto expected = ArithmeticRight64Words(low, high, shift_word);
			const uint32_t expected_scc = (expected[0] | expected[1]) != 0u ? 1u : 0u;
			for (const uint32_t destination: {0u, 2u})
			{
				auto code = Parse({Sop2Word(0x23u, destination, 0u, 2u), kEnd});
				const auto& instruction = code.GetInstructions().At(0);
				if (instruction.type != ShaderInstructionType::SAshrI64 || instruction.format != ShaderInstructionFormat::Sdst2Ssrc02Ssrc1 ||
				    instruction.dst.size != 2 || instruction.src[0].size != 2 || instruction.src[1].size != 1)
				{
					Fail("S_ASHR_I64 parser pair contract differs");
				}
				if (!ShaderInstructionLoweringPreconditions(instruction)) { Fail("aligned S_ASHR_I64 operands must pass lowering preconditions"); }
				const Program program(Emit(code, 0xc0u, false, false, true).c_str());
				uint32_t low_scc = UINT32_MAX;
				uint32_t high_scc = UINT32_MAX;
				const auto low_name = Core::String8::FromPrintf("%%s%u", destination);
				const auto high_name = Core::String8::FromPrintf("%%s%u", destination + 1u);
				Equal(EvaluateScalar(program, low, high, shift_word, 0u, 1u, low_name.c_str(), &low_scc), expected[0],
				      "S_ASHR_I64 low word");
				Equal(EvaluateScalar(program, low, high, shift_word, 0u, 1u, high_name.c_str(), &high_scc), expected[1],
				      "S_ASHR_I64 high word");
				Equal(low_scc, expected_scc, "S_ASHR_I64 SCC from low-word probe");
				Equal(high_scc, expected_scc, "S_ASHR_I64 SCC from high-word probe");
			}
		}
	}
	{
		auto odd_pair = Parse({Sop2Word(0x23u, 1u, 0u, 2u), kEnd});
		if (ShaderInstructionLoweringPreconditions(odd_pair.GetInstructions().At(0)))
		{
			Fail("S_ASHR_I64 odd SGPR destination pair must remain rejected");
		}
	}
	std::_Exit(0);
}

[[noreturn]] void RunNggFrontRefusalContracts()
{
	Initialize();
	const auto subb = Parse({Sop2Word(0x05u, 3u, 0u, 1u), kEnd}, ShaderType::Vertex);
	const auto subb_proof = ShaderProveNggFrontBodyLaneLocal(subb, 0u, 64u, {});
	if (subb_proof.lane_local || subb_proof.refused_pc != 0u || subb_proof.reason == nullptr ||
	    std::string_view(subb_proof.reason) != "S_SUBB_U32 borrow-in is not modeled by the NGG front proof")
	{
		Fail("NGG front body must explicitly refuse S_SUBB_U32");
	}

	const auto ashr = Parse({Sop2Word(0x23u, 3u, 0u, 2u), kEnd}, ShaderType::Vertex);
	const auto ashr_proof = ShaderProveNggFrontBodyLaneLocal(ashr, 0u, 64u, {});
	if (ashr_proof.lane_local || ashr_proof.refused_pc != 0u || ashr_proof.reason == nullptr ||
	    std::string_view(ashr_proof.reason) != "S_ASHR_I64 pair shifts are not modeled by the NGG front proof")
	{
		Fail("NGG front body must explicitly refuse S_ASHR_I64");
	}
	std::_Exit(0);
}

[[noreturn]] void RunCanonicalModules(ShaderType stage = ShaderType::Pixel)
{
	Initialize();
	struct Case { uint32_t opcode; ShaderInstructionType type; };
	const Case unary[] = {
	    {0x0a, ShaderInstructionType::VCvtF16F32}, {0x0b, ShaderInstructionType::VCvtF32F16},
	    {0x50, ShaderInstructionType::VCvtF16U16}, {0x51, ShaderInstructionType::VCvtF16I16},
	    {0x52, ShaderInstructionType::VCvtU16F16}, {0x53, ShaderInstructionType::VCvtI16F16},
	    {0x55, ShaderInstructionType::VSqrtF16}, {0x57, ShaderInstructionType::VLogF16},
	    {0x58, ShaderInstructionType::VExpF16}, {0x5b, ShaderInstructionType::VFloorF16},
	    {0x5c, ShaderInstructionType::VCeilF16}, {0x5d, ShaderInstructionType::VTruncF16},
	    {0x5e, ShaderInstructionType::VRndneF16}, {0x60, ShaderInstructionType::VSinF16},
	    {0x61, ShaderInstructionType::VCosF16},
	};
	for (const auto& test: unary)
	{
		for (const bool e64: {false, true})
		{
			const auto code = Parse(Unary(test.opcode, e64), stage);
			const auto& inst = code.GetInstructions().At(0);
			if (inst.type != test.type || inst.format != ShaderInstructionFormat::SVdstSVsrc0 || inst.src_num != 1)
			{
				Fail("e32/e64 identity differs");
			}
			const Program program(Emit(code, 0xc0u, false, false, true).c_str());
			// Exercise constant-dependent branches in the same modules that
			// just passed full validation, using literal numerical expectations.
			switch (test.type)
			{
				case ShaderInstructionType::VCvtU16F16:
				case ShaderInstructionType::VCvtI16F16:
					Equal(Evaluate(program, 0x7e01u), 0u, "validated integer conversion uses declared NaN zero");
					break;
				case ShaderInstructionType::VSqrtF16:
					Equal(Evaluate(program, 0x4400u), 0x4000u, "validated sqrt uses declared zero");
					break;
				case ShaderInstructionType::VLogF16:
					Equal(Evaluate(program, 0u), 0xfc00u, "validated log uses declared safe one");
					break;
				case ShaderInstructionType::VExpF16:
					Equal(Evaluate(program, 0xfc00u), 0u, "validated exp uses declared result zero");
					break;
				case ShaderInstructionType::VSinF16:
					Equal(Evaluate(program, 0x3400u), 0x3c00u, "validated sin uses declared half and one");
					break;
				case ShaderInstructionType::VCosF16:
					Equal(Evaluate(program, 0x3800u), 0xbc00u, "validated cos uses declared half and one");
					break;
				default: break;
			}
		}
	}
	for (uint32_t opcode: {0x32u, 0x33u, 0x34u, 0x35u, 0x39u, 0x3au})
	{
		for (const bool e64: {false, true})
		{
			const auto source = Emit(Parse(Binary(opcode, e64), stage), 0xc0u, false, false, true);
			if (opcode == 0x39u || opcode == 0x3au)
			{
				Equal(Evaluate(Program(source.c_str()), 0x8000u, 0u), opcode == 0x39u ? 0u : 0x8000u,
				      "validated min/max uses declared signed-zero comparison");
			}
		}
	}
	for (uint32_t opcode: {0x34bu, 0x351u, 0x354u, 0x357u})
	{
		for (uint32_t op_sel: {0u, 15u}) { (void)Emit(Parse(Ternary(opcode, op_sel), stage), 0xc0u, false, false, true); }
	}
	for (uint32_t round = 0; round < 4u; ++round)
	{
		(void)Emit(Parse(Unary(0x0a, true), stage), static_cast<uint8_t>(round << 2u), false, false, true);
	}
	std::_Exit(0);
}

[[noreturn]] void RunNarrowing(ShaderType stage = ShaderType::Pixel)
{
	Initialize();
	const uint32_t controls[] = {0u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x3f000000u, 0x40000000u,
	                             0x477fe000u, 0x477ff000u, 0x47800000u, 0x7f800000u, 0xff800000u,
	                             1u, 0x80000001u, 0x33000000u, 0x33000001u, 0x33800000u, 0x387fc000u,
	                             0x38800000u, 0x7f7fffffu, 0xff7fffffu};
	for (const bool e64: {false, true})
	{
		const auto code = Parse(Unary(0x0a, e64), stage);
		for (uint32_t round = 0; round < 4u; ++round)
		{
			const Program program(Emit(code, static_cast<uint8_t>((round << 2u) | 0x10u)).c_str());
			for (const auto bits: controls)
			{
				Equal(Evaluate(program, bits), HalfReference(Float(bits), round), "narrow special/boundary");
				Equal(Evaluate(program, bits, 0, 0, 0xdeadbeefu, 0u), 0xdeadbeefu, "inactive narrow");
			}
			// A spread of ties, and the adjacent binary32 values on both
			// sides, covers every half exponent and both mantissa parities.
			for (uint32_t half = 0u; half < 0x7bfeu; half += 127u)
			{
				const float a = Float(HalfFloatBits(half));
				const float b = Float(HalfFloatBits(half + 1u));
				const float midpoint = (a + b) * 0.5f;
				for (float v: {std::nextafter(midpoint, 0.0f), midpoint,
				              std::nextafter(midpoint, std::numeric_limits<float>::infinity())})
				{
					for (float signed_v: {v, -v})
					{
						Equal(Evaluate(program, Bits(signed_v)), HalfReference(signed_v, round), "narrow rounding lattice");
					}
				}
			}
			Equal(Evaluate(program, 0x7f800001u), 0x7e00u, "quiet signaling NaN");
			Equal(Evaluate(program, 0xffc02000u), 0xfe01u, "NaN sign/payload");
			const Program from_signed(Emit(Parse(Unary(0x51u, e64), stage), static_cast<uint8_t>(round << 2u)).c_str());
			const Program from_unsigned(Emit(Parse(Unary(0x50u, e64), stage), static_cast<uint8_t>(round << 2u)).c_str());
			for (uint32_t integer: {0u, 1u, 2049u, 32767u, 0x8000u, 0xffffu})
			{
				const int32_t signed_integer = (integer & 0x8000u) != 0u ? static_cast<int32_t>(integer) - 65536 : static_cast<int32_t>(integer);
				Equal(Evaluate(from_signed, integer | 0xa5a50000u), HalfReference(signed_integer, round), "signed integer half rounding mode");
				Equal(Evaluate(from_unsigned, integer | 0xa5a50000u), HalfReference(integer, round), "unsigned integer half rounding mode");
			}
		}
	}
	// ABS precedes NEG and acts on binary32 before narrowing.
	const Program modified(Emit(Parse(Unary(0x0a, true, 256u, 1u << 8u, 1u << 29u), stage)).c_str());
	Equal(Evaluate(modified, Bits(-2.0f)), 0xc000u, "narrow abs then negate");
	const Program flush_single(Emit(Parse(Unary(0x0a, true), stage), 0x04u).c_str());
	Equal(Evaluate(flush_single, 1u), 0u, "single input denorm flush before directed narrowing");
	Equal(Evaluate(flush_single, 0x33800000u), 1u, "conversion still creates half output denorm");
	const auto clamp_code = Parse(Unary(0x0a, true, 256u, 1u << 15u), stage);
	const Program clamped(Emit(clamp_code, 0x40u, false, true).c_str());
	Equal(Evaluate(clamped, Bits(-2.0f)), 0u, "clamp below zero");
	Equal(Evaluate(clamped, Bits(2.0f)), 0x3c00u, "clamp above one");
	Equal(Evaluate(clamped, 0x7fc00000u), 0u, "DX10 clamp NaN");
	const Program nan_preserved(Emit(clamp_code, 0x40u, false, false).c_str());
	Equal(Evaluate(nan_preserved, 0x7fc02000u), 0x7e01u, "non-DX10 clamp preserves NaN");
	const Program ieee_clamp(Emit(clamp_code, 0x40u, true, true).c_str());
	Equal(Evaluate(ieee_clamp, Bits(-2.0f)), 0u, "IEEE mode keeps the clamp modifier");
	Equal(Evaluate(ieee_clamp, 0x7fc00000u), 0u, "IEEE mode clamp follows DX10_CLAMP for NaN");
	const Program widened(Emit(Parse(Unary(0x0b, true), stage)).c_str());
	for (uint32_t half: {0u, 0x8000u, 1u, 0x3ffu, 0x400u, 0x3c00u, 0x7bffu, 0x7c00u, 0xfc00u})
	{
		Equal(Evaluate(widened, half | 0xa5000000u), HalfFloatBits(half), "widen low half");
	}
	for (uint32_t half = 0u; half < 0x10000u; ++half)
	{
		uint32_t expected = HalfFloatBits(half);
		if ((half & 0x7c00u) == 0x7c00u && (half & 0x3ffu) != 0u) { expected |= 0x00400000u; }
		Equal(Evaluate(widened, half), expected, "exhaustive widening/quiet NaN payload");
	}
	std::_Exit(0);
}

[[noreturn]] void RunHalfValues(ShaderType stage = ShaderType::Pixel)
{
	Initialize();
	struct Case { uint32_t opcode; uint32_t input; uint32_t expected; };
	const Case cases[] = {
	    {0x55, 0x4400, 0x4000}, {0x55, 0x8000, 0x8000},
	    {0x55, 0xbc00, 0xfe00}, {0x55, 0x7c00, 0x7c00},
	    {0x57, 0x3c00, 0}, {0x57, 0x4400, 0x4000},
	    {0x57, 0x8000, 0xfc00}, {0x57, 0x0000, 0xfc00}, {0x57, 0xbc00, 0xfe00}, {0x57, 0x7c00, 0x7c00},
	    {0x58, 0x3c00, 0x4000}, {0x58, 0x4400, 0x4c00},
	    {0x58, 0x7c00, 0x7c00}, {0x58, 0xfc00, 0}, {0x58, 0x7bff, 0x7c00}, {0x58, 0xfbff, 0},
	    {0x60, 0x3400, 0x3c00}, {0x60, 0x3800, 0}, {0x60, 0xb400, 0xbc00},
	    {0x60, 0x8000, 0x8000}, {0x60, 0x7bff, 0}, {0x60, 0xfbff, 0},
	    {0x61, 0x3400, 0}, {0x61, 0x3800, 0xbc00}, {0x61, 0x7bff, 0x3c00},
	    {0x5b, 0xbe00, 0xc000}, {0x5c, 0xbe00, 0xbc00}, {0x5d, 0xbe00, 0xbc00},
	    {0x5e, 0x3e00, 0x4000}, {0x5e, 0x4100, 0x4000},
	    {0x51, 0xffff, 0xbc00}, {0x51, 0x8000, 0xf800}, {0x50, 0xffff, 0x7c00},
	    {0x53, 0xbe00, 0xffff}, {0x53, 0x7c00, 0x7fff}, {0x53, 0xfc00, 0x8000},
	    {0x52, 0xbe00, 0}, {0x52, 0x7c00, 0xffff}, {0x52, 0x7e00, 0},
	};
	for (const bool e64: {false, true})
	{
		for (const auto& test: cases)
		{
			const Program program(Emit(Parse(Unary(test.opcode, e64), stage)).c_str());
			Equal(Evaluate(program, test.input | 0x5a5a0000u), test.expected, "half unary/conversion");
			Equal(Evaluate(program, test.input, 0, 0, 0xffffffffu, 0u), 0xffffffffu, "inactive half unary");
		}
		for (uint32_t opcode: {0x32u, 0x33u, 0x34u, 0x35u})
		{
			const Program program(Emit(Parse(Binary(opcode, e64), stage)).c_str());
			for (uint32_t a: {0u, 0x8000u, 1u, 0x400u, 0x3c01u, 0xbc00u, 0x7bffu})
			{
				for (uint32_t b: {0u, 0x8000u, 0x3555u, 0x3c00u, 0x8001u, 0xfbffu})
				{
					const double x = Float(HalfFloatBits(a));
					const double y = Float(HalfFloatBits(b));
					const double result = opcode == 0x32u ? x + y : (opcode == 0x33u ? x - y : (opcode == 0x34u ? y - x : x * y));
					Equal(Evaluate(program, a | 0x5a5a0000u, b | 0xa5a50000u), HalfReference(result, 0u), "binary half values/upper clear");
				}
			}
		}
	}
	// Precision-dependent inline constants, including 1/(2*pi), and f16
	// ABS/NEG must be interpreted before operating on the float value.
	const Program inline_one(Emit(Parse(Unary(0x58, true, 242u), stage)).c_str());
	Equal(Evaluate(inline_one, 0u), 0x4000u, "inline one is a half float");
	const Program inline_inv_2pi(Emit(Parse(Unary(0x0b, true, 248u), stage)).c_str());
	Equal(Evaluate(inline_inv_2pi, 0u), 0x3e230000u, "inline inverse two pi uses half precision");
	const Program neg_abs(Emit(Parse(Unary(0x5b, true, 256u, 1u << 8u, 1u << 29u), stage)).c_str());
	Equal(Evaluate(neg_abs, 0xbe00u), 0xc000u, "half abs then negate");
	// SDWA v_cvt_f32_f16 with a DWORD destination: SRC0_SEL WORD_1 reads bits
	// 31:16, WORD_0 bits 15:0, and SDWA NEG applies to the selected half.
	const uint32_t sdwa_widen = (0x3fu << 25u) | (16u << 17u) | (0x0bu << 9u) | 249u;
	const Program word1(Emit(Parse({sdwa_widen, (6u << 8u) | (5u << 16u), kEnd}, stage)).c_str());
	Equal(Evaluate(word1, 0x3c005a5au), 0x3f800000u, "sdwa high-word widening");
	const Program word0(Emit(Parse({sdwa_widen, (6u << 8u) | (4u << 16u) | (1u << 20u), kEnd}, stage)).c_str());
	Equal(Evaluate(word0, 0x5a5a4000u), 0xc0000000u, "sdwa negated low-word widening");
	for (uint32_t denorm = 0u; denorm < 4u; ++denorm)
	{
		const Program program(Emit(Parse(Binary(0x35u, false), stage), static_cast<uint8_t>(denorm << 6u)).c_str());
		Equal(Evaluate(program, 1u, 0x3c00u), denorm == 3u ? 1u : 0u, "input/output half denorm control");
		Equal(Evaluate(program, 0x400u, 0x3800u), (denorm & 2u) != 0u ? 0x200u : 0u, "half output underflow");
	}
	std::_Exit(0);
}

[[noreturn]] void RunTernaryAndMinMax(ShaderType stage = ShaderType::Pixel)
{
	Initialize();
	const Program min(Emit(Parse(Binary(0x3au, false), stage)).c_str());
	const Program max(Emit(Parse(Binary(0x39u, true), stage)).c_str());
	Equal(Evaluate(min, 0x8000u, 0u), 0x8000u, "min signed zero");
	Equal(Evaluate(max, 0x8000u, 0u), 0u, "max signed zero");
	Equal(Evaluate(min, 0x7e00u, 0x3c00u), 0x3c00u, "min ignores one NaN");
	const Program median(Emit(Parse(Ternary(0x357u), stage)).c_str());
	Equal(Evaluate(median, 0x4400u, 0x3c00u, 0x4000u), 0xa5a54000u, "median sorts all inputs");
	Equal(Evaluate(median, 0x7e00u, 0x3c00u, 0x4000u), 0xa5a53c00u, "median NaN uses minimum");
	const Program high(Emit(Parse(Ternary(0x351u, 15u, 0u), stage)).c_str());
	Equal(Evaluate(high, 0x44001234u, 0x3c00abcdu, 0x40009876u, 0u, 1u, "%v0"), 0x3c001234u,
	      "source/destination high half with alias");
	const Program fma(Emit(Parse(Ternary(0x34bu), stage)).c_str());
	// 1.5 * (1 + 2^-10) - 2^-24 lies just below a half midpoint.
	// Narrowing the rounded binary32 FMA alone incorrectly yields 0x3e02.
	Equal(Evaluate(fma, 0x3e00u, 0x3c01u, 0x8001u), 0xa5a53e01u, "fused half avoids double rounding");
	for (uint32_t a: {1u, 0x400u, 0x3555u, 0x3c01u, 0x3e00u, 0x4bffu, 0x7bffu})
	{
		for (uint32_t b: {0x3801u, 0x3c01u, 0x4000u, 0x7bffu})
		{
			for (uint32_t c: {1u, 0x1000u, 0x3bffu, 0x8001u, 0xbc00u, 0xfbffu})
			{
				const double exact = static_cast<double>(Float(HalfFloatBits(a))) * Float(HalfFloatBits(b)) + Float(HalfFloatBits(c));
				Equal(Evaluate(fma, a, b, c), 0xa5a50000u | HalfReference(exact, 0u), "fused half value");
			}
		}
	}
	std::_Exit(0);
}

[[noreturn]] void RunOverflow(ShaderType stage)
{
	Initialize();
	for (const auto overflow: {Fp16Overflow::Disabled, Fp16Overflow::Enabled})
	{
		const bool enabled = overflow == Fp16Overflow::Enabled;
		const uint32_t large = enabled ? 0x7bffu : 0x7c00u;
		for (const bool e64: {false, true})
		{
			for (uint32_t round = 0; round < 4u; ++round)
			{
				const uint8_t mode = 0xd0u | (round << 2u);
				const Program narrow(Emit(Parse(Unary(0x0au, e64), stage), mode, false, false, true, overflow).c_str());
				for (uint32_t magnitude: {0u, 1u, 0x33800000u, 0x3f800000u, 0x477fdfffu, 0x477fe000u,
				                          0x477fe001u, 0x477fefffu, 0x477ff000u, 0x477ff001u, 0x477fffffu,
				                          0x47800000u, 0x47800001u, 0x7f7fffffu, 0x7f800000u})
				{
					for (uint32_t sign: {0u, 0x80000000u})
					{
						const uint32_t bits = sign | magnitude;
						Equal(Evaluate(narrow, bits), HalfReference(Float(bits), round, false, enabled), "overflow mode/round/sign");
						Equal(Evaluate(narrow, bits, 0u, 0u, 0xdeadbeefu, 0u), 0xdeadbeefu, "inactive overflow conversion");
					}
				}
				Equal(Evaluate(narrow, 0x7f800001u), 0x7e00u, "overflow mode preserves quieted NaN");
				Equal(Evaluate(narrow, 0xffc02000u), 0xfe01u, "overflow mode preserves NaN sign/payload");
				// Literal expected values independently pin both kinds of infinity:
				// finite overflow is saturated; an actual input INF is retained.
				if (enabled)
				{
					Equal(Evaluate(narrow, 0x47800000u), 0x7bffu, "finite positive overflow saturates in every round mode");
					Equal(Evaluate(narrow, 0xc7800000u), 0xfbffu, "finite negative overflow saturates in every round mode");
				}
				Equal(Evaluate(narrow, 0x7f800000u), 0x7c00u, "true positive INF is not saturated");
				Equal(Evaluate(narrow, 0xff800000u), 0xfc00u, "true negative INF is not saturated");
				const Program from_unsigned(Emit(Parse(Unary(0x50u, e64), stage), mode, false, false, true, overflow).c_str());
				const Program from_signed(Emit(Parse(Unary(0x51u, e64), stage), mode, false, false, true, overflow).c_str());
				for (uint32_t integer: {0u, 1u, 2049u, 32767u, 0x8000u, 65503u, 65504u, 65505u, 65519u, 65520u, 65535u})
				{
					const int32_t signed_integer = (integer & 0x8000u) != 0u ? static_cast<int32_t>(integer) - 65536 : static_cast<int32_t>(integer);
					Equal(Evaluate(from_unsigned, 0xa5a50000u | integer), HalfReference(integer, round, false, enabled), "unsigned overflow conversion");
					Equal(Evaluate(from_signed, 0xa5a50000u | integer), HalfReference(signed_integer, round, false, enabled), "signed overflow control");
				}
			}
			for (uint32_t opcode: {0x32u, 0x33u, 0x34u, 0x35u, 0x39u, 0x3au})
			{
				const Program binary(Emit(Parse(Binary(opcode, e64), stage), 0xc0u, false, false, true, overflow).c_str());
				const uint32_t pairs[][2] = {{0x7bffu, 0x7bffu}, {0xfbffu, 0xfbffu}, {0x7bffu, 0xfbffu}, {0xfbffu, 0x7bffu},
				                              {0x7bffu, 0x4000u}, {0xfbffu, 0x4000u}, {0x7c00u, 0x3c00u}, {0xfc00u, 0x3c00u},
				                              {0x3c00u, 0x7c00u}, {0x3c00u, 0xfc00u}};
				for (const auto& pair: pairs)
				{
					const double a = Float(HalfFloatBits(pair[0]));
					const double b = Float(HalfFloatBits(pair[1]));
					double value = 0;
					switch (opcode)
					{
						case 0x32u: value = a + b; break;
						case 0x33u: value = a - b; break;
						case 0x34u: value = b - a; break;
						case 0x35u: value = a * b; break;
						case 0x39u: value = std::max(a, b); break;
						case 0x3au: value = std::min(a, b); break;
					}
					Equal(Evaluate(binary, pair[0], pair[1]), HalfReference(value, 0u, false, enabled), "binary finite overflow/true INF");
				}
			}
			const Program exp(Emit(Parse(Unary(0x58u, e64), stage), 0xc0u, false, false, true, overflow).c_str());
			Equal(Evaluate(exp, 0x4c00u), large, "Exp2 finite 16 overflows half");
			Equal(Evaluate(exp, 0x7bffu), large, "Exp2 finite huge input is not true INF");
			Equal(Evaluate(exp, 0x7c00u), 0x7c00u, "Exp2 true positive INF");
			Equal(Evaluate(exp, 0xfc00u), 0u, "Exp2 negative INF");
			Equal(Evaluate(exp, 0x7e01u), 0x7e01u, "Exp2 NaN payload with overflow mode");
			const Program log(Emit(Parse(Unary(0x57u, e64), stage), 0xc0u, false, false, true, overflow).c_str());
			Equal(Evaluate(log, 0u), 0xfc00u, "Log2 zero generates true negative INF");
			Equal(Evaluate(log, 0x7c00u), 0x7c00u, "Log2 true positive INF");
		}
		for (uint32_t op_sel: {0u, 15u})
		{
			const Program fma(Emit(Parse(Ternary(0x34bu, op_sel), stage), 0xc0u, false, false, true, overflow).c_str());
			const uint32_t triples[][3] = {{0x7bffu, 0x4000u, 0x3c00u}, {0xfbffu, 0x4000u, 0xbc00u},
			                              {0x7bffu, 0x7bffu, 0xfbffu}, {0x3e00u, 0x3c01u, 0x8001u},
			                              {0x7c00u, 0x3c00u, 0x3c00u}, {0xfc00u, 0x3c00u, 0x3c00u},
			                              {0x3c00u, 0x4000u, 0x7c00u}, {0x3c00u, 0x4000u, 0xfc00u}};
			for (const auto& triple: triples)
			{
				const double value = static_cast<double>(Float(HalfFloatBits(triple[0]))) * Float(HalfFloatBits(triple[1])) +
				                     Float(HalfFloatBits(triple[2]));
				const uint32_t half = HalfReference(value, 0u, false, enabled);
				const uint32_t shift = op_sel == 0u ? 0u : 16u;
				const uint32_t preserved = op_sel == 0u ? 0xa5a50000u : 0x1234u;
				Equal(Evaluate(fma, triple[0] << shift, triple[1] << shift, triple[2] << shift), preserved | (half << shift),
				      "FMA overflow/true INF preserves other destination half");
				Equal(Evaluate(fma, triple[0] << shift, triple[1] << shift, triple[2] << shift, 0xdeadbeefu, 0u), 0xdeadbeefu,
				      "inactive overflow FMA");
			}
		}
	}
	// Neither widening nor integer destinations can overflow to an FP16
	// result. Missing FP16_OVFL evidence must not reject these operations.
	for (const auto unknown: {Fp16Overflow::UnknownDisabled, Fp16Overflow::UnknownEnabled})
	{
		const Program widen(Emit(Parse(Unary(0x0bu, true), stage), 0xc0u, false, false, true, unknown).c_str());
		for (uint32_t half: {0u, 1u, 0x8001u, 0x7bffu, 0xfbffu, 0x7c00u, 0xfc00u})
		{
			Equal(Evaluate(widen, half), HalfFloatBits(half), "widening is overflow-mode independent");
		}
		for (uint32_t opcode: {0x52u, 0x53u})
		{
			const Program integer(Emit(Parse(Unary(opcode, true), stage), 0xc0u, false, false, true, unknown).c_str());
			Equal(Evaluate(integer, 0x4000u), 2u, "integer result is overflow-mode independent");
		}
	}
	std::_Exit(0);
}

[[noreturn]] void RunBfe()
{
	Initialize();
	const auto scalar_code = Parse({(0x27u << 23u) | (16u << 16u) | (1u << 8u) | 0x80000000u, kEnd});
	const Program scalar(Emit(scalar_code, 0u, false, false, true).c_str());
	const Program vector(Emit(Parse(Ternary(0x148u)), 0u, false, false, true).c_str());
	const Program signed_vector(Emit(Parse(Ternary(0x149u)), 0u, false, false, true).c_str());
	for (uint32_t data: {0u, 1u, 0x80000000u, 0xffffffffu, 0x12345678u})
	{
		for (uint32_t offset = 0; offset < 64u; ++offset)
		{
			for (uint32_t width = 0; width < 128u; ++width)
			{
				const auto shifted = data >> (offset & 31u);
				const uint64_t scalar_mask = width >= 32u ? UINT32_MAX : ((uint64_t {1} << width) - 1u);
				const uint32_t expected_scalar = shifted & scalar_mask;
				uint32_t scc = 0;
				Equal(scalar.Run({{"%s0", data}, {"%s1", offset | (width << 16u)}, {"%s16", 0u}, {"%scc", 7u}}, "%s16", &scc),
				      expected_scalar, "scalar BFE overrun/width");
				Equal(scc, expected_scalar != 0u ? 1u : 0u, "scalar BFE SCC");
				const uint32_t vector_mask = static_cast<uint32_t>((uint64_t {1} << (width & 31u)) - 1u);
				const uint32_t expected_vector = shifted & vector_mask;
				Equal(Evaluate(vector, data, offset, width), expected_vector, "vector BFE overrun/width");
				const uint32_t count = std::min(width & 31u, 32u - (offset & 31u));
				uint32_t expected_signed = expected_vector;
				if (count != 0u && ((expected_signed >> (count - 1u)) & 1u) != 0u)
				{
					expected_signed |= ~static_cast<uint32_t>((uint64_t {1} << count) - 1u);
				}
				Equal(Evaluate(signed_vector, data, offset, width), expected_signed, "signed BFE overrun/zero width");
			}
		}
	}
	Equal(Evaluate(vector, 0x80000000u, 31u, 2u, 0xdeadbeefu, 0u), 0xdeadbeefu, "inactive BFE");
	std::_Exit(0);
}

[[noreturn]] void RunSdwaHalfDestination(ShaderType stage = ShaderType::Pixel)
{
	Initialize();
	const auto run = [](const Program& program, uint32_t exec_lane)
	{
		return program.Run({{"%v0", 0x3c004000u}, {"%v3", 0x38004000u}, {"%v8", 0xdeadbeefu},
		                    {"%exec_lo", exec_lane != 0u ? UINT32_MAX : 0u}, {"%exec_hi", 0u},
		                    {"%exec_lane_lo", exec_lane}, {"%exec_lane_hi", 0u}},
		                   "%v8");
	};
	// V_MIN_F16 v8, v3, v0 as the hub encodes it: both sources read WORD_1,
	// the result fills WORD_0 and DST_U=2 preserves WORD_1.
	const Program preserve(Emit(Parse({0x741000f9u, 0x05051403u, kEnd}, stage)).c_str());
	Equal(run(preserve, 1u), 0xdead3800u, "sdwa half destination preserves the other half");
	Equal(run(preserve, 0u), 0xdeadbeefu, "inactive sdwa half destination");
	// DST_U=0 pads the word DST_SEL did not select with zeros.
	const Program zero(Emit(Parse({0x741000f9u, 0x05051403u & ~(3u << 11u), kEnd}, stage)).c_str());
	Equal(run(zero, 1u), 0x00003800u, "sdwa half destination zeroes the other half");
	// DST_SEL=WORD_1 writes the high half; DST_U=2 preserves the low half.
	const Program high(Emit(Parse({0x741000f9u, (0x05051403u & ~(7u << 8u)) | (5u << 8u), kEnd}, stage)).c_str());
	Equal(run(high, 1u), 0x3800beefu, "sdwa high half destination preserves the low half");
	std::_Exit(0);
}

[[noreturn]] void RunHalfCompareMaskDestination()
{
	Initialize();
	// V_CMP_NEQ_F16 with an SDWA destination names one mask dword (vcc_hi in
	// the hub's program); a plain compare names the VCC pair.
	const auto emit = [](ShaderOperandType dst_type, int dst_size)
	{
		ShaderCode code;
		code.SetType(ShaderType::Pixel);
		ShaderInstruction inst {};
		inst.pc                 = 0;
		inst.type               = ShaderInstructionType::VCmpNeqF16;
		inst.format             = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
		inst.src_num            = 2;
		inst.src[0].type        = ShaderOperandType::IntegerInlineConstant;
		inst.src[0].constant.u  = 0u;
		inst.src[1].type        = ShaderOperandType::Vgpr;
		inst.src[1].register_id = 15;
		inst.src[1].size        = 1;
		inst.dst.type           = dst_type;
		inst.dst.size           = dst_size;
		inst.dst.register_id    = 0;
		code.GetInstructions().Add(inst);
		ShaderInstruction end {};
		end.type = ShaderInstructionType::SEndpgm;
		end.format = ShaderInstructionFormat::Empty;
		code.GetInstructions().Add(end);
		return Emit(code, 0xc0u, false, false, true);
	};
	const auto stores = [](const Core::String8& source, const char* text)
	{
		return source.FindIndex(text) != Core::STRING8_INVALID_INDEX;
	};
	const auto single = emit(ShaderOperandType::VccHi, 1);
	if (!stores(single, "OpStore %vcc_hi %mask_out_lo_0") || stores(single, "OpStore %vcc_lo %mask_out_lo_0"))
	{
		Fail("a compare that names one mask word must not write the other");
	}
	const auto pair = emit(ShaderOperandType::VccLo, 2);
	if (!stores(pair, "OpStore %vcc_lo %mask_out_lo_0") || !stores(pair, "OpStore %vcc_hi %mask_out_hi_0"))
	{
		Fail("a two-word compare must write both mask words");
	}
	std::_Exit(0);
}

enum class Rejected
{
	OpSel, Sdwa, Dpp, Omod, Round, RoundDown, RoundZero, Ieee, UnknownComputeMode,
	UnknownOverflowConversion, UnknownOverflowInteger, UnknownOverflowArithmetic, IntClamp, BfeClamp, DenormClamp
};

Core::String8 RejectionDiagnostic(Rejected rejected, ShaderType stage)
{
	if (rejected == Rejected::BfeClamp)
	{
		// This emitter has the generator's missing-emitter diagnostic, rather
		// than an FP16 reason. Require the exact tuple and the printed clamp;
		// lowering-preconditions (including an S_ENDPGM failure) cannot match.
		return Core::String8::FromPrintf(
		    "shader emitter missing: stage=%u instruction=%u format=0x%016" PRIx64
		    " pc=0x00000000 sampled=0/0/0 inst=VBfeI32 .* clamp sopp=0xff raw=0xd5490010 crc=[0-9a-f]{8}\n",
		    static_cast<unsigned>(stage), static_cast<unsigned>(ShaderInstructionType::VBfeI32),
		    static_cast<uint64_t>(ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2));
	}
	const char* reason = nullptr;
	auto type = ShaderInstructionType::VSqrtF16;
	switch (rejected)
	{
		case Rejected::OpSel: reason = "unsupported-opsel"; break;
		case Rejected::Sdwa: reason = "unsupported-sdwa"; break;
		case Rejected::Dpp: reason = "unsupported-dpp"; break;
		case Rejected::Omod: reason = "unsupported-omod"; break;
		case Rejected::Round:
		case Rejected::RoundDown:
		case Rejected::RoundZero: reason = "unsupported-arithmetic-round-mode"; break;
		case Rejected::Ieee: reason = "unsupported-arithmetic-ieee-mode"; break;
		case Rejected::UnknownComputeMode: reason = "unknown-initial-mode"; break;
		case Rejected::UnknownOverflowConversion:
			type = ShaderInstructionType::VCvtF16F32;
			reason = "unknown-fp16-overflow-mode";
			break;
		case Rejected::UnknownOverflowInteger:
			type = ShaderInstructionType::VCvtF16U16;
			reason = "unknown-fp16-overflow-mode";
			break;
		case Rejected::UnknownOverflowArithmetic: reason = "unknown-fp16-overflow-mode"; break;
		case Rejected::IntClamp:
			type = ShaderInstructionType::VCvtI16F16;
			reason = "unsupported-integer-clamp";
			break;
		case Rejected::DenormClamp: reason = "unsupported-output-denorm-clamp"; break;
		default: Fail("missing rejection diagnostic contract");
	}
	return Core::String8::FromPrintf("shader fp16 unsupported: stage=%u instruction=%u pc=0x00000000 reason=%s\n",
	                                 static_cast<unsigned>(stage), static_cast<unsigned>(type), reason);
}

void CaptureRejectionStdout()
{
	// EXIT's detailed diagnostic is written to stdout. Death tests capture
	// stderr, so redirect only in the child; no temporary file is needed.
	std::fflush(stdout);
#if defined(_WIN32)
	if (_dup2(_fileno(stderr), _fileno(stdout)) < 0) { Fail("redirect rejection stdout"); }
#else
	if (dup2(fileno(stderr), fileno(stdout)) < 0) { Fail("redirect rejection stdout"); }
#endif
}

[[noreturn]] void RunRejected(Rejected rejected, ShaderType stage = ShaderType::Pixel, bool overflow_enabled = false)
{
	CaptureRejectionStdout();
	Initialize();
	uint32_t opcode = 0x55u;
	if (rejected == Rejected::IntClamp) { opcode = 0x53u; }
	if (rejected == Rejected::UnknownOverflowConversion) { opcode = 0x0au; }
	if (rejected == Rejected::UnknownOverflowInteger) { opcode = 0x50u; }
	auto code = Parse(rejected == Rejected::BfeClamp ? Ternary(0x149u) : Unary(opcode, true), stage);
	auto& inst = code.GetInstructions()[0];
	uint8_t mode = 0xc0u;
	bool ieee = false;
	Fp16Overflow overflow = overflow_enabled ? Fp16Overflow::Enabled : Fp16Overflow::Disabled;
	switch (rejected)
	{
		case Rejected::OpSel: inst.vop3_op_sel = 1u; break;
		case Rejected::Sdwa:
			inst.vop_sdwa = true;
			inst.vop_sdwa_ctrl = (6u << 8u) | (6u << 16u);
			break;
		case Rejected::Dpp: inst.src[0].dpp = true; break;
		case Rejected::Omod: inst.dst.multiplier = 2.0f; inst.vop3_omod = 1u; break;
		case Rejected::Round: mode = 0xc4u; break;
		case Rejected::RoundDown: mode = 0xc8u; break;
		case Rejected::RoundZero: mode = 0xccu; break;
		case Rejected::Ieee: ieee = true; break;
		case Rejected::IntClamp: inst.dst.clamp = true; break;
		case Rejected::BfeClamp: inst.dst.clamp = true; break;
		case Rejected::DenormClamp: inst.dst.clamp = true; break;
		case Rejected::UnknownOverflowConversion:
		case Rejected::UnknownOverflowInteger:
		case Rejected::UnknownOverflowArithmetic:
			overflow = overflow_enabled ? Fp16Overflow::UnknownEnabled : Fp16Overflow::UnknownDisabled;
			break;
		case Rejected::UnknownComputeMode:
		{
			code.SetType(ShaderType::Compute);
			ShaderComputeInputInfo input {};
			input.float_mode = 0xc0u;
			input.fp_mode_known = false;
			input.fp16_overflow = overflow_enabled;
			input.fp16_overflow_known = true;
			input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
			input.wave_layout = {ShaderComputeWaveStrategy::Native, {1, 1, 1}, {1, 1, 1}, 32, 32, 1, 1, 0};
			(void)SpirvGenerateSource(code, nullptr, nullptr, &input);
			std::_Exit(0);
		}
	}
	(void)Emit(code, mode, ieee, false, false, overflow);
	std::_Exit(0);
}


// VOP3 form of a VOP2 float opcode with the CLAMP bit and the OMOD field (ISA 6.2).
std::vector<uint32_t> ModifiedBinary(uint32_t opcode, bool clamp, uint32_t omod)
{
	return {(0x35u << 26u) | ((opcode + 0x100u) << 16u) | (clamp ? 0x8000u : 0u) | 16u,
	        256u | (257u << 9u) | (omod << 27u), kEnd};
}

ShaderType g_float_mode_stage = ShaderType::Pixel;

Program FloatModeProgram(const std::vector<uint32_t>& words, uint8_t float_mode, bool ieee, bool dx10)
{
	return Program(Emit(Parse(words, g_float_mode_stage), float_mode, ieee, dx10, true).c_str());
}

void IsNan(uint32_t actual, const char* label)
{
	if ((actual & 0x7fffffffu) <= 0x7f800000u) { Fail(std::string(label) + ": expected a NaN, got " + std::to_string(actual)); }
}

// Single-precision MODE contracts from the RDNA2 ISA (3.5 CLAMP/IEEE, 6.2 output
// modifiers, 12.7/12.12 min/max selection). Literal inputs: v0, v1, v2.
// Pixel programs take the initial RSRC1 controls; compute programs their recorded launch controls.
[[noreturn]] void RunFloatModeContracts(ShaderType stage = ShaderType::Pixel)
{
	Initialize();
	g_float_mode_stage = stage;
	constexpr uint32_t kAdd = 3u, kMul = 8u, kMin = 15u, kMax = 16u;
	constexpr uint32_t kMin3 = 0x151u, kMax3 = 0x154u, kMed3 = 0x157u;
	constexpr uint32_t kOne = 0x3f800000u, kTwo = 0x40000000u, kThree = 0x40400000u, kHalf = 0x3f000000u;
	constexpr uint32_t kOneAndHalf = 0x3fc00000u, kMinusThree = 0xc0400000u, kNegZero = 0x80000000u;
	constexpr uint32_t kQuietNan = 0x7fc00000u, kSignalingNan = 0x7f800001u, kQuietedSignaling = 0x7fc00001u;
	constexpr uint32_t kInf = 0x7f800000u;

	// CLAMP saturates in every IEEE mode; DX10_CLAMP alone decides the NaN result.
	{
		const auto words = ModifiedBinary(kAdd, true, 0u);
		const Program ieee_dx10 = FloatModeProgram(words, 0xc0u, true, true);
		Equal(Evaluate(ieee_dx10, kQuietNan, 0u), 0u, "IEEE clamp: NaN becomes +0 with DX10_CLAMP");
		Equal(Evaluate(ieee_dx10, kOneAndHalf, 0u), kOne, "IEEE clamp saturates above one");
		Equal(Evaluate(ieee_dx10, 0xbf800000u, 0u), 0u, "IEEE clamp saturates below zero");
		const Program ieee_pass = FloatModeProgram(words, 0xc0u, true, false);
		IsNan(Evaluate(ieee_pass, kQuietNan, 0u), "IEEE clamp without DX10_CLAMP passes NaN");
		Equal(Evaluate(ieee_pass, kOneAndHalf, 0u), kOne, "IEEE clamp without DX10_CLAMP saturates");
		const Program plain_dx10 = FloatModeProgram(words, 0xc0u, false, true);
		Equal(Evaluate(plain_dx10, kQuietNan, 0u), 0u, "clamp: NaN becomes +0 with DX10_CLAMP");
	}

	// OMOD applies only outside IEEE mode and only when f32 output denormals are flushed.
	{
		const auto mul2 = ModifiedBinary(kMul, false, 1u);
		const Program scale = FloatModeProgram(mul2, 0xc0u, false, true);
		Equal(Evaluate(scale, kOneAndHalf, kOne), kThree, "mul:2 doubles");
		Equal(Evaluate(scale, kNegZero, kOne), 0u, "mul:2 flushes -0 to +0");
		Equal(Evaluate(scale, 0x00400000u, kOne), 0u, "mul:2 flushes a denormal result");
		Equal(Evaluate(scale, 0x7f000000u, kOne), kInf, "mul:2 saturates to +infinity");
		Equal(Evaluate(scale, 0xff000000u, kOne), 0xff800000u, "mul:2 saturates to -infinity");
		Equal(Evaluate(scale, kInf, kOne), kInf, "mul:2 keeps infinity");
		IsNan(Evaluate(scale, kQuietNan, kOne), "mul:2 keeps NaN");
		const Program quad = FloatModeProgram(ModifiedBinary(kMul, false, 2u), 0xc0u, false, true);
		Equal(Evaluate(quad, kOne, kOne), 0x40800000u, "mul:4 quadruples");
		Equal(Evaluate(quad, 0x7e800000u, kOne), kInf, "mul:4 saturates to infinity");
		Equal(Evaluate(quad, 0x7e7fffffu, kOne), 0x7f7fffffu, "mul:4 reaches the largest finite value");
		const Program halve = FloatModeProgram(ModifiedBinary(kMul, false, 3u), 0xc0u, false, true);
		Equal(Evaluate(halve, kOne, kOne), kHalf, "div:2 halves");
		Equal(Evaluate(halve, 0x01000000u, kOne), 0x00800000u, "div:2 reaches the smallest normal");
		Equal(Evaluate(halve, 0x00800000u, kOne), 0u, "div:2 of the smallest normal flushes to +0");
		Equal(Evaluate(halve, 0x80800000u, kOne), 0u, "div:2 underflow of a negative result flushes to +0");
		Equal(Evaluate(FloatModeProgram(mul2, 0xc0u, true, true), kOneAndHalf, kOne), kOneAndHalf, "IEEE mode ignores OMOD");
		Equal(Evaluate(FloatModeProgram(mul2, 0xf0u, false, true), kOneAndHalf, kOne), kOneAndHalf,
		      "enabled f32 output denormals ignore OMOD");
		Equal(Evaluate(FloatModeProgram(mul2, 0xe0u, false, true), kOneAndHalf, kOne), kOneAndHalf,
		      "allowed output denormals ignore OMOD even when inputs flush");
		Equal(Evaluate(FloatModeProgram(mul2, 0xd0u, false, true), kOneAndHalf, kOne), kThree,
		      "flushed output denormals apply OMOD even when inputs are kept");
	}

	// V_MIN/V_MAX: NaN operand selection, signaling NaN quieting in IEEE mode, signed zeros.
	for (const bool ieee: {false, true})
	{
		const Program minimum = FloatModeProgram(ModifiedBinary(kMin, false, 0u), 0xc0u, ieee, true);
		const Program maximum = FloatModeProgram(ModifiedBinary(kMax, false, 0u), 0xc0u, ieee, true);
		const uint32_t signaling_first  = ieee ? kQuietedSignaling : kOne;
		Equal(Evaluate(minimum, kSignalingNan, kOne), signaling_first, "min: signaling NaN first operand");
		Equal(Evaluate(minimum, kOne, kSignalingNan), signaling_first, "min: signaling NaN second operand");
		Equal(Evaluate(maximum, kSignalingNan, kOne), signaling_first, "max: signaling NaN first operand");
		Equal(Evaluate(maximum, kOne, kSignalingNan), signaling_first, "max: signaling NaN second operand");
		Equal(Evaluate(minimum, 0x7fc12345u, kOne), kOne, "min: quiet NaN yields the other operand");
		Equal(Evaluate(maximum, kOne, 0x7fc12345u), kOne, "max: quiet NaN yields the other operand");
		Equal(Evaluate(minimum, 0x7fc12345u, 0xffd54321u), 0xffd54321u, "min: two quiet NaNs yield the second");
		Equal(Evaluate(maximum, 0xffd54321u, 0x7fc12345u), 0x7fc12345u, "max: two quiet NaNs yield the second");
		Equal(Evaluate(minimum, 0x7fc12345u, kSignalingNan), ieee ? kQuietedSignaling : kSignalingNan,
		      "min: a signaling NaN is quieted ahead of a quiet NaN");
		Equal(Evaluate(minimum, 0xff812345u, kSignalingNan), ieee ? 0xffc12345u : kSignalingNan,
		      "min: the first signaling NaN is quieted");
		Equal(Evaluate(minimum, 0u, kNegZero), kNegZero, "min of signed zeros is -0");
		Equal(Evaluate(minimum, kNegZero, 0u), kNegZero, "min of signed zeros is -0 (reversed)");
		Equal(Evaluate(maximum, kNegZero, 0u), 0u, "max of signed zeros is +0");
		Equal(Evaluate(maximum, 0u, kNegZero), 0u, "max of signed zeros is +0 (reversed)");
		Equal(Evaluate(minimum, kOne, kTwo), kOne, "ordered min");
		Equal(Evaluate(maximum, kOne, kTwo), kTwo, "ordered max");

		const Program min3 = FloatModeProgram(Ternary(kMin3), 0xc0u, ieee, true);
		const Program max3 = FloatModeProgram(Ternary(kMax3), 0xc0u, ieee, true);
		const Program med3 = FloatModeProgram(Ternary(kMed3), 0xc0u, ieee, true);
		Equal(Evaluate(min3, kOne, kTwo, kThree), kOne, "min3 ordered");
		Equal(Evaluate(max3, kOne, kTwo, kThree), kThree, "max3 ordered");
		Equal(Evaluate(med3, kOne, kTwo, kThree), kTwo, "med3 ordered");
		Equal(Evaluate(med3, kThree, kOne, kTwo), kTwo, "med3 maximum first");
		Equal(Evaluate(med3, kOne, kThree, kTwo), kTwo, "med3 maximum second");
		Equal(Evaluate(med3, 0u, 0u, kNegZero), 0u, "med3 of zeros");
		Equal(Evaluate(min3, 0u, kNegZero, kSignalingNan), ieee ? kQuietedSignaling : kNegZero, "min3 with a signaling NaN");
		Equal(Evaluate(max3, 0u, kNegZero, kSignalingNan), ieee ? kQuietedSignaling : 0u, "max3 with a signaling NaN");
		Equal(Evaluate(med3, 0u, kNegZero, kSignalingNan), ieee ? kQuietedSignaling : kNegZero, "med3 with a NaN takes min3");
		Equal(Evaluate(med3, kOne, kSignalingNan, kMinusThree), kMinusThree, "med3: quieted NaN then a number");
		Equal(Evaluate(med3, kOne, kMinusThree, kSignalingNan), ieee ? kQuietedSignaling : kMinusThree,
		      "med3: signaling NaN last");
	}
	std::_Exit(0);
}

[[noreturn]] void RunFloatToIntegerConversionContracts(ShaderType stage = ShaderType::Pixel)
{
	Initialize();
	const Program to_unsigned(Emit(Parse(Unary(0x07u, false), stage), 0xc0u, false, false, true).c_str());
	const Program to_signed(Emit(Parse(Unary(0x08u, false), stage), 0xc0u, false, false, true).c_str());

	// RDNA2 V_CVT_*32_F32 truncates toward zero, saturates infinities and
	// finite overflow, and returns zero for NaN. Boundary inputs stay inside
	// the defined domain of SPIR-V's conversion instructions.
	Equal(Evaluate(to_unsigned, Bits(1.9f)), 1u, "unsigned conversion truncates positive values");
	Equal(Evaluate(to_unsigned, Bits(-1.9f)), 0u, "unsigned conversion clamps negative values");
	Equal(Evaluate(to_unsigned, 0x80000000u), 0u, "unsigned conversion maps negative zero to zero");
	Equal(Evaluate(to_unsigned, 0xff800000u), 0u, "unsigned conversion clamps negative infinity");
	Equal(Evaluate(to_unsigned, 0x7f800000u), 0xffffffffu, "unsigned conversion saturates positive infinity");
	Equal(Evaluate(to_unsigned, 0x7fc00000u), 0u, "unsigned conversion maps quiet NaN to zero");
	Equal(Evaluate(to_unsigned, 0x7f800001u), 0u, "unsigned conversion maps signaling NaN to zero");
	Equal(Evaluate(to_unsigned, 0x4f7fffffu), 0xffffff00u, "unsigned conversion preserves the largest in-range f32");
	Equal(Evaluate(to_unsigned, 0x4f800000u), 0xffffffffu, "unsigned conversion saturates at 2^32");
	Equal(Evaluate(to_unsigned, 0x7f800000u, 0u, 0u, 0xdeadbeefu, 0u), 0xdeadbeefu, "inactive unsigned conversion retains the destination");

	Equal(Evaluate(to_signed, Bits(1.9f)), 1u, "signed conversion truncates positive values");
	Equal(Evaluate(to_signed, Bits(-1.9f)), 0xffffffffu, "signed conversion truncates negative values");
	Equal(Evaluate(to_signed, 0xcf000000u), 0x80000000u, "signed conversion preserves -2^31");
	Equal(Evaluate(to_signed, 0xcf000001u), 0x80000000u, "signed conversion saturates below -2^31");
	Equal(Evaluate(to_signed, 0x4effffffu), 0x7fffff80u, "signed conversion preserves the largest in-range f32");
	Equal(Evaluate(to_signed, 0x4f000000u), 0x7fffffffu, "signed conversion saturates at 2^31");
	Equal(Evaluate(to_signed, 0xff800000u), 0x80000000u, "signed conversion saturates negative infinity");
	Equal(Evaluate(to_signed, 0x7f800000u), 0x7fffffffu, "signed conversion saturates positive infinity");
	Equal(Evaluate(to_signed, 0x7fc00000u), 0u, "signed conversion maps quiet NaN to zero");
	Equal(Evaluate(to_signed, 0x7f800001u), 0u, "signed conversion maps signaling NaN to zero");
	Equal(Evaluate(to_signed, 0x7f800000u, 0u, 0u, 0xdeadbeefu, 0u), 0xdeadbeefu, "inactive signed conversion retains the destination");
	std::_Exit(0);
}

} // namespace

TEST(EmulatorShaderArithmetic, FloatModeClampOutputModifierAndMinMaxContracts) { ASSERT_EXIT(RunFloatModeContracts(), ::testing::ExitedWithCode(0), ""); }
TEST(EmulatorShaderArithmetic, ScalarSubBorrowAndSigned64ShiftContracts)
{
	ASSERT_EXIT(RunScalarSop2Contracts(), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, SignedScalarBfe64ClampsSignExtendsAndReportsFullPairScc)
{
	ASSERT_EXIT(RunScalarBfe64Contracts(), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, NggFrontBodyRefusesNewScalarPairOperations)
{
	ASSERT_EXIT(RunNggFrontRefusalContracts(), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, ComputeFloatModeClampOutputModifierAndMinMaxContracts)
{
	ASSERT_EXIT(RunFloatModeContracts(ShaderType::Compute), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, FloatToIntegerConversionsSaturateWithoutUndefinedSpirvConversions)
{
	ASSERT_EXIT(RunFloatToIntegerConversionContracts(), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, ComputeFloatToIntegerConversionsSaturateWithoutUndefinedSpirvConversions)
{
	ASSERT_EXIT(RunFloatToIntegerConversionContracts(ShaderType::Compute), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, CanonicalFp16EncodingsAssembleAndValidate) { ASSERT_EXIT(RunCanonicalModules(), ::testing::ExitedWithCode(0), ""); }
TEST(EmulatorShaderArithmetic, Fp16NarrowingValuesRoundingAndExec) { ASSERT_EXIT(RunNarrowing(), ::testing::ExitedWithCode(0), ""); }
TEST(EmulatorShaderArithmetic, Fp16UnaryAndConversionValues) { ASSERT_EXIT(RunHalfValues(), ::testing::ExitedWithCode(0), ""); }
TEST(EmulatorShaderArithmetic, Fp16TernaryHalfSelectAndFusedValues) { ASSERT_EXIT(RunTernaryAndMinMax(), ::testing::ExitedWithCode(0), ""); }
TEST(EmulatorShaderArithmetic, Fp16SdwaHalfDestinationReadsAndPreservesHalves)
{
	for (const auto stage: {ShaderType::Pixel, ShaderType::Compute})
	{
		ASSERT_EXIT(RunSdwaHalfDestination(stage), ::testing::ExitedWithCode(0), "");
	}
}
TEST(EmulatorShaderArithmetic, HalfCompareWritesOnlyTheMaskWordsTheDestinationNames)
{
	ASSERT_EXIT(RunHalfCompareMaskDestination(), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, BfeDefinedForEveryMaskedOffsetAndWidth) { ASSERT_EXIT(RunBfe(), ::testing::ExitedWithCode(0), ""); }
TEST(EmulatorShaderArithmetic, ComputeFp16EncodingsAssembleAndValidate)
{
	ASSERT_EXIT(RunCanonicalModules(ShaderType::Compute), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, ComputeFp16NarrowingValuesRoundingAndExec)
{
	ASSERT_EXIT(RunNarrowing(ShaderType::Compute), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, ComputeFp16UnaryBinaryDenormAndIntegerValues)
{
	ASSERT_EXIT(RunHalfValues(ShaderType::Compute), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, ComputeFp16TernaryHalfSelectAndFusedValues)
{
	ASSERT_EXIT(RunTernaryAndMinMax(ShaderType::Compute), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderArithmetic, Fp16OverflowModeFiniteAndTrueInfinity)
{
	ASSERT_EXIT(RunOverflow(ShaderType::Pixel), ::testing::ExitedWithCode(0), "");
}
TEST(EmulatorShaderArithmetic, ComputeFp16OverflowModeFiniteAndTrueInfinity)
{
	ASSERT_EXIT(RunOverflow(ShaderType::Compute), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderArithmetic, Fp16RefusesUnmodeledControlDomains)
{
	for (const auto rejected: {Rejected::OpSel, Rejected::Sdwa, Rejected::Dpp, Rejected::Omod, Rejected::Round, Rejected::RoundDown,
	                          Rejected::RoundZero, Rejected::Ieee, Rejected::IntClamp, Rejected::BfeClamp, Rejected::DenormClamp,
	                          Rejected::UnknownOverflowConversion, Rejected::UnknownOverflowInteger, Rejected::UnknownOverflowArithmetic})
	{
		for (const auto stage: {ShaderType::Pixel, ShaderType::Compute})
		{
			const auto diagnostic = RejectionDiagnostic(rejected, stage);
			for (const bool enabled: {false, true})
			{
				ASSERT_EXIT(RunRejected(rejected, stage, enabled), ::testing::ExitedWithCode(kRejectedExit), diagnostic.c_str());
			}
		}
	}
	const auto diagnostic = RejectionDiagnostic(Rejected::UnknownComputeMode, ShaderType::Compute);
	for (const bool enabled: {false, true})
	{
		ASSERT_EXIT(RunRejected(Rejected::UnknownComputeMode, ShaderType::Compute, enabled), ::testing::ExitedWithCode(kRejectedExit), diagnostic.c_str());
	}
}

UT_END();
