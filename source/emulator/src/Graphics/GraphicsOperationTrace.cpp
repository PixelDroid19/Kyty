#include "Emulator/Graphics/GraphicsOperationTrace.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Log.h"

#include "Kyty/Math/Crypto.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>

namespace Kyty::Libs::Graphics {
namespace {

constexpr const char* DIRECTORY_ENV = "KYTY_TRACE_OPERATIONS_DIR";
// Room for the input and meta headers and the closing brackets.
constexpr uint64_t DOCUMENT_RESERVE_BYTES = 4096;
constexpr const char* NOT_TRACED_KINDS_JSON =
    "[\"auto_draw\",\"depth_stencil_copy_draw\",\"dma\",\"indirect_dispatch\",\"indirect_draw\",\"resolve_and_copy\"]";

struct ReasonName
{
	uint32_t    bit;
	const char* name;
};

constexpr ReasonName REASON_NAMES[] = {
    {OperationTraceIncomplete::IndexTypeUnknown, "index_type_unknown"},
    {OperationTraceIncomplete::ColorAttachmentExtentUnknown, "color_attachment_extent_unknown"},
    {OperationTraceIncomplete::HtileMetadataNotRecorded, "htile_metadata_not_recorded"},
    {OperationTraceIncomplete::StencilAccessNotRecorded, "stencil_access_not_recorded"},
    {OperationTraceIncomplete::TextureExtentUnknown, "texture_extent_unknown"},
    {OperationTraceIncomplete::StorageImageExtentUnknown, "storage_image_extent_unknown"},
    {OperationTraceIncomplete::DeviceAddressResourcesNotSpanned, "device_address_resources_not_spanned"},
    {OperationTraceIncomplete::GdsPointersNotSpanned, "gds_pointers_not_spanned"},
    {OperationTraceIncomplete::StorageUsageUnknown, "storage_usage_unknown"},
    {OperationTraceIncomplete::ConflictingWriteSpan, "conflicting_write_span"},
    {OperationTraceIncomplete::SpanLimitReached, "span_limit_reached"},
    {OperationTraceIncomplete::InvalidSpan, "invalid_span"},
    {OperationTraceIncomplete::OperationWithoutExactSpans, "operation_without_exact_spans"},
    {OperationTraceIncomplete::AttachmentClearNotRecorded, "attachment_clear_not_recorded"},
};

// Collects the exact spans of one operation. Unknown or unusable spans set reason bits instead.
class RecordBuilder
{
public:
	explicit RecordBuilder(OperationTraceKind kind)
	{
		record_.kind = kind;
	}

	void Access(uint64_t address, uint64_t size, OperationTraceAccess access)
	{
		if (size == 0)
		{
			return;
		}
		if (address == 0 || address >= OperationTraceLimits::GUEST_ADDRESS_LIMIT ||
		    size > OperationTraceLimits::GUEST_ADDRESS_LIMIT - address)
		{
			Mark(OperationTraceIncomplete::InvalidSpan);
			return;
		}
		for (uint32_t i = 0; i < record_.span_count; ++i)
		{
			const auto& span = record_.spans[i];
			if (span.guest_address == address && span.size == size && span.access == access)
			{
				return;
			}
		}
		if (access == OperationTraceAccess::Write && OverlapsExistingWrite(address, size))
		{
			Mark(OperationTraceIncomplete::ConflictingWriteSpan);
			return;
		}
		if (record_.span_count == OperationTraceRecord::SPANS_MAX)
		{
			Mark(OperationTraceIncomplete::SpanLimitReached);
			return;
		}
		record_.spans[record_.span_count++] = {address, size, access};
	}

	void Mark(uint32_t reason)
	{
		record_.incomplete |= reason;
	}

	[[nodiscard]] OperationTraceRecord Finish() const
	{
		return record_;
	}

private:
	bool OverlapsExistingWrite(uint64_t address, uint64_t size) const
	{
		for (uint32_t i = 0; i < record_.span_count; ++i)
		{
			const auto& span = record_.spans[i];
			if (span.access == OperationTraceAccess::Write && address < span.guest_address + span.size &&
			    span.guest_address < address + size)
			{
				return true;
			}
		}
		return false;
	}

