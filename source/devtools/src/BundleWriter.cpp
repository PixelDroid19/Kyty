#include "Kyty/DevTools/Supervisor/BundleWriter.h"

#include "Kyty/DevTools/Diagnostics/Checksum.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace Kyty::DevTools {
namespace {

void WriteU16LE(uint8_t* p, uint16_t v) noexcept
{
	p[0] = static_cast<uint8_t>(v & 0xffu);
	p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
}

void WriteU32LE(uint8_t* p, uint32_t v) noexcept
{
	p[0] = static_cast<uint8_t>(v & 0xffu);
	p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
	p[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
	p[3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

void WriteU64LE(uint8_t* p, uint64_t v) noexcept
{
	for (int i = 0; i < 8; ++i)
	{
		p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xffu);
	}
}

uint16_t ReadU16LE(const uint8_t* p) noexcept
{
	return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadU32LE(const uint8_t* p) noexcept
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
	       (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* p) noexcept
{
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i)
	{
		v |= static_cast<uint64_t>(p[i]) << (8 * i);
	}
	return v;
}

// Bounded JSON builder: compiled keys + numeric values only.
class JsonBuilder
{
public:
	explicit JsonBuilder(char* buf, uint32_t cap) noexcept : buf_(buf), cap_(cap) {}

	[[nodiscard]] bool Ok() const noexcept { return ok_; }
	[[nodiscard]] uint32_t Size() const noexcept { return size_; }

	bool BeginObject() noexcept
	{
		if (!CommaIfNeeded() || !EmitChar('{'))
		{
			return false;
		}
		need_comma_ = false;
		return true;
	}
	bool EndObject() noexcept
	{
		if (!EmitChar('}'))
		{
			return false;
		}
		need_comma_ = true;
		return true;
	}
	bool BeginArray() noexcept
	{
		if (!CommaIfNeeded() || !EmitChar('['))
		{
			return false;
		}
		need_comma_ = false;
		return true;
	}
	bool EndArray() noexcept
	{
		if (!EmitChar(']'))
		{
			return false;
		}
		need_comma_ = true;
		return true;
	}

	bool Key(const char* key) noexcept
	{
		if (!CommaIfNeeded())
		{
			return false;
		}
		if (!EmitChar('"') || !EmitRaw(key) || !EmitChar('"') || !EmitChar(':'))
		{
			return false;
		}
		need_comma_ = false;
		return true;
	}

	bool String(const char* s) noexcept
	{
		if (!CommaIfNeeded() || !EmitChar('"'))
		{
			return false;
		}
		// Only allowlist-safe ASCII subsets: [A-Za-z0-9._-]
		if (s != nullptr)
		{
			for (const char* p = s; *p != '\0'; ++p)
			{
				const char c = *p;
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
				                c == '_' || c == '-';
				if (!ok)
				{
					ok_ = false;
					return false;
				}
				if (!EmitChar(c))
				{
					return false;
				}
			}
		}
		need_comma_ = true;
		return EmitChar('"');
	}

	bool U64(uint64_t v) noexcept
	{
		if (!CommaIfNeeded())
		{
			return false;
		}
		char tmp[32] = {};
		const int n  = std::snprintf(tmp, sizeof(tmp), "%llu", static_cast<unsigned long long>(v));
		if (n <= 0 || !EmitRaw(tmp))
		{
			ok_ = false;
			return false;
		}
		need_comma_ = true;
		return true;
	}

	bool I64(int64_t v) noexcept
	{
		if (!CommaIfNeeded())
		{
			return false;
		}
		char tmp[32] = {};
		const int n  = std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(v));
		if (n <= 0 || !EmitRaw(tmp))
		{
			ok_ = false;
			return false;
		}
		need_comma_ = true;
		return true;
	}

	bool Bool(bool v) noexcept
	{
		if (!CommaIfNeeded())
		{
			return false;
		}
		if (!EmitRaw(v ? "true" : "false"))
		{
			return false;
		}
		need_comma_ = true;
		return true;
	}

	bool Null() noexcept
	{
		if (!CommaIfNeeded() || !EmitRaw("null"))
		{
			return false;
		}
		need_comma_ = true;
		return true;
	}

	void ResetComma() noexcept { need_comma_ = false; }

private:
	bool CommaIfNeeded() noexcept
	{
		if (!ok_)
		{
			return false;
		}
		if (need_comma_)
		{
			return EmitChar(',');
		}
		return true;
	}

	bool EmitChar(char c) noexcept
	{
		if (!ok_ || size_ + 1u >= cap_)
		{
			ok_ = false;
			return false;
		}
		buf_[size_++] = c;
		buf_[size_]   = '\0';
		return true;
	}

	bool EmitRaw(const char* s) noexcept
	{
		if (s == nullptr)
		{
			ok_ = false;
			return false;
		}
		for (const char* p = s; *p != '\0'; ++p)
		{
			if (!EmitChar(*p))
			{
				return false;
			}
		}
		return true;
	}

	char*    buf_        = nullptr;
	uint32_t cap_        = 0;
	uint32_t size_       = 0;
	bool     ok_         = true;
	bool     need_comma_ = false;
};

[[nodiscard]] const char* DomainName(Domain d) noexcept
{
	switch (d)
	{
		case Domain::GuestThread: return "guest_thread";
		case Domain::Hle: return "hle";
		case Domain::CommandProcessor: return "command_processor";
		case Domain::Renderer: return "renderer";
		case Domain::GpuQueue: return "gpu_queue";
		case Domain::VideoOut: return "video_out";
		case Domain::Presentation: return "presentation";
		case Domain::Synchronization: return "synchronization";
		default: return "unknown";
	}
}

[[nodiscard]] const char* CategoryName(StallCategory c) noexcept
{
	switch (c)
	{
		case StallCategory::None: return "none";
		case StallCategory::HealthyIdle: return "healthy_idle";
		case StallCategory::HleStall: return "hle_stall";
		case StallCategory::GuestDeadlock: return "guest_deadlock";
		case StallCategory::CommandProcessorStall: return "command_processor_stall";
		case StallCategory::GpuStall: return "gpu_stall";
		case StallCategory::PresentationStall: return "presentation_stall";
		case StallCategory::WorkerUnresponsive: return "worker_unresponsive";
		case StallCategory::ProcessExited: return "process_exited";
		case StallCategory::ProcessCrashed: return "process_crashed";
		case StallCategory::ProcessTerminated: return "process_terminated";
		case StallCategory::UnknownStall: return "unknown_stall";
		default: return "unknown";
	}
}

[[nodiscard]] const char* ConfidenceName(Confidence c) noexcept
{
	switch (c)
	{
		case Confidence::Low: return "low";
		case Confidence::Medium: return "medium";
		case Confidence::High: return "high";
		default: return "low";
	}
}

[[nodiscard]] const char* TriggerName(BundleTrigger t) noexcept
{
	switch (t)
	{
		case BundleTrigger::SuspectedStall: return "suspected_stall";
		case BundleTrigger::ConfirmedStall: return "confirmed_stall";
		case BundleTrigger::ProcessTerminal: return "process_terminal";
		case BundleTrigger::StatusDecodeError: return "status_decode_error";
		case BundleTrigger::Manual: return "manual";
		default: return "unknown";
	}
}

[[nodiscard]] const char* LoggingName(LoggingMode m) noexcept
{
	switch (m)
	{
		case LoggingMode::Silent: return "silent";
		case LoggingMode::Console: return "console";
		case LoggingMode::File: return "file";
		case LoggingMode::Directory: return "directory";
		default: return "unknown";
	}
}

[[nodiscard]] const char* ShaderCacheName(ShaderCacheState s) noexcept
{
	switch (s)
	{
		case ShaderCacheState::NoPersistentCache: return "no_persistent_cache";
		case ShaderCacheState::PersistentCacheCold: return "persistent_cache_cold";
		case ShaderCacheState::PersistentCacheWarm: return "persistent_cache_warm";
		case ShaderCacheState::PersistentCacheDisabled: return "persistent_cache_disabled";
		default: return "unknown";
	}
}

[[nodiscard]] bool EmitLoss(JsonBuilder& j, const char* key, const GlobalLossCounter& c) noexcept
{
	if (!j.Key(key) || !j.BeginObject() || !j.Key("total") || !j.U64(c.total) || !j.Key("last_loss_monotonic_ns") ||
	    !j.U64(c.last_loss_monotonic_ns) || !j.EndObject())
	{
		return false;
	}
	return true;
}

[[nodiscard]] bool BuildProgressJson(const BundleInput& in, char* buf, uint32_t cap, uint32_t* out_size) noexcept
{
	JsonBuilder j(buf, cap);
	if (!j.BeginObject())
	{
		return false;
	}
	if (in.result != nullptr)
	{
		if (!j.Key("category") || !j.String(CategoryName(in.result->category)) || !j.Key("confidence") ||
		    !j.String(ConfidenceName(in.result->confidence)) || !j.Key("terminal") || !j.U64(in.result->terminal) ||
		    !j.Key("evidence_total") || !j.U64(in.result->evidence_total) || !j.Key("evidence_stored") ||
		    !j.U64(in.result->evidence_stored) || !j.Key("evidence_truncated") || !j.U64(in.result->evidence_truncated) ||
		    !j.Key("contradiction_total") || !j.U64(in.result->contradiction_total) || !j.Key("contradiction_stored") ||
		    !j.U64(in.result->contradiction_stored) || !j.Key("contradiction_truncated") ||
		    !j.U64(in.result->contradiction_truncated) || !j.Key("suspected_ns") || !j.U64(in.result->suspected_ns) ||
		    !j.Key("confirmed_ns") || !j.U64(in.result->confirmed_ns))
		{
			return false;
		}
		if (!j.Key("evidence") || !j.BeginArray())
		{
			return false;
		}
		const uint16_t n = in.result->evidence_stored < 16u ? in.result->evidence_stored : 16u;
		for (uint16_t i = 0; i < n; ++i)
		{
			const StallFact& f = in.result->evidence[i];
			j.ResetComma();
			if (i > 0)
			{
				// After first element, JsonBuilder needs comma via need_comma from previous EndObject.
			}
			if (!j.BeginObject() || !j.Key("domain") || !j.String(DomainName(f.domain)) || !j.Key("code") ||
			    !j.U64(f.code) || !j.Key("flags") || !j.U64(f.flags) || !j.Key("instance") || !j.U64(f.instance) ||
			    !j.Key("correlation") || !j.U64(f.correlation) || !j.EndObject())
			{
				return false;
			}
		}
		if (!j.EndArray())
		{
			return false;
		}
	}
	if (in.classifier != nullptr)
	{
		if (!j.Key("classifier") || !j.BeginObject() || !j.Key("causal_fingerprint") ||
		    !j.U64(in.classifier->causal_fingerprint) || !j.Key("first_observed_ns") ||
		    !j.U64(in.classifier->first_observed_ns) || !j.Key("suspected_ns") || !j.U64(in.classifier->suspected_ns) ||
		    !j.Key("confirmed_ns") || !j.U64(in.classifier->confirmed_ns) || !j.Key("terminal_finalized") ||
		    !j.Bool(in.classifier->terminal_finalized) || !j.Key("suspected_category") ||
		    !j.String(CategoryName(in.classifier->suspected.category)) || !j.Key("suspected_confidence") ||
		    !j.String(ConfidenceName(in.classifier->suspected.confidence)) || !j.Key("suspected_evidence_total") ||
		    !j.U64(in.classifier->suspected.evidence_total) || !j.Key("suspected_evidence_stored") ||
		    !j.U64(in.classifier->suspected.evidence_stored) || !j.EndObject())
		{
			return false;
		}
	}
	if (in.publication != nullptr)
	{
		const ProgressSnapshot& snap = in.publication->progress;
		if (!j.Key("entry_count") || !j.U64(snap.count) || !j.Key("unavailable_count") ||
		    !j.U64(snap.unavailable_count) || !j.Key("inventory_generation") || !j.U64(snap.inventory_generation))
		{
			return false;
		}
		if (!j.Key("entries") || !j.BeginArray())
		{
			return false;
		}
		const uint32_t n = snap.count < MaxProgressSnapshotEntries ? snap.count : MaxProgressSnapshotEntries;
		for (uint32_t i = 0; i < n; ++i)
		{
			const auto& e = snap.entries[i];
			if (!j.BeginObject() || !j.Key("domain") || !j.String(DomainName(e.key.domain)) || !j.Key("instance") ||
			    !j.U64(e.key.instance) || !j.Key("instance_key") || !j.U64(e.record.instance_key) ||
			    !j.Key("last_change_ns") || !j.U64(e.record.last_change_ns) || !j.Key("submitted") ||
			    !j.U64(e.record.submitted) || !j.Key("completed") || !j.U64(e.record.completed) || !j.Key("operation") ||
			    !j.U64(e.record.operation) || !j.Key("state") || !j.U64(e.record.state) || !j.Key("flags") ||
			    !j.U64(e.record.flags) || !j.Key("correlation") || !j.U64(e.record.correlation) || !j.EndObject())
			{
				return false;
			}
		}
		if (!j.EndArray())
		{
			return false;
		}
		// Per-domain capacity / rejected-update totals (provenance kept separate).
		if (!j.Key("domain_loss") || !j.BeginArray())
		{
			return false;
		}
		for (uint32_t d = 1; d < static_cast<uint32_t>(Domain::Count); ++d)
		{
			const auto& cap = in.publication->progress_loss.capacity[d];
			const auto& rej = in.publication->progress_loss.rejected_update[d];
			if (!j.BeginObject() || !j.Key("domain") || !j.String(DomainName(static_cast<Domain>(d))) ||
			    !j.Key("capacity_total") || !j.U64(cap.total) || !j.Key("capacity_last_loss_monotonic_ns") ||
			    !j.U64(cap.last_loss_monotonic_ns) || !j.Key("rejected_update_total") || !j.U64(rej.total) ||
			    !j.Key("rejected_update_last_loss_monotonic_ns") || !j.U64(rej.last_loss_monotonic_ns) || !j.EndObject())
			{
				return false;
			}
		}
		if (!j.EndArray())
		{
			return false;
		}
	}
	if (!j.EndObject() || !j.Ok())
	{
		return false;
	}
	*out_size = j.Size();
	return true;
}

[[nodiscard]] bool BuildThreadsJson(const BundleInput& in, char* buf, uint32_t cap, uint32_t* out_size) noexcept
{
	JsonBuilder j(buf, cap);
	if (!j.BeginObject() || !j.Key("writers") || !j.BeginArray())
	{
		return false;
	}
	if (in.publication != nullptr)
	{
		for (uint32_t i = 0; i < 512u; ++i)
		{
			const auto& e = in.publication->writer_inventory.entries[i];
			if (e.state == WriterState::Free || e.writer_key == 0u)
			{
				continue;
			}
			if (!j.BeginObject() || !j.Key("writer_key") || !j.U64(e.writer_key) || !j.Key("diagnostic_thread_instance") ||
			    !j.U64(e.diagnostic_thread_instance) || !j.Key("role") || !j.U64(static_cast<uint64_t>(e.role)) ||
			    !j.Key("state") || !j.U64(static_cast<uint64_t>(e.state)) || !j.EndObject())
			{
				return false;
			}
		}
	}
	if (!j.EndArray() || !j.EndObject() || !j.Ok())
	{
		return false;
	}
	*out_size = j.Size();
	return true;
}

[[nodiscard]] bool BuildWaitGraphJson(const BundleInput& in, char* buf, uint32_t cap, uint32_t* out_size) noexcept
{
	JsonBuilder j(buf, cap);
	if (!j.BeginObject())
	{
		return false;
	}
	const WaitGraphSnapshot empty {};
	const WaitGraphSnapshot& g = in.wait_graph != nullptr ? *in.wait_graph : empty;
	if (!j.Key("unknown_producer_count") || !j.U64(g.unknown_producer_count) || !j.Key("rejected_reference_count") ||
	    !j.U64(g.rejected_reference_count) || !j.Key("count") || !j.U64(g.count) || !j.Key("edges") || !j.BeginArray())
	{
		return false;
	}
	const uint32_t n = g.count < 512u ? g.count : 512u;
	for (uint32_t i = 0; i < n; ++i)
	{
		const WaitEdge& e = g.edges[i];
		if (!j.BeginObject() || !j.Key("wait_ref") || !j.U64(e.wait_ref) || !j.Key("waiter_ref") ||
		    !j.U64(e.waiter_ref) || !j.Key("producer_ref") || !j.U64(e.producer_ref) || !j.EndObject())
		{
			return false;
		}
	}
	if (!j.EndArray() || !j.EndObject() || !j.Ok())
	{
		return false;
	}
	*out_size = j.Size();
	return true;
}

[[nodiscard]] bool BuildGpuJson(const BundleInput& in, char* buf, uint32_t cap, uint32_t* out_size) noexcept
{
	JsonBuilder j(buf, cap);
	GpuFaultSnapshot fault {};
	if (in.publication != nullptr)
	{
		fault = in.publication->gpu_fault;
	}
	if (!j.BeginObject() || !j.Key("state") || !j.U64(static_cast<uint64_t>(fault.state)) || !j.Key("flags") ||
	    !j.U64(fault.flags) || !j.Key("device_lost_result") || !j.I64(fault.device_lost_result) || !j.Key("query_result") ||
	    !j.I64(fault.query_result) || !j.Key("capture_monotonic_ns") || !j.U64(fault.capture_monotonic_ns) ||
	    !j.Key("gpu_submission_id") || !j.U64(fault.gpu_submission_id) || !j.Key("address_info_count") ||
	    !j.U64(fault.address_info_count) || !j.Key("vendor_info_count") || !j.U64(fault.vendor_info_count) ||
	    !j.Key("vendor_binary_size") || !j.U64(fault.vendor_binary_size) || !j.EndObject() || !j.Ok())
	{
		return false;
	}
	*out_size = j.Size();
	return true;
}

[[nodiscard]] bool BuildManifestJson(const BundleInput& in, const uint64_t artifact_crc[6],
                                     const uint64_t artifact_size[6], char* buf, uint32_t cap,
                                     uint32_t* out_size) noexcept
{
	static constexpr const char* kNames[6] = {"progress.json", "threads.json", "wait_graph.json",
	                                          "gpu.json",       "timeline.bin", "manifest.json"};
	JsonBuilder j(buf, cap);
	if (!j.BeginObject() || !j.Key("schema_major") || !j.U64(kBundleSchemaMajor) || !j.Key("schema_minor") ||
	    !j.U64(kBundleSchemaMinor) || !j.Key("bundle_generation") || !j.U64(in.bundle_generation) || !j.Key("trigger") ||
	    !j.String(TriggerName(in.trigger)) || !j.Key("host_platform") || !j.String("linux") || !j.Key("logging_mode") ||
	    !j.String(LoggingName(in.logging_mode)) || !j.Key("shader_cache_state") ||
	    !j.String(ShaderCacheName(in.shader_cache_state)) || !j.Key("validation_enabled") ||
	    !j.U64(in.validation_enabled) || !j.Key("resolution_width") || !j.U64(in.resolution_width) ||
	    !j.Key("resolution_height") || !j.U64(in.resolution_height) || !j.Key("dirty") || !j.U64(in.dirty))
	{
		return false;
	}
	if (in.revision_hex40 != nullptr)
	{
		if (!j.Key("revision") || !j.String(in.revision_hex40))
		{
			return false;
		}
	}
	// Process status (numeric only).
	if (!j.Key("process") || !j.BeginObject() || !j.Key("liveness") ||
	    !j.U64(static_cast<uint64_t>(in.process.liveness)) || !j.Key("termination") ||
	    !j.U64(static_cast<uint64_t>(in.process.termination)) || !j.Key("error") ||
	    !j.U64(static_cast<uint64_t>(in.process.error)) || !j.Key("code_valid") || !j.U64(in.process.code_valid) ||
	    !j.Key("code") || !j.U64(in.process.code) || !j.Key("platform_error") || !j.U64(in.process.platform_error) ||
	    !j.EndObject())
	{
		return false;
	}
	// Loss owners — never merged.
	if (!j.Key("loss") || !j.BeginObject() || !EmitLoss(j, "aggregate_ring", in.health.aggregate_ring) ||
	    !EmitLoss(j, "unregistered_writers", in.health.unregistered_writers) ||
	    !EmitLoss(j, "registration_capacity", in.health.registration_capacity) ||
	    !EmitLoss(j, "instance_capacity", in.health.instance_capacity) ||
	    !EmitLoss(j, "skipped_publications", in.health.skipped_publications) ||
	    !EmitLoss(j, "disconnects", in.health.disconnects) || !EmitLoss(j, "rejected_samples", in.health.rejected_samples) ||
	    !EmitLoss(j, "inactive_token_attempts", in.health.inactive_token_attempts) || !j.Key("max_loss_monotonic_ns") ||
	    !j.U64(in.health.max_loss_monotonic_ns) || !j.EndObject())
	{
		return false;
	}
	if (in.publication != nullptr)
	{
		if (!EmitLoss(j, "writer_aggregate_ring", in.publication->writer_loss.aggregate_ring) ||
		    !EmitLoss(j, "writer_registration_capacity", in.publication->writer_loss.registration_capacity) ||
		    !EmitLoss(j, "writer_inactive_attempts", in.publication->writer_loss.inactive_writer_attempts))
		{
			return false;
		}
	}
	if (!j.Key("artifacts") || !j.BeginArray())
	{
		return false;
	}
	for (int i = 0; i < 5; ++i)
	{
		if (!j.BeginObject() || !j.Key("name") || !j.String(kNames[i]) || !j.Key("size") || !j.U64(artifact_size[i]) ||
		    !j.Key("crc64_ecma") || !j.U64(artifact_crc[i]) || !j.EndObject())
		{
			return false;
		}
	}
	if (!j.EndArray() || !j.EndObject() || !j.Ok())
	{
		return false;
	}
	*out_size = j.Size();
	return true;
}

[[nodiscard]] BundleWriteResult MapIo(DurableIoResult r) noexcept
{
	switch (r)
	{
		case DurableIoResult::Ok: return BundleWriteResult::Ok;
		case DurableIoResult::InvalidArgument: return BundleWriteResult::InvalidInput;
		case DurableIoResult::Conflict: return BundleWriteResult::Conflict;
		case DurableIoResult::DurabilityError: return BundleWriteResult::DurabilityError;
		default: return BundleWriteResult::IoError;
	}
}

[[nodiscard]] bool JoinPath(char* out, size_t out_cap, const char* dir, const char* name) noexcept
{
	const int n = std::snprintf(out, out_cap, "%s/%s", dir, name);
	return n > 0 && static_cast<size_t>(n) < out_cap;
}

} // namespace

bool EncodeCompleteMarker(uint64_t manifest_size, uint64_t manifest_crc, uint64_t generation,
                          uint8_t out[kBundleMarkerSize]) noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	std::memset(out, 0, kBundleMarkerSize);
	WriteU64LE(out + 0x00, kBundleMarkerMagic);
	WriteU16LE(out + 0x08, kBundleSchemaMajor);
	WriteU16LE(out + 0x0a, kBundleSchemaMinor);
	WriteU32LE(out + 0x0c, 0u); // flags
	WriteU64LE(out + 0x10, manifest_size);
	WriteU64LE(out + 0x18, manifest_crc);
	WriteU64LE(out + 0x20, generation);
	// 0x28..0x3f remain zero
	return true;
}

