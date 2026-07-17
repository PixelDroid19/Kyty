#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TRANSPORT_WORKERSESSION_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TRANSPORT_WORKERSESSION_H_

#include "Kyty/DevTools/Telemetry/WorkerTelemetry.h"

#include <cstdint>
#include <memory>

namespace Kyty::DevTools {

enum class WorkerSessionResult: uint8_t
{
	Attached          = 0,
	MissingBootstrap  = 1,
	MalformedBootstrap = 2,
	MappingFailed     = 3,
	ProtocolRejected  = 4,
	AlreadyActive     = 5
};

// Owns the host mapping inherited from kyty_devtools and delegates all
// protocol publication to WorkerTelemetry. Missing bootstrap is the explicit
// standalone mode; every present-but-invalid bootstrap fails closed.
class WorkerSession
{
public:
	WorkerSession() noexcept;
	~WorkerSession();
	WorkerSession(const WorkerSession&)            = delete;
	WorkerSession& operator=(const WorkerSession&) = delete;

	[[nodiscard]] WorkerSessionResult StartFromBootstrap(const char* value,
	                                                    const WorkerTelemetryOptions& options) noexcept;
	[[nodiscard]] bool Record(EventRecord record) noexcept;
	[[nodiscard]] bool Publish() noexcept;
	[[nodiscard]] bool Stop() noexcept;
	[[nodiscard]] bool Active() const noexcept;

private:
	struct State;
	static void CloseWorkerHandles(State* state) noexcept;
	static void UnmapWorkerMapping(State* state) noexcept;
	std::unique_ptr<State> state_;
};

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TRANSPORT_WORKERSESSION_H_ */
