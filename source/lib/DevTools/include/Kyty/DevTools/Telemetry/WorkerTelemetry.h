#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_WORKERTELEMETRY_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_WORKERTELEMETRY_H_

#include "Kyty/DevTools/Protocol/Protocol.h"

#include <cstdint>
#include <memory>

namespace Kyty::DevTools {

struct WorkerTelemetryOptions
{
	uint64_t         worker_pid                 = 0;
	uint64_t         worker_start_token         = 0;
	RecordingMode    requested_mode             = RecordingMode::MetricsOnly;
	LoggingMode      logging_mode               = LoggingMode::Silent;
	ShaderCacheState shader_cache_state         = ShaderCacheState::Unknown;
	const char*      revision                   = nullptr;
	uint8_t          dirty                      = 0;
	uint8_t          validation_enabled         = 0;
	uint32_t         resolution_width           = 0;
	uint32_t         resolution_height          = 0;
	uint64_t         capabilities[2]            = {};
	uint64_t         diagnostic_thread_instance = 0;
};

// Single-owner telemetry bridge used by a worker process. It keeps the
// protocol publication and its bounded in-process registries together so a
// disabled worker has no diagnostic state or side effects.
class WorkerTelemetry
{
public:
	WorkerTelemetry() noexcept;
	~WorkerTelemetry();
	WorkerTelemetry(const WorkerTelemetry&)            = delete;
	WorkerTelemetry& operator=(const WorkerTelemetry&) = delete;

	[[nodiscard]] bool Start(MutableMappingView mapping, const WorkerTelemetryOptions& options) noexcept;
	[[nodiscard]] bool Record(EventRecord record) noexcept;
	[[nodiscard]] bool Publish() noexcept;
	[[nodiscard]] bool Stop() noexcept;
	[[nodiscard]] bool Active() const noexcept;

private:
	struct State;
	std::unique_ptr<State> state_;
};

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_WORKERTELEMETRY_H_ */