bool ValidateCompleteMarker(const uint8_t* data, uint64_t size, uint64_t expected_manifest_size,
                            uint64_t expected_manifest_crc, uint64_t expected_generation) noexcept
{
	if (data == nullptr || size != kBundleMarkerSize)
	{
		return false;
	}
	if (ReadU64LE(data + 0x00) != kBundleMarkerMagic)
	{
		return false;
	}
	if (ReadU16LE(data + 0x08) != kBundleSchemaMajor || ReadU16LE(data + 0x0a) != kBundleSchemaMinor)
	{
		return false;
	}
	if (ReadU32LE(data + 0x0c) != 0u)
	{
		return false;
	}
	if (ReadU64LE(data + 0x10) != expected_manifest_size || ReadU64LE(data + 0x18) != expected_manifest_crc ||
	    ReadU64LE(data + 0x20) != expected_generation)
	{
		return false;
	}
	for (uint32_t i = 0x28; i < kBundleMarkerSize; ++i)
	{
		if (data[i] != 0)
		{
			return false;
		}
	}
	return true;
}

uint64_t EncodeTimelineFile(const TimelineSnapshot& timeline, uint8_t* out, uint64_t out_capacity) noexcept
{
	if (out == nullptr || timeline.count > kTimelineMaxEvents)
	{
		return 0;
	}
	const uint64_t payload = static_cast<uint64_t>(timeline.count) * kTimelineRecordSize;
	const uint64_t total   = kTimelineHeaderSize + payload;
	if (out_capacity < total)
	{
		return 0;
	}
	std::memset(out, 0, kTimelineHeaderSize);
	WriteU64LE(out + 0x00, kTimelineFileMagic);
	WriteU16LE(out + 0x08, 1u);
	WriteU16LE(out + 0x0a, 0u);
	WriteU32LE(out + 0x0c, kTimelineByteOrderTag);
	WriteU32LE(out + 0x10, kTimelineHeaderSize);
	WriteU32LE(out + 0x14, kTimelineRecordSize);
	WriteU32LE(out + 0x18, timeline.count);
	WriteU32LE(out + 0x1c, kTimelineDictionaryVer);
	WriteU64LE(out + 0x20, payload);
	const uint64_t payload_crc =
	    timeline.count == 0u ? Crc64Ecma("", 0) : Crc64Ecma(timeline.events, static_cast<size_t>(payload));
	WriteU64LE(out + 0x28, payload_crc);
	// Header CRC with field at 0x30 zero.
	WriteU64LE(out + 0x30, 0);
	const uint64_t header_crc = Crc64Ecma(out, kTimelineHeaderSize);
	WriteU64LE(out + 0x30, header_crc);
	if (timeline.count > 0u)
	{
		std::memcpy(out + kTimelineHeaderSize, timeline.events, static_cast<size_t>(payload));
	}
	return total;
}

