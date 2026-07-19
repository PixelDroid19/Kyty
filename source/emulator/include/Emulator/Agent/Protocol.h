#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_PROTOCOL_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_PROTOCOL_H_

#include "Kyty/Agent/WireContract.h"
#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DomainResult.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/ModuleLoad.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <cstdint>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
struct DebugStatsPerformanceSnapshot;
} // namespace Kyty::Libs::Graphics

namespace Kyty::Emulator::Agent {

enum class Tool
{
	Unknown,
	Help,
	Ping,
	Status,
	Diagnostics,
	PerfSnapshot,
	SyncWaits,
	Threads,
	Events,
	LastError,
	Capture,
	Score,
	PadDown,
	PadUp,
	PadTap,
	PadAxis,
	PadClear,
	WaitPresent,
	WaitFrame,
	WaitPhase,
	WaitEvent,
	Watch,
};

[[nodiscard]] Tool ParseTool(const char* name) noexcept;

struct Request
{
	uint64_t    id   = 0;
	Tool        kind = Tool::Unknown;
	std::string tool;
	std::string args_json; // raw object body, may be "{}"
};

struct ErrorInfo
{
	std::string code;
	std::string message;
};

// Typed validate+parse (no mutation beyond filling *out on success).
// Structural JSON issues → malformed; unknown tool / pad policy → typed codes.
[[nodiscard]] Core::Domain::ValidationResult ParseAndValidateRequestLine(const char* line, Request* out);

// Adapter for call sites that still use ErrorInfo bags.
bool        ParseRequestLine(const char* line, Request* out, ErrorInfo* error);
std::string FormatOk(uint64_t id, const std::string& result_json_object);
std::string FormatErr(uint64_t id, const char* code, const char* message);

// Extract helpers from a flat args object (string/number/bool only).
bool ArgsGetString(const std::string& args_json, const char* key, std::string* out);
bool ArgsGetU64(const std::string& args_json, const char* key, uint64_t* out);
bool ArgsGetU32(const std::string& args_json, const char* key, uint32_t* out);
bool ArgsGetBool(const std::string& args_json, const char* key, bool* out);

void AppendGpuMemoryPerformanceJson(const Libs::Graphics::DebugStatsPerformanceSnapshot& performance, std::string* out);

std::string BuildDiagnosticsResult(const Core::BringUp::Config& config, const Core::BringUp::Diagnostics& diagnostics,
                                   const Loader::MissingImportDiagnostics& imports, const Loader::ModuleLoadPlanDiagnostics& load_plan);

std::string JsonEscape(const char* value);
std::string JsonString(const char* value);

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_PROTOCOL_H_ */