	OperationTraceRecord record_ {};
};

void AddStorageBuffers(RecordBuilder* builder, const ShaderBindResources& bind)
{
	const auto& storage = bind.storage_buffers;
	if (storage.buffers_num < 0 || storage.buffers_num > ShaderStorageResources::BUFFERS_MAX)
	{
		builder->Mark(OperationTraceIncomplete::InvalidSpan);
		return;
	}
	for (int i = 0; i < storage.buffers_num; ++i)
	{
		if (storage.accesses[i] == ShaderStorageAccess::UnusedMetadata)
		{
			continue;
		}
		const auto&    buffer = storage.buffers[i];
		const uint64_t address = buffer.Base48();
		const uint64_t size    = ShaderBufferByteSize(buffer.Stride(), buffer.NumRecords());
		switch (storage.usages[i])
		{
			case ShaderStorageUsage::Constant:
			case ShaderStorageUsage::ReadOnly: builder->Access(address, size, OperationTraceAccess::Read); break;
			case ShaderStorageUsage::ReadWrite:
				// Read before write: a read-modify-write reads the prior contents, then publishes.
				builder->Access(address, size, OperationTraceAccess::Read);
				builder->Access(address, size, OperationTraceAccess::Write);
				break;
			default: builder->Mark(OperationTraceIncomplete::StorageUsageUnknown); break;
		}
	}
}

void AddBindResources(RecordBuilder* builder, const ShaderBindResources& bind)
{
	if (bind.textures2D.textures_num > 0)
	{
		builder->Mark(OperationTraceIncomplete::TextureExtentUnknown);
	}
	if (bind.textures2D.textures2d_storage_num > 0)
	{
		builder->Mark(OperationTraceIncomplete::StorageImageExtentUnknown);
	}
	if (bind.gds_pointers.pointers_num > 0)
	{
		builder->Mark(OperationTraceIncomplete::GdsPointersNotSpanned);
	}
	if (bind.device_address_used)
	{
		builder->Mark(OperationTraceIncomplete::DeviceAddressResourcesNotSpanned);
	}
	AddStorageBuffers(builder, bind);
}

void AddDepthTarget(RecordBuilder* builder, const OperationTraceDepthTarget& depth)
{
	if (depth.reads || depth.writes)
	{
		if (depth.size == 0)
		{
			builder->Mark(OperationTraceIncomplete::InvalidSpan);
		} else
		{
			if (depth.reads)
			{
				builder->Access(depth.address, depth.size, OperationTraceAccess::Read);
			}
			if (depth.writes)
			{
				builder->Access(depth.address, depth.size, OperationTraceAccess::Write);
			}
		}
	}
	if (depth.htile_present)
	{
		builder->Mark(OperationTraceIncomplete::HtileMetadataNotRecorded);
	}
	if (depth.stencil_present)
	{
		builder->Mark(OperationTraceIncomplete::StencilAccessNotRecorded);
	}
	if (depth.clear_present)
	{
		// A clear is a load-op write at pass begin; the record has no span for it.
		builder->Mark(OperationTraceIncomplete::AttachmentClearNotRecorded);
	}
}

std::string EncodeSpan(const OperationTraceSpan& span)
{
	return "{\"resource\":\"guest\",\"offset\":" + std::to_string(span.guest_address) + ",\"size\":" +
	       std::to_string(span.size) + ",\"access\":\"" +
	       (span.access == OperationTraceAccess::Read ? "read" : "write") + "\"}";
}

std::string EncodeRecord(const std::string& id, const OperationTraceRecord& record)
{
	std::string text = "{\"id\":\"" + id + "\",\"kind\":\"" +
	                   (record.kind == OperationTraceKind::Draw ? "draw" : "dispatch") + "\",\"spans\":[";
	for (uint32_t i = 0; i < record.span_count; ++i)
	{
		if (i > 0)
		{
			text += ',';
		}
		text += EncodeSpan(record.spans[i]);
	}
	return text + "]}";
}

// Returns an empty string when no reason bit is set.
std::string EncodeIncomplete(const std::string& id, uint32_t reasons)
{
	if (reasons == 0)
	{
		return {};
	}
	std::string text = "{\"id\":\"" + id + "\",\"reasons\":[";
	bool        first = true;
	for (const auto& reason : REASON_NAMES)
	{
		if ((reasons & reason.bit) == 0)
		{
			continue;
		}
		text += first ? "\"" : ",\"";
		text += reason.name;
		text += '"';
		first = false;
	}
	return text + "]}";
}

// Lowercase hexadecimal MD5 of the exact bytes of one emitted document.
std::string Md5Hex(const std::string& text)
{
	static constexpr char HEX[] = "0123456789abcdef";
	auto digest = Kyty::Math::MD5::Hash(reinterpret_cast<const uint8_t*>(text.data()), static_cast<uint32_t>(text.size()));
	std::string hex;
	hex.reserve(2 * digest.Size());
	for (uint32_t i = 0; i < digest.Size(); ++i)
	{
		const auto byte = static_cast<uint8_t>(digest[i]);
		hex += HEX[byte >> 4u];
		hex += HEX[byte & 0xfu];
	}
	return hex;
}

bool IsSafeStem(const std::string& stem)
{
	if (stem.empty() || stem.size() > 128)
	{
		return false;
	}
	return std::all_of(stem.begin(), stem.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
	});
}

// "wbx" is exclusive creation: an existing file, a symbolic link or a FIFO at the path makes the open fail.
bool WriteExclusive(const std::filesystem::path& path, const std::string& text)
{
	std::FILE* file = std::fopen(path.string().c_str(), "wbx");
	if (file == nullptr)
	{
		return false;
	}
	const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
	return std::fclose(file) == 0 && written;
}

std::string ProcessDirectory()
{
	const char* value = std::getenv(DIRECTORY_ENV);
	return value == nullptr ? std::string() : std::string(value);
}

void FlushAtExit()
{
	GraphicsOperationTraceFlush();
}

OperationTraceCollector& ProcessCollector()
{
	static OperationTraceCollector collector(ProcessDirectory());
	static const bool            armed = collector.Enabled() && std::atexit(FlushAtExit) == 0;
	(void)armed;
	return collector;
}

} // namespace

