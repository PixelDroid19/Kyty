#include "Kyty/UnitTest.h"
#include "ScopedTestDirectory.h"

#include "Kyty/Core/JsonReader.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Agent/Protocol.h"
#include "Emulator/Graphics/DiagnosticDump.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderProgramSnapshot.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

UT_BEGIN(EmulatorNativeWaveSnapshot);

using namespace Libs::Graphics;

namespace {

namespace fs = std::filesystem;

class SnapshotFiles final
{
public:
	SnapshotFiles(): m_directory("native-wave-snapshot")
	{
		if (m_directory.Path().empty())
		{
			std::_Exit(90);
		}
	}
	[[nodiscard]] std::string Directory() const { return m_directory.Path().string(); }
	[[nodiscard]] std::string Prefix() const { return File("record").string(); }
	[[nodiscard]] fs::path File(const char* name) const { return m_directory.Path() / name; }
	[[nodiscard]] fs::path Report() const { return File("record-native-wave-input.json"); }

private:
	ScopedTestDirectory m_directory;
};

std::string ReadAll(const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::unique_ptr<const Core::Json> ReadJson(const fs::path& path)
{
	Core::Json::Init();
	return std::unique_ptr<const Core::Json>(Core::Json::Create(String::FromUtf8(ReadAll(path).c_str())));
}

ShaderNativeWaveInfo SensitiveWave(uint32_t size = 64u)
{
	ShaderNativeWaveInfo wave;
	wave.guest_wave_size = size;
	wave.proof = ShaderNativeWaveProof::ExactSubgroup;
	return wave;
}

GraphicsSkippedGeState SyntheticState()
{
	GraphicsSkippedGeState state;
	state.stages = 0x2030u;
	state.stages_raw.SetRaw(state.stages);
	state.ge_control_raw.SetDecoded();
	state.gs_resource1_raw.SetRaw(0xa523abcdu);
	state.gs_resource2_raw.SetRaw(0u);
	state.gs_resource3_raw.SetDecoded();
	state.es_program = 0x111122223300ull;
	state.gs_back_program = 0x555566667700ull;
	state.legacy_gs_program = 0x9999aaaabb00ull;
	state.gs_checksum = 0x1122334455667788ull;
	state.gs_user_data_address = 0xddddeeeeff00ull;
	state.max_vertex_out = 72u;
	state.max_output_per_subgroup = 216u;
	state.primitive_group_size = 3u;
	state.vertex_group_size = 24u;
	state.user_sgpr_count = 32u;
	for (size_t i = 0; i < state.user_sgprs.size(); ++i)
	{
		state.user_sgprs[i] = 0xa5000000u + static_cast<uint32_t>(i);
	}
	return state;
}

ShaderProgramSnapshot CompleteProgram(std::initializer_list<uint32_t> words)
{
	ShaderProgramSnapshot snapshot;
	snapshot.status = ShaderProgramSnapshotStatus::Complete;
	snapshot.words = words;
	snapshot.mapped_bytes = static_cast<uint32_t>(snapshot.words.size() * sizeof(uint32_t));
	return snapshot;
}

// The callback records every program access, including attempts with unmapped
// identities. Synthetic words are never parsed or executed as shader code.
struct CaptureFixture
{
	const SnapshotFiles* files = nullptr;
	DiagnosticDumpWriter* writer = nullptr;
	GraphicsSkippedGeState state = SyntheticState();
	ShaderProgramSnapshot es = CompleteProgram({0x11223344u, 0xaabbccddu});
	ShaderProgramSnapshot gs_back = CompleteProgram({0x55667788u});
	std::vector<uint64_t> addresses;
	std::vector<uint32_t> limits;
	bool write_seen_during_capture = false;

	static ShaderProgramSnapshot Copy(uint64_t address, uint32_t max_bytes, void* context)
	{
		auto& fixture = *static_cast<CaptureFixture*>(context);
		fixture.addresses.push_back(address);
		fixture.limits.push_back(max_bytes);
		if (fixture.files != nullptr)
		{
			fixture.write_seen_during_capture = fixture.write_seen_during_capture || fs::exists(fixture.files->Report()) ||
			                                  fs::exists(fixture.files->File("native-wave-es.bin")) ||
			                                  fs::exists(fixture.files->File("native-wave-gs-back.bin"));
		}
		if (fixture.writer != nullptr)
		{
			fixture.write_seen_during_capture = fixture.write_seen_during_capture || fixture.writer->TotalBytesWritten() != 0u;
		}
		if (address == fixture.state.es_program) { return fixture.es; }
		if (address == fixture.state.gs_back_program) { return fixture.gs_back; }
		return {};
	}
};

void ExpectRaw(const Core::Json* raw, const char* name, bool known, bool written, uint32_t value = 0u)
{
	const auto* field = raw->GetItem(name);
	ASSERT_NE(field, nullptr);
	EXPECT_EQ(field->GetBool("known", !known), known);
	EXPECT_EQ(field->GetBool("written", !written), written);
	if (known)
	{
		EXPECT_EQ(field->GetInt("value", -1), value);
	} else
	{
		EXPECT_TRUE(field->GetItem("value")->IsNull());
	}
}

} // namespace

TEST(EmulatorNativeWaveSnapshot, OutputRegistersDistinguishResetZeroFromAssignedZero)
{
	HW::Context context;
	HW::UserConfig user;
	HW::Shader shader;
	const auto reset = GraphicsDescribeSkippedGeState(context, user, shader);
	ASSERT_TRUE(reset.output_state.has_value());
	for (const auto* raw: {&reset.output_state->vs_out_config_raw, &reset.output_state->position_format_raw,
	                      &reset.output_state->output_control_raw, &reset.output_state->output_primitive_raw})
	{
		EXPECT_FALSE(raw->known);
		EXPECT_FALSE(raw->written);
	}
	context.SetVsOutConfig(0u);
	context.SetShaderPosFormat(0xfedcba98u);
	context.SetClVsOutCntl(0x80000000u);
	context.SetGsOutPrimType(0u);
	const HW::Context copied = context;
	context.Reset();
	const auto state = GraphicsDescribeSkippedGeState(copied, user, shader);
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	GraphicsNativeWaveInputReporter reporter(writer);
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), nullptr, state, SensitiveWave(), 64), "written");
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	const auto* output = json->GetItem("ge_state")->GetItem("output_state");
	const auto* raw = output->GetItem("raw_registers");
	ExpectRaw(raw, "vs_out_config", true, true, 0u);
	ExpectRaw(raw, "position_format", true, true, 0xfedcba98u);
	ExpectRaw(raw, "output_control", true, true, 0x80000000u);
	ExpectRaw(raw, "output_primitive", true, true, 0u);
	EXPECT_EQ(output->GetItem("effective")->GetInt("position_format", -1), 0xfedcba98u);
	const auto cleared = GraphicsDescribeSkippedGeState(context, user, shader);
	EXPECT_FALSE(cleared.output_state->position_format_raw.known);
	EXPECT_FALSE(cleared.output_state->position_format_raw.written);
	EXPECT_EQ(cleared.output_state->position_format, 0u);
}

