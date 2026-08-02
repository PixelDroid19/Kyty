#include "Emulator/Agent/DebugSnapshot.h"

#include "Kyty/Agent/WireContract.h"

#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {
namespace {

void AppendComponent(std::string* out, const char* name, const std::string& value)
{
	*out += ',';
	*out += '"';
	*out += name;
	*out += "\":";
	*out += value.empty() ? "null" : value;
}

} // namespace

std::string BuildDebugSnapshotResult(const DebugSnapshotParts& parts)
{
	std::string out;
	out.reserve(512);
	out += "{\"protocol_version\":";
	out += std::to_string(Kyty::Agent::kProtocolVersion);
	out += ",\"schema\":\"debug_snapshot\",\"event_seq_start\":";
	out += std::to_string(parts.event_seq_start);
	out += ",\"event_seq_end\":";
	out += std::to_string(parts.event_seq_end);
	out += ",\"stable\":";
	out += parts.event_seq_start == parts.event_seq_end ? "true" : "false";
	AppendComponent(&out, "status", parts.status_json);
	AppendComponent(&out, "diagnostics", parts.diagnostics_json);
	AppendComponent(&out, "threads", parts.threads_json);
	AppendComponent(&out, "sync_waits", parts.sync_waits_json);
	AppendComponent(&out, "events", parts.events_json);
	AppendComponent(&out, "last_error", parts.last_error_json);
	out += '}';

	// FormatOk adds this envelope prefix, a closing brace, and the transport
	// appends one newline. Reserve the worst request id so the complete wire
	// line remains bounded regardless of the caller's id.
	const std::string envelope_prefix =
	    "{\"id\":" + std::to_string(std::numeric_limits<uint64_t>::max()) +
	    ",\"ok\":true,\"protocol_version\":" + std::to_string(Kyty::Agent::kProtocolVersion) + ",\"result\":";
	if (envelope_prefix.size() + out.size() + 2 > Kyty::Agent::kResponseLineMax)
	{
		return {};
	}
	return out;
}

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED
