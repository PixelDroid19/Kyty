#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace Kyty::Libs::Graphics {

struct ShaderVertexInputInfo;
struct ShaderPixelInputInfo;
struct ShaderBindResources;

// Opt-in local diagnostic. Disabled unless KYTY_TRACE_OPERATIONS_DIR is an
// absolute directory. Records guest-virtual spans of indexed draws and direct
// dispatches in CPU command-recording order, in the kyty_operation_graph_input_v1 format.
namespace OperationTraceLimits {
inline constexpr uint32_t RECORDS_MAX      = 4096;
inline constexpr uint32_t SPANS_TOTAL_MAX  = 65536;
inline constexpr uint64_t OUTPUT_BYTES_MAX = 4ull << 20u;
inline constexpr uint64_t GUEST_ADDRESS_LIMIT = 1ull << 48u;
} // namespace OperationTraceLimits

enum class OperationTraceKind : uint8_t
{
	Draw,
	Dispatch,
};

enum class OperationTraceAccess : uint8_t
{
	Read,
	Write,
};

// Bits of OperationTraceRecord::incomplete. A set bit means at least one span
// the operation touches is absent from the record; it was never guessed.
namespace OperationTraceIncomplete {
inline constexpr uint32_t IndexTypeUnknown                = 1u << 0u;
inline constexpr uint32_t ColorAttachmentExtentUnknown    = 1u << 1u;
inline constexpr uint32_t HtileMetadataNotRecorded        = 1u << 2u;
inline constexpr uint32_t StencilAccessNotRecorded        = 1u << 3u;
inline constexpr uint32_t TextureExtentUnknown            = 1u << 4u;
inline constexpr uint32_t StorageImageExtentUnknown       = 1u << 5u;
inline constexpr uint32_t DeviceAddressResourcesNotSpanned = 1u << 6u;
inline constexpr uint32_t GdsPointersNotSpanned           = 1u << 7u;
inline constexpr uint32_t StorageUsageUnknown             = 1u << 8u;
inline constexpr uint32_t ConflictingWriteSpan            = 1u << 9u;
inline constexpr uint32_t SpanLimitReached                = 1u << 10u;
inline constexpr uint32_t InvalidSpan                     = 1u << 11u;
inline constexpr uint32_t OperationWithoutExactSpans      = 1u << 12u;
inline constexpr uint32_t AttachmentClearNotRecorded     = 1u << 13u;
} // namespace OperationTraceIncomplete

struct OperationTraceSpan
{
	uint64_t              guest_address = 0;
	uint64_t              size          = 0;
	OperationTraceAccess  access        = OperationTraceAccess::Read;
};

struct OperationTraceRecord
{
	static constexpr uint32_t SPANS_MAX = 64;

	OperationTraceKind kind       = OperationTraceKind::Draw;
	uint32_t           span_count = 0;
	OperationTraceSpan spans[SPANS_MAX] {};
	uint32_t           incomplete = 0;
};

// Depth plane of the draw's depth target. A zero size with reads or writes set is invalid.
struct OperationTraceDepthTarget
{
	uint64_t address        = 0;
	uint64_t size           = 0;
	bool     reads          = false;
	bool     writes         = false;
	bool     htile_present  = false;
	bool     stencil_present = false;
	bool     clear_present   = false;
};

// Builds one indexed draw. Span order is index, vertex buffers, VS storage,
// PS storage, depth. Storage read-write yields read then write, so a
// read-modify-write is never its own writer.
[[nodiscard]] OperationTraceRecord BuildIndexedDrawRecord(uint64_t index_address, uint64_t index_bytes, bool index_type_known,
                                                         const ShaderVertexInputInfo& vs_input,
                                                         const ShaderPixelInputInfo& ps_input, uint32_t color_outputs,
                                                         const OperationTraceDepthTarget& depth);
// Builds one direct dispatch from its storage buffers.
[[nodiscard]] OperationTraceRecord BuildDispatchRecord(const ShaderBindResources& bind);

// Collects records in CPU command-recording order and encodes the producer documents.
// An operation with no exact span is dropped and counted, and recording continues after it,
// so the input is the recorded order minus those counted drops. Once a cap is reached, every
// later operation is dropped and counted. The trace does not establish GPU execution order.
class OperationTraceCollector
{
public:
	struct Documents
	{
		std::string input; // empty when no operation was recorded
		std::string meta;
	};

	explicit OperationTraceCollector(std::string directory);
	OperationTraceCollector(const OperationTraceCollector&)            = delete;
	OperationTraceCollector& operator=(const OperationTraceCollector&) = delete;

	[[nodiscard]] bool Enabled() const noexcept { return enabled_; }
	// Allocation failure follows the project out-of-memory policy: it is not recoverable, and
	// the trace does not record it as truncation. Exceptions are not used on this path.
	void               Append(const OperationTraceRecord& record) noexcept;
	[[nodiscard]] Documents Encode() const;
	// Creates <stem>.operations.input.json and <stem>.operations.meta.json with exclusive creation.
	// Nothing is overwritten or followed through a symbolic link. Returns false on any failure.
	// The input file is created first. If creating the meta file then fails, the input file
	// remains and no sidecar exists, so the pair is incomplete and the adapter rejects it.
	[[nodiscard]] bool WriteFiles(const std::string& stem) const;

	[[nodiscard]] uint64_t OperationsSeen() const;
	[[nodiscard]] uint64_t OperationsDropped() const;
	[[nodiscard]] uint32_t RecordsEmitted() const;
	[[nodiscard]] uint64_t SpansEmitted() const;
	[[nodiscard]] uint64_t OutputBytes() const;
	[[nodiscard]] bool     Truncated() const;

private:
	std::string directory_;
	bool        enabled_ = false;

	mutable std::mutex mutex_;
	std::string        records_text_;
	std::string        incomplete_text_;
	uint64_t           operations_seen_ = 0;
	uint64_t           operations_dropped_ = 0;
	uint32_t           records_emitted_ = 0;
	uint64_t           spans_emitted_ = 0;
	uint64_t           output_bytes_ = 0;
	uint32_t           incomplete_entries_ = 0;
	bool               truncated_ = false;
};

// Process-wide hooks called from the draw and dispatch recording paths. They do
// nothing unless the trace is enabled, and they never change draw behavior.
void GraphicsOperationTraceIndexedDraw(uint64_t index_address, uint64_t index_bytes, bool index_type_known,
                                       const ShaderVertexInputInfo& vs_input, const ShaderPixelInputInfo& ps_input,
                                       uint32_t color_outputs, const OperationTraceDepthTarget& depth) noexcept;
void GraphicsOperationTraceDispatch(const ShaderBindResources& bind) noexcept;
// Writes the process collector's files once. Also runs at normal process exit.
void GraphicsOperationTraceFlush() noexcept;

} // namespace Kyty::Libs::Graphics