bool ValidateTimelineFile(const uint8_t* data, uint64_t size) noexcept
{
	if (data == nullptr || size < kTimelineHeaderSize)
	{
		return false;
	}
	if (ReadU64LE(data + 0x00) != kTimelineFileMagic)
	{
		return false;
	}
	if (ReadU16LE(data + 0x08) != 1u || ReadU16LE(data + 0x0a) != 0u)
	{
		return false;
	}
	if (ReadU32LE(data + 0x0c) != kTimelineByteOrderTag)
	{
		return false;
	}
	if (ReadU32LE(data + 0x10) != kTimelineHeaderSize || ReadU32LE(data + 0x14) != kTimelineRecordSize)
	{
		return false;
	}
	const uint32_t count = ReadU32LE(data + 0x18);
	if (count > kTimelineMaxEvents || ReadU32LE(data + 0x1c) != kTimelineDictionaryVer)
	{
		return false;
	}
	const uint64_t payload = ReadU64LE(data + 0x20);
	if (payload != static_cast<uint64_t>(count) * kTimelineRecordSize)
	{
		return false;
	}
	if (size != kTimelineHeaderSize + payload)
	{
		return false;
	}
	const uint64_t stored_payload_crc = ReadU64LE(data + 0x28);
	const uint64_t actual_payload_crc =
	    count == 0u ? Crc64Ecma("", 0) : Crc64Ecma(data + kTimelineHeaderSize, static_cast<size_t>(payload));
	if (stored_payload_crc != actual_payload_crc)
	{
		return false;
	}
	uint8_t header_copy[kTimelineHeaderSize] = {};
	std::memcpy(header_copy, data, kTimelineHeaderSize);
	WriteU64LE(header_copy + 0x30, 0);
	const uint64_t actual_header_crc = Crc64Ecma(header_copy, kTimelineHeaderSize);
	if (ReadU64LE(data + 0x30) != actual_header_crc)
	{
		return false;
	}
	for (uint32_t i = 0x38; i < kTimelineHeaderSize; ++i)
	{
		if (data[i] != 0)
		{
			return false;
		}
	}
	return true;
}

