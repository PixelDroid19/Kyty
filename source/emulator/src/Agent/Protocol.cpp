#include "Emulator/Agent/Protocol.h"

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {
namespace {

const char* SkipWs(const char* p)
{
	while (p != nullptr && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
	{
		++p;
	}
	return p;
}

bool MatchKey(const char* p, const char* key, const char** after_colon)
{
	p = SkipWs(p);
	if (p == nullptr || *p != '"')
	{
		return false;
	}
	++p;
	const size_t key_len = std::strlen(key);
	if (std::strncmp(p, key, key_len) != 0 || p[key_len] != '"')
	{
		return false;
	}
	p = SkipWs(p + key_len + 1);
	if (*p != ':')
	{
		return false;
	}
	*after_colon = SkipWs(p + 1);
	return true;
}

bool ParseStringValue(const char* p, std::string* out, const char** end)
{
	p = SkipWs(p);
	if (p == nullptr || *p != '"')
	{
		return false;
	}
	++p;
	std::string value;
	while (*p != '\0' && *p != '"')
	{
		if (*p == '\\' && p[1] != '\0')
		{
			++p;
			switch (*p)
			{
				case '"':
				case '\\':
				case '/': value.push_back(*p); break;
				case 'n': value.push_back('\n'); break;
				case 't': value.push_back('\t'); break;
				default: value.push_back(*p); break;
			}
		} else
		{
			value.push_back(*p);
		}
		++p;
	}
	if (*p != '"')
	{
		return false;
	}
	*out = value;
	*end = p + 1;
	return true;
}

bool ParseU64Value(const char* p, uint64_t* out, const char** end)
{
	p = SkipWs(p);
	if (p == nullptr || !std::isdigit(static_cast<unsigned char>(*p)))
	{
		return false;
	}
	char*       parse_end = nullptr;
	const auto  value     = std::strtoull(p, &parse_end, 10);
	if (parse_end == p)
	{
		return false;
	}
	*out = value;
	*end = parse_end;
	return true;
}

bool FindObjectField(const char* json, const char* key, const char** value_start)
{
	if (json == nullptr || key == nullptr || value_start == nullptr)
	{
		return false;
	}
	const char* p = SkipWs(json);
	if (*p != '{')
	{
		return false;
	}
	++p;
	while (*p != '\0' && *p != '}')
	{
		const char* after = nullptr;
		if (MatchKey(p, key, &after))
		{
			*value_start = after;
			return true;
		}
		// Skip this field roughly: advance to next comma at depth 1 or end.
		int  depth   = 0;
		bool in_str  = false;
		bool escape  = false;
		bool seen_colon = false;
		for (; *p != '\0'; ++p)
		{
			const char c = *p;
			if (in_str)
			{
				if (escape)
				{
					escape = false;
				} else if (c == '\\')
				{
					escape = true;
				} else if (c == '"')
				{
					in_str = false;
				}
				continue;
			}
			if (c == '"')
			{
				in_str = true;
				continue;
			}
			if (c == '{' || c == '[')
			{
				++depth;
				continue;
			}
			if (c == '}' || c == ']')
			{
				if (depth == 0)
				{
					return false;
				}
				--depth;
				continue;
			}
			if (c == ':' && depth == 0)
			{
				seen_colon = true;
				continue;
			}
			if (seen_colon && depth == 0 && c == ',')
			{
				++p;
				break;
			}
			if (seen_colon && depth == 0 && c == '}')
			{
				return false;
			}
		}
	}
	return false;
}

} // namespace

std::string JsonEscape(const char* value)
{
	std::string out;
	if (value == nullptr)
	{
		return out;
	}
	for (const char* p = value; *p != '\0'; ++p)
	{
		switch (*p)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\t': out += "\\t"; break;
			default: out.push_back(*p); break;
		}
	}
	return out;
}

std::string JsonString(const char* value)
{
	return std::string("\"") + JsonEscape(value) + "\"";
}

std::string FormatOk(uint64_t id, const std::string& result_json_object)
{
	char prefix[64];
	std::snprintf(prefix, sizeof(prefix), "{\"id\":%llu,\"ok\":true,\"result\":", static_cast<unsigned long long>(id));
	return std::string(prefix) + result_json_object + "}";
}

std::string FormatErr(uint64_t id, const char* code, const char* message)
{
	char buf[kAgentLineMax];
	std::snprintf(buf, sizeof(buf),
	              "{\"id\":%llu,\"ok\":false,\"error\":{\"code\":%s,\"message\":%s}}",
	              static_cast<unsigned long long>(id), JsonString(code != nullptr ? code : "error").c_str(),
	              JsonString(message != nullptr ? message : "").c_str());
	return std::string(buf);
}

bool ParseRequestLine(const char* line, Request* out, ErrorInfo* error)
{
	if (line == nullptr || out == nullptr || error == nullptr)
	{
		return false;
	}
	*out   = Request {};
	*error = ErrorInfo {};

	const char* id_value = nullptr;
	if (!FindObjectField(line, "id", &id_value))
	{
		error->code    = "invalid_args";
		error->message = "missing id";
		return false;
	}
	const char* end = nullptr;
	if (!ParseU64Value(id_value, &out->id, &end))
	{
		error->code    = "invalid_args";
		error->message = "id must be an integer";
		return false;
	}

	const char* tool_value = nullptr;
	if (!FindObjectField(line, "tool", &tool_value) || !ParseStringValue(tool_value, &out->tool, &end))
	{
		error->code    = "invalid_args";
		error->message = "missing tool";
		return false;
	}

	const char* args_value = nullptr;
	if (FindObjectField(line, "args", &args_value))
	{
		args_value = SkipWs(args_value);
		if (*args_value != '{')
		{
			error->code    = "invalid_args";
			error->message = "args must be an object";
			return false;
		}
		int  depth  = 0;
		bool in_str = false;
		bool escape = false;
		const char* start = args_value;
		const char* p     = args_value;
		for (; *p != '\0'; ++p)
		{
			const char c = *p;
			if (in_str)
			{
				if (escape)
				{
					escape = false;
				} else if (c == '\\')
				{
					escape = true;
				} else if (c == '"')
				{
					in_str = false;
				}
				continue;
			}
			if (c == '"')
			{
				in_str = true;
				continue;
			}
			if (c == '{')
			{
				++depth;
				continue;
			}
			if (c == '}')
			{
				--depth;
				if (depth == 0)
				{
					out->args_json.assign(start, static_cast<size_t>(p - start + 1));
					break;
				}
			}
		}
		if (out->args_json.empty())
		{
			error->code    = "invalid_args";
			error->message = "args object truncated";
			return false;
		}
	} else
	{
		out->args_json = "{}";
	}

	return true;
}

bool ArgsGetString(const std::string& args_json, const char* key, std::string* out)
{
	const char* value = nullptr;
	const char* end   = nullptr;
	if (!FindObjectField(args_json.c_str(), key, &value) || !ParseStringValue(value, out, &end))
	{
		return false;
	}
	return true;
}

bool ArgsGetU64(const std::string& args_json, const char* key, uint64_t* out)
{
	const char* value = nullptr;
	const char* end   = nullptr;
	if (!FindObjectField(args_json.c_str(), key, &value) || !ParseU64Value(value, out, &end))
	{
		return false;
	}
	return true;
}

bool ArgsGetU32(const std::string& args_json, const char* key, uint32_t* out)
{
	uint64_t value = 0;
	if (!ArgsGetU64(args_json, key, &value) || value > UINT32_MAX)
	{
		return false;
	}
	*out = static_cast<uint32_t>(value);
	return true;
}

bool ArgsGetBool(const std::string& args_json, const char* key, bool* out)
{
	const char* value = nullptr;
	if (!FindObjectField(args_json.c_str(), key, &value))
	{
		return false;
	}
	value = SkipWs(value);
	if (std::strncmp(value, "true", 4) == 0)
	{
		*out = true;
		return true;
	}
	if (std::strncmp(value, "false", 5) == 0)
	{
		*out = false;
		return true;
	}
	return false;
}

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED
