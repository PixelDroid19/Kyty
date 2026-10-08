#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderResourceFoldCapture.h"
#include "../../../emulator/src/Graphics/ShaderStorageAnalysis.h"

#include "Kyty/Core/VirtualMemory.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

UT_BEGIN(EmulatorShaderResourceFoldReplay);

using namespace Libs::Graphics;

namespace {

constexpr uint32_t k_recorded_reads_cap = 16;

// The sampled-image path of the evaluator reads the configured guest platform.
void EnsurePs5Config()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

struct ParseOutput
{
	ShaderParsedUsage   usage {};
	ShaderBindResources bind {};
};

// One owned ShaderParseUsage2 call: metadata, registers and code all live here.
struct Evaluation
{
	std::vector<uint16_t> direct;
	std::vector<ShaderSharp> sharp[4];
	ShaderUserData        user_data {};
	HW::UserSgprInfo      user_sgpr {};
	ShaderCode            code;
	bool                  has_code              = true;
	int                   user_sgpr_num         = 0;
	bool                  vertex_resource_types = false;
	uint64_t              table                 = 0;
	ParseOutput           initial; // output objects as the caller passes them in

	Evaluation() = default;
	Evaluation(const Evaluation&)            = delete;
	Evaluation& operator=(const Evaluation&) = delete;
	~Evaluation()
	{
		if (table != 0)
		{
			Core::VirtualMemory::Free(table);
		}
	}

	void BindMetadata()
	{
		user_data.direct_resource_offset = direct.empty() ? nullptr : direct.data();
		user_data.direct_resource_count  = static_cast<uint16_t>(direct.size());
		for (int category = 0; category < 4; ++category)
		{
			user_data.sharp_resource_offset[category] = sharp[category].empty() ? nullptr : sharp[category].data();
			user_data.sharp_resource_count[category]  = static_cast<uint16_t>(sharp[category].size());
		}
	}
	[[nodiscard]] const ShaderCode* Code() const { return has_code ? &code : nullptr; }
	[[nodiscard]] ShaderResourceFoldInputs Inputs() const
	{
		ShaderResourceFoldInputs inputs;
		inputs.user_data             = &user_data;
		inputs.user_sgpr             = &user_sgpr;
		inputs.user_sgpr_num         = user_sgpr_num;
		inputs.code                  = Code();
		inputs.vertex_resource_types = vertex_resource_types;
		inputs.initial_usage         = &initial.usage;
		inputs.initial_bind          = &initial.bind;
		return inputs;
	}
};

// Production ShaderParseUsage2 under an optional reader (nullptr is the default read).
std::unique_ptr<ParseOutput> Parse(const Evaluation& eval, ShaderGuestDescriptorReader* reader)
{
	auto                                    output = std::make_unique<ParseOutput>(eval.initial);
	const ScopedShaderGuestDescriptorReader scope(reader);
	ShaderParseUsage2(&eval.user_data, &output->usage, &output->bind, eval.user_sgpr, eval.user_sgpr_num, eval.Code(), 0,
	                  eval.vertex_resource_types);
	return output;
}

// The descriptor-table fixture of the Gen5 EUD snapshot coherence integration
// check: the EUD pointer is direct metadata type 5 at SGPR 28, and the
// instruction stream loads table dwords 40..43 and consumes them as the V# of
// an s_buffer_load, so those words become storage buffer 0.
constexpr uint32_t                k_eud_table_dwords      = 44;
constexpr uint32_t                k_consumed_first_dword = 40;
constexpr std::array<uint32_t, 4> k_storage_words        = {0x00001000u, 0x00000010u, 0x00000020u, 0x00000030u};

std::unique_ptr<Evaluation> BuildEudStorageEvaluation()
{
	auto eval   = std::make_unique<Evaluation>();
	eval->table = Core::VirtualMemory::Alloc(0, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_NE(eval->table, 0u);
	std::array<uint32_t, k_eud_table_dwords> words {};
	for (uint32_t i = 0; i < 4; ++i)
	{
		words[k_consumed_first_dword + i] = k_storage_words[i];
	}
	EXPECT_TRUE(Core::VirtualMemory::CopyToGuest(eval->table, words.data(), sizeof(words)));

	eval->direct.assign(6, 0xffffu);
	eval->direct[k_gen5_eud_direct_type] = 28;
	eval->user_data.eud_size_dw          = 4;
	eval->user_sgpr.value[28]            = static_cast<uint32_t>(eval->table);
	eval->user_sgpr.value[29]            = static_cast<uint32_t>(eval->table >> 32u);
	eval->user_sgpr_num                  = 30;
	eval->BindMetadata();

	ShaderInstruction eud_load {};
	eud_load.pc                = 0u;
	eud_load.type              = ShaderInstructionType::SLoadDwordx4;
	eud_load.format            = ShaderInstructionFormat::Sdst4SbaseSoffset;
	eud_load.dst               = {.type = ShaderOperandType::Sgpr, .register_id = 76, .size = 4};
	eud_load.src[0]            = {.type = ShaderOperandType::Sgpr, .register_id = 28, .size = 2};
	eud_load.src[1].type       = ShaderOperandType::LiteralConstant;
	eud_load.src[1].constant.u = k_consumed_first_dword * sizeof(uint32_t);
	eud_load.src_num           = 2;
	ShaderInstruction storage_consumer {};
	storage_consumer.pc                = 4u;
	storage_consumer.type              = ShaderInstructionType::SBufferLoadDword;
	storage_consumer.format            = ShaderInstructionFormat::SdstSbaseSoffset;
	storage_consumer.dst               = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 1};
	storage_consumer.src[0]            = {.type = ShaderOperandType::Sgpr, .register_id = 76, .size = 4};
	storage_consumer.src[1].type       = ShaderOperandType::IntegerInlineConstant;
	storage_consumer.src[1].constant.u = 0u;
	storage_consumer.src_num           = 2;
	ShaderInstruction end {};
	end.pc     = 8u;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	eval->code.SetType(ShaderType::Pixel);
	eval->code.GetInstructions().Add(eud_load);
	eval->code.GetInstructions().Add(storage_consumer);
	eval->code.GetInstructions().Add(end);
	return eval;
}

// A code-backed direct sampled T#: direct metadata names SGPR 8, and an image
// sample consumes s[8:15] as its T# and s[4:7] as its S#. This drives the
// sampled-texture path that reads the configured guest platform.
std::unique_ptr<Evaluation> BuildSampledTextureEvaluation()
{
	auto eval = std::make_unique<Evaluation>();
	eval->direct.assign(1, 8u);
	eval->user_sgpr_num = 16;
	for (int i = 0; i < 16; ++i)
	{
		eval->user_sgpr.type[i] = HW::UserSgprType::Region;
	}
	for (uint32_t i = 0; i < 4; ++i)
	{
		eval->user_sgpr.value[4 + i] = 0x00000100u + i; // fixture S# words
	}
	for (uint32_t i = 0; i < 8; ++i)
	{
		eval->user_sgpr.value[8 + i] = 0x00010000u + i; // fixture T# words
	}
	eval->user_sgpr.value[11] = 0x90000000u; // T# dword 3: type nibble 9 (2D)
	eval->BindMetadata();

	ShaderInstruction sample {};
	sample.pc      = 0u;
	sample.type    = ShaderInstructionType::ImageSampleDrefLz;
	sample.src[1]  = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 8};
	sample.src[2]  = {.type = ShaderOperandType::Sgpr, .register_id = 4, .size = 4};
	sample.src_num = 3;
	ShaderInstruction end {};
	end.pc   = 8u;
	end.type = ShaderInstructionType::SEndpgm;
	eval->code.SetType(ShaderType::Pixel);
	eval->code.GetInstructions().Add(sample);
	eval->code.GetInstructions().Add(end);
	return eval;
}