BundleWriteResult WriteBundle(const char* absolute_output_dir, const BundleInput& input,
                              BundlePath* completed_path) noexcept
{
	if (absolute_output_dir == nullptr || absolute_output_dir[0] != '/' || completed_path == nullptr ||
	    input.bundle_generation == 0u)
	{
		return BundleWriteResult::InvalidInput;
	}
	if (!ValidateProcessStatus(input.process))
	{
		return BundleWriteResult::InvalidInput;
	}
	if (input.publication != nullptr && !ValidateGpuFaultSnapshot(input.publication->gpu_fault))
	{
		return BundleWriteResult::InvalidInput;
	}
	*completed_path = {};

	DurableClockNs clock = input.clock != nullptr ? input.clock : DefaultDurableClock();
	TempCleanupResult cleanup {};
	(void)DurableCleanupOwnTemps(absolute_output_dir, clock, &cleanup);

	uint64_t self_pid = 0;
	uint64_t self_start = 0;
	if (!QuerySelfProcessIdentity(&self_pid, &self_start))
	{
		return BundleWriteResult::IoError;
	}

	char temp_dir[1100] = {};
	char final_dir[1100] = {};
	if (std::snprintf(temp_dir, sizeof(temp_dir), "%s/.kyty-bundle-tmp.%llu.%llu.%llu", absolute_output_dir,
	                  static_cast<unsigned long long>(self_pid), static_cast<unsigned long long>(self_start),
	                  static_cast<unsigned long long>(input.bundle_generation)) <= 0)
	{
		return BundleWriteResult::IoError;
	}
	if (std::snprintf(final_dir, sizeof(final_dir), "%s/stall-bundle-%llu", absolute_output_dir,
	                  static_cast<unsigned long long>(input.bundle_generation)) <= 0)
	{
		return BundleWriteResult::IoError;
	}
	if (DurablePathExists(final_dir))
	{
		return BundleWriteResult::Conflict;
	}
	if (DurablePathExists(temp_dir))
	{
		// Own live temp → conflict (do not remove live owner).
		return BundleWriteResult::Conflict;
	}

	BundleWriteResult cr = MapIo(DurableCreateDirectory(temp_dir));
	if (cr != BundleWriteResult::Ok)
	{
		return cr;
	}

	// Stack buffers for JSON / timeline (bounded).
	// progress can be large with many entries; allocate on heap via new (not hot path).
	constexpr uint32_t kJsonCap = 2u * 1024u * 1024u;
	auto progress_buf = std::unique_ptr<char[]>(new (std::nothrow) char[kJsonCap]);
	auto threads_buf  = std::unique_ptr<char[]>(new (std::nothrow) char[64u * 1024u]);
	auto wait_buf     = std::unique_ptr<char[]>(new (std::nothrow) char[128u * 1024u]);
	auto gpu_buf      = std::unique_ptr<char[]>(new (std::nothrow) char[8u * 1024u]);
	auto manifest_buf = std::unique_ptr<char[]>(new (std::nothrow) char[64u * 1024u]);
	const uint64_t timeline_cap =
	    kTimelineHeaderSize + static_cast<uint64_t>(kTimelineMaxEvents) * kTimelineRecordSize;
	auto timeline_buf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[static_cast<size_t>(timeline_cap)]);
	if (!progress_buf || !threads_buf || !wait_buf || !gpu_buf || !manifest_buf || !timeline_buf)
	{
		return BundleWriteResult::IoError;
	}

	uint32_t progress_sz = 0;
	uint32_t threads_sz  = 0;
	uint32_t wait_sz     = 0;
	uint32_t gpu_sz      = 0;
	uint32_t manifest_sz = 0;
	if (!BuildProgressJson(input, progress_buf.get(), kJsonCap, &progress_sz) ||
	    !BuildThreadsJson(input, threads_buf.get(), 64u * 1024u, &threads_sz) ||
	    !BuildWaitGraphJson(input, wait_buf.get(), 128u * 1024u, &wait_sz) ||
	    !BuildGpuJson(input, gpu_buf.get(), 8u * 1024u, &gpu_sz))
	{
		return BundleWriteResult::InvalidInput;
	}

	TimelineSnapshot empty_timeline {};
	const TimelineSnapshot& tl = input.timeline != nullptr ? *input.timeline : empty_timeline;
	const uint64_t timeline_sz = EncodeTimelineFile(tl, timeline_buf.get(), timeline_cap);
	if (timeline_sz == 0u || !ValidateTimelineFile(timeline_buf.get(), timeline_sz))
	{
		return BundleWriteResult::InvalidInput;
	}

	uint64_t crc[6]  = {};
	uint64_t sizes[6] = {};
	sizes[0] = progress_sz;
	sizes[1] = threads_sz;
	sizes[2] = wait_sz;
	sizes[3] = gpu_sz;
	sizes[4] = timeline_sz;
	crc[0]   = Crc64Ecma(progress_buf.get(), progress_sz);
	crc[1]   = Crc64Ecma(threads_buf.get(), threads_sz);
	crc[2]   = Crc64Ecma(wait_buf.get(), wait_sz);
	crc[3]   = Crc64Ecma(gpu_buf.get(), gpu_sz);
	crc[4]   = Crc64Ecma(timeline_buf.get(), static_cast<size_t>(timeline_sz));

	if (!BuildManifestJson(input, crc, sizes, manifest_buf.get(), 64u * 1024u, &manifest_sz))
	{
		return BundleWriteResult::InvalidInput;
	}
	sizes[5] = manifest_sz;
	crc[5]   = Crc64Ecma(manifest_buf.get(), manifest_sz);

	char path[1200] = {};
	auto write_named = [&](const char* name, const void* data, uint64_t sz) -> BundleWriteResult {
		if (!JoinPath(path, sizeof(path), temp_dir, name))
		{
			return BundleWriteResult::IoError;
		}
		return MapIo(DurableWriteFile(path, data, sz));
	};

	BundleWriteResult wr = write_named("progress.json", progress_buf.get(), progress_sz);
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}
	wr = write_named("threads.json", threads_buf.get(), threads_sz);
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}
	wr = write_named("wait_graph.json", wait_buf.get(), wait_sz);
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}
	wr = write_named("gpu.json", gpu_buf.get(), gpu_sz);
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}
	wr = write_named("timeline.bin", timeline_buf.get(), timeline_sz);
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}
	// Manifest after data artifacts.
	wr = write_named("manifest.json", manifest_buf.get(), manifest_sz);
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}

	uint8_t marker[kBundleMarkerSize] = {};
	if (!EncodeCompleteMarker(manifest_sz, crc[5], input.bundle_generation, marker) ||
	    !ValidateCompleteMarker(marker, kBundleMarkerSize, manifest_sz, crc[5], input.bundle_generation))
	{
		return BundleWriteResult::InvalidInput;
	}
	wr = write_named("complete.marker", marker, kBundleMarkerSize);
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}

	// fsync temp directory, rename, fsync parent.
	wr = MapIo(DurableFsyncDirectory(temp_dir));
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}
	wr = MapIo(DurableRename(temp_dir, final_dir));
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}
	wr = MapIo(DurableFsyncDirectory(absolute_output_dir));
	if (wr != BundleWriteResult::Ok)
	{
		return wr;
	}

	const int n = std::snprintf(completed_path->bytes, sizeof(completed_path->bytes), "%s", final_dir);
	if (n <= 0 || static_cast<size_t>(n) >= sizeof(completed_path->bytes))
	{
		return BundleWriteResult::IoError;
	}
	completed_path->size = static_cast<uint32_t>(n);
	return BundleWriteResult::Ok;
}

} // namespace Kyty::DevTools
