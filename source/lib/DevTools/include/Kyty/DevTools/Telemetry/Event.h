#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_EVENT_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_EVENT_H_

#include <cstdint>
#include <type_traits>

namespace Kyty::DevTools {

enum class Domain: uint16_t
{
	Unknown           = 0,
	GuestThread       = 1,
	Hle               = 2,
	CommandProcessor  = 3,
	Renderer          = 4,
	GpuQueue          = 5,
	VideoOut          = 6,
	Presentation      = 7,
	Synchronization   = 8,
	Count             = 9
};

enum class EventId: uint16_t
{
	Unknown            = 0,
	ThreadStart        = 1,
	ThreadExit         = 2,
	OperationSubmit    = 3,
	OperationProgress  = 4,
	OperationComplete  = 5,
	WaitBegin          = 6,
	WaitEnd            = 7,
	Count              = 8
};

enum class TimelineFlag: uint32_t
{
	CorrelationValid = 0x01u,
	Payload0Valid    = 0x02u,
	Payload1Valid    = 0x04u,
	Payload2Valid    = 0x08u,
	Payload3Valid    = 0x10u,
	ResultError      = 0x20u,
	TimedWait        = 0x40u
};

enum class ThreadRole: uint32_t
{
	Unknown            = 0,
	MainGuest          = 1,
	GuestPthread       = 2,
	GraphicsBatch      = 3,
	GraphicsDraw       = 4,
	GraphicsConstant   = 5,
	GraphicsCompute    = 6,
	GraphicsLabel      = 7,
	GraphicsRender     = 8,
	VideoOut           = 9,
	Presentation       = 10,
	SnapshotPublisher  = 11,
	Count              = 12
};

enum class ResultCategory: uint32_t
{
	Unknown      = 0,
	Success      = 1,
	GuestError   = 2,
	HostError    = 3,
	Timeout      = 4,
	Cancelled    = 5,
	Unsupported  = 6,
	Count        = 7
};

enum class WaitOutcome: uint32_t
{
	Unknown    = 0,
	Satisfied  = 1,
	TimedOut   = 2,
	Cancelled  = 3,
	Error      = 4,
	Count      = 5
};

enum class HleCallKind: uint32_t
{
	Unknown            = 0,
	WaitEventFlag      = 1,
	SetEventFlag       = 2,
	WaitSemaphore      = 3,
	SignalSemaphore    = 4,
	WaitEventQueue     = 5,
	TriggerEventQueue  = 6,
	Count              = 7
};

enum class OperationCode: uint32_t
{
	Unknown                = 0,
	ExecutionBoundary      = 0x0101u,
	HleCall                = 0x0201u,
	CommandSubmit          = 0x0301u,
	CommandPacket          = 0x0302u,
	RenderFrame            = 0x0401u,
	RenderDraw             = 0x0402u,
	RenderDispatch         = 0x0403u,
	RenderCommandBuffer    = 0x0404u,
	QueueSubmit            = 0x0501u,
	FenceWait              = 0x0502u,
	FencePoll              = 0x0503u,
	LabelPoll              = 0x0504u,
	VideoOutFlip           = 0x0601u,
	PresentationAcquire    = 0x0701u,
	PresentationPresent    = 0x0702u,
	SwapchainRecreate      = 0x0703u,
	PresentationDeviceIdle = 0x0704u,
	EventFlagWait          = 0x0801u,
	SemaphoreWait          = 0x0802u,
	EventQueueWait         = 0x0803u,
	RegisterMemoryWait     = 0x0804u
};

struct WriterKey
{
	uint32_t slot       = 0;
	uint32_t generation = 0;
	[[nodiscard]] constexpr uint64_t Pack() const noexcept
	{
		return (uint64_t {generation} << 32u) | slot;
	}
};

struct EventRecord
{
	uint64_t sequence     = 0;
	uint64_t monotonic_ns = 0;
	uint64_t writer_key   = 0;
	uint16_t domain       = 0;
	uint16_t event        = 0;
	uint32_t flags        = 0;
	uint64_t correlation  = 0;
	uint64_t payload[4]   = {};
};

static_assert(sizeof(EventRecord) == 72);
static_assert(std::is_trivially_copyable_v<EventRecord>);

[[nodiscard]] bool ValidateEventSchema(const EventRecord& record) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TELEMETRY_EVENT_H_ */