ShaderInstruction ScalarBufferLoadFrom(int base_register, uint32_t pc)
{
	ShaderInstruction load {};
	load.pc                = pc;
	load.type              = ShaderInstructionType::SBufferLoadDword;
	load.format            = ShaderInstructionFormat::SdstSbaseSoffset;
	load.dst               = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 1};
	load.src[0]            = {.type = ShaderOperandType::Sgpr, .register_id = base_register, .size = 4};
	load.src[1].type       = ShaderOperandType::IntegerInlineConstant;
	load.src[1].constant.u = 0u;
	load.src_num           = 2;
	return load;
}

ShaderInstruction EndProgram(uint32_t pc)
{
	ShaderInstruction end {};
	end.pc     = pc;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	return end;
}

// A code-backed direct V# in the last four user SGPRs: default direct metadata
// names SGPR 28 of a 32-SGPR window and an s_buffer_load consumes s[28:31].
std::unique_ptr<Evaluation> BuildLastWindowStorageEvaluation()
{
	auto eval = std::make_unique<Evaluation>();
	eval->direct.assign(1, 28u);
	eval->user_sgpr_num = HW::UserSgprInfo::SGPRS_MAX;
	for (uint32_t i = 0; i < 4; ++i)
	{
		eval->user_sgpr.value[28 + i] = k_storage_words[i];
	}
	eval->BindMetadata();
	eval->code.SetType(ShaderType::Pixel);
	eval->code.GetInstructions().Add(ScalarBufferLoadFrom(28, 0u));
	eval->code.GetInstructions().Add(EndProgram(4u));
	return eval;
}

// A document of evaluator inputs only, for calls that production refuses
// before it returns an output.
std::string InputDocument(const Evaluation& eval)
{
	std::string parse_section;
	std::string document;
	EXPECT_TRUE(ShaderResourceFoldSerializeInputs(eval.Inputs(), &parse_section));
	EXPECT_TRUE(ShaderResourceFoldAssembleDocument(parse_section, {}, "", &document));
	return document;
}

std::string BuildDocument(const Evaluation& eval, const ShaderResourceFoldTranscript& transcript, const ParseOutput& output)
{
	std::string document;
	EXPECT_TRUE(ShaderResourceFoldSerializeDocument(eval.Inputs(), transcript, output.usage, output.bind, &document));
	return document;
}

ShaderResourceFoldReplayReport Replay(const std::string& document)
{
	ShaderResourceFoldReplayReport report;
	std::string                    error;
	EXPECT_TRUE(ShaderResourceFoldReplayDocument(document, &report, &error)) << error;
	return report;
}

bool Rejected(const std::string& document)
{
	std::string error;
	return !ShaderResourceFoldValidateDocument(document, &error) && !error.empty();
}

