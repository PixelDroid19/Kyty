#ifndef KYTY_INCLUDE_KYTY_AGENT_JSON_H_
#define KYTY_INCLUDE_KYTY_AGENT_JSON_H_

#include <cstdio>
#include <string>

namespace Kyty::Agent {

// Escape one null-terminated byte string for use inside a JSON string. Kyty's
// agent protocol is UTF-8; non-control bytes are preserved verbatim.
inline std::string JsonEscape(const char* value)
{
	std::string out;
	if (value == nullptr)
	{
		return out;
	}
	for (const char* cursor = value; *cursor != '\0'; ++cursor)
	{
		const auto byte = static_cast<unsigned char>(*cursor);
		switch (byte)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (byte < 0x20)
				{
					char escaped[7];
					std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
					out += escaped;
				} else
				{
					out.push_back(*cursor);
				}
				break;
		}
	}
	return out;
}

// Return one complete JSON string value, including quotes.
inline std::string JsonString(const char* value)
{
	return std::string("\"") + JsonEscape(value) + '"';
}

} // namespace Kyty::Agent

#endif /* KYTY_INCLUDE_KYTY_AGENT_JSON_H_ */
