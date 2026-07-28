#include "Emulator/Network/HttpUri.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Network::Http {

LIB_NAME("Http", "Http");

namespace {

constexpr size_t kMaxUriBytes = 64 * 1024;

struct ParsedUri
{
	bool        opaque = true;
	bool        has_explicit_port = false;
	std::string scheme;
	std::string username;
	std::string password;
	std::string hostname;
	std::string path;
	std::string query;
	std::string fragment;
	uint16_t    port = 0;
};

bool AsciiEqualNoCase(std::string_view left, std::string_view right)
{
	if (left.size() != right.size())
	{
		return false;
	}

	for (size_t i = 0; i < left.size(); i++)
	{
		if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i])))
		{
			return false;
		}
	}

	return true;
}

bool IsValidScheme(std::string_view value)
{
	if (value.empty() || std::isalpha(static_cast<unsigned char>(value.front())) == 0)
	{
		return false;
	}

	return std::all_of(value.begin() + 1, value.end(), [](char c) {
		const auto byte = static_cast<unsigned char>(c);
		return std::isalnum(byte) != 0 || c == '+' || c == '-' || c == '.';
	});
}

bool ParsePort(std::string_view text, uint16_t* out)
{
	if (out == nullptr || text.empty() || text.size() > 5)
	{
		return false;
	}

	uint32_t value = 0;
	for (char c: text)
	{
		if (std::isdigit(static_cast<unsigned char>(c)) == 0)
		{
			return false;
		}
		value = value * 10u + static_cast<uint32_t>(c - '0');
	}

	if (value > UINT16_MAX)
	{
		return false;
	}

	*out = static_cast<uint16_t>(value);
	return true;
}

bool ParseUri(std::string_view input, ParsedUri* out)
{
	if (out == nullptr)
	{
		return false;
	}

	size_t       position    = 0;
	const size_t colon       = input.find(':');
	const size_t first_delim = input.find_first_of("/?#");
	if (colon != std::string_view::npos && (first_delim == std::string_view::npos || colon < first_delim))
	{
		if (!IsValidScheme(input.substr(0, colon)))
		{
			return false;
		}
		out->scheme.assign(input.substr(0, colon));
		position = colon + 1;
	}

	const bool has_authority = input.substr(position, 2) == "//";
	out->opaque              = !has_authority;
	if (has_authority)
	{
		position += 2;
		size_t authority_end = input.find_first_of("/?#", position);
		if (authority_end == std::string_view::npos)
		{
			authority_end = input.size();
		}

		std::string_view authority = input.substr(position, authority_end - position);
		position                  = authority_end;
		const size_t at           = authority.rfind('@');
		if (at != std::string_view::npos)
		{
			const std::string_view user_info = authority.substr(0, at);
			authority.remove_prefix(at + 1);
			const size_t separator = user_info.find(':');
			out->username.assign(user_info.substr(0, separator));
			if (separator != std::string_view::npos)
			{
				out->password.assign(user_info.substr(separator + 1));
			}
		}

		if (!authority.empty() && authority.front() == '[')
		{
			const size_t close = authority.find(']');
			if (close == std::string_view::npos)
			{
				return false;
			}

			out->hostname.assign(authority.substr(1, close - 1));
			if (close + 1 < authority.size())
			{
				if (authority[close + 1] != ':' || !ParsePort(authority.substr(close + 2), &out->port))
				{
					return false;
				}
				out->has_explicit_port = true;
			}
		} else
		{
			const size_t port_separator = authority.rfind(':');
			if (port_separator != std::string_view::npos)
			{
				out->hostname.assign(authority.substr(0, port_separator));
				if (!ParsePort(authority.substr(port_separator + 1), &out->port))
				{
					return false;
				}
				out->has_explicit_port = true;
			} else
			{
				out->hostname.assign(authority);
			}
		}
	}

	size_t path_end = input.find_first_of("?#", position);
	if (path_end == std::string_view::npos)
	{
		path_end = input.size();
	}
	out->path.assign(input.substr(position, path_end - position));
	position = path_end;

	if (position < input.size() && input[position] == '?')
	{
		size_t query_end = input.find('#', position);
		if (query_end == std::string_view::npos)
		{
			query_end = input.size();
		}
		out->query.assign(input.substr(position, query_end - position));
		position = query_end;
	}

	if (position < input.size() && input[position] == '#')
	{
		out->fragment.assign(input.substr(position));
	}

	if (!out->has_explicit_port)
	{
		if (AsciiEqualNoCase(out->scheme, "http"))
		{
			out->port = 80;
		} else if (AsciiEqualNoCase(out->scheme, "https"))
		{
			out->port = 443;
		}
	}

	return true;
}

size_t RequiredPoolSize(const ParsedUri& uri)
{
	return uri.scheme.size() + uri.username.size() + uri.password.size() + uri.hostname.size() + uri.path.size() +
	       uri.query.size() + uri.fragment.size() + 7;
}

char* CopyComponent(char** cursor, const std::string& value)
{
	char* result = *cursor;
	std::memcpy(*cursor, value.c_str(), value.size() + 1);
	*cursor += value.size() + 1;
	return result;
}

bool IsAligned(const void* value, size_t alignment)
{
	return value == nullptr || (reinterpret_cast<uintptr_t>(value) % alignment) == 0;
}

} // namespace

int KYTY_SYSV_ABI HttpUriParse(HttpUriElement* out, const char* source, void* pool, uint64_t* required_size, size_t pool_size)
{
	PRINT_NAME();

	if (source == nullptr)
	{
		return HTTP_ERROR_INVALID_URL;
	}

	if (!IsAligned(out, alignof(HttpUriElement)) || !IsAligned(required_size, alignof(uint64_t)))
	{
		return HTTP_ERROR_INVALID_VALUE;
	}

	const bool has_output = out != nullptr;
	const bool has_pool   = pool != nullptr;
	if (has_output != has_pool || (!has_output && required_size == nullptr))
	{
		return HTTP_ERROR_INVALID_VALUE;
	}

	const size_t length = strnlen(source, kMaxUriBytes);
	if (length == kMaxUriBytes)
	{
		return HTTP_ERROR_INVALID_URL;
	}

	ParsedUri parsed;
	if (!ParseUri(std::string_view(source, length), &parsed))
	{
		return HTTP_ERROR_INVALID_URL;
	}

	const size_t required = RequiredPoolSize(parsed);
	if (!has_output)
	{
		*required_size = required;
		return OK;
	}

	if (pool_size < required)
	{
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	std::memset(out, 0, sizeof(*out));
	auto* cursor  = static_cast<char*>(pool);
	out->opaque   = parsed.opaque;
	out->scheme   = CopyComponent(&cursor, parsed.scheme);
	out->username = CopyComponent(&cursor, parsed.username);
	out->password = CopyComponent(&cursor, parsed.password);
	out->hostname = CopyComponent(&cursor, parsed.hostname);
	out->path     = CopyComponent(&cursor, parsed.path);
	out->query    = CopyComponent(&cursor, parsed.query);
	out->fragment = CopyComponent(&cursor, parsed.fragment);
	out->port     = parsed.port;
	if (required_size != nullptr)
	{
		*required_size = required;
	}

	return OK;
}

} // namespace Kyty::Libs::Network::Http

#endif // KYTY_EMU_ENABLED