// Death-test child: a refused replay halts through the production EXIT path.
[[noreturn]] void ReplayInChildAndExit(const std::string& document)
{
	ShaderResourceFoldReplayReport report;
	std::string                    error;
	(void)ShaderResourceFoldReplayDocument(document, &report, &error);
	std::_Exit(0);
}

std::string Replace(std::string text, const std::string& from, const std::string& to)
{
	const auto at = text.find(from);
	EXPECT_NE(at, std::string::npos) << from;
	if (at != std::string::npos)
	{
		text.replace(at, from.size(), to);
	}
	return text;
}

struct SnapshotMutation
{
	uint64_t        address     = 0;
	const uint32_t* replacement = nullptr;
	uint64_t        bytes       = 0;
	bool            changed     = false;
};

void MutateOnce(void* opaque)
{
	auto* state = static_cast<SnapshotMutation*>(opaque);
	if (!state->changed)
	{
		state->changed = Core::VirtualMemory::CopyToGuest(state->address, state->replacement, state->bytes);
	}
}

// Captures a parse whose table changes between the snapshot read and its verification read.
ShaderResourceFoldTranscript CaptureMutatedRetry(const Evaluation& eval, std::unique_ptr<ParseOutput>* output)
{
	std::array<uint32_t, k_eud_table_dwords> replacement {};
	replacement[k_consumed_first_dword] = 0x00002000u;
	replacement[k_consumed_first_dword + 1] = 0x00000011u;
	SnapshotMutation mutation {eval.table, replacement.data(), sizeof(replacement)};
	ShaderSetGen5EudSnapshotTestHook(&MutateOnce, &mutation);
	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	*output = Parse(eval, &recorder);
	ShaderSetGen5EudSnapshotTestHook(nullptr, nullptr);
	return recorder.Transcript();
}

struct CaptureSinkResult
{
	int                              calls   = 0;
	ShaderResourceFoldCaptureOutcome outcome = ShaderResourceFoldCaptureOutcome::DroppedUnrepresentable;
	std::string                      document;
};

void RecordCapture(void* context, ShaderResourceFoldCaptureOutcome outcome, const std::string& document)
{
	auto* result = static_cast<CaptureSinkResult*>(context);
	result->calls++;
	result->outcome  = outcome;
	result->document = document;
}

class TemporaryDocument
{
public:
	explicit TemporaryDocument(const std::string& text)
	    : m_path(std::filesystem::temp_directory_path() /
	             ("kyty-resource-fold-" + std::to_string(std::random_device {}()) + "-" + std::to_string(s_sequence++) + ".json"))
	{
		std::ofstream(m_path, std::ios::binary) << text;
	}
	TemporaryDocument(const TemporaryDocument&)            = delete;
	TemporaryDocument& operator=(const TemporaryDocument&) = delete;
	~TemporaryDocument()
	{
		std::error_code error;
		std::filesystem::remove(m_path, error);
	}
	[[nodiscard]] std::string Path() const { return m_path.string(); }

private:
	static inline uint32_t s_sequence = 0;
	std::filesystem::path  m_path;
};

} // namespace

// The fixture binds the consumed descriptor words; the capture records the
// production double read and the document replays to identical output bytes.
TEST(EmulatorShaderResourceFoldReplay, StableTableRecordsTwoReadsAndReplaysExactly)
{
	EnsurePs5Config();
	const auto eval     = BuildEudStorageEvaluation();
	const auto baseline = Parse(*eval, nullptr);
	ASSERT_EQ(baseline->bind.storage_buffers.buffers_num, 1);
	for (int field = 0; field < 4; ++field)
	{
		EXPECT_EQ(baseline->bind.storage_buffers.buffers[0].fields[field], k_storage_words[field]);
	}

	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	const auto                 captured = Parse(*eval, &recorder);
	EXPECT_EQ(ShaderResourceFoldCanonicalOutput(captured->usage, captured->bind),
	          ShaderResourceFoldCanonicalOutput(baseline->usage, baseline->bind));
	const auto& reads = recorder.Transcript().reads;
	ASSERT_EQ(reads.size(), 2u);
	EXPECT_EQ(reads[0].address, eval->table);
	EXPECT_EQ(reads[0].dwords, k_eud_table_dwords);
	EXPECT_TRUE(reads[0].success && reads[1].success);
	EXPECT_EQ(reads[0].words, reads[1].words);

	const auto report = Replay(BuildDocument(*eval, recorder.Transcript(), *captured));
	EXPECT_EQ(report.status, ShaderResourceFoldReplayStatus::Ok);
	EXPECT_TRUE(report.output_matches_document);
	EXPECT_EQ(report.canonical_output, ShaderResourceFoldCanonicalOutput(captured->usage, captured->bind));
}

