#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_PROTOCOL_PROTOCOL_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_PROTOCOL_PROTOCOL_H_

#include "Kyty/DevTools/Diagnostics/StallClassifier.h"
#include "Kyty/DevTools/Telemetry/Progress.h"
#include "Kyty/DevTools/Telemetry/WriterRegistry.h"

#include <cstdint>

namespace Kyty::DevTools {

// Fixed shared-memory layout (v1). Offsets are little-endian wire positions.
inline constexpr uint64_t kProtocolMappingSize  = 0x141000ull;
inline constexpr uint32_t kProtocolHeaderSize   = 0x1000u;
inline constexpr uint32_t kProtocolMajor        = 1u;
inline constexpr uint32_t kProtocolMinor        = 0u;
inline constexpr uint32_t kProtocolByteOrderTag = 0x01020304u;
inline constexpr uint32_t kProtocolWordSize     = 8u;
// Magic "KYTYDVT1" as little-endian u64 of those ASCII bytes.
inline constexpr uint64_t kProtocolMagic = 0x315456445954594Bull; // 'K''Y''T''Y''D''V''T''1' LE
inline constexpr uint64_t kProtocolHandshakeStateOffset = 0x100ull;

inline constexpr uint32_t kProgressSchemaId = 0x31475250u; // 'PRG1'
inline constexpr uint32_t kTimelineSchemaId = 0x314e4c54u; // 'TLN1'
inline constexpr uint32_t kMeasureSchemaId  = 0x3154454du; // 'MET1'
inline constexpr uint32_t kGpuFaultSchemaId = 0x31465047u; // 'GPF1'

inline constexpr uint64_t kProgressBufferAOffset = 0x001000ull;
inline constexpr uint64_t kProgressBufferBOffset = 0x021000ull;
inline constexpr uint64_t kProgressBufferSize    = 0x020000ull;
inline constexpr uint64_t kTimelineBufferAOffset = 0x041000ull;
inline constexpr uint64_t kTimelineBufferBOffset = 0x0c1000ull;
inline constexpr uint64_t kTimelineBufferSize    = 0x080000ull;

inline constexpr uint32_t kEventRecordWireSize    = 72u;
inline constexpr uint32_t kTimelineMaxEvents      = 4096u;
inline constexpr uint32_t kTimelineMaxPayloadSize = kTimelineMaxEvents * kEventRecordWireSize; // 0x48000

// Header control cells start at 0x300, 8 cells × 64 bytes.
inline constexpr uint64_t kControlCellBase = 0x300ull;
inline constexpr uint64_t kControlCellSize = 0x40ull;

enum class RecordingMode: uint32_t
{
	MetricsOnly = 1,
	Full        = 2
};

enum class HandshakeState: uint32_t
{
	Uninitialized = 0,
	ParentReady   = 1,
	WorkerReady   = 2,
	WorkerClosing = 3,
	WorkerRejected = 4
};

enum class LoggingMode: uint32_t
{
	Unknown   = 0,
	Silent    = 1,
	Console   = 2,
	File      = 3,
	Directory = 4
};

enum class ShaderCacheState: uint32_t
{
	Unknown                 = 0,
	NoPersistentCache       = 1,
	PersistentCacheCold     = 2,
	PersistentCacheWarm     = 3,
	PersistentCacheDisabled = 4
};

enum class ProtocolResult: uint8_t
{
	Ok              = 0,
	InvalidArgument = 1,
	Incompatible    = 2,
	InvalidLayout   = 3,
	Corrupt         = 4,
	Busy            = 5,
	Rejected        = 6
};

enum class ControlCell: uint8_t
{
	AggregateRing          = 0,
	UnregisteredWriters    = 1,
	RegistrationCapacity   = 2,
	InstanceCapacity       = 3,
	SkippedPublications    = 4,
	Disconnects            = 5,
	RejectedSamples        = 6,
	InactiveTokenAttempts  = 7
};

enum class GpuFaultState: uint32_t
{
	CapabilityPending    = 0,
	NotObserved          = 1,
	ExtensionUnavailable = 2,
	CountsSucceeded      = 3,
	CountsFailed         = 4
};

inline constexpr int32_t GpuFaultResultSuccess    = 0;
inline constexpr int32_t GpuFaultResultDeviceLost = -4;

struct MeasurementSnapshot
{
	uint32_t mode                     = 0;
	uint32_t flags                    = 0;
	uint64_t frame_count              = 0;
	uint64_t completed_flip_count     = 0;
	uint64_t first_presentation_ns    = 0;
	uint64_t last_presentation_ns     = 0;
	uint64_t overflow_count           = 0;
	uint64_t frame_interval_ms[1024]  = {};
};

struct GpuFaultSnapshot
{
	GpuFaultState state               = GpuFaultState::NotObserved;
	uint32_t      flags               = 0;
	int32_t       device_lost_result  = 0;
	int32_t       query_result        = 0;
	uint64_t      capture_monotonic_ns = 0;
	uint64_t      gpu_submission_id   = 0;
	uint32_t      address_info_count  = 0;
	uint32_t      vendor_info_count   = 0;
	uint64_t      vendor_binary_size  = 0;
};

struct ProtocolHealthSnapshot
{
	GlobalLossCounter aggregate_ring {};
	GlobalLossCounter unregistered_writers {};
	GlobalLossCounter registration_capacity {};
	GlobalLossCounter instance_capacity {};
	GlobalLossCounter skipped_publications {};
	GlobalLossCounter disconnects {};
	GlobalLossCounter rejected_samples {};
	GlobalLossCounter inactive_token_attempts {};
	uint64_t          max_loss_monotonic_ns = 0;
};

struct ParentProtocolInit
{
	uint64_t      supervisor_pid         = 0;
	uint64_t      supervisor_start_token = 0;
	uint8_t       nonce[16]              = {};
	RecordingMode requested_mode         = RecordingMode::MetricsOnly;
};

struct WorkerHandshake
{
	uint64_t         worker_pid           = 0;
	uint64_t         worker_start_token   = 0;
	uint8_t          nonce[16]            = {};
	RecordingMode    accepted_mode        = RecordingMode::MetricsOnly;
	LoggingMode      logging_mode         = LoggingMode::Silent;
	ShaderCacheState shader_cache_state   = ShaderCacheState::Unknown;
	char             revision[40]         = {};
	uint32_t         dirty                = 0;
	uint32_t         validation_enabled   = 0;
	uint32_t         resolution_width     = 0;
	uint32_t         resolution_height    = 0;
	uint64_t         capabilities[2]      = {};
};

struct ProgressPublication
{
	ProgressSnapshot         progress {};
	WriterLossSnapshot       writer_loss {};
	WriterInventorySnapshot  writer_inventory {};
	ProgressLossSnapshot     progress_loss {};
	MeasurementSnapshot      measurement {};
	GpuFaultSnapshot         gpu_fault {};
};

struct TimelineSnapshot
{
	EventRecord events[4096] {};
	uint32_t    count     = 0;
	uint32_t    reserved  = 0;
	uint64_t    generation = 0;
};

struct MutableMappingView
{
	uint8_t* data = nullptr;
	uint64_t size = 0;
};

struct ConstMappingView
{
	const uint8_t* data = nullptr;
	uint64_t       size = 0;
};

struct ProtocolReadLossState
{
	GlobalLossCounter rejected_samples {};
};

[[nodiscard]] bool ValidateGpuFaultSnapshot(const GpuFaultSnapshot& snap) noexcept;

[[nodiscard]] ProtocolResult InitializeProtocolOwner(MutableMappingView mapping,
                                                     const ParentProtocolInit& init) noexcept;
[[nodiscard]] ProtocolResult PublishWorkerHandshake(MutableMappingView mapping,
                                                    const WorkerHandshake& handshake) noexcept;
[[nodiscard]] ProtocolResult AcceptWorkerHandshake(MutableMappingView mapping,
                                                   const ParentProtocolInit& init,
                                                   WorkerHandshake* out) noexcept;
[[nodiscard]] ProtocolResult CloseWorkerHandshake(MutableMappingView mapping) noexcept;
[[nodiscard]] ProtocolResult ReadWorkerBootstrap(ConstMappingView mapping, const uint8_t* nonce,
                                                 RecordingMode* requested_mode) noexcept;
[[nodiscard]] ProtocolResult PublishProgress(MutableMappingView mapping,
                                             const ProgressPublication& publication) noexcept;
[[nodiscard]] ProtocolResult PublishTimeline(MutableMappingView mapping,
                                             const TimelineSnapshot& timeline) noexcept;
[[nodiscard]] ProtocolResult ReadProgressPublication(ConstMappingView mapping,
                                                     ProtocolReadLossState* loss_state,
                                                     ProgressPublication* out) noexcept;
[[nodiscard]] ProtocolResult ReadTimeline(ConstMappingView mapping, ProtocolReadLossState* loss_state,
                                          TimelineSnapshot* out) noexcept;
[[nodiscard]] ProtocolResult ReadProtocolHealth(ConstMappingView mapping,
                                                ProtocolHealthSnapshot* out) noexcept;
[[nodiscard]] ProtocolResult PublishWorkerControl(MutableMappingView mapping, ControlCell cell,
                                                  const GlobalLossCounter& counter) noexcept;

// Layout helpers for tests / supervisor.
[[nodiscard]] uint64_t ProtocolMagicAt(const uint8_t* data) noexcept;
[[nodiscard]] uint16_t ProtocolMajorAt(const uint8_t* data) noexcept;
[[nodiscard]] uint16_t ProtocolMinorAt(const uint8_t* data) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_PROTOCOL_PROTOCOL_H_ */
