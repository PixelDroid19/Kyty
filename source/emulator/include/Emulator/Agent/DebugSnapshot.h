#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_DEBUGSNAPSHOT_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_DEBUGSNAPSHOT_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {

struct DebugSnapshotParts
{
	uint64_t    event_seq_start = 0;
	uint64_t    event_seq_end   = 0;
	std::string status_json;
	std::string diagnostics_json;
	std::string threads_json;
	std::string sync_waits_json;
	std::string events_json;
	std::string last_error_json;
};

std::string BuildDebugSnapshotResult(const DebugSnapshotParts& parts);

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_DEBUGSNAPSHOT_H_ */
