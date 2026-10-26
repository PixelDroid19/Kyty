#ifndef KYTY_AGENT_SRC_RUNNER_RUNNERSUPPORT_H_
#define KYTY_AGENT_SRC_RUNNER_RUNNERSUPPORT_H_

#include "Kyty/Agent/Runner/JsonValue.h"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Kyty::Agent::Runner {

// Text form of a JSON value as the runner reports have always written it:
// strings verbatim, integers in decimal, floats in shortest form, null as None.
[[nodiscard]] std::string ValueText(const Json& value);
// `first` when truthy, otherwise `second`.
[[nodiscard]] const Json& FirstTruthy(const Json& first, const Json& second);
// The "result" object of an agent reply, or the reply itself.
[[nodiscard]] const Json& ExtractResult(const Json& reply);

[[nodiscard]] std::string Strip(std::string_view text, std::string_view characters = " \t\r\n\v\f");
[[nodiscard]] std::string Lower(std::string text);
// At most `count` UTF-8 code points.
[[nodiscard]] std::string TruncateCodePoints(const std::string& text, size_t count);
[[nodiscard]] std::vector<std::string> SplitLines(const std::string& text);

[[nodiscard]] bool WriteTextFile(const std::string& path, const std::string& text, std::string* error);
[[nodiscard]] bool ReadTextFile(const std::string& path, size_t max_bytes, std::string* text, std::string* error);

// UTC wall-clock time formatted with strftime.
[[nodiscard]] std::string UtcTime(const char* format);

// Parses a whole decimal integer or float argument.
[[nodiscard]] bool ParseInt64(const std::string& text, int64_t* value);
[[nodiscard]] bool ParseDouble(const std::string& text, double* value);

struct ParsedOptions
{
	std::multimap<std::string, std::string> values; // "--name" -> value; flags map to ""
	std::vector<std::string>                positionals;
};

// Accepts `--name VALUE`, `--name=VALUE` for value options and bare flags.
[[nodiscard]] bool ParseOptions(const std::vector<std::string>& args, const std::set<std::string>& flags,
                                const std::set<std::string>& value_options, ParsedOptions* out, std::string* error);
[[nodiscard]] std::string OptionValue(const ParsedOptions& options, const std::string& name, const std::string& fallback = {});
[[nodiscard]] bool        HasOption(const ParsedOptions& options, const std::string& name);

// Expands a leading "~/" with HOME, then makes the path absolute and normal.
[[nodiscard]] std::string ResolvePath(const std::string& path, const std::string& base = {});

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_SRC_RUNNER_RUNNERSUPPORT_H_ */
