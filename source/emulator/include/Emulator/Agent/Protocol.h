#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_PROTOCOL_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_PROTOCOL_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {

inline constexpr uint32_t kAgentProtocolVersion = 1u;
inline constexpr uint32_t kAgentArgsJsonMax     = 1024u;
inline constexpr uint32_t kAgentLineMax         = 4096u;

struct Request
{
	uint64_t    id = 0;
	std::string tool;
	std::string args_json; // raw object body, may be "{}"
};

struct ErrorInfo
{
	std::string code;
	std::string message;
};

// Minimal JSON-lines helpers. Not a general JSON library.
bool ParseRequestLine(const char* line, Request* out, ErrorInfo* error);
std::string FormatOk(uint64_t id, const std::string& result_json_object);
std::string FormatErr(uint64_t id, const char* code, const char* message);

// Extract helpers from a flat args object (string/number/bool only).
bool ArgsGetString(const std::string& args_json, const char* key, std::string* out);
bool ArgsGetU64(const std::string& args_json, const char* key, uint64_t* out);
bool ArgsGetU32(const std::string& args_json, const char* key, uint32_t* out);
bool ArgsGetBool(const std::string& args_json, const char* key, bool* out);

std::string JsonEscape(const char* value);
std::string JsonString(const char* value);

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_PROTOCOL_H_ */