TEST(EmulatorNativeWaveSnapshot, OwnsEffectivePixelAndRasterStateBeforeContextChanges)
{
	HW::Context context;
	HW::UserConfig user;
	HW::Shader shader;
	shader.SetPsShaderBase(0x123456789a00ull);
	shader.SetPsShaderChksum(0x12345678u);
	shader.SetPsShaderChksum(0x9abcdef0u);
	shader.SetPsEmbedded(7u);
	context.SetPsInputEna(0x11223344u);
	context.SetPsInputAddr(0x55667788u);
	context.SetPsInControl(0x99aabbccu);
	context.SetBarycCntl(0xddeeff00u);
	context.SetPsInputSettings(0u, 0u);
	context.SetPsInputSettings(31u, 0x80000000u);
	HW::ModeControl mode;
	mode.cull_front = true;
	mode.poly_mode = 3u;
	mode.polymode_back_ptype = 5u;
	mode.provoking_vtx_last = true;
	context.SetModeControl(mode);
	HW::ClipControl clip;
	clip.user_clip_planes = 63u;
	clip.user_clip_plane_mode = 2u;
	clip.force_viewport_index_from_vs_enable = true;
	context.SetClipControl(clip);
	const auto state = GraphicsDescribeSkippedGeState(context, user, shader);
	context.Reset();
	shader.SetPsShaderBase(0u);
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	GraphicsNativeWaveInputReporter reporter(writer);
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), nullptr, state, SensitiveWave(), 64), "written");
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	const auto* output = json->GetItem("ge_state")->GetItem("output_state");
	EXPECT_EQ(output->GetString("pixel_program"), String::FromUtf8("0x0000123456789a00"));
	EXPECT_EQ(output->GetString("pixel_checksum"), String::FromUtf8("0x123456789abcdef0"));
	EXPECT_TRUE(output->GetBool("pixel_embedded", false));
	EXPECT_EQ(output->GetInt("pixel_embedded_id", -1), 7);
	EXPECT_EQ(output->GetString("effective_state_means"), String::FromUtf8("emulator_state_not_raw_assignment"));
	ExpectRaw(output->GetItem("raw_registers"), "position_format", false, false);
	const auto* effective = output->GetItem("effective");
	EXPECT_EQ(effective->GetInt("pixel_input_enable", -1), 0x11223344u);
	EXPECT_EQ(effective->GetInt("pixel_input_address", -1), 0x55667788u);
	EXPECT_EQ(effective->GetInt("pixel_input_control", -1), 0x99aabbccu);
	EXPECT_EQ(effective->GetInt("barycentric_control", -1), 0xddeeff00u);
	EXPECT_EQ(effective->GetInt("interpolator_written_mask", -1), 0x80000001u);
	EXPECT_EQ(effective->GetItem("raster_mode")->GetInt("cull_front", -1), 1);
	EXPECT_EQ(effective->GetItem("raster_mode")->GetInt("cull_back", -1), 0);
	EXPECT_EQ(effective->GetItem("raster_mode")->GetInt("poly_mode", -1), 3);
	EXPECT_EQ(effective->GetItem("raster_mode")->GetInt("polymode_back_ptype", -1), 5);
	EXPECT_EQ(effective->GetItem("raster_mode")->GetInt("provoking_vtx_last", -1), 1);
	EXPECT_EQ(effective->GetItem("clip_control")->GetInt("user_clip_planes", -1), 63);
	EXPECT_EQ(effective->GetItem("clip_control")->GetInt("user_clip_plane_mode", -1), 2);
	EXPECT_EQ(effective->GetItem("clip_control")->GetInt("force_viewport_index_from_vs_enable", -1), 1);
	ASSERT_TRUE(state.output_state.has_value());
	EXPECT_EQ(state.output_state->interpolators[31], 0x80000000u);
	EXPECT_EQ(state.output_state->interpolators[0], 0u);
}

