#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderArithmeticTestInterpreter.h"

#include <cfenv>
#include <cinttypes>
#include <limits>
#include <set>

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

std::vector<uint32_t> Ternary(uint32_t opcode, uint32_t op_sel = 0u, uint32_t destination = 16u)
{
	return {(0x35u << 26u) | (opcode << 16u) | (op_sel << 11u) | destination,
	        256u | (257u << 9u) | (258u << 18u), kEnd};
}

ShaderCode Parse(const std::vector<uint32_t>& words, ShaderType stage = ShaderType::Pixel)
{
	ShaderCode code;
	code.SetType(stage);
	if (!ShaderTryParseBounded(words.data(), words.size() * sizeof(uint32_t), &code) || code.GetInstructions().Size() != 2u)
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
	Equal(Evaluate(ieee_clamp, Bits(-2.0f)), 0xc000u, "IEEE mode disables float clamp");
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
		    " pc=0x00000000 sampled=0/0/0 inst=VBfeI32 .* clamp sopp=0xff raw=0xd5490010\n",
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

} // namespace

TEST(EmulatorShaderArithmetic, CanonicalFp16EncodingsAssembleAndValidate) { ASSERT_EXIT(RunCanonicalModules(), ::testing::ExitedWithCode(0), ""); }
TEST(EmulatorShaderArithmetic, Fp16NarrowingValuesRoundingAndExec) { ASSERT_EXIT(RunNarrowing(), ::testing::ExitedWithCode(0), ""); }
TEST(EmulatorShaderArithmetic, Fp16UnaryAndConversionValues) { ASSERT_EXIT(RunHalfValues(), ::testing::ExitedWithCode(0), ""); }
TEST(EmulatorShaderArithmetic, Fp16TernaryHalfSelectAndFusedValues) { ASSERT_EXIT(RunTernaryAndMinMax(), ::testing::ExitedWithCode(0), ""); }
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
