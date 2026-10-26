#include "RunnerSupport.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iterator>

namespace Kyty::Agent::Runner {

std::string ValueText(const Json& value)
{
	switch (value.Kind())
	{
		case Json::Type::Null: return "None";
		case Json::Type::Bool: return value.AsBool() ? "True" : "False";
		case Json::Type::String: return value.AsString();
		case Json::Type::Number:
			return value.IsInteger() ? std::to_string(value.AsInt()) : FormatDouble(value.AsNumber());
		default: return value.Dump();
	}
}

const Json& FirstTruthy(const Json& first, const Json& second)
{
	return first.Truthy() ? first : second;
}

const Json& ExtractResult(const Json& reply)
{
	return reply["result"].IsObject() ? reply["result"] : reply;
}

std::string Strip(std::string_view text, std::string_view characters)
{
	const size_t begin = text.find_first_not_of(characters);
	if (begin == std::string_view::npos)
	{
		return {};
	}
	const size_t end = text.find_last_not_of(characters);
	return std::string(text.substr(begin, end - begin + 1));
}

std::string Lower(std::string text)
{
	for (char& c: text)
	{
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

std::string TruncateCodePoints(const std::string& text, size_t count)
{
	size_t points = 0;
	for (size_t i = 0; i < text.size(); ++i)
	{
		const bool continuation = (static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u;
		if (!continuation && points++ == count)
		{
			return text.substr(0, i);
		}
	}
	return text;
}

std::vector<std::string> SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	size_t                   start = 0;
	while (start < text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
		{
			end = text.size();
		}
		std::string line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		lines.push_back(std::move(line));
		start = end + 1;
	}
	return lines;
}

bool WriteTextFile(const std::string& path, const std::string& text, std::string* error)
{
	std::FILE* file = std::fopen(path.c_str(), "wb");
	if (file == nullptr)
	{
		*error = "cannot write " + path;
		return false;
	}
	const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
	const bool closed  = std::fclose(file) == 0;
	if (!written || !closed)
	{
		*error = "write failed: " + path;
		return false;
	}
	return true;
}

bool ReadTextFile(const std::string& path, size_t max_bytes, std::string* text, std::string* error)
{
	std::FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
	{
		*error = "cannot open " + path;
		return false;
	}
	text->clear();
	char   chunk[65536];
	size_t got = 0;
	while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
	{
		if (text->size() + got > max_bytes)
		{
			std::fclose(file);
			*error = "file too large: " + path;
			return false;
		}
		text->append(chunk, got);
	}
	const bool failed = std::ferror(file) != 0;
	std::fclose(file);
	if (failed)
	{
		*error = "read failed: " + path;
		return false;
	}
	return true;
}

std::string UtcTime(const char* format)
{
	const std::time_t now = std::time(nullptr);
	std::tm           utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	char text[64];
	return std::strftime(text, sizeof(text), format, &utc) > 0 ? std::string(text) : std::string();
}

bool ParseInt64(const std::string& text, int64_t* value)
{
	if (text.empty())
	{
		return false;
	}
	char* end = nullptr;
	errno     = 0;
	const long long parsed = std::strtoll(text.c_str(), &end, 10);
	if (errno != 0 || end == nullptr || *end != '\0')
	{
		return false;
	}
	*value = parsed;
	return true;
}

bool ParseDouble(const std::string& text, double* value)
{
	if (text.empty())
	{
		return false;
	}
	char* end = nullptr;
	errno     = 0;
	const double parsed = std::strtod(text.c_str(), &end);
	if (errno != 0 || end == nullptr || *end != '\0' || !std::isfinite(parsed))
	{
		return false;
	}
	*value = parsed;
	return true;
}

bool ParseOptions(const std::vector<std::string>& args, const std::set<std::string>& flags, const std::set<std::string>& value_options,
                  ParsedOptions* out, std::string* error)
{
	for (size_t index = 0; index < args.size(); ++index)
	{
		const std::string& arg = args[index];
		if (arg.rfind("--", 0) != 0)
		{
			out->positionals.push_back(arg);
			continue;
		}
		const size_t      equals = arg.find('=');
		const std::string name   = arg.substr(0, equals);
		if (flags.count(name) != 0 && equals == std::string::npos)
		{
			out->values.emplace(name, "");
			continue;
		}
		if (value_options.count(name) == 0)
		{
			*error = "unknown option " + name;
			return false;
		}
		if (equals != std::string::npos)
		{
			out->values.emplace(name, arg.substr(equals + 1));
			continue;
		}
		if (index + 1 >= args.size())
		{
			*error = name + " needs a value";
			return false;
		}
		out->values.emplace(name, args[++index]);
	}
	return true;
}

std::string OptionValue(const ParsedOptions& options, const std::string& name, const std::string& fallback)
{
	const auto range = options.values.equal_range(name);
	// The last occurrence wins, as with argparse.
	return range.first == range.second ? fallback : std::prev(range.second)->second;
}

bool HasOption(const ParsedOptions& options, const std::string& name)
{
	return options.values.count(name) != 0;
}

std::string ResolvePath(const std::string& path, const std::string& base)
{
	namespace fs = std::filesystem;
	std::string expanded = path;
	if (expanded == "~" || expanded.rfind("~/", 0) == 0)
	{
		const char* home = std::getenv("HOME");
		if (home != nullptr && *home != '\0')
		{
			expanded = std::string(home) + expanded.substr(1);
		}
	}
	std::error_code ec;
	fs::path        resolved(expanded);
	if (resolved.is_relative())
	{
		resolved = (base.empty() ? fs::current_path(ec) : fs::path(base)) / resolved;
	}
	const fs::path canonical = fs::weakly_canonical(resolved, ec);
	return (ec ? resolved.lexically_normal() : canonical).string();
}

} // namespace Kyty::Agent::Runner