// A table that changes between the snapshot read and its verification read is
// recorded as the production retry: four ordered reads, not one collapsed snapshot.
TEST(EmulatorShaderResourceFoldReplay, MutatedTableRecordsRetryAsFourOrderedReads)
{
	EnsurePs5Config();
	const auto                   eval = BuildEudStorageEvaluation();
	std::unique_ptr<ParseOutput> captured;
	const auto                   transcript = CaptureMutatedRetry(*eval, &captured);

	ASSERT_EQ(transcript.reads.size(), 4u);
	EXPECT_NE(transcript.reads[0].words, transcript.reads[1].words);
	EXPECT_EQ(transcript.reads[1].words, transcript.reads[2].words);
	EXPECT_EQ(transcript.reads[2].words, transcript.reads[3].words);
	EXPECT_EQ(captured->bind.storage_buffers.buffers[0].fields[0], 0x00002000u);

	const auto report = Replay(BuildDocument(*eval, transcript, *captured));
	EXPECT_EQ(report.status, ShaderResourceFoldReplayStatus::Ok);
	EXPECT_TRUE(report.output_matches_document);
}

// A collapsed retry (snapshot plus one verification) is refused as a missing read.
TEST(EmulatorShaderResourceFoldReplay, CollapsedRetryIsRejectedAsMissing)
{
	EnsurePs5Config();
	const auto                   eval = BuildEudStorageEvaluation();
	std::unique_ptr<ParseOutput> captured;
	auto                         transcript = CaptureMutatedRetry(*eval, &captured);
	transcript.reads.erase(transcript.reads.begin() + 1, transcript.reads.begin() + 3);
	const std::string document = BuildDocument(*eval, transcript, *captured);

	ASSERT_EXIT(ReplayInChildAndExit(document), [](int status) { return status != 0; }, "status=missing");
}

// A request that does not match the recorded read at the cursor is refused.
TEST(EmulatorShaderResourceFoldReplay, ReorderedReadsAreRejectedAsOrderMismatch)
{
	EnsurePs5Config();
	const auto                 eval = BuildEudStorageEvaluation();
	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	const auto                 captured   = Parse(*eval, &recorder);
	auto                       transcript = recorder.Transcript();
	ASSERT_EQ(transcript.reads.size(), 2u);
	transcript.reads[0].address += 8u;
	const std::string document = BuildDocument(*eval, transcript, *captured);

	ASSERT_EXIT(ReplayInChildAndExit(document), [](int status) { return status != 0; }, "status=order");
}

// A recorded read that production never requests is a failed replay, not a warning.
TEST(EmulatorShaderResourceFoldReplay, UnusedTrailingReadIsRejected)
{
	EnsurePs5Config();
	const auto                 eval = BuildEudStorageEvaluation();
	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	const auto                 captured   = Parse(*eval, &recorder);
	auto                       transcript = recorder.Transcript();
	transcript.reads.push_back(transcript.reads.back());

	const auto report = Replay(BuildDocument(*eval, transcript, *captured));
	EXPECT_EQ(report.status, ShaderResourceFoldReplayStatus::Unused);
	EXPECT_FALSE(report.output_matches_document);
}

// An injected unreadable read reproduces the production descriptor-table refusal.
TEST(EmulatorShaderResourceFoldReplay, InjectedUnreadableReadReproducesFoldRefusal)
{
	EnsurePs5Config();
	const auto                 eval = BuildEudStorageEvaluation();
	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	const auto                 captured   = Parse(*eval, &recorder);
	auto                       transcript = recorder.Transcript();
	transcript.reads[0].success = false;
	transcript.reads[0].words.clear();
	const std::string document = BuildDocument(*eval, transcript, *captured);

	ASSERT_EXIT(ReplayInChildAndExit(document), [](int status) { return status != 0; }, "status=unreadable");
}

// Replay answers from the transcript only: the captured guest page is made
// inaccessible after capture and the document still replays exactly.
TEST(EmulatorShaderResourceFoldReplay, ReplayNeverReadsCapturedGuestAddress)
{
	EnsurePs5Config();
	const auto                 eval = BuildEudStorageEvaluation();
	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	const auto                 captured = Parse(*eval, &recorder);
	const std::string          document = BuildDocument(*eval, recorder.Transcript(), *captured);
	ASSERT_TRUE(Core::VirtualMemory::Protect(eval->table, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::NoAccess));

	const auto report = Replay(document);
	EXPECT_EQ(report.status, ShaderResourceFoldReplayStatus::Ok);
	EXPECT_TRUE(report.output_matches_document);
	ASSERT_TRUE(Core::VirtualMemory::Protect(eval->table, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite));
}

// Changing the consumed descriptor words consistently in both recorded reads
// changes the bound storage descriptor, and the replayed output is byte-equal
// to production evaluating a guest table that holds the same words.
TEST(EmulatorShaderResourceFoldReplay, ConsumedDescriptorMutationChangesBoundWords)
{
	EnsurePs5Config();
	const auto                 eval = BuildEudStorageEvaluation();
	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	const auto                 captured   = Parse(*eval, &recorder);
	auto                       transcript = recorder.Transcript();
	constexpr std::array<uint32_t, 4> mutated = {0x00003000u, 0x00000012u, 0x00000024u, 0x00000036u};
	for (auto& read: transcript.reads)
	{
		for (uint32_t i = 0; i < 4; ++i)
		{
			read.words[k_consumed_first_dword + i] = mutated[i];
		}
	}

	const auto report = Replay(BuildDocument(*eval, transcript, *captured));
	EXPECT_EQ(report.status, ShaderResourceFoldReplayStatus::Ok);
	EXPECT_FALSE(report.output_matches_document);
	EXPECT_NE(report.canonical_output, ShaderResourceFoldCanonicalOutput(captured->usage, captured->bind));

	ASSERT_TRUE(Core::VirtualMemory::CopyToGuest(eval->table + k_consumed_first_dword * sizeof(uint32_t), mutated.data(),
	                                             sizeof(mutated)));
	const auto production = Parse(*eval, nullptr);
	ASSERT_EQ(production->bind.storage_buffers.buffers_num, 1);
	for (int field = 0; field < 4; ++field)
	{
		EXPECT_EQ(production->bind.storage_buffers.buffers[0].fields[field], mutated[field]);
	}
	EXPECT_EQ(report.canonical_output, ShaderResourceFoldCanonicalOutput(production->usage, production->bind));
}