TEST(EmulatorNativeWaveSnapshot, DisabledPrefixNeverCapturesOrWrites)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	CaptureFixture capture;
	GraphicsNativeWaveInputReporter reporter(writer, CaptureFixture::Copy, &capture);
	for (const char* prefix: {static_cast<const char*>(nullptr), ""})
	{
		EXPECT_STREQ(reporter.Report(prefix, files.Directory().c_str(), capture.state, SensitiveWave(), 64), "disabled");
	}
	EXPECT_TRUE(capture.addresses.empty());
	EXPECT_EQ(writer.TotalBytesWritten(), 0u);
	EXPECT_TRUE(fs::is_empty(files.Directory()));
	// Enabling after a disabled call still selects the first eligible input.
	EXPECT_STREQ(reporter.Report(files.Prefix().c_str(), nullptr, capture.state, SensitiveWave(), 64), "written");
	EXPECT_TRUE(capture.addresses.empty());
}

TEST(EmulatorNativeWaveSnapshot, LaneLocalAndUnclassifiedInputsDoNotConsumeTheLatch)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	CaptureFixture capture;
	GraphicsNativeWaveInputReporter reporter(writer, CaptureFixture::Copy, &capture);
	for (const auto proof: {ShaderNativeWaveProof::LaneLocal, ShaderNativeWaveProof::Unclassified})
	{
		auto wave = SensitiveWave();
		wave.proof = proof;
		EXPECT_STREQ(reporter.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, wave, 64), "not_wave_sensitive");
	}
	EXPECT_TRUE(capture.addresses.empty());
	EXPECT_TRUE(fs::is_empty(files.Directory()));
	// Selection also applies to guest32; it is not tied to a host width or refusal.
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), nullptr, capture.state, SensitiveWave(32), 0), "written");
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	EXPECT_EQ(json->GetItem("native_wave")->GetInt("guest_wave_size", -1), 32);
	EXPECT_EQ(json->GetItem("native_wave")->GetInt("requested_subgroup_size", -1), 0);
}

