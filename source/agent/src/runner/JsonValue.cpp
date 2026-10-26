#include "Kyty/Agent/Runner/JsonValue.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Kyty::Agent::Runner {
namespace {

const Json& NullJson()
{
	static const Json null_value;
	return null_value;
}

const std::string& EmptyString()
{
	static const std::string empty;
	return empty;
}

void AppendUtf8(uint32_t code_point, std::string* out)
{
	if (code_point < 0x80u)
	{
		out->push_back(static_cast<char>(code_point));
	} else if (code_point < 0x800u)
	{
		out->push_back(static_cast<char>(0xC0u | (code_point >> 6u)));
		out->push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
	} else if (code_point < 0x10000u)
	{
		out->push_back(static_cast<char>(0xE0u | (code_point >> 12u)));
		out->push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
		out->push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
	} else
	{
		out->push_back(static_cast<char>(0xF0u | (code_point >> 18u)));
		out->push_back(static_cast<char>(0x80u | ((code_point >> 12u) & 0x3Fu)));
		out->push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
		out->push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
	}
}

void AppendEscaped(const std::string& value, std::string* out)
{
	out->push_back('"');
	for (const char c: value)
	{
		const auto byte = static_cast<unsigned char>(c);
		switch (byte)
		{
			case '"': *out += "\\\""; break;
			case '\\': *out += "\\\\"; break;
			case '\b': *out += "\\b"; break;
			case '\f': *out += "\\f"; break;
			case '\n': *out += "\\n"; break;
			case '\r': *out += "\\r"; break;
			case '\t': *out += "\\t"; break;
			default:
				if (byte < 0x20u)
				{
					char escaped[8];
					std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
					*out += escaped;
				} else
				{
					out->push_back(c);
				}
				break;
		}
	}
	out->push_back('"');
}

class Parser
{
public:
	explicit Parser(std::string_view text): m_text(text) {}

	bool ParseDocument(Json* out, std::string* error)
	{
		SkipSpace();
		if (!ParseValue(out, 0))
		{
			return Fail(error);
		}
		SkipSpace();
		if (m_pos != m_text.size())
		{
			m_error = "trailing characters";
			return Fail(error);
		}
		return true;
	}

private:
	static constexpr int kMaxDepth = 64;

	bool Fail(std::string* error) const
	{
		if (error != nullptr)
		{
			*error = m_error + " at offset " + std::to_string(m_pos);
		}
		return false;
	}

	void SkipSpace()
	{
		while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
		{
			++m_pos;
		}
	}

	bool Consume(std::string_view literal)
	{
		if (m_text.substr(m_pos, literal.size()) != literal)
		{
			m_error = "invalid literal";
			return false;
		}
		m_pos += literal.size();
		return true;
	}

	bool ParseValue(Json* out, int depth)
	{
		if (depth > kMaxDepth)
		{
			m_error = "nesting too deep";
			return false;
		}
		if (m_pos >= m_text.size())
		{
			m_error = "unexpected end";
			return false;
		}
		switch (m_text[m_pos])
		{
			case '{': return ParseObject(out, depth);
			case '[': return ParseArray(out, depth);
			case '"':
			{
				std::string value;
				if (!ParseString(&value))
				{
					return false;
				}
				*out = Json(std::move(value));
				return true;
			}
			case 't': *out = Json(true); return Consume("true");
			case 'f': *out = Json(false); return Consume("false");
			case 'n': *out = Json(); return Consume("null");
			default: return ParseNumber(out);
		}
	}

	bool ParseObject(Json* out, int depth)
	{
		*out = Json::MakeObject();
		++m_pos;
		SkipSpace();
		if (m_pos < m_text.size() && m_text[m_pos] == '}')
		{
			++m_pos;
			return true;
		}
		for (;;)
		{
			SkipSpace();
			std::string key;
			if (m_pos >= m_text.size() || m_text[m_pos] != '"' || !ParseString(&key))
			{
				m_error = m_error.empty() ? "expected object key" : m_error;
				return false;
			}
			SkipSpace();
			if (m_pos >= m_text.size() || m_text[m_pos] != ':')
			{
				m_error = "expected ':'";
				return false;
			}
			++m_pos;
			SkipSpace();
			Json value;
			if (!ParseValue(&value, depth + 1))
			{
				return false;
			}
			// A repeated key has no defined value in evidence documents.
			if (out->Has(key))
			{
				m_error = "duplicate object key";
				return false;
			}
			out->Set(key, std::move(value));
			SkipSpace();
			if (m_pos < m_text.size() && m_text[m_pos] == ',')
			{
				++m_pos;
				continue;
			}
			if (m_pos < m_text.size() && m_text[m_pos] == '}')
			{
				++m_pos;
				return true;
			}
			m_error = "expected ',' or '}'";
			return false;
		}
	}

	bool ParseArray(Json* out, int depth)
	{
		*out = Json::MakeArray();
		++m_pos;
		SkipSpace();
		if (m_pos < m_text.size() && m_text[m_pos] == ']')
		{
			++m_pos;
			return true;
		}
		for (;;)
		{
			SkipSpace();
			Json value;
			if (!ParseValue(&value, depth + 1))
			{
				return false;
			}
			out->Push(std::move(value));
			SkipSpace();
			if (m_pos < m_text.size() && m_text[m_pos] == ',')
			{
				++m_pos;
				continue;
			}
			if (m_pos < m_text.size() && m_text[m_pos] == ']')
			{
				++m_pos;
				return true;
			}
			m_error = "expected ',' or ']'";
			return false;
		}
	}

	bool ParseHex4(uint32_t* out)
	{
		if (m_pos + 4 > m_text.size())
		{
			m_error = "truncated escape";
			return false;
		}
		uint32_t value = 0;
		for (int i = 0; i < 4; ++i)
		{
			const char c     = m_text[m_pos++];
			uint32_t   digit = 0;
			if (c >= '0' && c <= '9')
			{
				digit = static_cast<uint32_t>(c - '0');
			} else if (c >= 'a' && c <= 'f')
			{
				digit = static_cast<uint32_t>(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F')
			{
				digit = static_cast<uint32_t>(c - 'A' + 10);
			} else
			{
				m_error = "invalid escape";
				return false;
			}
			value = value * 16u + digit;
		}
		*out = value;
		return true;
	}

	bool ParseEscape(std::string* out)
	{
		const char c = m_text[m_pos++];
		switch (c)
		{
			case '"': out->push_back('"'); return true;
			case '\\': out->push_back('\\'); return true;
			case '/': out->push_back('/'); return true;
			case 'b': out->push_back('\b'); return true;
			case 'f': out->push_back('\f'); return true;
			case 'n': out->push_back('\n'); return true;
			case 'r': out->push_back('\r'); return true;
			case 't': out->push_back('\t'); return true;
			case 'u': break;
			default: m_error = "invalid escape"; return false;
		}
		uint32_t code_point = 0;
		if (!ParseHex4(&code_point))
		{
			return false;
		}
		if (code_point >= 0xD800u && code_point < 0xDC00u && m_text.substr(m_pos, 2) == "\\u")
		{
			const size_t saved = m_pos;
			m_pos += 2;
			uint32_t low = 0;
			if (ParseHex4(&low) && low >= 0xDC00u && low < 0xE000u)
			{
				code_point = 0x10000u + ((code_point - 0xD800u) << 10u) + (low - 0xDC00u);
			} else
			{
				m_pos = saved;
			}
		}
		AppendUtf8(code_point, out);
		return true;
	}

	bool ParseString(std::string* out)
	{
		++m_pos;
		while (m_pos < m_text.size())
		{
			const char c = m_text[m_pos++];
			if (c == '"')
			{
				return true;
			}
			if (static_cast<unsigned char>(c) < 0x20u)
			{
				m_error = "control character in string";
				return false;
			}
			if (c != '\\')
			{
				out->push_back(c);
				continue;
			}
			if (m_pos >= m_text.size() || !ParseEscape(out))
			{
				m_error = m_error.empty() ? "truncated escape" : m_error;
				return false;
			}
		}
		m_error = "unterminated string";
		return false;
	}

	bool ParseNumber(Json* out)
	{
		const size_t start   = m_pos;
		bool         integer = true;
		if (m_pos < m_text.size() && m_text[m_pos] == '-')
		{
			++m_pos;
		}
		const size_t digits_start = m_pos;
		while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
		{
			++m_pos;
		}
		if (m_pos == digits_start || (m_text[digits_start] == '0' && m_pos - digits_start > 1))
		{
			m_error = "invalid number";
			return false;
		}
		if (m_pos < m_text.size() && m_text[m_pos] == '.')
		{
			integer = false;
			const size_t fraction = ++m_pos;
			while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
			{
				++m_pos;
			}
			if (m_pos == fraction)
			{
				m_error = "invalid number";
				return false;
			}
		}
		if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
		{
			integer = false;
			++m_pos;
			if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
			{
				++m_pos;
			}
			const size_t exponent = m_pos;
			while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
			{
				++m_pos;
			}
			if (m_pos == exponent)
			{
				m_error = "invalid number";
				return false;
			}
		}
		const std::string literal(m_text.substr(start, m_pos - start));
		*out = Json::MakeNumber(std::strtod(literal.c_str(), nullptr), integer);
		return true;
	}

	std::string_view m_text;
	size_t           m_pos = 0;
	std::string      m_error;
};

void AppendIndent(std::string* out, int indent, int depth)
{
	out->push_back('\n');
	out->append(static_cast<size_t>(indent) * static_cast<size_t>(depth), ' ');
}

} // namespace

Json::Json(bool value): m_type(Type::Bool), m_bool(value) {}
Json::Json(int value): Json(static_cast<int64_t>(value)) {}
Json::Json(int64_t value): m_type(Type::Number), m_integer(true), m_number(static_cast<double>(value)) {}
Json::Json(uint64_t value): m_type(Type::Number), m_integer(true), m_number(static_cast<double>(value)) {}
Json::Json(double value): m_type(Type::Number), m_number(value) {}
Json::Json(const char* value): m_type(Type::String), m_string(value != nullptr ? value : "") {}
Json::Json(std::string value): m_type(Type::String), m_string(std::move(value)) {}

Json Json::MakeObject()
{
	Json value;
	value.m_type = Type::Object;
	return value;
}

Json Json::MakeArray()
{
	Json value;
	value.m_type = Type::Array;
	return value;
}

Json Json::MakeNumber(double value, bool integer)
{
	Json json(value);
	json.m_integer = integer;
	return json;
}

bool Json::Truthy() const
{
	switch (m_type)
	{
		case Type::Null: return false;
		case Type::Bool: return m_bool;
		case Type::Number: return m_number != 0.0;
		case Type::String: return !m_string.empty();
		case Type::Array: return !m_items.empty();
		case Type::Object: return !m_members.empty();
	}
	return false;
}

int64_t Json::AsInt(int64_t fallback) const
{
	if (m_type == Type::Bool)
	{
		return m_bool ? 1 : 0;
	}
	if (m_type != Type::Number || !std::isfinite(m_number))
	{
		return fallback;
	}
	return static_cast<int64_t>(m_number);
}

const std::string& Json::AsString() const
{
	return m_type == Type::String ? m_string : EmptyString();
}

bool Json::Has(const std::string& key) const
{
	return m_type == Type::Object && m_members.count(key) != 0;
}

const Json& Json::operator[](const std::string& key) const
{
	if (m_type != Type::Object)
	{
		return NullJson();
	}
	const auto it = m_members.find(key);
	return it == m_members.end() ? NullJson() : it->second;
}

const Json& Json::operator[](size_t index) const
{
	return m_type == Type::Array && index < m_items.size() ? m_items[index] : NullJson();
}

size_t Json::Size() const
{
	if (m_type == Type::Array)
	{
		return m_items.size();
	}
	return m_type == Type::Object ? m_members.size() : 0;
}

const std::vector<Json>& Json::Items() const
{
	return m_items;
}

const std::map<std::string, Json>& Json::Members() const
{
	return m_members;
}

Json& Json::Set(const std::string& key, Json value)
{
	if (m_type != Type::Object)
	{
		*this = MakeObject();
	}
	m_members[key] = std::move(value);
	return *this;
}

Json& Json::Push(Json value)
{
	if (m_type != Type::Array)
	{
		*this = MakeArray();
	}
	m_items.push_back(std::move(value));
	return *this;
}

void Json::Erase(const std::string& key)
{
	m_members.erase(key);
}

std::string Json::Dump(int indent) const
{
	std::string out;
	DumpTo(&out, indent, 0);
	return out;
}

void Json::DumpTo(std::string* out, int indent, int depth) const
{
	switch (m_type)
	{
		case Type::Null: *out += "null"; return;
		case Type::Bool: *out += m_bool ? "true" : "false"; return;
		case Type::Number:
			if (m_integer && std::fabs(m_number) < 9.007199254740992e15)
			{
				*out += std::to_string(static_cast<int64_t>(m_number));
			} else
			{
				*out += FormatDouble(m_number);
			}
			return;
		case Type::String: AppendEscaped(m_string, out); return;
		case Type::Array:
		{
			if (m_items.empty())
			{
				*out += "[]";
				return;
			}
			out->push_back('[');
			for (size_t i = 0; i < m_items.size(); ++i)
			{
				if (i != 0)
				{
					out->push_back(',');
				}
				if (indent >= 0)
				{
					AppendIndent(out, indent, depth + 1);
				}
				m_items[i].DumpTo(out, indent, depth + 1);
			}
			if (indent >= 0)
			{
				AppendIndent(out, indent, depth);
			}
			out->push_back(']');
			return;
		}
		case Type::Object:
		{
			if (m_members.empty())
			{
				*out += "{}";
				return;
			}
			out->push_back('{');
			bool first = true;
			for (const auto& [key, value]: m_members)
			{
				if (!first)
				{
					out->push_back(',');
				}
				first = false;
				if (indent >= 0)
				{
					AppendIndent(out, indent, depth + 1);
				}
				AppendEscaped(key, out);
				*out += indent >= 0 ? ": " : ":";
				value.DumpTo(out, indent, depth + 1);
			}
			if (indent >= 0)
			{
				AppendIndent(out, indent, depth);
			}
			out->push_back('}');
			return;
		}
	}
}

bool operator==(const Json& left, const Json& right)
{
	if (left.m_type != right.m_type)
	{
		return false;
	}
	switch (left.m_type)
	{
		case Json::Type::Null: return true;
		case Json::Type::Bool: return left.m_bool == right.m_bool;
		case Json::Type::Number: return left.m_number == right.m_number;
		case Json::Type::String: return left.m_string == right.m_string;
		case Json::Type::Array: return left.m_items == right.m_items;
		case Json::Type::Object: return left.m_members == right.m_members;
	}
	return false;
}

bool ParseJson(std::string_view text, Json* out, std::string* error)
{
	Parser parser(text);
	return parser.ParseDocument(out, error);
}

std::string FormatDouble(double value)
{
	if (!std::isfinite(value))
	{
		return "null";
	}
	if (value == 0.0)
	{
		return std::signbit(value) ? "-0.0" : "0.0";
	}
	// Shortest round-trip digits, then Python repr layout: positional when the
	// decimal exponent is in [-4, 16), scientific otherwise.
	char       buffer[64];
	const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
	std::string scientific(buffer, result.ptr);
	const bool  negative = scientific[0] == '-';
	if (negative)
	{
		scientific.erase(0, 1);
	}
	const size_t e_pos    = scientific.find('e');
	const int    exponent = std::atoi(scientific.c_str() + e_pos + 1);
	std::string  digits   = scientific.substr(0, e_pos);
	digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());

	std::string out = negative ? "-" : "";
	if (exponent < -4 || exponent >= 16)
	{
		out += digits.substr(0, 1);
		if (digits.size() > 1)
		{
			out += "." + digits.substr(1);
		}
		char suffix[16];
		std::snprintf(suffix, sizeof(suffix), "e%c%02d", exponent < 0 ? '-' : '+', exponent < 0 ? -exponent : exponent);
		return out + suffix;
	}
	if (exponent < 0)
	{
		return out + "0." + std::string(static_cast<size_t>(-exponent - 1), '0') + digits;
	}
	const auto integer_digits = static_cast<size_t>(exponent) + 1;
	if (digits.size() <= integer_digits)
	{
		return out + digits + std::string(integer_digits - digits.size(), '0') + ".0";
	}
	return out + digits.substr(0, integer_digits) + "." + digits.substr(integer_digits);
}

double RoundDecimal(double value, int digits)
{
	char buffer[512];
	std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
	return std::strtod(buffer, nullptr);
}

} // namespace Kyty::Agent::Runner