// A parse without code (code==nullptr) is captured with code_present=0 and replays exactly.
TEST(EmulatorShaderResourceFoldReplay, NullCodeCaptureRoundTripsWithoutCodeSection)
{
	EnsurePs5Config();
	const auto eval = BuildEudStorageEvaluation();
	eval->has_code  = false;
	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	const auto                 captured = Parse(*eval, &recorder);
	const std::string          document = BuildDocument(*eval, recorder.Transcript(), *captured);
	EXPECT_NE(document.find("\"code_present\":0"), std::string::npos);
	const auto report = Replay(document);
	EXPECT_EQ(report.status, ShaderResourceFoldReplayStatus::Ok);
	EXPECT_TRUE(report.output_matches_document);
}

// The code-backed sampled T# replays through the CLI file entry with the PS5
// configuration, and the file entry maps each failure to its own result.
TEST(EmulatorShaderResourceFoldReplay, SampledTextureReplaysThroughToolFileEntry)
{
	EnsurePs5Config();
	const auto eval   = BuildSampledTextureEvaluation();
	const auto output = Parse(*eval, nullptr);
	ASSERT_EQ(output->bind.textures2D.textures_num, 1);
	ASSERT_EQ(output->bind.samplers.samplers_num, 1);
	for (int field = 0; field < 8; ++field)
	{
		EXPECT_EQ(output->bind.textures2D.desc[0].texture.fields[field], eval->user_sgpr.value[8 + field]);
	}
	const std::string document = BuildDocument(*eval, {}, *output);

	ShaderResourceFoldReplayReport report;
	std::string                    error;
	const TemporaryDocument        valid(document);
	EXPECT_EQ(ShaderResourceFoldRunFile(valid.Path(), true, &report, &error), ShaderResourceFoldToolResult::Ok);
	EXPECT_EQ(ShaderResourceFoldRunFile(valid.Path(), false, &report, &error), ShaderResourceFoldToolResult::Ok) << error;
	EXPECT_TRUE(report.output_matches_document);

	const TemporaryDocument trailing(document + "{}");
	EXPECT_EQ(ShaderResourceFoldRunFile(trailing.Path(), false, &report, &error), ShaderResourceFoldToolResult::Malformed);

	ShaderResourceFoldTranscript extra;
	extra.reads.push_back({0x1000u, 4u, true, {1u, 2u, 3u, 4u}});
	const TemporaryDocument unused(BuildDocument(*eval, extra, *output));
	EXPECT_EQ(ShaderResourceFoldRunFile(unused.Path(), false, &report, &error), ShaderResourceFoldToolResult::Unused);

	std::string parse_section;
	std::string mismatched;
	ASSERT_TRUE(ShaderResourceFoldSerializeInputs(eval->Inputs(), &parse_section));
	ASSERT_TRUE(ShaderResourceFoldAssembleDocument(parse_section, {}, "usage.fetch=1\n", &mismatched));
	const TemporaryDocument mismatch(mismatched);
	EXPECT_EQ(ShaderResourceFoldRunFile(mismatch.Path(), false, &report, &error), ShaderResourceFoldToolResult::OutputMismatch);

	EXPECT_EQ(ShaderResourceFoldRunFile(valid.Path() + ".missing", false, &report, &error), ShaderResourceFoldToolResult::Malformed);
}

// Seventeen sampler sharps certainly exceed the sixteen-entry sampler array, so
// the document is refused before the evaluator runs.
TEST(EmulatorShaderResourceFoldReplay, SamplerMetadataOverCapacityIsRejected)
{
	EnsurePs5Config();
	Evaluation eval;
	eval.has_code      = false;
	eval.user_sgpr_num = 16;
	ShaderSharp sampler {};
	sampler.offset_dw = 0;
	sampler.size      = 1;
	eval.sharp[2].assign(ShaderSamplerResources::RES_MAX + 1, sampler);
	eval.BindMetadata();
	ParseOutput output;
	const std::string document = BuildDocument(eval, {}, output);
	EXPECT_TRUE(Rejected(document));
}

// Seventeen non-null storage sharps overflow the sixteen-entry storage array; the
// evaluator refuses the seventeenth write instead of writing past the array.
TEST(EmulatorShaderResourceFoldReplay, StorageOverCapacityIsRefusedByTheEvaluator)
{
	EnsurePs5Config();
	Evaluation eval;
	eval.has_code      = false;
	eval.user_sgpr_num = 16;
	for (uint32_t i = 0; i < 4; ++i)
	{
		eval.user_sgpr.value[i] = k_storage_words[i];
	}
	ShaderSharp storage {};
	storage.offset_dw = 0;
	storage.size      = 1;
	eval.sharp[3].assign(ShaderStorageResources::BUFFERS_MAX + 1, storage);
	eval.BindMetadata();
	ParseOutput       output;
	const std::string document = BuildDocument(eval, {}, output);
	EXPECT_FALSE(Rejected(document)); // not provable from metadata alone: null V#s are skipped

	ASSERT_EXIT(ReplayInChildAndExit(document), [](int status) { return status != 0; }, "");
}