TEST(EmulatorNativeWaveSnapshot, PreservesRawProvenanceAndExactDrawArguments)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	GraphicsNativeWaveInputReporter reporter(writer);
	const auto state = SyntheticState();
	auto wave = SensitiveWave();
	wave.refusal_pc = 0xabcdef10u;
	wave.refusal_reason = "synthetic \"reason\"\nwith a backslash \\";
	GraphicsNativeWaveDrawInfo draw;
	draw.count = UINT32_MAX;
	draw.indexed = true;
	draw.index_type = 0x80010002u;
	draw.first_instance = UINT32_MAX - 1u;
	draw.instance_count = UINT32_MAX - 2u;
	draw.index_address = 0xfffffffffffffff1ull;
	draw.vertex_offset_add = INT32_MIN;
	draw.draw_modifier = 0xfedcba9876543210ull;
	draw.primitive_type = 0xfedcba98u;
	draw.index_offset = 0x80000000u;
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), nullptr, state, wave, 64, draw), "written");
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	EXPECT_EQ(json->GetString("schema"), String::FromUtf8("native_wave_input"));
	EXPECT_EQ(json->GetString("stage"), String::FromUtf8("vertex"));
	EXPECT_EQ(json->GetString("recorded_means"), String::FromUtf8("input_observed_not_admitted_or_executed"));
	const auto* launch = json->GetItem("draw");
	EXPECT_EQ(launch->GetInt("count", -1), UINT32_MAX);
	EXPECT_TRUE(launch->GetBool("indexed", false));
	EXPECT_EQ(launch->GetInt("index_type", -1), 0x80010002u);
	EXPECT_EQ(launch->GetInt("first_instance", -1), UINT32_MAX - 1u);
	EXPECT_EQ(launch->GetInt("instance_count", -1), UINT32_MAX - 2u);
	EXPECT_EQ(launch->GetString("index_address"), String::FromUtf8("0xfffffffffffffff1"));
	EXPECT_EQ(launch->GetInt("vertex_offset_add", 0), INT32_MIN);
	EXPECT_EQ(launch->GetString("draw_modifier"), String::FromUtf8("0xfedcba9876543210"));
	EXPECT_EQ(launch->GetInt("primitive_type", -1), 0xfedcba98u);
	EXPECT_EQ(launch->GetInt("index_offset", -1), 0x80000000u);
	const auto* native = json->GetItem("native_wave");
	EXPECT_EQ(native->GetInt("guest_wave_size", -1), 64);
	EXPECT_EQ(native->GetInt("proof", -1), static_cast<uint32_t>(ShaderNativeWaveProof::ExactSubgroup));
	EXPECT_EQ(native->GetInt("requested_subgroup_size", -1), 64);
	EXPECT_EQ(native->GetInt("refusal_pc", -1), wave.refusal_pc);
	EXPECT_EQ(native->GetString("refusal_reason"), String::FromUtf8(wave.refusal_reason));
	EXPECT_FALSE(native->GetBool("refusal_reason_truncated", true));
	const auto* ge = json->GetItem("ge_state");
	const auto* raw = ge->GetItem("raw_registers");
	ExpectRaw(raw, "stages", true, true, 0x2030u);
	ExpectRaw(raw, "ge_control", false, true);
	ExpectRaw(raw, "ge_user_vgpr_en", false, false);
	ExpectRaw(raw, "gs_resource1", true, true, 0xa523abcdu);
	ExpectRaw(raw, "gs_resource2", true, true, 0u);
	ExpectRaw(raw, "gs_resource3", false, true);
	EXPECT_EQ(ge->GetString("es_program"), String::FromUtf8("0x0000111122223300"));
	EXPECT_EQ(ge->GetString("gs_back_program"), String::FromUtf8("0x0000555566667700"));
	EXPECT_EQ(ge->GetString("legacy_gs_program"), String::FromUtf8("0x00009999aaaabb00"));
	EXPECT_EQ(ge->GetString("gs_checksum"), String::FromUtf8("0x1122334455667788"));
	EXPECT_EQ(ge->GetString("gs_user_data_address"), String::FromUtf8("0x0000ddddeeeeff00"));
	EXPECT_EQ(ge->GetInt("max_vertex_out", -1), 72);
	EXPECT_EQ(ge->GetInt("max_output_per_subgroup", -1), 216);
	EXPECT_EQ(ge->GetInt("user_sgpr_count", -1), 32);
	const auto& sgprs = ge->GetItem("user_sgprs")->ToArray();
	ASSERT_EQ(sgprs.Size(), 32u);
	for (uint32_t i = 0; i < 32u; ++i) { EXPECT_EQ(sgprs.At(i)->ToInt(), state.user_sgprs[i]); }
	EXPECT_TRUE(json->GetItem("program_snapshots")->IsNull());
	EXPECT_LE(ReadAll(files.Report()).size(), NATIVE_WAVE_REPORT_BYTES_MAX);
}

TEST(EmulatorNativeWaveSnapshot, UnknownDrawFieldsAndRefusalReasonStayNull)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	GraphicsNativeWaveInputReporter reporter(writer);
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), nullptr, SyntheticState(), SensitiveWave(), 64), "written");
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	EXPECT_TRUE(json->GetItem("ge_state")->GetItem("output_state")->IsNull());
	const auto* draw = json->GetItem("draw");
	for (const char* name: {"count", "indexed", "index_type", "first_instance", "instance_count", "index_address", "vertex_offset_add",
	                       "draw_modifier", "primitive_type", "index_offset"})
	{
		EXPECT_TRUE(draw->GetItem(name)->IsNull()) << name;
	}
	const auto* native = json->GetItem("native_wave");
	EXPECT_EQ(native->GetInt("refusal_pc", -1), 0);
	EXPECT_TRUE(native->GetItem("refusal_reason")->IsNull());
	EXPECT_EQ(writer.TotalBytesWritten(), 0u);
}

