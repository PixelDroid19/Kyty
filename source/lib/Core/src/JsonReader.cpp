#include "Kyty/Core/JsonReader.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/SafeDelete.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <system_error>

namespace Kyty::Core {

namespace {

constexpr uint32_t kJsonMaxNestingDepth = 256;
constexpr double   kInt64LowerBound     = -9223372036854775808.0;
constexpr double   kInt64UpperBound     = 9223372036854775808.0;

} // namespace

static String* g_json_error = nullptr;
static Json*   g_null       = nullptr;

static void set_error(const String& str)
{
	if (g_json_error == nullptr)
	{
		g_json_error = new String;
	}
	*g_json_error = str;
}

static const char32_t* skip(const char32_t* in)
{
	if (in == nullptr)
	{
		return nullptr;
	}

	while ((*in != 0u) && Char::IsSpace(*in))
	{
		in++;
	}

	return in;
}

static bool is_ascii_digit(char32_t value)
{
	return value >= U'0' && value <= U'9';
}

static const char32_t* skip_number(const char32_t* in)
{
	if (in == nullptr)
	{
		return nullptr;
	}

	const char32_t* ptr = in;

	if (*ptr == U'-')
	{
		ptr++;
	}

	if (*ptr == U'0')
	{
		ptr++;
	} else if (*ptr >= U'1' && *ptr <= U'9')
	{
		do
		{
			ptr++;
		} while (is_ascii_digit(*ptr));
	} else
	{
		return nullptr;
	}

	if (*ptr == U'.')
	{
		ptr++;
		if (!is_ascii_digit(*ptr))
		{
			return nullptr;
		}
		do
		{
			ptr++;
		} while (is_ascii_digit(*ptr));
	}

	if (*ptr == U'e' || *ptr == U'E')
	{
		ptr++;
		if (*ptr == U'+' || *ptr == U'-')
		{
			ptr++;
		}
		if (!is_ascii_digit(*ptr))
		{
			return nullptr;
		}
		do
		{
			ptr++;
		} while (is_ascii_digit(*ptr));
	}

	return ptr;
}

const char32_t* Json::parse_value(const char32_t* value, uint32_t depth)
{
	if (value == nullptr)
	{
		return nullptr;
	}

	if ((*value == U'[' || *value == U'{') && depth >= kJsonMaxNestingDepth)
	{
		set_error(value);
		return nullptr;
	}

	switch (*value)
	{
		case U'n':
		{
			if (Char::EqualAsciiN(value, "null", 4))
			{
				m_type = JsonNULL;
				return value + 4;
			}
			break;
		}
		case U'f':
		{
			if (Char::EqualAsciiN(value, "false", 5))
			{
				m_type = JsonBool;
				return value + 5;
			}
			break;
		}
		case U't':
		{
			if (Char::EqualAsciiN(value, "true", 4))
			{
				m_type       = JsonBool;
				m_value_bool = true;
				m_value_int  = 1;
				return value + 4;
			}
			break;
		}
		case U'\"': return parse_string(value);
		case U'[': return parse_array(value, depth);
		case U'{': return parse_object(value, depth);
		case U'-':
		case U'0':
		case U'1':
		case U'2':
		case U'3':
		case U'4':
		case U'5':
		case U'6':
		case U'7':
		case U'8':
		case U'9': return parse_number(value);
		default: break;
	}

	set_error(value);
	return nullptr;
}

const char32_t* Json::parse_array(const char32_t* value, uint32_t depth)
{
	if (*value != U'[')
	{
		set_error(value);
		return nullptr;
	}

	m_type = JsonArray;

	value = skip(value + 1);

	if (*value == U']')
	{
		return value + 1;
	}

	Json* child = new Json();

	value = skip(child->parse_value(skip(value), depth + 1u));

	if (value == nullptr)
	{
		Delete(child);
		return nullptr;
	}

	m_list.Add(child);

	while (*value == U',')
	{
		child = new Json();

		value = skip(child->parse_value(skip(value + 1), depth + 1u));

		if (value == nullptr)
		{
			Delete(child);
			return nullptr;
		}

		m_list.Add(child);
	}

	if (*value == ']')
	{
		return value + 1;
	}

	set_error(value);
	return nullptr;
}

const char32_t* Json::parse_object(const char32_t* value, uint32_t depth)
{
	if (*value != U'{')
	{
		set_error(value);
		return nullptr;
	}

	m_type = JsonObject;

	value = skip(value + 1);

	if (*value == U'}')
	{
		return value + 1;
	}

	Json* child = new Json();

	const char32_t* key = skip(value);
	value = skip(child->parse_value(key, depth + 1u));

	if (value == nullptr)
	{
		Delete(child);
		return nullptr;
	}

	if (child->m_type != JsonString)
	{
		Delete(child);
		set_error(key);
		return nullptr;
	}

	child->m_name         = child->m_value_string;
	child->m_value_string = U"";

	if (*value != U':')
	{
		Delete(child);
		set_error(value);
		return nullptr;
	}

	value = skip(child->parse_value(skip(value + 1), depth + 1u));

	if (value == nullptr)
	{
		Delete(child);
		return nullptr;
	}

	m_list.Add(child);

	while (*value == U',')
	{
		child = new Json();

		const char32_t* key = skip(value + 1);
		value = skip(child->parse_value(key, depth + 1u));

		if (value == nullptr)
		{
			Delete(child);
			return nullptr;
		}

		if (child->m_type != JsonString)
		{
			Delete(child);
			set_error(key);
			return nullptr;
		}

		child->m_name         = child->m_value_string;
		child->m_value_string = U"";

		if (*value != U':')
		{
			Delete(child);
			set_error(value);
			return nullptr;
		}

		value = skip(child->parse_value(skip(value + 1), depth + 1u));

		if (value == nullptr)
		{
			Delete(child);
			return nullptr;
		}

		m_list.Add(child);
	}

	if (*value == U'}')
	{
		return value + 1;
	}

	set_error(value);
	return nullptr;
}

const char32_t* Json::parse_number(const char32_t* value)
{
	const char32_t* end = skip_number(value);

	if (end == nullptr)
	{
		set_error(value);
		return nullptr;
	}

	const uint32_t len = end - value;
	String         str(U' ', len);
	std::memcpy(str.GetData(), value, len * sizeof(char32_t));

	std::string ascii_number;
	ascii_number.reserve(len);
	for (uint32_t i = 0; i < len; i++)
	{
		ascii_number.push_back(static_cast<char>(value[i]));
	}

	const char* first = ascii_number.data();
	const char* last  = first + ascii_number.size();

	double parsed_double = 0.0;
	const auto double_result = std::from_chars(first, last, parsed_double, std::chars_format::general);
	if (double_result.ec != std::errc{} || double_result.ptr != last || !std::isfinite(parsed_double))
	{
		set_error(value);
		return nullptr;
	}

	m_type            = JsonNumber;
	m_value_double    = parsed_double;
	m_value_int_valid = false;
	m_value_string    = str;

	if (ascii_number.find_first_of(".eE") == std::string::npos)
	{
		int64_t parsed_int = 0;
		const auto int_result = std::from_chars(first, last, parsed_int, 10);
		if (int_result.ec == std::errc{} && int_result.ptr == last)
		{
			m_value_int       = parsed_int;
			m_value_int_valid = true;
		}
	} else
	{
		const double truncated = std::trunc(parsed_double);
		if (truncated >= kInt64LowerBound && truncated < kInt64UpperBound)
		{
			m_value_int       = static_cast<int64_t>(truncated);
			m_value_int_valid = true;
		}
	}

	return end;
}

const char32_t* Json::parse_string(const char32_t* str)
{
	if (*str != U'\"')
	{
		set_error(str);
		return nullptr;
	}

	const char32_t* ptr = str + 1;

	uint32_t len = 0;
	for (;;)
	{
		if (*ptr == U'\"')
		{
			break;
		}
		if (*ptr == 0)
		{
			set_error(ptr);
			return nullptr;
		}
		if (*ptr == U'\\')
		{
			ptr++;
			if (*ptr == 0)
			{
				set_error(ptr);
				return nullptr;
			}
		}
		len++;
		ptr++;
	}

	String out(' ', len);

	ptr            = str + 1;
	char32_t* ptr2 = out.GetData();
	while (*ptr != '\"' && *ptr != 0)
	{
		if (*ptr != '\\')
		{
			*ptr2++ = *ptr++;
		} else
		{
			ptr++;
			switch (*ptr)
			{
				case U'b': *ptr2++ = U'\b'; break;
				case U'f': *ptr2++ = U'\f'; break;
				case U'n': *ptr2++ = U'\n'; break;
				case U'r': *ptr2++ = U'\r'; break;
				case U't': *ptr2++ = U'\t'; break;
				case U'u':
					set_error(ptr);
					return nullptr;
					break;
				default: *ptr2++ = *ptr; break;
			}
			ptr++;
		}
	}

	if (*ptr == U'\"')
	{
		ptr++;
	}

	len = ptr2 - out.GetDataConst();

	m_value_string = out.Left(len);
	m_type         = JsonString;

	return ptr;
}

// Json::Json()
//{
//	m_value_int    = 0;
//	m_value_double = 0.0;
//	m_value_bool   = false;
//	m_type         = JsonNULL;
//}

Json::~Json()
{
	FOR (i, m_list)
	{
		Delete(m_list[i]);
	}
}

const Json* Json::Create(const String& str)
{
	Json* c = new Json();

	const char32_t* value = skip(str.GetDataConst());
	if (value == nullptr)
	{
		Delete(c);
		return nullptr;
	}

	if (*value == 0)
	{
		set_error(value);
		Delete(c);
		return nullptr;
	}

	value = c->parse_value(value, 0);

	if (value == nullptr)
	{
		Delete(c);
		return nullptr;
	}

	value = skip(value);
	if (*value != 0)
	{
		set_error(value);
		Delete(c);
		return nullptr;
	}

	return c;
}

String Json::GetError()
{
	if (g_json_error != nullptr)
	{
		return *g_json_error;
	}

	return U"";
}

void Json::Init()
{
	if (g_null == nullptr)
	{
		g_null = new Json();
	}
}

// const Json* Json::GetItem(const String& string) const
const Json* Json::GetItem(const char* string) const
{
	FOR (i, m_list)
	{
		const Json* n = m_list.At(i);
		if (n->m_name.EqualAsciiNoCase(string))
		{
			return n;
		}
	}

	Init();

	return g_null;
}

// String Json::GetString(const String& name, const String& default_value) const
String Json::GetString(const char* name, const String& default_value) const
{
	const Json* n = GetItem(name);
	if (!n->IsNull())
	{
		return n->m_value_string;
	}
	return default_value;
}

String Json::GetString(const char* name) const
{
	const Json* n = GetItem(name);
	if (!n->IsNull())
	{
		return n->m_value_string;
	}
	return U"";
}

// double Json::GetFloat(const String& name, double default_value) const
double Json::GetFloat(const char* name, double default_value) const
{
	const Json* n = GetItem(name);
	if (!n->IsNull())
	{
		return n->m_value_double;
	}
	return default_value;
}

int64_t Json::ToInt() const
{
	if (m_type == JsonNumber && !m_value_int_valid)
	{
		EXIT("JSON number cannot be represented as int64\n");
	}
	return m_value_int;
}

bool Json::TryToInt(int64_t* value) const
{
	if (value == nullptr || m_type != JsonNumber || !m_value_int_valid)
	{
		return false;
	}

	*value = m_value_int;
	return true;
}

// int64_t Json::GetInt(const String& name, int64_t default_value) const
int64_t Json::GetInt(const char* name, int64_t default_value) const
{
	const Json* n = GetItem(name);
	if (!n->IsNull())
	{
		return n->ToInt();
	}
	return default_value;
}

bool Json::GetBool(const char* name, bool default_value) const
{
	const Json* n = GetItem(name);
	if (!n->IsNull())
	{
		return n->m_value_bool;
	}
	return default_value;
}

StringList Json::DbgCheckList(const StringList& required, const StringList& optional) const
{
	StringList errors;

	FOR (i, required)
	{
		if (GetItem(required.At(i).C_Str())->m_name.IsEmpty())
		{
			errors.Add(U"missing: " + required.At(i));
		}
	}

	FOR (i, m_list)
	{
		const Json* n = m_list.At(i);
		if (!n->m_name.IsEmpty() && !(required.Contains(n->m_name) || optional.Contains(n->m_name)))
		{
			errors.Add(U"unknown: " + n->m_name);
		}
	}

	return errors;
}

} // namespace Kyty::Core
