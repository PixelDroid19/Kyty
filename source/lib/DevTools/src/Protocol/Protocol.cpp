#include "Kyty/DevTools/Protocol/Protocol.h"

#include "Kyty/DevTools/Diagnostics/Checksum.h"

#include <atomic>
#include <cstring>

namespace Kyty::DevTools {
namespace {

// Descriptor generation at end of header (owner-local double buffer index).
// Wire: offset 0x0f0 active buffer selector (0/1), 0x0f8 generation.
constexpr uint64_t kOffMagic        = 0x000;
constexpr uint64_t kOffMajor        = 0x008;
constexpr uint64_t kOffMinor        = 0x00a;
constexpr uint64_t kOffHeaderSize   = 0x00c;
constexpr uint64_t kOffTotalSize    = 0x010;
constexpr uint64_t kOffByteOrder    = 0x018;
constexpr uint64_t kOffWordSize     = 0x01c;
constexpr uint64_t kOffParentPid    = 0x020;
constexpr uint64_t kOffParentToken  = 0x028;
constexpr uint64_t kOffNonce        = 0x030;
constexpr uint64_t kOffReqMode      = 0x0d4;
constexpr uint64_t kOffAcceptedMode = 0x0d8;
constexpr uint64_t kOffActiveBuf    = 0x0f0;
constexpr uint64_t kOffGeneration   = 0x0f8;

void WriteU16(uint8_t* base, uint64_t off, uint16_t v) noexcept
{
	std::memcpy(base + off, &v, sizeof(v));
}
void WriteU32(uint8_t* base, uint64_t off, uint32_t v) noexcept
{
	std::memcpy(base + off, &v, sizeof(v));
}
void WriteU64(uint8_t* base, uint64_t off, uint64_t v) noexcept
{
	std::memcpy(base + off, &v, sizeof(v));
}
uint16_t ReadU16(const uint8_t* base, uint64_t off) noexcept
{
	uint16_t v = 0;
	std::memcpy(&v, base + off, sizeof(v));
	return v;
}
uint32_t ReadU32(const uint8_t* base, uint64_t off) noexcept
{
	uint32_t v = 0;
	std::memcpy(&v, base + off, sizeof(v));
	return v;
}
uint64_t ReadU64(const uint8_t* base, uint64_t off) noexcept
{
	uint64_t v = 0;
	std::memcpy(&v, base + off, sizeof(v));
	return v;
}

[[nodiscard]] bool MappingOk(const uint8_t* data, uint64_t size) noexcept
{
	return data != nullptr && size >= kProtocolMappingSize;
}

[[nodiscard]] ProtocolResult ValidateHeader(const uint8_t* data) noexcept
{
	if (ReadU64(data, kOffMagic) != kProtocolMagic)
	{
		return ProtocolResult::InvalidLayout;
	}
	if (ReadU16(data, kOffMajor) != kProtocolMajor || ReadU16(data, kOffMinor) != kProtocolMinor)
	{
		return ProtocolResult::Incompatible;
	}
	if (ReadU32(data, kOffHeaderSize) != kProtocolHeaderSize || ReadU64(data, kOffTotalSize) != kProtocolMappingSize)
	{
		return ProtocolResult::InvalidLayout;
	}
	if (ReadU32(data, kOffByteOrder) != kProtocolByteOrderTag || ReadU32(data, kOffWordSize) != kProtocolWordSize)
	{
		return ProtocolResult::InvalidLayout;
	}
	return ProtocolResult::Ok;
}

// Progress payload: [u32 schema][u32 flags][u64 gen][u32 count][u32 reserved]
// then ProgressPublication bytes + trailing CRC64 of payload without CRC field.
constexpr uint64_t kProgSchemaOff = 0;
constexpr uint64_t kProgFlagsOff  = 4;
constexpr uint64_t kProgGenOff    = 8;
constexpr uint64_t kProgCountOff  = 16;
constexpr uint64_t kProgBodyOff   = 32;

// Timeline: [u32 schema][u32 flags][u64 gen][u32 count][u32 reserved][events...][crc]
constexpr uint64_t kTlSchemaOff = 0;
constexpr uint64_t kTlFlagsOff  = 4;
constexpr uint64_t kTlGenOff    = 8;
constexpr uint64_t kTlCountOff  = 16;
constexpr uint64_t kTlBodyOff   = 32;

void WriteControlCell(uint8_t* data, ControlCell cell, const GlobalLossCounter& c) noexcept
{
	const uint64_t off = kControlCellBase + static_cast<uint64_t>(cell) * kControlCellSize;
	WriteU64(data, off + 0x00, c.total);
	WriteU64(data, off + 0x08, c.last_loss_monotonic_ns);
	// remainder of cell remains zero from Initialize.
}

void ReadControlCell(const uint8_t* data, ControlCell cell, GlobalLossCounter* c) noexcept
{
	const uint64_t off = kControlCellBase + static_cast<uint64_t>(cell) * kControlCellSize;
	c->total                  = ReadU64(data, off + 0x00);
	c->last_loss_monotonic_ns = ReadU64(data, off + 0x08);
}

} // namespace

bool ValidateGpuFaultSnapshot(const GpuFaultSnapshot& snap) noexcept
{
	if (snap.vendor_binary_size != 0u)
	{
		return false;
	}
	switch (snap.state)
	{
		case GpuFaultState::CapabilityPending:
		case GpuFaultState::NotObserved:
		case GpuFaultState::ExtensionUnavailable:
			return snap.flags == 0u && snap.device_lost_result == 0 && snap.query_result == 0 &&
			       snap.capture_monotonic_ns == 0u && snap.gpu_submission_id == 0u && snap.address_info_count == 0u &&
			       snap.vendor_info_count == 0u;
		case GpuFaultState::CountsSucceeded:
			return (snap.flags & 1u) != 0u && snap.device_lost_result == GpuFaultResultSuccess &&
			       snap.capture_monotonic_ns != 0u;
		case GpuFaultState::CountsFailed:
			return (snap.flags & 1u) != 0u && snap.device_lost_result == GpuFaultResultDeviceLost &&
			       snap.capture_monotonic_ns != 0u;
		default: return false;
	}
}

uint64_t ProtocolMagicAt(const uint8_t* data) noexcept
{
	return data == nullptr ? 0 : ReadU64(data, kOffMagic);
}
uint16_t ProtocolMajorAt(const uint8_t* data) noexcept
{
	return data == nullptr ? 0 : ReadU16(data, kOffMajor);
}
uint16_t ProtocolMinorAt(const uint8_t* data) noexcept
{
	return data == nullptr ? 0 : ReadU16(data, kOffMinor);
}

ProtocolResult InitializeProtocolOwner(MutableMappingView mapping, const ParentProtocolInit& init) noexcept
{
	if (!MappingOk(mapping.data, mapping.size))
	{
		return ProtocolResult::InvalidArgument;
	}
	if (init.requested_mode != RecordingMode::MetricsOnly && init.requested_mode != RecordingMode::Full)
	{
		return ProtocolResult::InvalidArgument;
	}
	std::memset(mapping.data, 0, static_cast<size_t>(kProtocolMappingSize));
	WriteU64(mapping.data, kOffMagic, kProtocolMagic);
	WriteU16(mapping.data, kOffMajor, static_cast<uint16_t>(kProtocolMajor));
	WriteU16(mapping.data, kOffMinor, static_cast<uint16_t>(kProtocolMinor));
	WriteU32(mapping.data, kOffHeaderSize, kProtocolHeaderSize);
	WriteU64(mapping.data, kOffTotalSize, kProtocolMappingSize);
	WriteU32(mapping.data, kOffByteOrder, kProtocolByteOrderTag);
	WriteU32(mapping.data, kOffWordSize, kProtocolWordSize);
	WriteU64(mapping.data, kOffParentPid, init.supervisor_pid);
	WriteU64(mapping.data, kOffParentToken, init.supervisor_start_token);
	std::memcpy(mapping.data + kOffNonce, init.nonce, 16);
	WriteU32(mapping.data, kOffReqMode, static_cast<uint32_t>(init.requested_mode));
	WriteU32(mapping.data, kOffActiveBuf, 0);
	WriteU64(mapping.data, kOffGeneration, 0);
	return ProtocolResult::Ok;
}

ProtocolResult PublishWorkerHandshake(MutableMappingView mapping, const WorkerHandshake& handshake) noexcept
{
	if (!MappingOk(mapping.data, mapping.size))
	{
		return ProtocolResult::InvalidArgument;
	}
	const auto hdr = ValidateHeader(mapping.data);
	if (hdr != ProtocolResult::Ok)
	{
		return hdr;
	}
	if (handshake.accepted_mode != RecordingMode::MetricsOnly && handshake.accepted_mode != RecordingMode::Full)
	{
		return ProtocolResult::Rejected;
	}
	if (handshake.dirty > 1u || handshake.validation_enabled > 1u)
	{
		return ProtocolResult::Rejected;
	}
	// Store accepted mode; full handshake blob is beyond v1 minimal path.
	WriteU32(mapping.data, kOffAcceptedMode, static_cast<uint32_t>(handshake.accepted_mode));
	return ProtocolResult::Ok;
}

ProtocolResult AcceptWorkerHandshake(MutableMappingView mapping, const ParentProtocolInit& init,
                                     WorkerHandshake* out) noexcept
{
	if (!MappingOk(mapping.data, mapping.size) || out == nullptr)
	{
		return ProtocolResult::InvalidArgument;
	}
	const auto hdr = ValidateHeader(mapping.data);
	if (hdr != ProtocolResult::Ok)
	{
		return hdr;
	}
	if (ReadU64(mapping.data, kOffParentToken) != init.supervisor_start_token)
	{
		return ProtocolResult::Rejected;
	}
	if (std::memcmp(mapping.data + kOffNonce, init.nonce, 16) != 0)
	{
		return ProtocolResult::Rejected;
	}
	*out = {};
	out->accepted_mode =
	    static_cast<RecordingMode>(ReadU32(mapping.data, kOffAcceptedMode));
	if (out->accepted_mode != init.requested_mode)
	{
		return ProtocolResult::Rejected;
	}
	return ProtocolResult::Ok;
}

ProtocolResult ReadWorkerBootstrap(ConstMappingView mapping, const uint8_t* nonce, RecordingMode* requested_mode) noexcept
{
	if (!MappingOk(mapping.data, mapping.size) || nonce == nullptr || requested_mode == nullptr)
	{
		return ProtocolResult::InvalidArgument;
	}
	const auto hdr = ValidateHeader(mapping.data);
	if (hdr != ProtocolResult::Ok)
	{
		return hdr;
	}
	if (std::memcmp(mapping.data + kOffNonce, nonce, 16) != 0)
	{
		return ProtocolResult::Rejected;
	}
	const auto mode = static_cast<RecordingMode>(ReadU32(mapping.data, kOffReqMode));
	if (mode != RecordingMode::MetricsOnly && mode != RecordingMode::Full)
	{
		return ProtocolResult::Rejected;
	}
	*requested_mode = mode;
	return ProtocolResult::Ok;
}

ProtocolResult PublishProgress(MutableMappingView mapping, const ProgressPublication& publication) noexcept
{
	if (!MappingOk(mapping.data, mapping.size))
	{
		return ProtocolResult::InvalidArgument;
	}
	const auto hdr = ValidateHeader(mapping.data);
	if (hdr != ProtocolResult::Ok)
	{
		return hdr;
	}
	if (!ValidateGpuFaultSnapshot(publication.gpu_fault))
	{
		return ProtocolResult::Rejected;
	}

	const uint32_t active = ReadU32(mapping.data, kOffActiveBuf) & 1u;
	const uint32_t inactive = active ^ 1u;
	const uint64_t buf_off =
	    (inactive == 0u) ? kProgressBufferAOffset : kProgressBufferBOffset;
	uint8_t* buf = mapping.data + buf_off;
	std::memset(buf, 0, static_cast<size_t>(kProgressBufferSize));

	WriteU32(buf, kProgSchemaOff, kProgressSchemaId);
	WriteU32(buf, kProgFlagsOff, 0);
	const uint64_t gen = ReadU64(mapping.data, kOffGeneration) + 1u;
	WriteU64(buf, kProgGenOff, gen);
	const uint32_t count =
	    (publication.progress.count < MaxProgressSnapshotEntries) ? publication.progress.count : MaxProgressSnapshotEntries;
	WriteU32(buf, kProgCountOff, count);

	// Compact body (full C++ ProgressPublication exceeds 0x20000). Layout:
	// u32 count already in header; then entries[count], aggregates, measurement, gpu_fault.
	uint64_t cursor = kProgBodyOff;
	auto write_bytes = [&](const void* src, uint64_t n) -> bool {
		if (cursor + n + 8u > kProgressBufferSize)
		{
			return false;
		}
		std::memcpy(buf + cursor, src, static_cast<size_t>(n));
		cursor += n;
		return true;
	};
	if (count > 0u)
	{
		if (!write_bytes(publication.progress.entries, static_cast<uint64_t>(count) * sizeof(ProgressSnapshotEntry)))
		{
			return ProtocolResult::Rejected;
		}
	}
	// Writer loss aggregates + max (not full 512-slot table in this payload slice).
	if (!write_bytes(&publication.writer_loss.aggregate_ring, sizeof(GlobalLossCounter)) ||
	    !write_bytes(&publication.writer_loss.registration_capacity, sizeof(GlobalLossCounter)) ||
	    !write_bytes(&publication.writer_loss.inactive_writer_attempts, sizeof(GlobalLossCounter)) ||
	    !write_bytes(&publication.writer_loss.max_loss_monotonic_ns, sizeof(uint64_t)) ||
	    !write_bytes(&publication.progress_loss, sizeof(ProgressLossSnapshot)) ||
	    !write_bytes(&publication.measurement, sizeof(MeasurementSnapshot)) ||
	    !write_bytes(&publication.gpu_fault, sizeof(GpuFaultSnapshot)))
	{
		return ProtocolResult::Rejected;
	}
	const uint64_t crc_len = cursor;
	const uint64_t crc     = Crc64Ecma(buf, static_cast<size_t>(crc_len));
	WriteU64(buf, crc_len, crc);

	// Publish: generation then flip active buffer.
	WriteU64(mapping.data, kOffGeneration, gen);
	WriteU32(mapping.data, kOffActiveBuf, inactive);

	// Mirror child-owned health aggregates from writer_loss / progress_loss.
	WriteControlCell(mapping.data, ControlCell::AggregateRing, publication.writer_loss.aggregate_ring);
	WriteControlCell(mapping.data, ControlCell::RegistrationCapacity, publication.writer_loss.registration_capacity);
	WriteControlCell(mapping.data, ControlCell::InactiveTokenAttempts, publication.writer_loss.inactive_writer_attempts);
	return ProtocolResult::Ok;
}

ProtocolResult ReadProgressPublication(ConstMappingView mapping, ProtocolReadLossState* loss_state,
                                       ProgressPublication* out) noexcept
{
	if (!MappingOk(mapping.data, mapping.size) || out == nullptr)
	{
		return ProtocolResult::InvalidArgument;
	}
	const auto hdr = ValidateHeader(mapping.data);
	if (hdr != ProtocolResult::Ok)
	{
		return hdr;
	}
	const uint32_t active  = ReadU32(mapping.data, kOffActiveBuf) & 1u;
	const uint64_t gen_hdr = ReadU64(mapping.data, kOffGeneration);
	if (gen_hdr == 0u)
	{
		if (loss_state != nullptr)
		{
			loss_state->rejected_samples.total += 1u;
		}
		return ProtocolResult::Rejected;
	}
	const uint64_t buf_off = (active == 0u) ? kProgressBufferAOffset : kProgressBufferBOffset;
	const uint8_t* buf     = mapping.data + buf_off;
	if (ReadU32(buf, kProgSchemaOff) != kProgressSchemaId)
	{
		if (loss_state != nullptr)
		{
			loss_state->rejected_samples.total += 1u;
		}
		return ProtocolResult::Corrupt;
	}
	const uint64_t gen_buf = ReadU64(buf, kProgGenOff);
	if (gen_buf != gen_hdr)
	{
		if (loss_state != nullptr)
		{
			loss_state->rejected_samples.total += 1u;
		}
		return ProtocolResult::Busy;
	}
	*out              = {};
	out->progress.count = ReadU32(buf, kProgCountOff);
	if (out->progress.count > MaxProgressSnapshotEntries)
	{
		if (loss_state != nullptr)
		{
			loss_state->rejected_samples.total += 1u;
		}
		return ProtocolResult::Corrupt;
	}
	uint64_t cursor = kProgBodyOff;
	auto read_bytes = [&](void* dst, uint64_t n) -> bool {
		if (cursor + n + 8u > kProgressBufferSize)
		{
			return false;
		}
		std::memcpy(dst, buf + cursor, static_cast<size_t>(n));
		cursor += n;
		return true;
	};
	if (out->progress.count > 0u)
	{
		if (!read_bytes(out->progress.entries, static_cast<uint64_t>(out->progress.count) * sizeof(ProgressSnapshotEntry)))
		{
			if (loss_state != nullptr)
			{
				loss_state->rejected_samples.total += 1u;
			}
			return ProtocolResult::Corrupt;
		}
	}
	if (!read_bytes(&out->writer_loss.aggregate_ring, sizeof(GlobalLossCounter)) ||
	    !read_bytes(&out->writer_loss.registration_capacity, sizeof(GlobalLossCounter)) ||
	    !read_bytes(&out->writer_loss.inactive_writer_attempts, sizeof(GlobalLossCounter)) ||
	    !read_bytes(&out->writer_loss.max_loss_monotonic_ns, sizeof(uint64_t)) ||
	    !read_bytes(&out->progress_loss, sizeof(ProgressLossSnapshot)) ||
	    !read_bytes(&out->measurement, sizeof(MeasurementSnapshot)) ||
	    !read_bytes(&out->gpu_fault, sizeof(GpuFaultSnapshot)))
	{
		if (loss_state != nullptr)
		{
			loss_state->rejected_samples.total += 1u;
		}
		return ProtocolResult::Corrupt;
	}
	const uint64_t crc_len = cursor;
	const uint64_t expect  = Crc64Ecma(buf, static_cast<size_t>(crc_len));
	const uint64_t got     = ReadU64(buf, crc_len);
	if (expect != got)
	{
		if (loss_state != nullptr)
		{
			loss_state->rejected_samples.total += 1u;
		}
		return ProtocolResult::Corrupt;
	}
	if (!ValidateGpuFaultSnapshot(out->gpu_fault))
	{
		if (loss_state != nullptr)
		{
			loss_state->rejected_samples.total += 1u;
		}
		return ProtocolResult::Rejected;
	}
	return ProtocolResult::Ok;
}

ProtocolResult PublishTimeline(MutableMappingView mapping, const TimelineSnapshot& timeline) noexcept
{
	if (!MappingOk(mapping.data, mapping.size))
	{
		return ProtocolResult::InvalidArgument;
	}
	const auto hdr = ValidateHeader(mapping.data);
	if (hdr != ProtocolResult::Ok)
	{
		return hdr;
	}
	if (timeline.count > kTimelineMaxEvents)
	{
		return ProtocolResult::Rejected;
	}
	const uint32_t active   = ReadU32(mapping.data, kOffActiveBuf) & 1u;
	const uint32_t inactive = active ^ 1u;
	// Timeline uses same active index convention independently; store in buffer pair.
	const uint64_t buf_off =
	    (inactive == 0u) ? kTimelineBufferAOffset : kTimelineBufferBOffset;
	uint8_t* buf = mapping.data + buf_off;
	std::memset(buf, 0, static_cast<size_t>(kTimelineBufferSize));
	WriteU32(buf, kTlSchemaOff, kTimelineSchemaId);
	WriteU32(buf, kTlFlagsOff, 0);
	WriteU64(buf, kTlGenOff, timeline.generation);
	WriteU32(buf, kTlCountOff, timeline.count);
	const uint64_t bytes = static_cast<uint64_t>(timeline.count) * kEventRecordWireSize;
	if (bytes > kTimelineMaxPayloadSize)
	{
		return ProtocolResult::Rejected;
	}
	if (timeline.count > 0u)
	{
		std::memcpy(buf + kTlBodyOff, timeline.events, static_cast<size_t>(bytes));
	}
	const uint64_t crc_len = kTlBodyOff + bytes;
	WriteU64(buf, crc_len, Crc64Ecma(buf, static_cast<size_t>(crc_len)));
	// Note: does not flip progress active selector; timeline generation is self-contained.
	return ProtocolResult::Ok;
}

ProtocolResult ReadTimeline(ConstMappingView mapping, ProtocolReadLossState* loss_state,
                            TimelineSnapshot* out) noexcept
{
	if (!MappingOk(mapping.data, mapping.size) || out == nullptr)
	{
		return ProtocolResult::InvalidArgument;
	}
	const auto hdr = ValidateHeader(mapping.data);
	if (hdr != ProtocolResult::Ok)
	{
		return hdr;
	}
	// Read both buffers; pick the one with valid schema + CRC and highest generation.
	TimelineSnapshot best {};
	bool             found = false;
	for (uint32_t i = 0; i < 2u; ++i)
	{
		const uint64_t buf_off = (i == 0u) ? kTimelineBufferAOffset : kTimelineBufferBOffset;
		const uint8_t* buf     = mapping.data + buf_off;
		if (ReadU32(buf, kTlSchemaOff) != kTimelineSchemaId)
		{
			continue;
		}
		const uint32_t count = ReadU32(buf, kTlCountOff);
		if (count > kTimelineMaxEvents)
		{
			continue;
		}
		const uint64_t bytes   = static_cast<uint64_t>(count) * kEventRecordWireSize;
		const uint64_t crc_len = kTlBodyOff + bytes;
		if (ReadU64(buf, crc_len) != Crc64Ecma(buf, static_cast<size_t>(crc_len)))
		{
			continue;
		}
		TimelineSnapshot cur {};
		cur.generation = ReadU64(buf, kTlGenOff);
		cur.count      = count;
		if (count > 0u)
		{
			std::memcpy(cur.events, buf + kTlBodyOff, static_cast<size_t>(bytes));
		}
		if (!found || cur.generation >= best.generation)
		{
			best  = cur;
			found = true;
		}
	}
	if (!found)
	{
		if (loss_state != nullptr)
		{
			loss_state->rejected_samples.total += 1u;
		}
		return ProtocolResult::Rejected;
	}
	*out = best;
	return ProtocolResult::Ok;
}

ProtocolResult ReadProtocolHealth(ConstMappingView mapping, ProtocolHealthSnapshot* out) noexcept
{
	if (!MappingOk(mapping.data, mapping.size) || out == nullptr)
	{
		return ProtocolResult::InvalidArgument;
	}
	const auto hdr = ValidateHeader(mapping.data);
	if (hdr != ProtocolResult::Ok)
	{
		return hdr;
	}
	*out = {};
	ReadControlCell(mapping.data, ControlCell::AggregateRing, &out->aggregate_ring);
	ReadControlCell(mapping.data, ControlCell::UnregisteredWriters, &out->unregistered_writers);
	ReadControlCell(mapping.data, ControlCell::RegistrationCapacity, &out->registration_capacity);
	ReadControlCell(mapping.data, ControlCell::InstanceCapacity, &out->instance_capacity);
	ReadControlCell(mapping.data, ControlCell::SkippedPublications, &out->skipped_publications);
	ReadControlCell(mapping.data, ControlCell::Disconnects, &out->disconnects);
	ReadControlCell(mapping.data, ControlCell::RejectedSamples, &out->rejected_samples);
	ReadControlCell(mapping.data, ControlCell::InactiveTokenAttempts, &out->inactive_token_attempts);
	return ProtocolResult::Ok;
}

ProtocolResult PublishWorkerControl(MutableMappingView mapping, ControlCell cell,
                                    const GlobalLossCounter& counter) noexcept
{
	if (!MappingOk(mapping.data, mapping.size) || static_cast<uint8_t>(cell) > 7u)
	{
		return ProtocolResult::InvalidArgument;
	}
	const auto hdr = ValidateHeader(mapping.data);
	if (hdr != ProtocolResult::Ok)
	{
		return hdr;
	}
	// Parent owns rejected samples; child owns the rest.
	if (cell == ControlCell::RejectedSamples)
	{
		return ProtocolResult::Rejected;
	}
	WriteControlCell(mapping.data, cell, counter);
	return ProtocolResult::Ok;
}

} // namespace Kyty::DevTools