TEST(EmulatorNativeWaveSnapshot, KnownZeroAutoDrawValuesAreNotUnknown)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	GraphicsNativeWaveInputReporter reporter(writer);
	GraphicsNativeWaveDrawInfo draw;
	draw.count = 0u;
	draw.indexed = false;
	draw.first_instance = 0u;
	draw.instance_count = 0u;
	draw.vertex_offset_add = 0;
	draw.draw_modifier = 0u;
	draw.primitive_type = 0u;
	draw.index_offset = 0u;
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), nullptr, SyntheticState(), SensitiveWave(), 64, draw), "written");
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	const auto* launch = json->GetItem("draw");
	EXPECT_EQ(launch->GetInt("count", -1), 0);
	EXPECT_FALSE(launch->GetBool("indexed", true));
	EXPECT_EQ(launch->GetInt("first_instance", -1), 0);
	EXPECT_EQ(launch->GetInt("instance_count", -1), 0);
	EXPECT_EQ(launch->GetInt("vertex_offset_add", -1), 0);
	EXPECT_EQ(launch->GetString("draw_modifier"), String::FromUtf8("0x0000000000000000"));
	EXPECT_EQ(launch->GetInt("primitive_type", -1), 0);
	EXPECT_EQ(launch->GetInt("index_offset", -1), 0);
	EXPECT_TRUE(launch->GetItem("index_type")->IsNull());
	EXPECT_TRUE(launch->GetItem("index_address")->IsNull());
}

TEST(EmulatorNativeWaveSnapshot, CapturesBothProgramsBeforeIoAndSeparatesTheBackProgram)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	CaptureFixture capture;
	capture.files = &files;
	capture.writer = &writer;
	GraphicsNativeWaveInputReporter reporter(writer, CaptureFixture::Copy, &capture);
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64), "written");
	EXPECT_EQ(capture.addresses, (std::vector<uint64_t> {capture.state.es_program, capture.state.gs_back_program}));
	EXPECT_EQ(capture.limits, (std::vector<uint32_t> {kShaderProgramSnapshotBytesMax, kShaderProgramSnapshotBytesMax}));
	EXPECT_FALSE(capture.write_seen_during_capture);
	EXPECT_EQ(ReadAll(files.File("native-wave-es.bin")), std::string(reinterpret_cast<const char*>(capture.es.words.data()), 8u));
	EXPECT_EQ(ReadAll(files.File("native-wave-gs-back.bin")), std::string(reinterpret_cast<const char*>(capture.gs_back.words.data()), 4u));
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	for (const char* name: {"es", "gs_back"})
	{
		const auto* program = json->GetItem("program_snapshots")->GetItem(name);
		EXPECT_EQ(program->GetString("snapshot"), String::FromUtf8("complete"));
		EXPECT_EQ(program->GetString("write"), String::FromUtf8("written"));
	}
}

TEST(EmulatorNativeWaveSnapshot, UnknownProgramsNeverCreateZeroFilledCode)
{
	for (const auto status: {ShaderProgramSnapshotStatus::Unmapped, ShaderProgramSnapshotStatus::InvalidRange,
	                        ShaderProgramSnapshotStatus::Unreadable})
	{
		const SnapshotFiles files;
		DiagnosticDumpWriter writer(64, 128);
		CaptureFixture capture;
		capture.es = {};
		capture.gs_back = {};
		capture.es.status = status;
		capture.gs_back.status = status;
		if (status == ShaderProgramSnapshotStatus::Unreadable)
		{
			capture.es.mapped_bytes = 16u;
			capture.gs_back.mapped_bytes = 8u;
		}
		GraphicsNativeWaveInputReporter reporter(writer, CaptureFixture::Copy, &capture);
		ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64), "written");
		const auto json = ReadJson(files.Report());
		ASSERT_NE(json, nullptr);
		for (const char* name: {"es", "gs_back"})
		{
			const auto* program = json->GetItem("program_snapshots")->GetItem(name);
			EXPECT_EQ(program->GetString("snapshot"), String::FromUtf8(ShaderProgramSnapshotStatusName(status)));
			EXPECT_EQ(program->GetInt("captured_bytes", -1), 0);
			EXPECT_EQ(program->GetString("write"), String::FromUtf8("invalid_data"));
		}
		EXPECT_FALSE(fs::exists(files.File("native-wave-es.bin")));
		EXPECT_FALSE(fs::exists(files.File("native-wave-gs-back.bin")));
		EXPECT_EQ(writer.TotalBytesWritten(), 0u);
	}
}