// A default direct V# in the last four user SGPRs is a valid four-dword binding:
// production binds it, and the document is accepted and replays byte-equal.
TEST(EmulatorShaderResourceFoldReplay, LastWindowStorageBindingReplaysExactly)
{
	EnsurePs5Config();
	const auto eval   = BuildLastWindowStorageEvaluation();
	const auto output = Parse(*eval, nullptr);
	ASSERT_EQ(output->bind.storage_buffers.buffers_num, 1);
	EXPECT_EQ(output->bind.storage_buffers.start_register[0], 28);
	EXPECT_FALSE(output->bind.storage_buffers.extended[0]);
	for (int field = 0; field < 4; ++field)
	{
		EXPECT_EQ(output->bind.storage_buffers.buffers[0].fields[field], k_storage_words[field]);
	}

	const std::string document = BuildDocument(*eval, {}, *output);
	EXPECT_FALSE(Rejected(document));
	const auto report = Replay(document);
	EXPECT_EQ(report.status, ShaderResourceFoldReplayStatus::Ok);
	EXPECT_TRUE(report.output_matches_document);
	EXPECT_EQ(report.canonical_output, ShaderResourceFoldCanonicalOutput(output->usage, output->bind));
}

// The same default slot sampled as an 8-dword T# would read past the user-SGPR
// window. The input is schema-valid, so validation accepts it; the production
// texture writer refuses it during replay instead of reading out of bounds.
TEST(EmulatorShaderResourceFoldReplay, TextureOutsideSourceWindowIsRefusedByTheEvaluator)
{
	EnsurePs5Config();
	auto eval = std::make_unique<Evaluation>();
	eval->direct.assign(1, 28u);
	eval->user_sgpr_num = HW::UserSgprInfo::SGPRS_MAX;
	eval->BindMetadata();
	ShaderInstruction sample {};
	sample.pc      = 0u;
	sample.type    = ShaderInstructionType::ImageSampleDrefLz;
	sample.src[1]  = {.type = ShaderOperandType::Sgpr, .register_id = 28, .size = 8};
	sample.src[2]  = {.type = ShaderOperandType::Sgpr, .register_id = 4, .size = 4};
	sample.src_num = 3;
	eval->code.SetType(ShaderType::Pixel);
	eval->code.GetInstructions().Add(sample);
	eval->code.GetInstructions().Add(EndProgram(8u));

	const std::string document = InputDocument(*eval);
	EXPECT_FALSE(Rejected(document));
	ASSERT_EXIT(ReplayInChildAndExit(document), [](int status) { return status != 0; }, "");
}

// Two direct slots naming the observed ordered-append m0 source would both bind
// as the single GDS pointer; the production GDS writer refuses the second.
TEST(EmulatorShaderResourceFoldReplay, SecondGdsPointerIsRefusedByTheEvaluator)
{
	EnsurePs5Config();
	auto eval = std::make_unique<Evaluation>();
	eval->direct.assign(2, 14u);
	eval->user_sgpr_num = 16;
	eval->BindMetadata();
	// Same ordered-append feed as EmulatorGraphicsState's observed-m0 GDS test.
	ShaderInstruction feed {};
	feed.pc                 = 0u;
	feed.type               = ShaderInstructionType::SMovB32;
	feed.format             = ShaderInstructionFormat::SVdstSVsrc0;
	feed.dst.type           = ShaderOperandType::M0;
	feed.src[0].type        = ShaderOperandType::Sgpr;
	feed.src[0].register_id = 14;
	feed.src_num            = 1;
	ShaderInstruction append {};
	append.pc       = 4u;
	append.type     = ShaderInstructionType::DsAppend;
	append.format   = ShaderInstructionFormat::VdstGds;
	append.dst.type = ShaderOperandType::Vgpr;
	eval->code.SetType(ShaderType::Compute);
	eval->code.GetInstructions().Add(feed);
	eval->code.GetInstructions().Add(append);
	eval->code.GetInstructions().Add(EndProgram(8u));

	const std::string document = InputDocument(*eval);
	EXPECT_FALSE(Rejected(document));
	ASSERT_EXIT(ReplayInChildAndExit(document), [](int status) { return status != 0; }, "");
}

// A metadata count without its array would be dereferenced as null by the evaluator.
TEST(EmulatorShaderResourceFoldReplay, CountWithoutMetadataArrayIsRejected)
{
	EnsurePs5Config();
	const auto                 eval = BuildEudStorageEvaluation();
	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	const auto                 captured = Parse(*eval, &recorder);
	const std::string          document = BuildDocument(*eval, recorder.Transcript(), *captured);

	const std::string direct_array = "\"direct_present\":1,\"direct\":[65535,65535,65535,65535,65535,28]";
	EXPECT_TRUE(Rejected(Replace(document, direct_array, "\"direct_present\":0,\"direct\":[]")));
	const std::string sharp_category = "{\"count\":0,\"present\":0,\"entries\":[]}";
	EXPECT_TRUE(Rejected(Replace(document, sharp_category, "{\"count\":1,\"present\":0,\"entries\":[]}")));

	Evaluation unrepresentable;
	unrepresentable.has_code                            = false;
	unrepresentable.user_data.direct_resource_count     = 1; // count with a null array
	ParseOutput output;
	std::string unused;
	EXPECT_FALSE(ShaderResourceFoldSerializeDocument(unrepresentable.Inputs(), {}, output.usage, output.bind, &unused));
}