OperationTraceRecord BuildIndexedDrawRecord(uint64_t index_address, uint64_t index_bytes, bool index_type_known,
                                            const ShaderVertexInputInfo& vs_input, const ShaderPixelInputInfo& ps_input,
                                            uint32_t color_outputs, const OperationTraceDepthTarget& depth)
{
	RecordBuilder builder(OperationTraceKind::Draw);
	if (index_type_known)
	{
		builder.Access(index_address, index_bytes, OperationTraceAccess::Read);
	} else
	{
		builder.Mark(OperationTraceIncomplete::IndexTypeUnknown);
	}
	if (vs_input.buffers_num < 0 || vs_input.buffers_num > ShaderVertexInputInfo::RES_MAX)
	{
		builder.Mark(OperationTraceIncomplete::InvalidSpan);
	} else
	{
		for (int i = 0; i < vs_input.buffers_num; ++i)
		{
			const auto& buffer = vs_input.buffers[i];
			builder.Access(buffer.addr, ShaderBufferByteSize(buffer.stride, buffer.num_records), OperationTraceAccess::Read);
		}
	}
	AddBindResources(&builder, vs_input.bind);
	AddBindResources(&builder, ps_input.bind);
	if (color_outputs > 0)
	{
		builder.Mark(OperationTraceIncomplete::ColorAttachmentExtentUnknown);
	}
	AddDepthTarget(&builder, depth);
	return builder.Finish();
}

OperationTraceRecord BuildDispatchRecord(const ShaderBindResources& bind)
{
	RecordBuilder builder(OperationTraceKind::Dispatch);
	AddBindResources(&builder, bind);
	return builder.Finish();
}

OperationTraceCollector::OperationTraceCollector(std::string directory) : directory_(std::move(directory))
{
	enabled_ = !directory_.empty() && std::filesystem::path(directory_).is_absolute();
}

void OperationTraceCollector::Append(const OperationTraceRecord& record) noexcept
{
	if (!enabled_)
	{
		return;
	}
	std::lock_guard lock(mutex_);
	const std::string id = "op" + std::to_string(operations_seen_);
	++operations_seen_;
	if (truncated_)
	{
		++operations_dropped_;
		return;
	}
	const bool no_spans = record.span_count == 0;
	const uint32_t reasons = no_spans ? (record.incomplete | OperationTraceIncomplete::OperationWithoutExactSpans)
	                                  : record.incomplete;
	const std::string incomplete = EncodeIncomplete(id, reasons);
	const std::string text       = no_spans ? std::string() : EncodeRecord(id, record);
	const uint64_t added_bytes = text.size() + (records_emitted_ > 0 ? 1u : 0u) + incomplete.size() +
	                             (incomplete_entries_ > 0 ? 1u : 0u);
	const bool over_budget = output_bytes_ + added_bytes + DOCUMENT_RESERVE_BYTES > OperationTraceLimits::OUTPUT_BYTES_MAX;
	if (no_spans)
	{
		++operations_dropped_;
		if (over_budget)
		{
			truncated_ = true;
			return;
		}
	} else if (records_emitted_ >= OperationTraceLimits::RECORDS_MAX ||
	           spans_emitted_ + record.span_count > OperationTraceLimits::SPANS_TOTAL_MAX || over_budget)
	{
		truncated_ = true;
		++operations_dropped_;
		return;
	} else
	{
		if (records_emitted_ > 0)
		{
			records_text_ += ',';
		}
		records_text_ += text;
		++records_emitted_;
		spans_emitted_ += record.span_count;
	}
	if (!incomplete.empty())
	{
		if (incomplete_entries_ > 0)
		{
			incomplete_text_ += ',';
		}
		incomplete_text_ += incomplete;
		++incomplete_entries_;
	}
	output_bytes_ += added_bytes;
}