TEST(EmulatorNativeWaveSnapshot, CodeFilesAndReportAreExclusive)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	CaptureFixture capture;
	{
		std::ofstream existing(files.File("native-wave-es.bin"), std::ios::binary);
		existing << "preserved evidence";
	}
	GraphicsNativeWaveInputReporter reporter(writer, CaptureFixture::Copy, &capture);
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64), "written");
	EXPECT_EQ(ReadAll(files.File("native-wave-es.bin")), "preserved evidence");
	const auto original_report = ReadAll(files.Report());
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	EXPECT_EQ(json->GetItem("program_snapshots")->GetItem("es")->GetString("write"), String::FromUtf8("open_failed"));
	EXPECT_EQ(json->GetItem("program_snapshots")->GetItem("gs_back")->GetString("write"), String::FromUtf8("written"));
	GraphicsNativeWaveInputReporter other_process(writer, CaptureFixture::Copy, &capture);
	EXPECT_STREQ(other_process.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64), "open_failed");
	EXPECT_EQ(ReadAll(files.Report()), original_report);
	EXPECT_EQ(ReadAll(files.File("native-wave-es.bin")), "preserved evidence");
	EXPECT_EQ(writer.TotalBytesWritten(), 4u);
}

TEST(EmulatorNativeWaveSnapshot, MissingDirectoriesAndOverlongPathsReportFailureWithoutRetry)
{
	const SnapshotFiles files;
	const auto missing = files.File("missing");
	DiagnosticDumpWriter writer(64, 128);
	CaptureFixture capture;
	GraphicsNativeWaveInputReporter raw_failure(writer, CaptureFixture::Copy, &capture);
	ASSERT_STREQ(raw_failure.Report(files.Prefix().c_str(), missing.string().c_str(), capture.state, SensitiveWave(), 64), "written");
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	for (const char* name: {"es", "gs_back"})
	{
		EXPECT_EQ(json->GetItem("program_snapshots")->GetItem(name)->GetString("write"), String::FromUtf8("directory_invalid"));
	}
	EXPECT_FALSE(fs::exists(missing));
	GraphicsNativeWaveInputReporter report_failure(writer, CaptureFixture::Copy, &capture);
	EXPECT_STREQ(report_failure.Report((missing / "record").string().c_str(), nullptr, capture.state, SensitiveWave(), 64), "open_failed");
	EXPECT_STREQ(report_failure.Report(files.Prefix().c_str(), nullptr, capture.state, SensitiveWave(), 64), "already_reported");
	const auto captures_before = capture.addresses.size();
	GraphicsNativeWaveInputReporter path_failure(writer, CaptureFixture::Copy, &capture);
	EXPECT_STREQ(path_failure.Report(std::string(1024u, 'p').c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64), "path_too_long");
	EXPECT_STREQ(path_failure.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64), "already_reported");
	EXPECT_EQ(capture.addresses.size(), captures_before);
	EXPECT_EQ(writer.TotalBytesWritten(), 0u);
}

TEST(EmulatorNativeWaveSnapshot, DumpBudgetTruncationIsReportedSeparatelyFromCapturedBytes)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(2, 3);
	CaptureFixture capture;
	GraphicsNativeWaveInputReporter reporter(writer, CaptureFixture::Copy, &capture);
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64), "written");
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	const auto* programs = json->GetItem("program_snapshots");
	EXPECT_EQ(programs->GetItem("es")->GetInt("captured_bytes", -1), 8);
	EXPECT_EQ(programs->GetItem("gs_back")->GetInt("captured_bytes", -1), 4);
	for (const char* name: {"es", "gs_back"})
	{
		EXPECT_EQ(programs->GetItem(name)->GetString("snapshot"), String::FromUtf8("complete"));
		EXPECT_EQ(programs->GetItem(name)->GetString("write"), String::FromUtf8("truncated"));
	}
	EXPECT_EQ(fs::file_size(files.File("native-wave-es.bin")), 2u);
	EXPECT_EQ(fs::file_size(files.File("native-wave-gs-back.bin")), 1u);
	EXPECT_EQ(writer.TotalBytesWritten(), 3u);
}

