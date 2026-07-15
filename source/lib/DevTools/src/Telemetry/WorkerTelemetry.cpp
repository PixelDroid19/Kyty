#include "Kyty/DevTools/Telemetry/WorkerTelemetry.h"

#include "Kyty/DevTools/Telemetry/ThreadInstanceAllocator.h"
#include "Kyty/DevTools/Time/MonotonicClock.h"

#include <cstring>
#include <new>

namespace Kyty::DevTools {
namespace {

constexpr ThreadRole kWorkerRole = ThreadRole::MainGuest;

[[nodiscard]] EventRecord MakeThreadStart(uint64_t instance) noexcept
{
	EventRecord event {};
	event.domain      = static_cast<uint16_t>(Domain::GuestThread);
	event.event       = static_cast<uint16_t>(EventId::ThreadStart);
	event.flags       = static_cast<uint32_t>(TimelineFlag::CorrelationValid) |
	               static_cast<uint32_t>(TimelineFlag::Payload0Valid);
	event.correlation = instance;
	event.payload[0]  = static_cast<uint64_t>(kWorkerRole);
	return event;
}

[[nodiscard]] EventRecord MakeThreadExit(uint64_t instance) noexcept
{
	EventRecord event {};
	event.domain      = static_cast<uint16_t>(Domain::GuestThread);
	event.event       = static_cast<uint16_t>(EventId::ThreadExit);
	event.flags       = static_cast<uint32_t>(TimelineFlag::CorrelationValid) |
	               static_cast<uint32_t>(TimelineFlag::Payload0Valid) |
	               static_cast<uint32_t>(TimelineFlag::Payload1Valid);
	event.correlation = instance;
	event.payload[0]  = static_cast<uint64_t>(kWorkerRole);
	event.payload[1]  = static_cast<uint64_t>(ResultCategory::Success);
	return event;
}

void CopyRevision(const char* revision, char* out, uint32_t capacity) noexcept
{
	if (out == nullptr || capacity == 0u)
	{
		return;
	}
	out[0] = '\0';
	if (revision == nullptr)
	{
		return;
	}

	uint32_t written = 0;
	for (const char* p = revision; *p != '\0' && written < capacity; ++p)
	{
		const char c = *p;
		const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
		                     c == '_' || c == '-';
		if (!allowed)
		{
			break;
		}
		out[written++] = c;
	}
	out[written] = '\0';
}

} // namespace

struct WorkerTelemetry::State
{
	MutableMappingView    mapping {};
	WriterRegistry        writers {};
	ProgressRegistry      progress {};
	ThreadInstanceAllocator thread_instances {};
	TelemetryWriterToken  writer_token {};
	RecordingMode         mode = RecordingMode::MetricsOnly;
	uint64_t              thread_instance = 0;
	uint64_t              publication_generation = 0;
	bool                  active = false;
};

WorkerTelemetry::WorkerTelemetry() noexcept : state_(new (std::nothrow) State) {}

WorkerTelemetry::~WorkerTelemetry() = default;

bool WorkerTelemetry::Start(MutableMappingView mapping, const WorkerTelemetryOptions& options) noexcept
{
	if (!state_ || state_->active || mapping.data == nullptr || mapping.size < kProtocolMappingSize ||
	    (options.requested_mode != RecordingMode::MetricsOnly && options.requested_mode != RecordingMode::Full) ||
	    options.logging_mode == LoggingMode::Unknown || options.logging_mode > LoggingMode::Directory ||
		options.dirty > 1u || options.validation_enabled > 1u ||
	    (options.requested_mode == RecordingMode::Full && options.diagnostic_thread_instance > ThreadInstanceAllocator::kMaxInstance))
	{
		return false;
	}

	uint64_t thread_instance = options.diagnostic_thread_instance;
	if (options.requested_mode == RecordingMode::Full && thread_instance == 0u && !state_->thread_instances.Allocate(&thread_instance))
	{
		return false;
	}

	WorkerHandshake handshake {};
	handshake.worker_pid          = options.worker_pid;
	handshake.worker_start_token  = options.worker_start_token;
	handshake.accepted_mode      = options.requested_mode;
	handshake.logging_mode       = options.logging_mode;
	handshake.shader_cache_state = options.shader_cache_state;
	handshake.dirty              = options.dirty;
	handshake.validation_enabled = options.validation_enabled;
	handshake.resolution_width   = options.resolution_width;
	handshake.resolution_height  = options.resolution_height;
	handshake.capabilities[0]   = options.capabilities[0];
	handshake.capabilities[1]   = options.capabilities[1];
	CopyRevision(options.revision, handshake.revision, sizeof(handshake.revision));

	if (PublishWorkerHandshake(mapping, handshake) != ProtocolResult::Ok)
	{
		return false;
	}

	state_->mapping         = mapping;
	state_->mode            = options.requested_mode;
	state_->thread_instance = thread_instance;
	state_->active          = true;

	if (state_->mode == RecordingMode::MetricsOnly)
	{
		if (!Publish())
		{
			state_->mapping = {};
			state_->active  = false;
			return false;
		}
		return true;
	}

	TelemetryWriterToken token {};
	if (!state_->writers.Reserve(kWorkerRole, thread_instance, &token))
	{
		state_->mapping = {};
		state_->active  = false;
		return false;
	}
	if (!state_->writers.Activate(token))
	{
		state_->writers.Abandon(token);
		state_->mapping = {};
		state_->active  = false;
		return false;
	}

	state_->writer_token = token;
	if (!Record(MakeThreadStart(state_->thread_instance)) || !Publish())
	{
		state_->writers.Close(token, MakeThreadExit(state_->thread_instance));
		EventRecord discarded[kTimelineMaxEvents] {};
		(void)state_->writers.Drain(discarded, kTimelineMaxEvents);
		state_->mapping  = {};
		state_->active   = false;
		return false;
	}
	return true;
}

bool WorkerTelemetry::Record(EventRecord record) noexcept
{
	if (!state_ || !state_->active)
	{
		return false;
	}
	if (state_->mode == RecordingMode::MetricsOnly)
	{
		return true;
	}
	return state_->writers.TryRecord(state_->writer_token, record);
}

bool WorkerTelemetry::Publish() noexcept
{
	if (!state_ || !state_->active || state_->mapping.data == nullptr)
	{
		return false;
	}

	EventRecord drained[kTimelineMaxEvents] {};
	(void)state_->writers.Drain(drained, kTimelineMaxEvents);

	ProgressPublication publication {};
	if (!state_->progress.SnapshotAll(&publication.progress) ||
	    !state_->writers.SnapshotInventory(&publication.writer_inventory))
	{
		return false;
	}
	publication.writer_loss  = state_->writers.SnapshotLoss();
	publication.progress_loss = state_->progress.SnapshotLoss();
	publication.gpu_fault    = {};
	if (PublishProgress(state_->mapping, publication) != ProtocolResult::Ok)
	{
		return false;
	}
	if (state_->mode == RecordingMode::MetricsOnly)
	{
		return true;
	}

	TimelineSnapshot timeline {};
	timeline.count      = state_->writers.History().SnapshotNewest(timeline.events, kTimelineMaxEvents);
	timeline.generation = ++state_->publication_generation;
	return PublishTimeline(state_->mapping, timeline) == ProtocolResult::Ok;
}

bool WorkerTelemetry::Stop() noexcept
{
	if (!state_ || !state_->active)
	{
		return false;
	}

	if (state_->mode == RecordingMode::Full)
	{
		state_->writers.Close(state_->writer_token, MakeThreadExit(state_->thread_instance));
	}
	const bool published = Publish();
	state_->active        = false;
	state_->mapping       = {};
	return published;
}

bool WorkerTelemetry::Active() const noexcept
{
	return state_ != nullptr && state_->active;
}

} // namespace Kyty::DevTools