// The strict reader accepts only the exact schema grammar.
TEST(EmulatorShaderResourceFoldReplay, StrictDocumentGrammarRejectsHostileInput)
{
	EnsurePs5Config();
	const auto                 eval = BuildEudStorageEvaluation();
	ShaderResourceFoldRecorder recorder(k_recorded_reads_cap);
	const auto                 captured = Parse(*eval, &recorder);
	const std::string          valid    = BuildDocument(*eval, recorder.Transcript(), *captured);
	ASSERT_FALSE(Rejected(valid));
	EXPECT_FALSE(Rejected(valid + "\n"));

	const std::string schema = "{\"schema_version\":3,";
	EXPECT_TRUE(Rejected(Replace(valid, schema, "{\"schema_version\":3.0,")));
	EXPECT_TRUE(Rejected(Replace(valid, schema, "{\"schema_version\":3e0,")));
	EXPECT_TRUE(Rejected(Replace(valid, schema, "{\"schema_version\":03,")));
	EXPECT_TRUE(Rejected(Replace(valid, schema, "{\"schema_version\":2,")));
	EXPECT_TRUE(Rejected(Replace(valid, schema, "{\"schema_version\":3,\"schema_version\":3,")));
	EXPECT_TRUE(Rejected(Replace(valid, "\"initial_output\":{\"push_constant_offset\"", "\"initial_output\":{\"push_constant_size\"")));
	EXPECT_TRUE(Rejected(Replace(valid, "\"user_sgpr_num\":30", "\"user_sgpr_num\":-30")));
	EXPECT_TRUE(Rejected(Replace(valid, "\"user_sgpr_num\":30", "\"user_sgpr_num\":30000000000")));
	EXPECT_TRUE(Rejected(Replace(valid, "\"parse\":", "\"parsed\":")));
	EXPECT_TRUE(Rejected(valid + "{}"));
	EXPECT_TRUE(Rejected(valid.substr(0, valid.size() - 1)));
	EXPECT_TRUE(Rejected(std::string(10000, '[')));
	EXPECT_TRUE(Rejected(std::string(SHADER_RESOURCE_FOLD_MAX_DOCUMENT_BYTES + 1u, ' ')));
	std::string with_nul = valid;
	with_nul[with_nul.size() / 2] = '\0';
	EXPECT_TRUE(Rejected(with_nul));
	EXPECT_TRUE(Rejected(Replace(valid, "\"fingerprint_lo\":", "\"fingerprint_lo\":1")));

	// 257 copies of a recorded read exceed the transcript bound.
	const auto begin = valid.find("\"reads\":[") + 9;
	const auto end   = valid.find("},{", begin) + 1;
	ASSERT_NE(begin, std::string::npos);
	const std::string read_object = valid.substr(begin, end - begin);
	std::string       reads;
	for (uint32_t i = 0; i <= SHADER_RESOURCE_FOLD_MAX_READS; ++i)
	{
		reads += (i == 0 ? "" : ",") + read_object;
	}
	const auto reads_end = valid.find("]},\"canonical_output\"");
	ASSERT_NE(reads_end, std::string::npos);
	EXPECT_TRUE(Rejected(valid.substr(0, begin) + reads + valid.substr(reads_end)));
}

// ShaderParseUsage2 adds to the caller's bind, so the layout seeds the pixel path
// sets (push constant offset, descriptor set slot) and the usage fields it never
// resets are evaluator input: they are serialized and seed the replay.
TEST(EmulatorShaderResourceFoldReplay, InitialOutputSeedsReplayExactly)
{
	EnsurePs5Config();
	const auto eval                         = BuildSampledTextureEvaluation();
	eval->initial.bind.push_constant_offset = 80;
	eval->initial.bind.descriptor_set_slot  = 1;
	eval->initial.usage.vertex_attrib       = true;
	eval->initial.usage.vertex_attrib_reg   = 6;
	const auto output                       = Parse(*eval, nullptr);
	EXPECT_EQ(output->bind.push_constant_offset, 80u);
	EXPECT_EQ(output->bind.descriptor_set_slot, 1u);
	EXPECT_TRUE(output->usage.vertex_attrib);

	const std::string document = BuildDocument(*eval, {}, *output);
	const auto        report   = Replay(document);
	EXPECT_EQ(report.status, ShaderResourceFoldReplayStatus::Ok);
	EXPECT_TRUE(report.output_matches_document);
	EXPECT_EQ(report.canonical_output, ShaderResourceFoldCanonicalOutput(output->usage, output->bind));

	// The seeds drive the output: different seeds no longer reproduce the call.
	const auto reseeded = Replay(Replace(document, "\"push_constant_offset\":80,", "\"push_constant_offset\":0,"));
	EXPECT_FALSE(reseeded.output_matches_document);
}