TEST(EmulatorNativeWaveSnapshot, RefusalTextIsEscapedAndBoundedWithoutTruncatingJson)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	GraphicsNativeWaveInputReporter reporter(writer);
	auto state = SyntheticState();
	state.user_sgprs.fill(UINT32_MAX);
	auto& output = state.output_state.emplace();
	output.vs_out_config_raw.SetRaw(UINT32_MAX);
	output.position_format_raw.SetRaw(UINT32_MAX);
	output.output_control_raw.SetRaw(UINT32_MAX);
	output.output_primitive_raw.SetRaw(UINT32_MAX);
	output.vs_out_config = output.position_format = output.output_control = output.output_primitive = UINT32_MAX;
	output.pixel_program = output.pixel_checksum = UINT64_MAX;
	output.pixel_input_enable = output.pixel_input_address = output.pixel_input_control = output.barycentric_control = UINT32_MAX;
	output.interpolator_written_mask = UINT32_MAX;
	output.interpolators.fill(UINT32_MAX);
	output.raster_mode.fill(UINT32_MAX);
	output.clip_control.fill(UINT32_MAX);
	const std::string reason(2048u, '\x01');
	auto wave = SensitiveWave();
	wave.refusal_reason = reason.c_str();
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), nullptr, state, wave, UINT32_MAX), "written");
	const auto report = ReadAll(files.Report());
	EXPECT_LE(report.size(), NATIVE_WAVE_REPORT_BYTES_MAX);
	// Core::Json rejects every \u escape. Validate the entire original report
	// with the Unicode-capable protocol parser, without changing its contents.
	Emulator::Agent::Request request;
	Emulator::Agent::ErrorInfo error;
	ASSERT_TRUE(Emulator::Agent::ParseRequestLine(("{\"id\":1,\"tool\":\"status\",\"args\":" + report + '}').c_str(),
	                                            &request, &error)) << error.message;
	// This controlled fixture contains no braces in its reason. Keep field
	// checks on the original nested object, using the same parser's typed API.
	const std::string key = "\"native_wave\":";
	const auto begin = report.find(key);
	ASSERT_NE(begin, std::string::npos);
	const auto end = report.find("},\"draw\":", begin + key.size());
	ASSERT_NE(end, std::string::npos);
	const auto native = report.substr(begin + key.size(), end + 1u - begin - key.size());
	std::string decoded_reason;
	bool truncated = false;
	uint32_t requested = 0;
	ASSERT_TRUE(Emulator::Agent::ArgsGetString(native, "refusal_reason", &decoded_reason));
	EXPECT_EQ(decoded_reason, reason.substr(0, 160u));
	ASSERT_TRUE(Emulator::Agent::ArgsGetBool(native, "refusal_reason_truncated", &truncated));
	EXPECT_TRUE(truncated);
	ASSERT_TRUE(Emulator::Agent::ArgsGetU32(native, "requested_subgroup_size", &requested));
	EXPECT_EQ(requested, UINT32_MAX);
}

TEST(EmulatorNativeWaveSnapshot, FirstSnapshotKeepsItsOwnDrawArgumentsAndDoesNotCountASkip)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	CaptureFixture capture;
	GraphicsNativeWaveInputReporter reporter(writer, CaptureFixture::Copy, &capture);
	const auto counts_before = GraphicsGetSkippedDrawCounts();
	GraphicsNativeWaveDrawInfo draw;
	draw.count = 37u;
	draw.instance_count = 5u;
	draw.first_instance = 19u;
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64, draw), "written");
	const auto original_report = ReadAll(files.Report());
	draw.count = 1000u;
	draw.instance_count = 8u;
	draw.first_instance = 2u;
	capture.state.gs_back_program += 256u;
	EXPECT_STREQ(reporter.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64, draw), "already_reported");
	EXPECT_EQ(capture.addresses.size(), 2u);
	EXPECT_EQ(ReadAll(files.Report()), original_report);
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	EXPECT_EQ(json->GetItem("draw")->GetInt("count", -1), 37);
	EXPECT_EQ(json->GetItem("draw")->GetInt("instance_count", -1), 5);
	EXPECT_EQ(json->GetItem("draw")->GetInt("first_instance", -1), 19);
	EXPECT_EQ(json->GetItem("ge_state")->GetString("gs_back_program"), String::FromUtf8("0x0000555566667700"));
	const auto counts_after = GraphicsGetSkippedDrawCounts();
	EXPECT_EQ(counts_after.invalid_vertex_shader, counts_before.invalid_vertex_shader);
	EXPECT_EQ(counts_after.unsupported_ge_state, counts_before.unsupported_ge_state);
}

TEST(EmulatorNativeWaveSnapshot, ConcurrentCallersHaveOneWinnerWithMatchingDrawMetadata)
{
	const SnapshotFiles files;
	DiagnosticDumpWriter writer(64, 128);
	CaptureFixture capture;
	GraphicsNativeWaveInputReporter reporter(writer, CaptureFixture::Copy, &capture);
	std::array<const char*, 8> outcomes {};
	std::vector<std::thread> workers;
	for (size_t i = 0; i < outcomes.size(); ++i)
	{
		workers.emplace_back([&, i]
		{
			GraphicsNativeWaveDrawInfo draw;
			draw.count = 100u + static_cast<uint32_t>(i);
			outcomes[i] = reporter.Report(files.Prefix().c_str(), files.Directory().c_str(), capture.state, SensitiveWave(), 64, draw);
		});
	}
	for (auto& worker: workers) { worker.join(); }
	uint32_t winners = 0;
	uint32_t winning_count = 0;
	for (size_t i = 0; i < outcomes.size(); ++i)
	{
		if (std::string(outcomes[i]) == "written")
		{
			++winners;
			winning_count = 100u + static_cast<uint32_t>(i);
		} else
		{
			EXPECT_STREQ(outcomes[i], "already_reported");
		}
	}
	ASSERT_EQ(winners, 1u);
	EXPECT_EQ(capture.addresses.size(), 2u);
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	EXPECT_EQ(json->GetItem("draw")->GetInt("count", -1), winning_count);
}

