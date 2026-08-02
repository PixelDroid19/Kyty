#include "Emulator/Agent/DebugSnapshot.h"

#include "Kyty/Agent/WireContract.h"

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

	// The transport appends one newline to every response line.
	if (out.size() + 1 > Kyty::Agent::kResponseLineMax)
	{
		return {};
	}
	return out;
}

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED
