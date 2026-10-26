#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_JSONVALUE_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_JSONVALUE_H_

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Kyty::Agent::Runner {

// Small JSON document model for runner profiles, contracts, manifests and agent
// responses. Integer literals are remembered so contracts can require integers.
class Json
{
public:
	enum class Type : uint8_t
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object,
	};

	Json() = default;
	Json(bool value);               // NOLINT(google-explicit-constructor)
	Json(int value);                // NOLINT(google-explicit-constructor)
	Json(int64_t value);            // NOLINT(google-explicit-constructor)
	Json(uint64_t value);           // NOLINT(google-explicit-constructor)
	Json(double value);             // NOLINT(google-explicit-constructor)
	Json(const char* value);        // NOLINT(google-explicit-constructor)
	Json(std::string value);        // NOLINT(google-explicit-constructor)

	static Json MakeObject();
	static Json MakeArray();
	static Json MakeNumber(double value, bool integer);

	[[nodiscard]] Type Kind() const { return m_type; }
	[[nodiscard]] bool IsNull() const { return m_type == Type::Null; }
	[[nodiscard]] bool IsBool() const { return m_type == Type::Bool; }
	[[nodiscard]] bool IsNumber() const { return m_type == Type::Number; }
	[[nodiscard]] bool IsInteger() const { return m_type == Type::Number && m_integer; }
	[[nodiscard]] bool IsString() const { return m_type == Type::String; }
	[[nodiscard]] bool IsArray() const { return m_type == Type::Array; }
	[[nodiscard]] bool IsObject() const { return m_type == Type::Object; }

	// Python-style truthiness: null, false, 0, "", [] and {} are false.
	[[nodiscard]] bool Truthy() const;
	[[nodiscard]] bool AsBool() const { return m_type == Type::Bool && m_bool; }
	[[nodiscard]] double AsNumber(double fallback = 0.0) const { return m_type == Type::Number ? m_number : fallback; }
	// Truncates toward zero like Python int(); non-numbers give the fallback.
	[[nodiscard]] int64_t AsInt(int64_t fallback = 0) const;
	[[nodiscard]] const std::string& AsString() const;

	[[nodiscard]] bool               Has(const std::string& key) const;
	[[nodiscard]] const Json&        operator[](const std::string& key) const;
	[[nodiscard]] const Json&        operator[](size_t index) const;
	[[nodiscard]] size_t             Size() const;
	[[nodiscard]] const std::vector<Json>&            Items() const;
	[[nodiscard]] const std::map<std::string, Json>& Members() const;

	Json& Set(const std::string& key, Json value);
	Json& Push(Json value);
	void  Erase(const std::string& key);

	// Compact when indent < 0, otherwise pretty-printed with that many spaces.
	// Object keys are always written in sorted order.
	[[nodiscard]] std::string Dump(int indent = -1) const;

	friend bool operator==(const Json& left, const Json& right);
	friend bool operator!=(const Json& left, const Json& right) { return !(left == right); }

private:
	void DumpTo(std::string* out, int indent, int depth) const;

	Type                        m_type    = Type::Null;
	bool                        m_bool    = false;
	bool                        m_integer = false;
	double                      m_number  = 0.0;
	std::string                 m_string;
	std::vector<Json>           m_items;
	std::map<std::string, Json> m_members;
};

// Parses one complete JSON text (surrounding whitespace allowed).
[[nodiscard]] bool ParseJson(std::string_view text, Json* out, std::string* error);

// Formats a double the way Python repr does, so reports match across runners.
[[nodiscard]] std::string FormatDouble(double value);

// Rounds to `digits` decimals with correct decimal rounding, like Python round().
[[nodiscard]] double RoundDecimal(double value, int digits);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_JSONVALUE_H_ */
