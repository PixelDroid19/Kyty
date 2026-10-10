#ifndef KYTY_SHADER_ARITHMETIC_TEST_INTERPRETER_H_
#define KYTY_SHADER_ARITHMETIC_TEST_INTERPRETER_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// CPU contract evaluator for the first straight-line guest instruction of a
// production-generated module. It deliberately does not execute the shader
// prologue or claim device execution. Unknown instructions, absent ids and
// undefined SPIR-V extraction/conversion/shift domains fail the fixture.
namespace ShaderArithmeticTest {

[[noreturn]] inline void Fail(const std::string& message)
{
	std::fprintf(stderr, "arithmetic fixture: %s\n", message.c_str());
	std::_Exit(10);
}

inline uint32_t Bits(float value)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

inline float Float(uint32_t bits)
{
	float value = 0;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

inline uint32_t HalfFloatBits(uint32_t bits)
{
	const auto exponent = (bits >> 10u) & 31u;
	const auto fraction = bits & 1023u;
	const auto sign = (bits & 0x8000u) << 16u;
	if (exponent == 31u) { return sign | 0x7f800000u | (fraction << 13u); }
	const float magnitude = exponent == 0u ? std::ldexp(static_cast<float>(fraction), -24) :
	                                       std::ldexp(static_cast<float>(1024u + fraction), static_cast<int>(exponent) - 25);
	return Bits(magnitude) | sign;
}

// Independent numerical oracle: search the ordered lattice of positive half
// values, then select by distance and the requested direction. No bit-shift
// narrowing algorithm is shared with the production emitter.
inline uint32_t HalfReference(double value, uint32_t round, bool negative_zero = false, bool fp16_overflow = false)
{
	const bool negative = std::signbit(value) || negative_zero;
	const uint32_t sign = negative ? 0x8000u : 0u;
	if (std::isnan(value)) { return sign | 0x7e00u; }
	if (std::isinf(value)) { return sign | 0x7c00u; }
	const double magnitude = std::fabs(value);
	// Overflow saturation bounds only finite values; infinity remains a
	// distinct lattice endpoint even when MODE.FP16_OVFL is enabled.
	if (fp16_overflow && magnitude > 65504.0) { return sign | 0x7bffu; }
	uint32_t low = 0;
	uint32_t high = 0x7bffu;
	while (low < high)
	{
		const auto middle = (low + high + 1u) / 2u;
		if (static_cast<double>(Float(HalfFloatBits(middle))) <= magnitude) { low = middle; }
		else { high = middle - 1u; }
	}
	const double below = Float(HalfFloatBits(low));
	if (below == magnitude) { return sign | low; }
	if (round == 3u || (round == 1u && negative) || (round == 2u && !negative)) { return sign | low; }
	const uint32_t upper = low + 1u;
	if (round != 0u) { return sign | upper; }
	// Infinity's rounding boundary is halfway to the next exponent, 65536.
	const double above = upper == 0x7c00u ? 65536.0 : Float(HalfFloatBits(upper));
	if (magnitude - below < above - magnitude) { return sign | low; }
	if (magnitude - below > above - magnitude) { return sign | upper; }
	return sign | ((low & 1u) == 0u ? low : upper);
}

struct Value
{
	std::array<uint32_t, 4> words {};
	Value() = default;
	explicit Value(uint32_t word) { words[0] = word; }
};

class Program
{
public:
	explicit Program(const char* source)
	{
		std::istringstream input(source);
		std::string line;
		bool body = false;
		while (std::getline(input, line))
		{
			if (line.rfind("; ", 0) == 0 && line.find('[') != std::string::npos)
			{
				if (body) { break; }
				body = true;
				continue;
			}
			line = line.substr(0, line.find(';'));
			std::istringstream parts(line);
			std::vector<std::string> tokens;
			std::string token;
			while (parts >> token) { tokens.push_back(token); }
			if (tokens.empty()) { continue; }
			if (tokens.size() >= 4u && tokens[2] == "OpConstant")
			{
				const auto& literal = tokens.at(4);
				const auto word = tokens[3] == "%float" ? Bits(std::strtof(literal.c_str(), nullptr)) :
				                                        static_cast<uint32_t>(std::strtoll(literal.c_str(), nullptr, 0));
				m_constants[tokens[0]] = Value(word);
			} else if (tokens.size() >= 3u && (tokens[2] == "OpConstantTrue" || tokens[2] == "OpConstantFalse"))
			{
				m_constants[tokens[0]] = Value(tokens[2] == "OpConstantTrue" ? 1u : 0u);
			} else if (body)
			{
				m_body.push_back(tokens);
			}
		}
		if (m_body.empty()) { Fail("no guest instruction body"); }
	}

	uint32_t Run(std::initializer_list<std::pair<const char*, uint32_t>> inputs, const char* destination, uint32_t* scc = nullptr) const
	{
		auto values = m_constants;
		for (const auto& input: inputs) { values[input.first] = Value(input.second); }
		const auto value = [&](const std::string& id) -> Value {
			const auto found = values.find(id);
			if (found == values.end()) { Fail("undefined id " + id); }
			return found->second;
		};
		for (const auto& t: m_body)
		{
			if (t[0] == "OpStore") { values[t.at(1)] = value(t.at(2)); continue; }
			if (t.size() < 4u || t[1] != "=") { Fail("unexpected instruction " + t[0]); }
			const auto& op = t[2];
			const auto arg = [&](size_t i) { return value(t.at(i)).words[0]; };
			const auto fp = [&](size_t i) { return Float(arg(i)); };
			Value result;
			uint32_t& r = result.words[0];
			if (op == "OpLoad" || op == "OpBitcast" || op == "OpCopyObject") { result = value(t.at(4)); }
			else if (op == "OpSelect") { result = value(t.at(arg(4) != 0u ? 5 : 6)); }
			else if (op == "OpBitwiseAnd") { r = arg(4) & arg(5); }
			else if (op == "OpBitwiseOr") { r = arg(4) | arg(5); }
			else if (op == "OpBitwiseXor") { r = arg(4) ^ arg(5); }
			else if (op == "OpNot") { r = ~arg(4); }
			else if (op == "OpIAdd") { r = arg(4) + arg(5); }
			else if (op == "OpISub") { r = arg(4) - arg(5); }
			else if (op == "OpShiftLeftLogical" || op == "OpShiftRightLogical" || op == "OpShiftRightArithmetic")
			{
				const auto shift = arg(5);
				if (shift >= 32u) { Fail("undefined shift " + t[0]); }
				if (op == "OpShiftLeftLogical") { r = arg(4) << shift; }
				else if (op == "OpShiftRightLogical") { r = arg(4) >> shift; }
				else
				{
					r = arg(4) >> shift;
					if ((arg(4) & 0x80000000u) != 0u && shift != 0u) { r |= ~0u << (32u - shift); }
				}
			}
			else if (op == "OpBitFieldUExtract" || op == "OpBitFieldSExtract" || op == "OpBitFieldInsert")
			{
				const bool insert = op == "OpBitFieldInsert";
				const auto offset = arg(insert ? 6 : 5);
				const auto count = arg(insert ? 7 : 6);
				if (offset > 32u || count > 32u || offset + count > 32u) { Fail("undefined extraction " + t[0]); }
				const auto mask = count == 32u ? UINT32_MAX : static_cast<uint32_t>((uint64_t {1} << count) - 1u);
				if (insert) { r = count == 0u ? arg(4) : (arg(4) & ~(mask << offset)) | ((arg(5) & mask) << offset); }
				else
				{
					r = count == 0u ? 0u : (arg(4) >> offset) & mask;
					if (op == "OpBitFieldSExtract" && count != 0u && ((r >> (count - 1u)) & 1u) != 0u) { r |= ~mask; }
				}
			}
			else if (op == "OpIEqual" || op == "OpLogicalEqual") { r = arg(4) == arg(5); }
			else if (op == "OpINotEqual") { r = arg(4) != arg(5); }
			else if (op == "OpULessThan") { r = arg(4) < arg(5); }
			else if (op == "OpSLessThan") { r = static_cast<int32_t>(arg(4)) < static_cast<int32_t>(arg(5)); }
			else if (op == "OpULessThanEqual") { r = arg(4) <= arg(5); }
			else if (op == "OpUGreaterThan") { r = arg(4) > arg(5); }
			else if (op == "OpUGreaterThanEqual") { r = arg(4) >= arg(5); }
			else if (op == "OpLogicalAnd") { r = arg(4) != 0u && arg(5) != 0u; }
			else if (op == "OpLogicalOr") { r = arg(4) != 0u || arg(5) != 0u; }
			else if (op == "OpLogicalNot") { r = arg(4) == 0u; }
			else if (op == "OpFAdd") { r = Bits(fp(4) + fp(5)); }
			else if (op == "OpFSub") { r = Bits(fp(4) - fp(5)); }
			else if (op == "OpFMul") { r = Bits(fp(4) * fp(5)); }
			else if (op == "OpFNegate") { r = arg(4) ^ 0x80000000u; }
			else if (op == "OpFOrdEqual") { r = fp(4) == fp(5); }
			else if (op == "OpFOrdNotEqual") { r = !std::isnan(fp(4)) && !std::isnan(fp(5)) && fp(4) != fp(5); }
			else if (op == "OpFOrdLessThan") { r = fp(4) < fp(5); }
			else if (op == "OpFOrdLessThanEqual") { r = fp(4) <= fp(5); }
			else if (op == "OpFOrdGreaterThan") { r = fp(4) > fp(5); }
			else if (op == "OpIsNan") { r = std::isnan(fp(4)); }
			else if (op == "OpIsInf") { r = std::isinf(fp(4)); }
			else if (op == "OpConvertUToF") { r = Bits(static_cast<float>(arg(4))); }
			else if (op == "OpConvertSToF") { r = Bits(static_cast<float>(static_cast<int32_t>(arg(4)))); }
			else if (op == "OpConvertFToS")
			{
				if (!std::isfinite(fp(4)) || fp(4) < -2147483648.0 || fp(4) >= 2147483648.0) { Fail("undefined conversion"); }
				r = static_cast<uint32_t>(static_cast<int32_t>(fp(4)));
			}
			else if (op == "OpCompositeExtract") { r = value(t.at(4)).words.at(std::strtoul(t.at(5).c_str(), nullptr, 0)); }
			else if (op == "OpExtInst")
			{
				const auto& ext = t.at(5);
				if (ext == "UMin") { r = std::min(arg(6), arg(7)); }
				else if (ext == "UMax") { r = std::max(arg(6), arg(7)); }
				else if (ext == "SClamp")
				{
					const auto signed_arg = [&](size_t index) { return static_cast<int32_t>(arg(index)); };
					r = static_cast<uint32_t>(std::min(std::max(signed_arg(6), signed_arg(7)), signed_arg(8)));
				}
				else if (ext == "FindUMsb")
				{
					uint32_t word = arg(6);
					r = UINT32_MAX;
					while (word != 0u) { ++r; word >>= 1u; }
				}
				else if (ext == "UnpackHalf2x16")
				{
					r = HalfFloatBits(arg(6));
					result.words[1] = HalfFloatBits(arg(6) >> 16u);
				}
				else if (ext == "FAbs") { r = arg(6) & 0x7fffffffu; }
				else if (ext == "FClamp") { r = Bits(std::min(std::max(fp(6), fp(7)), fp(8))); }
				else if (ext == "Trunc") { r = Bits(std::trunc(fp(6))); }
				else if (ext == "Floor") { r = Bits(std::floor(fp(6))); }
				else if (ext == "Ceil") { r = Bits(std::ceil(fp(6))); }
				else if (ext == "RoundEven") { r = Bits(std::nearbyint(fp(6))); }
				else if (ext == "Sqrt") { r = Bits(std::sqrt(fp(6))); }
				else if (ext == "Log2") { r = Bits(std::log2(fp(6))); }
				else if (ext == "Exp2") { r = Bits(std::exp2(fp(6))); }
				else if (ext == "Sin") { r = Bits(std::sin(fp(6))); }
				else if (ext == "Cos") { r = Bits(std::cos(fp(6))); }
				else { Fail("unimplemented extended instruction " + ext); }
			}
			else { Fail("unimplemented instruction " + op); }
			values[t[0]] = result;
		}
		if (scc != nullptr) { *scc = value("%scc").words[0]; }
		return value(destination).words[0];
	}

private:
	std::unordered_map<std::string, Value> m_constants;
	std::vector<std::vector<std::string>> m_body;
};

inline void Equal(uint32_t actual, uint32_t expected, const char* label)
{
	if (actual != expected)
	{
		std::fprintf(stderr, "%s: got 0x%08x, expected 0x%08x\n", label, actual, expected);
		Fail(label);
	}
}

} // namespace ShaderArithmeticTest

#endif // KYTY_SHADER_ARITHMETIC_TEST_INTERPRETER_H_