// The live capture shape: a pixel bind seeded with a nonzero layout is captured by
// the production scope and replays byte-equal.
TEST(EmulatorShaderResourceFoldReplay, CaptureScopeRecordsInitialLayoutSeeds)
{
	EnsurePs5Config();
	const auto eval                         = BuildSampledTextureEvaluation();
	eval->initial.bind.push_constant_offset = 80;
	eval->initial.bind.descriptor_set_slot  = 1;
	CaptureSinkResult sink;
	ShaderResourceFoldSetCaptureTestSink(&RecordCapture, &sink);
	const auto output = Parse(*eval, nullptr);
	ShaderResourceFoldSetCaptureTestSink(nullptr, nullptr);

	ASSERT_EQ(sink.calls, 1);
	ASSERT_EQ(sink.outcome, ShaderResourceFoldCaptureOutcome::Captured);
	const auto report = Replay(sink.document);
	EXPECT_TRUE(report.output_matches_document);
	EXPECT_EQ(report.canonical_output, ShaderResourceFoldCanonicalOutput(output->usage, output->bind));
}

// A bind prepopulated beyond the layout seeds carries input the schema does not
// hold: it is not serialized, the capture is dropped, and production is unchanged.
TEST(EmulatorShaderResourceFoldReplay, PrepopulatedBindStateIsNotRepresentable)
{
	EnsurePs5Config();
	const auto eval                       = BuildSampledTextureEvaluation();
	eval->initial.bind.thread_limits_used = true; // state the evaluator does not set
	const auto  baseline                  = Parse(*eval, nullptr);
	ParseOutput output;
	std::string document;
	EXPECT_FALSE(ShaderResourceFoldSerializeDocument(eval->Inputs(), {}, output.usage, output.bind, &document));

	CaptureSinkResult sink;
	ShaderResourceFoldSetCaptureTestSink(&RecordCapture, &sink);
	const auto captured = Parse(*eval, nullptr);
	ShaderResourceFoldSetCaptureTestSink(nullptr, nullptr);
	ASSERT_EQ(sink.calls, 1);
	EXPECT_EQ(sink.outcome, ShaderResourceFoldCaptureOutcome::DroppedUnrepresentable);
	EXPECT_EQ(ShaderResourceFoldCanonicalOutput(captured->usage, captured->bind),
	          ShaderResourceFoldCanonicalOutput(baseline->usage, baseline->bind));
}

// The production capture scope snapshots its inputs at entry; a stable call is
// delivered as a replayable document.
TEST(EmulatorShaderResourceFoldReplay, CaptureScopeDeliversReplayableDocument)
{
	EnsurePs5Config();
	const auto        eval = BuildEudStorageEvaluation();
	CaptureSinkResult sink;
	ShaderResourceFoldSetCaptureTestSink(&RecordCapture, &sink);
	const auto output = Parse(*eval, nullptr);
	ShaderResourceFoldSetCaptureTestSink(nullptr, nullptr);

	ASSERT_EQ(sink.calls, 1);
	ASSERT_EQ(sink.outcome, ShaderResourceFoldCaptureOutcome::Captured);
	const auto report = Replay(sink.document);
	EXPECT_EQ(report.status, ShaderResourceFoldReplayStatus::Ok);
	EXPECT_TRUE(report.output_matches_document);
	EXPECT_EQ(report.canonical_output, ShaderResourceFoldCanonicalOutput(output->usage, output->bind));
}

// Metadata that changes while the evaluator runs is not recorded as exact input.
TEST(EmulatorShaderResourceFoldReplay, CaptureScopeDropsInputsMutatedDuringEvaluation)
{
	EnsurePs5Config();
	const auto eval = BuildEudStorageEvaluation();
	ShaderSetGen5EudSnapshotTestHook([](void* opaque) { static_cast<ShaderUserData*>(opaque)->srt_size_dw = 1; },
	                                 &eval->user_data);
	CaptureSinkResult sink;
	ShaderResourceFoldSetCaptureTestSink(&RecordCapture, &sink);
	(void)Parse(*eval, nullptr);
	ShaderResourceFoldSetCaptureTestSink(nullptr, nullptr);
	ShaderSetGen5EudSnapshotTestHook(nullptr, nullptr);

	ASSERT_EQ(sink.calls, 1);
	EXPECT_EQ(sink.outcome, ShaderResourceFoldCaptureOutcome::DroppedUnstableInput);
	EXPECT_TRUE(sink.document.empty());
}

// The bounded recorder stops at its cap, passes reads through, and never changes the production output.
TEST(EmulatorShaderResourceFoldReplay, OverflowingRecorderKeepsProductionOutput)
{
	EnsurePs5Config();
	const auto                   eval     = BuildEudStorageEvaluation();
	const auto                   baseline = Parse(*eval, nullptr);
	ShaderResourceFoldRecorder   recorder(1u);
	const auto                   captured = Parse(*eval, &recorder);
	EXPECT_EQ(ShaderResourceFoldCanonicalOutput(captured->usage, captured->bind),
	          ShaderResourceFoldCanonicalOutput(baseline->usage, baseline->bind));
	EXPECT_TRUE(recorder.Overflowed());
	EXPECT_EQ(recorder.Transcript().reads.size(), 1u);
}

UT_END();
