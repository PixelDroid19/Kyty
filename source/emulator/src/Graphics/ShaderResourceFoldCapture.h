#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RESOURCE_FOLD_CAPTURE_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RESOURCE_FOLD_CAPTURE_H_

#include "Emulator/Graphics/Shader.h"

#include "ShaderStorageAnalysis.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Kyty::Libs::Graphics {

// Exact evaluator-input replay of ShaderParseUsage2. The captured input is the
// owned ShaderCode and metadata that the evaluator consumes; it is not a replay
// of the ISA parser, and no raw instruction words are retained or required.
constexpr uint32_t SHADER_RESOURCE_FOLD_SCHEMA_VERSION      = 3;
constexpr uint32_t SHADER_RESOURCE_FOLD_MAX_CAPTURES        = 8;
constexpr uint32_t SHADER_RESOURCE_FOLD_MAX_READS           = 256;
constexpr uint32_t SHADER_RESOURCE_FOLD_MAX_DOCUMENT_BYTES  = 4u * 1024u * 1024u;
constexpr uint32_t SHADER_RESOURCE_FOLD_MAX_CANONICAL_BYTES = 1024u * 1024u;

// One guest descriptor-table request, in production order. Failed reads are
// recorded too, so a refused table reproduces the same refusal on replay.
struct ShaderResourceFoldRead
{
	uint64_t              address = 0;
	uint32_t              dwords  = 0;
	bool                  success = false;
	std::vector<uint32_t> words; // exactly `dwords` entries on success, empty otherwise
};

struct ShaderResourceFoldTranscript
{
	std::vector<ShaderResourceFoldRead> reads;
};

// Default read (Core::VirtualMemory::CopyFromGuest) with bounded exact recording.
// Past the cap it stops recording and reports Overflowed(); reads still pass through.
class ShaderResourceFoldRecorder final : public ShaderGuestDescriptorReader
{
public:
	explicit ShaderResourceFoldRecorder(uint32_t max_reads);
	[[nodiscard]] bool Read(uint64_t guest_address, uint32_t dwords, uint32_t* words) override;
	[[nodiscard]] bool Overflowed() const { return m_overflowed; }
	[[nodiscard]] const ShaderResourceFoldTranscript& Transcript() const { return m_transcript; }

private:
	uint32_t                     m_max_reads;
	bool                         m_overflowed = false;
	ShaderResourceFoldTranscript m_transcript;
};

enum class ShaderResourceFoldReplayStatus : uint8_t
{
	Ok,
	Missing,    // production asked for a read the transcript does not contain
	Order,      // the request at the cursor does not match the recorded address/size
	Unreadable, // the recorded read failed; production refuses the table
	Invalid,    // a recorded success does not carry exactly `dwords` words
	Unused,     // the transcript holds reads that production never requested
};

// Answers only the request at the current cursor and never touches guest memory.
// A mismatch prints a machine token (status=...) to stderr and refuses the read.
class ShaderResourceFoldReplayReader final : public ShaderGuestDescriptorReader
{
public:
	explicit ShaderResourceFoldReplayReader(const ShaderResourceFoldTranscript& transcript);
	[[nodiscard]] bool Read(uint64_t guest_address, uint32_t dwords, uint32_t* words) override;
	[[nodiscard]] ShaderResourceFoldReplayStatus Status() const;

private:
	void Fail(ShaderResourceFoldReplayStatus status, uint64_t guest_address, uint32_t dwords);

	const ShaderResourceFoldTranscript& m_transcript;
	std::size_t                         m_cursor = 0;
	ShaderResourceFoldReplayStatus      m_status = ShaderResourceFoldReplayStatus::Ok;
};

// Everything ShaderParseUsage2 consumes, plus the configured guest platform that
// its sampled-image path reads. Pointers are only read during serialization.
//
// ShaderParseUsage2 adds to the caller's output objects instead of replacing them,
// so their state at entry is input too. The callers seed only the bind layout
// (push_constant_offset, push_constant_size, descriptor_set_slot); the usage
// fields the evaluator does not reset are vertex_attrib and vertex_attrib_reg.
// Those five values are serialized. Every other bind field must equal a freshly
// initialized ShaderBindResources; any other prepopulated state is not representable.
struct ShaderResourceFoldInputs
{
	const ShaderUserData*      user_data               = nullptr;
	const HW::UserSgprInfo*    user_sgpr               = nullptr;
	int                        user_sgpr_num           = 0;
	const ShaderCode*          code                    = nullptr; // nullptr is captured as code_present=0
	int                        user_data_register_base = 0;
	bool                       vertex_resource_types   = false;
	const ShaderParsedUsage*   initial_usage           = nullptr; // output objects as passed in, before evaluation
	const ShaderBindResources* initial_bind            = nullptr;
};