namespace {

class ScopedMappedProgram final
{
public:
	explicit ScopedMappedProgram(const std::vector<uint32_t>& words)
	{
		const uint64_t bytes = words.size() * sizeof(uint32_t);
		const uint64_t page = Core::VirtualMemory::GetPageSize();
		address = Core::VirtualMemory::Alloc(0, ((bytes + page - 1u) / page) * page, Core::VirtualMemory::Mode::ReadWrite);
		if (address != 0u)
		{
			copied = Core::VirtualMemory::CopyToGuest(address, words.data(), bytes);
			ShaderMappedData mapped;
			mapped.code_size_bytes = static_cast<uint32_t>(bytes);
			ShaderMapUserData(address, mapped);
		}
	}
	~ScopedMappedProgram()
	{
		if (address != 0u)
		{
			ShaderMapUserData(address, {});
			(void)Core::VirtualMemory::Free(address);
		}
	}
	uint64_t address = 0;
	bool copied = false;
};

void CheckDefaultCaptureBound()
{
	ShaderInit();
	const SnapshotFiles files;
	std::vector<uint32_t> es_words(kShaderProgramSnapshotBytesMax / sizeof(uint32_t) + 1u);
	for (size_t i = 0; i < es_words.size(); ++i) { es_words[i] = 0x40000000u + static_cast<uint32_t>(i); }
	const std::vector<uint32_t> back_words {0x12345678u, 0x89abcdefu};
	const ScopedMappedProgram es(es_words);
	const ScopedMappedProgram back(back_words);
	ASSERT_TRUE(es.copied);
	ASSERT_TRUE(back.copied);
	auto state = SyntheticState();
	state.es_program = es.address;
	state.gs_back_program = back.address;
	DiagnosticDumpWriter writer(kShaderProgramSnapshotBytesMax, 2u * kShaderProgramSnapshotBytesMax);
	GraphicsNativeWaveInputReporter reporter(writer);
	ASSERT_STREQ(reporter.Report(files.Prefix().c_str(), files.Directory().c_str(), state, SensitiveWave(), 64), "written");
	EXPECT_EQ(ReadAll(files.File("native-wave-es.bin")), std::string(reinterpret_cast<const char*>(es_words.data()), kShaderProgramSnapshotBytesMax));
	EXPECT_EQ(ReadAll(files.File("native-wave-gs-back.bin")), std::string(reinterpret_cast<const char*>(back_words.data()), 8u));
	const auto json = ReadJson(files.Report());
	ASSERT_NE(json, nullptr);
	const auto* programs = json->GetItem("program_snapshots");
	const auto* front = programs->GetItem("es");
	EXPECT_EQ(front->GetString("snapshot"), String::FromUtf8("truncated"));
	EXPECT_EQ(front->GetInt("mapped_bytes", -1), kShaderProgramSnapshotBytesMax + 4u);
	EXPECT_EQ(front->GetInt("captured_bytes", -1), kShaderProgramSnapshotBytesMax);
	EXPECT_EQ(front->GetString("write"), String::FromUtf8("written"));
	EXPECT_EQ(programs->GetItem("gs_back")->GetString("snapshot"), String::FromUtf8("complete"));
	EXPECT_EQ(programs->GetItem("gs_back")->GetInt("mapped_bytes", -1), 8);
}

class ScopedSnapshotDeathTestStyle final
{
public:
	ScopedSnapshotDeathTestStyle(): previous(::testing::FLAGS_gtest_death_test_style) { ::testing::FLAGS_gtest_death_test_style = "threadsafe"; }
	~ScopedSnapshotDeathTestStyle() { ::testing::FLAGS_gtest_death_test_style = previous; }

private:
	std::string previous;
};

} // namespace

TEST(EmulatorNativeWaveSnapshot, DefaultCaptureUsesMetadataAndThe256KiBCodeLimit)
{
	const ScopedSnapshotDeathTestStyle style;
	ASSERT_EXIT(
	    {
		    CheckDefaultCaptureBound();
		    std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