OperationTraceCollector::Documents OperationTraceCollector::Encode() const
{
	std::lock_guard lock(mutex_);
	Documents       docs;
	if (records_emitted_ > 0)
	{
		docs.input = "{\"schema\":\"kyty_operation_graph_input_v1\",\"records\":[" + records_text_ + "]}";
	}
	// The digest binds the sidecar to these exact input bytes; an empty input has the empty-string digest.
	docs.meta = "{\"schema\":\"kyty_operation_trace_meta_v1\",\"input_md5\":\"" + Md5Hex(docs.input) +
	            "\",\"operations_seen\":" + std::to_string(operations_seen_) +
	            ",\"records_emitted\":" + std::to_string(records_emitted_) +
	            ",\"operations_dropped\":" + std::to_string(operations_dropped_) +
	            ",\"truncated\":" + (truncated_ ? "true" : "false") +
	            ",\"not_traced_kinds\":" + NOT_TRACED_KINDS_JSON + ",\"incomplete_records\":[" + incomplete_text_ + "]}";
	return docs;
}

bool OperationTraceCollector::WriteFiles(const std::string& stem) const
{
	if (!enabled_ || !IsSafeStem(stem))
	{
		return false;
	}
	const auto docs = Encode();
	const std::filesystem::path base(directory_);
	if (!docs.input.empty() && !WriteExclusive(base / (stem + ".operations.input.json"), docs.input))
	{
		return false;
	}
	return WriteExclusive(base / (stem + ".operations.meta.json"), docs.meta);
}

uint64_t OperationTraceCollector::OperationsSeen() const
{
	std::lock_guard lock(mutex_);
	return operations_seen_;
}

uint64_t OperationTraceCollector::OperationsDropped() const
{
	std::lock_guard lock(mutex_);
	return operations_dropped_;
}

uint32_t OperationTraceCollector::RecordsEmitted() const
{
	std::lock_guard lock(mutex_);
	return records_emitted_;
}

uint64_t OperationTraceCollector::SpansEmitted() const
{
	std::lock_guard lock(mutex_);
	return spans_emitted_;
}

uint64_t OperationTraceCollector::OutputBytes() const
{
	std::lock_guard lock(mutex_);
	return output_bytes_;
}

bool OperationTraceCollector::Truncated() const
{
	std::lock_guard lock(mutex_);
	return truncated_;
}

void GraphicsOperationTraceIndexedDraw(uint64_t index_address, uint64_t index_bytes, bool index_type_known,
                                       const ShaderVertexInputInfo& vs_input, const ShaderPixelInputInfo& ps_input,
                                       uint32_t color_outputs, const OperationTraceDepthTarget& depth) noexcept
{
	auto& collector = ProcessCollector();
	if (collector.Enabled())
	{
		collector.Append(BuildIndexedDrawRecord(index_address, index_bytes, index_type_known, vs_input, ps_input,
		                                        color_outputs, depth));
	}
}

void GraphicsOperationTraceDispatch(const ShaderBindResources& bind) noexcept
{
	auto& collector = ProcessCollector();
	if (collector.Enabled())
	{
		collector.Append(BuildDispatchRecord(bind));
	}
}

void GraphicsOperationTraceFlush() noexcept
{
	static std::atomic_bool flushed {false};
	auto& collector = ProcessCollector();
	if (!collector.Enabled() || flushed.exchange(true))
	{
		return;
	}
	const auto now  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
	const auto stem = "operations-" + std::to_string(now.count());
	if (!collector.WriteFiles(stem))
	{
		KYTY_LOG_WARN("operation trace could not create its exclusive output files\n");
	}
}

} // namespace Kyty::Libs::Graphics