// Canonical, deterministic text of every ShaderParsedUsage and ShaderBindResources
// field, including fixed-array entries past the live counts.
[[nodiscard]] std::string ShaderResourceFoldCanonicalOutput(const ShaderParsedUsage& usage, const ShaderBindResources& bind);
[[nodiscard]] uint64_t    ShaderResourceFoldFingerprint(const std::string& text);

// Serializes the evaluator input section (the document's "parse" object). Returns
// false when the input is not exactly representable: uninitialized guest platform,
// missing initial output objects, a prepopulated bind beyond the layout seeds,
// metadata count with a null array, a count above the schema bound, or debug-printf code.
[[nodiscard]] bool ShaderResourceFoldSerializeInputs(const ShaderResourceFoldInputs& inputs, std::string* parse_section);

// Builds a document from an input section taken at evaluator entry, the transcript
// and the canonical output. The canonical text is stored verbatim and fingerprinted.
[[nodiscard]] bool ShaderResourceFoldAssembleDocument(const std::string& parse_section, const ShaderResourceFoldTranscript& transcript,
                                                      const std::string& canonical_output, std::string* document);

// ShaderResourceFoldSerializeInputs followed by ShaderResourceFoldAssembleDocument.
[[nodiscard]] bool ShaderResourceFoldSerializeDocument(const ShaderResourceFoldInputs& inputs,
                                                       const ShaderResourceFoldTranscript& transcript,
                                                       const ShaderParsedUsage& usage, const ShaderBindResources& bind,
                                                       std::string* document);

struct ShaderResourceFoldReplayReport
{
	ShaderResourceFoldReplayStatus status = ShaderResourceFoldReplayStatus::Ok;
	std::string                    canonical_output;
	uint64_t                       fingerprint             = 0;
	bool                           output_matches_document = false; // exact byte comparison
};

// Schema and safe-input validation: strictly parses one bounded schema-v3 document
// and rejects metadata the evaluator would consume outside a fixed array on an
// unchecked path. No evaluation happens, so the evaluator can still refuse a
// valid document during replay.
[[nodiscard]] bool ShaderResourceFoldValidateDocument(const std::string& document, std::string* error);

// ShaderResourceFoldValidateDocument, then production ShaderParseUsage2 against the
// recorded transcript only. The configured guest platform must match the document.
// Returns false with *error for rejected documents. A refused replay halts through
// the production EXIT path.
[[nodiscard]] bool ShaderResourceFoldReplayDocument(const std::string& document, ShaderResourceFoldReplayReport* report,
                                                    std::string* error);

enum class ShaderResourceFoldToolResult : int
{
	Ok             = 0,
	Usage          = 1,
	Malformed      = 2,
	Unused         = 3,
	OutputMismatch = 4,
};

// File entry shared by the CLI and its tests: bounded regular-file read, then
// validation only or a full replay with exact output comparison.
[[nodiscard]] ShaderResourceFoldToolResult ShaderResourceFoldRunFile(const std::string& path, bool validate_only,
                                                                     ShaderResourceFoldReplayReport* report, std::string* error);

enum class ShaderResourceFoldCaptureOutcome : uint8_t
{
	Captured,
	DroppedUnstableInput,   // evaluator inputs differed between entry and exit
	DroppedOverflow,        // more reads than the transcript bound
	DroppedUnrepresentable, // input or output outside the schema
};

// Test-only capture sink. While set, ShaderParseUsage2 capture scopes on every
// thread bypass the environment gate and deliver here instead of writing a file.
using ShaderResourceFoldCaptureTestSink = void (*)(void* context, ShaderResourceFoldCaptureOutcome outcome, const std::string& document);
void ShaderResourceFoldSetCaptureTestSink(ShaderResourceFoldCaptureTestSink sink, void* context);

// Default-off capture of one ShaderParseUsage2 call. Enabled only when
// KYTY_RESOURCE_FOLD_CAPTURE_DIR names a directory; at most
// SHADER_RESOURCE_FOLD_MAX_CAPTURES per process, each written with exclusive create.
// The inputs and the initial output state are snapshotted at entry; a capture whose
// borrowed inputs changed by exit, or whose initial bind holds more than the layout
// seeds, is dropped.
class ShaderResourceFoldCaptureScope
{
public:
	ShaderResourceFoldCaptureScope(const ShaderUserData* user_data, const ShaderParsedUsage* info, const ShaderBindResources* bind,
	                               const HW::UserSgprInfo& user_sgpr, int user_sgpr_num, const ShaderCode* code,
	                               int user_data_register_base, bool vertex_resource_types);
	ShaderResourceFoldCaptureScope(const ShaderResourceFoldCaptureScope&)            = delete;
	ShaderResourceFoldCaptureScope& operator=(const ShaderResourceFoldCaptureScope&) = delete;
	~ShaderResourceFoldCaptureScope();

private:
	struct Session;
	std::unique_ptr<Session> m_session;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_RESOURCE_FOLD_CAPTURE_H_ */
