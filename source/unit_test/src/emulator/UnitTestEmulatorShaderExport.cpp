#include "Kyty/UnitTest.h"

#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderNggFront.h"
#include "Emulator/Graphics/Objects/RenderTexture.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "../../../emulator/src/Graphics/ShaderDebugInternal.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

UT_BEGIN(EmulatorShaderExport);

using namespace Libs::Graphics;

namespace {

constexpr uint8_t kShaderColorExportModeUint16Abgr = 7u;
constexpr uint8_t kShaderColorExportNumberUint     = 4u;

#if defined(_WIN32)
constexpr int kRejectedExit = 321;
#else
constexpr int kRejectedExit = 65;
#endif

constexpr uint32_t kEndpgm = 0xbf810000u;
constexpr uint32_t kExecZero = 0xbefe0480u; // s_mov_b64 exec, 0
constexpr uint32_t kExecFull = 0xbefe04c1u; // s_mov_b64 exec, -1
// Every physical field is distinct, including unused and disabled fields.
// Packed sources contain half pairs (0.5, 1) and (2, 4); Z also has nonzero
// layer bits. These are synthetic values, unrelated to a captured program.
constexpr std::array<int, 4> kRegisters = {17, 61, 173, 239};
constexpr std::array<uint32_t, 4> kValues = {0x3c003800u, 0x44004000u, 0x40a00537u, 0x3f800000u};
constexpr uint32_t kSourceWord = 0xefad3d11u;

struct ExportCase
{
	ShaderType stage;
	uint32_t word;
	ShaderInstructionFormat::Format format;
	int sources;
	uint8_t enable;
	uint8_t control;
	int first_physical = 0;
};

constexpr ExportCase kPrim = {ShaderType::Vertex, 0xf8000941u, ShaderInstructionFormat::PrimVsrc0OffOffOffDone, 1, 1, 2};
constexpr ExportCase kPos1 = {ShaderType::Vertex, 0xf80000d4u, ShaderInstructionFormat::Pos1OffOffVsrc0Off, 1, 4, 0, 2};
constexpr ExportCase kPos1Done = {ShaderType::Vertex, 0xf80008d4u, ShaderInstructionFormat::Pos1OffOffVsrc0Off, 1, 4, 2, 2};
constexpr ExportCase kPos1X = {ShaderType::Vertex, 0xf80000d1u, ShaderInstructionFormat::Pos1Vsrc0OffOffOff, 1, 1, 0};
constexpr ExportCase kPixelZ = {ShaderType::Pixel, 0xf8001881u, ShaderInstructionFormat::PixelZVsrc0VmDone, 1, 1, 3};
constexpr ExportCase kPixelZNoDone = {ShaderType::Pixel, 0xf8001081u, ShaderInstructionFormat::PixelZVsrc0Vm, 1, 1, 1};
constexpr ExportCase kNull = {ShaderType::Pixel, 0xf8001890u, ShaderInstructionFormat::NullVmDone, 0, 0, 3};
constexpr ExportCase kPixelZNull = {ShaderType::Pixel, 0xf8001880u, ShaderInstructionFormat::NullVmDone, 0, 0, 3};
constexpr ExportCase kPos0 = {ShaderType::Vertex, 0xf80008cfu, ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done, 4, 15, 2};
constexpr auto kPos0NoDoneFormat = ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3;
constexpr auto kPos1XDoneFormat = ShaderInstructionFormat::Pos1Vsrc0OffOffOffDone;
constexpr ExportCase kPos0NoDone = {ShaderType::Vertex, 0xf80000cfu, kPos0NoDoneFormat, 4, 15, 0};
constexpr ExportCase kPos1XDone = {ShaderType::Vertex, 0xf80008d1u, kPos1XDoneFormat, 1, 1, 2};
constexpr ExportCase kParam0 = {ShaderType::Vertex, 0xf800020fu, ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3, 4, 15, 0};
constexpr ExportCase kParam31 = {ShaderType::Vertex, 0xf80003ffu, ShaderInstructionFormat::Param31Vsrc0Vsrc1Vsrc2Vsrc3, 4, 15, 0};

std::string g_case;

void Require(bool condition, const std::string& message)
{
	if (!condition)
	{
		std::fprintf(stderr, "EXP fixture: %s\ncase: %s\n", message.c_str(), g_case.c_str());
		std::fflush(stderr);
		std::_Exit(7);
	}
}

void Initialize(bool next_gen = true)
{
	// Redirect only the child: Core EXIT uses stdout, while the death-test
	// matcher captures stderr. Match the actual EXP diagnostic, not any crash.
	std::fflush(stdout);
#if defined(_WIN32)
	Require(::_dup2(::_fileno(stderr), ::_fileno(stdout)) >= 0, "redirect diagnostic");
#else
	Require(::dup2(::fileno(stderr), ::fileno(stdout)) >= 0, "redirect diagnostic");
#endif
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(next_gen);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation;
	Config::Load(validation);
}

ShaderOperand Vgpr(int reg)
{
	ShaderOperand operand {};
	operand.type = ShaderOperandType::Vgpr;
	operand.register_id = reg;
	operand.size = 1;
	return operand;
}

bool ExactOperand(const ShaderOperand& operand, const ShaderOperand& expected)
{
	// operator== intentionally excludes some modifiers; check those as well.
	return operand == expected && operand.multiplier == expected.multiplier && operand.absolute == expected.absolute &&
	       operand.negate == expected.negate && operand.clamp == expected.clamp;
}

uint32_t ExportWord(uint32_t target, uint8_t enable, uint8_t control)
{
	return 0xf8000000u | (target << 4u) | enable | ((control & 1u) << 12u) | ((control & 2u) << 10u) |
	       ((control & 4u) << 8u);
}

ExportCase Mrt(uint32_t target, uint8_t enable, uint8_t control)
{
	constexpr ShaderInstructionFormat::Format compressed[] = {
	    ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone, ShaderInstructionFormat::Mrt1Vsrc0Vsrc1ComprVm,
	    ShaderInstructionFormat::Mrt2Vsrc0Vsrc1ComprVm, ShaderInstructionFormat::Mrt3Vsrc0Vsrc1ComprVm,
	    ShaderInstructionFormat::Mrt4Vsrc0Vsrc1ComprVm, ShaderInstructionFormat::Mrt5Vsrc0Vsrc1ComprVm,
	    ShaderInstructionFormat::Mrt6Vsrc0Vsrc1ComprVm, ShaderInstructionFormat::Mrt7Vsrc0Vsrc1ComprVm};
	constexpr ShaderInstructionFormat::Format full[] = {
	    ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone, ShaderInstructionFormat::Mrt1Vsrc0Vsrc1Vsrc2Vsrc3Vm,
	    ShaderInstructionFormat::Mrt2Vsrc0Vsrc1Vsrc2Vsrc3Vm, ShaderInstructionFormat::Mrt3Vsrc0Vsrc1Vsrc2Vsrc3Vm,
	    ShaderInstructionFormat::Mrt4Vsrc0Vsrc1Vsrc2Vsrc3Vm, ShaderInstructionFormat::Mrt5Vsrc0Vsrc1Vsrc2Vsrc3Vm,
	    ShaderInstructionFormat::Mrt6Vsrc0Vsrc1Vsrc2Vsrc3Vm, ShaderInstructionFormat::Mrt7Vsrc0Vsrc1Vsrc2Vsrc3Vm};
	constexpr ShaderInstructionFormat::Format null[] = {
	    ShaderInstructionFormat::Mrt0OffOffComprVmDone, ShaderInstructionFormat::Mrt1OffOffComprVmDone,
	    ShaderInstructionFormat::Mrt2OffOffComprVmDone, ShaderInstructionFormat::Mrt3OffOffComprVmDone,
	    ShaderInstructionFormat::Mrt4OffOffComprVmDone, ShaderInstructionFormat::Mrt5OffOffComprVmDone,
	    ShaderInstructionFormat::Mrt6OffOffComprVmDone, ShaderInstructionFormat::Mrt7OffOffComprVmDone};
	Require(target < 8u, "bounded MRT target");
	const bool packed = (control & 4u) != 0u;
	return {ShaderType::Pixel, ExportWord(target, enable, control), enable == 0u ? null[target] : packed ? compressed[target] : full[target],
	        enable == 0u ? 0 : packed ? 2 : 4, enable, control};
}

void CheckExport(const ShaderInstruction& inst, const ExportCase& test, uint32_t pc)
{
	Require(inst.type == ShaderInstructionType::Exp && inst.format == test.format, "exact EXP identity/format");
	Require(inst.pc == pc && inst.raw_word == test.word && inst.sopp_opcode == 0xffu, "original PC and raw opcode/control word");
	Require(inst.src_num == test.sources && inst.exp_enable_mask == test.enable && inst.exp_control == test.control,
	        "physical source count, EN and VM/DONE/COMPR");
	Require(ExactOperand(inst.dst, {}) && ExactOperand(inst.dst2, {}), "no fabricated destinations");
	for (int source = 0; source < 4; ++source)
	{
		if (source < test.sources)
		{
			Require(ExactOperand(inst.src[source], Vgpr(kRegisters[source + test.first_physical])), "active physical VGPR mapping");
		} else
		{
			Require(inst.src[source].type == ShaderOperandType::Unknown && inst.src[source].size == 0 &&
			            ExactOperand(inst.src[source], {}), "inactive source is exactly Unknown/size0/default, not a stale VGPR");
		}
	}
	Require(ShaderInstructionLoweringPreconditions(inst), "parsed EXP passes the unchanged shared preconditions");
}

struct Program
{
	ShaderCode code;
	uint32_t export_index = 4;
	uint32_t export_pc = 32;
};

Program Parse(const ExportCase& test, bool position_before = false, bool position_after = false, bool exec_zero = false)
{
	g_case = String8::FromPrintf("stage=%u word=0x%08" PRIx32 " en=%u control=%u position=%u/%u exec_zero=%u",
	                            static_cast<unsigned>(test.stage), test.word, static_cast<unsigned>(test.enable),
	                            static_cast<unsigned>(test.control), static_cast<unsigned>(position_before),
	                            static_cast<unsigned>(position_after), static_cast<unsigned>(exec_zero)).c_str();
	std::vector<uint32_t> words;
	for (unsigned source = 0; source < 4; ++source)
	{
		words.push_back(0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u)); // v_mov_b32 vN, literal
		words.push_back(kValues[source]);
	}
	Program result;
	if (position_before)
	{
		words.insert(words.end(), {kPos0.word, kSourceWord});
		++result.export_index;
	}
	if (exec_zero)
	{
		words.push_back(kExecZero);
		++result.export_index;
	}
	result.export_pc = static_cast<uint32_t>(words.size() * sizeof(uint32_t));
	words.insert(words.end(), {test.word, kSourceWord});
	if (position_after) { words.insert(words.end(), {kPos0.word, kSourceWord}); }
	words.push_back(kEndpgm);
	result.code.SetType(test.stage);
	Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &result.code),
	        "complete bounded parser input");
	const auto& instructions = result.code.GetInstructions();
	Require(instructions.Size() == result.export_index + 2u + (position_after ? 1u : 0u), "instruction count includes real terminator");
	for (unsigned source = 0; source < 4; ++source)
	{
		const auto& move = instructions.At(source);
		Require(move.type == ShaderInstructionType::VMovB32 && move.pc == source * 8u && move.raw_word == words[source * 2u] &&
		            ExactOperand(move.dst, Vgpr(kRegisters[source])) && move.src_num == 1 &&
		            move.src[0].type == ShaderOperandType::LiteralConstant && move.src[0].constant.u == kValues[source],
		        "distinct original producer values/registers/PCs survive parsing");
	}
	CheckExport(instructions.At(result.export_index), test, result.export_pc);
	if (position_before) { CheckExport(instructions.At(4), kPos0, 32u); }
	if (position_after) { CheckExport(instructions.At(result.export_index + 1u), kPos0, result.export_pc + 8u); }
	const auto& end = instructions.At(instructions.Size() - 1u);
	Require(end.type == ShaderInstructionType::SEndpgm && end.format == ShaderInstructionFormat::Empty && end.raw_word == kEndpgm &&
	            end.pc == (words.size() - 1u) * sizeof(uint32_t), "following terminal PC/raw word preserved");
	for (const auto& inst: instructions)
	{
		Require(ShaderInstructionLoweringPreconditions(inst), "every parsed instruction satisfies preconditions");
	}
	return result;
}

// Two color exports in program order. Producers and source words match Parse.
Program ParseColorPair(const ExportCase& first, const ExportCase& second)
{
	g_case = String8::FromPrintf("pair first=0x%08" PRIx32 " second=0x%08" PRIx32, first.word, second.word).c_str();
	std::vector<uint32_t> words;
	for (unsigned source = 0; source < 4; ++source)
	{
		words.push_back(0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u)); // v_mov_b32 vN, literal
		words.push_back(kValues[source]);
	}
	words.insert(words.end(), {first.word, kSourceWord, second.word, kSourceWord, kEndpgm});
	Program result;
	result.code.SetType(ShaderType::Pixel);
	Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &result.code),
	        "complete bounded parser input");
	const auto& instructions = result.code.GetInstructions();
	Require(instructions.Size() == 7u, "four literal producers, two exports and the terminator");
	CheckExport(instructions.At(4), first, 32u);
	CheckExport(instructions.At(5), second, 40u);
	return result;
}

Program ParsePositionSequence(std::initializer_list<ExportCase> exports)
{
	std::vector<uint32_t> words;
	for (unsigned source = 0; source < 4; ++source)
	{
		words.push_back(0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u)); // v_mov_b32 vN, literal
		words.push_back(kValues[source]);
	}
	for (const auto& test: exports) { words.insert(words.end(), {test.word, kSourceWord}); }
	words.push_back(kEndpgm);
	g_case = "synthetic full POS0(no DONE), POS1.X(DONE), PARAM0 suffix";

	Program result;
	result.export_index = 4u;
	result.export_pc = 32u;
	result.code.SetType(ShaderType::Vertex);
	Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &result.code),
	        "complete bounded position sequence parses");
	const auto& instructions = result.code.GetInstructions();
	Require(instructions.Size() == 5u + exports.size(), "all ordered exports and the real terminator remain separate instructions");
	uint32_t index = result.export_index;
	for (const auto& test: exports)
	{
		CheckExport(instructions.At(index), test, result.export_pc + (index - result.export_index) * 8u);
		++index;
	}
	const auto& end = instructions.At(instructions.Size() - 1u);
	Require(end.type == ShaderInstructionType::SEndpgm && end.format == ShaderInstructionFormat::Empty &&
	            end.raw_word == kEndpgm && end.pc == (words.size() - 1u) * sizeof(uint32_t),
	        "terminal PC/raw word follows the complete export suffix");
	for (const auto& inst: instructions) { Require(ShaderInstructionLoweringPreconditions(inst), "sequence instruction satisfies shared preconditions"); }
	return result;
}

Program ParseExecParameterSequence(const ExportCase& parameter)
{
	g_case = String8::FromPrintf("parameter target=0x%08" PRIx32 " with EXEC clear/restore", parameter.word).c_str();
	std::vector<uint32_t> words;
	for (unsigned source = 0; source < 4; ++source)
	{
		words.push_back(0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u)); // v_mov_b32 vN, literal
		words.push_back(kValues[source]);
	}
	words.insert(words.end(), {parameter.word, kSourceWord, kExecZero, parameter.word, kSourceWord,
	                           kExecFull, parameter.word, kSourceWord, kPos0.word, kSourceWord, kEndpgm});

	Program result;
	result.code.SetType(ShaderType::Vertex);
	Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &result.code),
	        "complete bounded parameter EXEC sequence parses");
	const auto& instructions = result.code.GetInstructions();
	Require(instructions.Size() == 11u, "parameter sequence contains the seed, clear, inactive and restored exports, POS0, and terminator");
	CheckExport(instructions.At(4u), parameter, 32u);
	Require(instructions.At(5u).type == ShaderInstructionType::SMovB64 && instructions.At(5u).raw_word == kExecZero,
	        "zero EXEC separates the seed export from the inactive write attempt");
	CheckExport(instructions.At(6u), parameter, 44u);
	Require(instructions.At(7u).type == ShaderInstructionType::SMovB64 && instructions.At(7u).raw_word == kExecFull,
	        "full EXEC is restored after the inactive export");
	CheckExport(instructions.At(8u), parameter, 56u);
	CheckExport(instructions.At(9u), kPos0, 64u);
	const auto& end = instructions.At(10u);
	Require(end.type == ShaderInstructionType::SEndpgm && end.format == ShaderInstructionFormat::Empty && end.raw_word == kEndpgm &&
	            end.pc == 72u,
	        "terminal POS0 is followed by the real program terminator");
	for (const auto& inst: instructions) { Require(ShaderInstructionLoweringPreconditions(inst), "sequence instruction satisfies shared preconditions"); }
	return result;
}

Program ParseParameterThenScalar(const ExportCase& parameter)
{
	g_case = String8::FromPrintf("parameter target=0x%08" PRIx32 " followed by scalar EXEC write", parameter.word).c_str();
	std::vector<uint32_t> words;
	for (unsigned source = 0; source < 4; ++source)
	{
		words.push_back(0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u)); // v_mov_b32 vN, literal
		words.push_back(kValues[source]);
	}
	words.insert(words.end(), {parameter.word, kSourceWord, kExecZero, kEndpgm});

	Program result;
	result.code.SetType(ShaderType::Vertex);
	Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &result.code),
	        "complete bounded parameter/scalar input parses");
	const auto& instructions = result.code.GetInstructions();
	Require(instructions.Size() == 7u, "parameter export is followed by scalar EXEC and the real terminator");
	CheckExport(instructions.At(4u), parameter, 32u);
	Require(instructions.At(5u).type == ShaderInstructionType::SMovB64 && instructions.At(5u).raw_word == kExecZero,
	        "scalar EXEC instruction remains after PARAM");
	const auto& end = instructions.At(6u);
	Require(end.type == ShaderInstructionType::SEndpgm && end.format == ShaderInstructionFormat::Empty && end.raw_word == kEndpgm &&
	            end.pc == 44u,
	        "terminal instruction follows scalar EXEC");
	for (const auto& inst: instructions) { Require(ShaderInstructionLoweringPreconditions(inst), "sequence instruction satisfies shared preconditions"); }
	return result;
}

std::string Instructions(const String8& source)
{
	std::istringstream input(source.c_str());
	std::string result = "\n";
	for (std::string line; std::getline(input, line);)
	{
		std::istringstream words(line.substr(0, line.find(';')));
		std::string instruction;
		for (std::string word; words >> word;)
		{
			if (!instruction.empty()) { instruction += ' '; }
			instruction += word;
		}
		if (!instruction.empty()) { result += instruction + '\n'; }
	}
	return result;
}

void Has(const std::string& source, const String8& expected)
{
	Require(source.find(Instructions(expected)) != std::string::npos, "missing instruction block: " + std::string(expected.c_str()));
}

std::string Validate(const Program& program, const ShaderVertexInputInfo* vertex, const ShaderPixelInputInfo* pixel)
{
	Require((program.code.GetType() == ShaderType::Vertex) == (vertex != nullptr) &&
	            (program.code.GetType() == ShaderType::Pixel) == (pixel != nullptr), "stage-correct input metadata");
	const auto assembly = SpirvGenerateSource(program.code, vertex, pixel, nullptr);
	Require(!assembly.IsEmpty(), "complete production module source");
	const auto source = Instructions(assembly);
	Require(source.find(vertex != nullptr ? "OpEntryPoint Vertex %main" : "OpEntryPoint Fragment %main") != std::string::npos,
	        "correct SPIR-V entry stage");
	for (unsigned index = 0; index < 4; ++index)
	{
		// Pin the actual literal-to-VGPR dataflow as well as each export's
		// consumer below. No numerical conversion or replacement value is used.
		Has(source, String8::FromPrintf("%%uint_0x%08" PRIx32 " = OpConstant %%uint 0x%08" PRIx32, kValues[index], kValues[index]));
		Has(source, String8::FromPrintf("%%t0_%u = OpBitcast %%float %%uint_0x%08" PRIx32, index, kValues[index]));
		Has(source, String8::FromPrintf("%%tdst_%u = OpLoad %%float %%v%d\n"
		                               "%%tval_%u = OpSelect %%float %%exec_lo_b_%u %%t0_%u %%tdst_%u\n"
		                               "OpStore %%v%d %%tval_%u", index, kRegisters[index], index, index, index, index,
		                               kRegisters[index], index));
	}
	Require(source.find("\nOpReturn\nOpFunctionEnd\n") != std::string::npos ||
	            source.find("\nOpKill\nOpFunctionEnd\n") != std::string::npos, "main has its real return or discard terminator");
	Require(Config::ShaderValidationEnabled(), "Vulkan 1.4 module validation enabled at the toolchain boundary");
	Vector<uint32_t> binary;
	String8 error;
	const bool valid = ShaderToolchain::Run(assembly, &binary, &error);
	Require(valid, error.c_str());
	Require(!binary.IsEmpty(), "validated complete module binary");
	return source;
}

void RequirePositionStoreGuard(const std::string& source, uint32_t index, bool require_clip_probe = false)
{
	std::istringstream input(Instructions(String8(source.c_str())));
	std::vector<std::string> lines;
	for (std::string line; std::getline(input, line);) { lines.push_back(line); }
	const auto store = "OpStore %t5_" + std::to_string(index) + " %t4_" + std::to_string(index);
	const auto store_it = std::find(lines.begin(), lines.end(), store);
	Require(store_it != lines.end(), "position output store is emitted");
	const auto store_index = static_cast<uint32_t>(std::distance(lines.begin(), store_it));

	uint32_t branch_index = UINT32_MAX;
	for (uint32_t line = 0; line < store_index; ++line)
	{
		if (lines[line].find("OpBranchConditional ") != std::string::npos) { branch_index = line; }
	}
	Require(branch_index != UINT32_MAX, "position store has a preceding structured conditional");
	std::istringstream branch_stream(lines[branch_index]);
	std::string branch_op;
	std::string condition;
	std::string active_target;
	std::string merge_target;
	branch_stream >> branch_op >> condition >> active_target >> merge_target;
	Require(branch_op == "OpBranchConditional" && !condition.empty() && !active_target.empty() && !merge_target.empty() &&
	            active_target != merge_target,
	        "position conditional has distinct active and merge successors");

	const auto condition_prefix = condition + " = OpINotEqual %bool ";
	uint32_t condition_index = UINT32_MAX;
	for (uint32_t line = 0; line < branch_index; ++line)
	{
		if (lines[line].compare(0, condition_prefix.size(), condition_prefix) == 0) { condition_index = line; }
	}
	Require(condition_index != UINT32_MAX, "position branch condition compares active EXEC with zero");
	std::istringstream condition_stream(lines[condition_index]);
	std::string condition_result;
	std::string equals;
	std::string compare;
	std::string boolean_type;
	std::string exec_value;
	std::string zero_value;
	condition_stream >> condition_result >> equals >> compare >> boolean_type >> exec_value >> zero_value;
	Require(condition_result == condition && equals == "=" && compare == "OpINotEqual" && boolean_type == "%bool" &&
	            zero_value == "%uint_0",
	        "position branch predicate is EXEC != 0");
	const auto exec_load = exec_value + " = OpLoad %uint %exec_lane_lo";
	Require(std::find(lines.begin(), lines.begin() + condition_index, exec_load) != lines.begin() + condition_index,
	        "position branch reads the resolved per-invocation EXEC lane");

	const auto active_label = active_target + " = OpLabel";
	const auto merge_label = merge_target + " = OpLabel";
	const auto active_it = std::find(lines.begin() + branch_index + 1u, store_it, active_label);
	const auto merge_it = std::find(store_it + 1u, lines.end(), merge_label);
	Require(active_it != store_it && merge_it != lines.end(), "position store is in the active successor before the merge");
	if (require_clip_probe)
	{
		const auto probe = "%vertex_clip_probe_invocations_prior_" + std::to_string(index) + " = OpAtomicIAdd";
		const auto probe_it = std::find_if(active_it, merge_it, [&probe](const std::string& line) {
			return line.compare(0, probe.size(), probe) == 0 && (line.size() == probe.size() || line[probe.size()] == ' ');
		});
		Require(probe_it != merge_it,
		        "position clip-probe invocation update shares the active EXEC region");
	}
}

uint32_t RequireParameterStoreGuard(const std::string& source, uint32_t index, uint32_t parameter, bool require_clip_probe)
{
	std::istringstream input(Instructions(String8(source.c_str())));
	std::vector<std::string> lines;
	for (std::string line; std::getline(input, line);) { lines.push_back(line); }
	const auto store = "OpStore %param" + std::to_string(parameter) + " %t4_" + std::to_string(index);
	const auto store_it = std::find(lines.begin(), lines.end(), store);
	Require(store_it != lines.end(), "parameter output store is emitted");
	const auto store_index = static_cast<uint32_t>(std::distance(lines.begin(), store_it));

	uint32_t branch_index = UINT32_MAX;
	for (uint32_t line = 0; line < store_index; ++line)
	{
		if (lines[line].find("OpBranchConditional ") != std::string::npos) { branch_index = line; }
	}
	Require(branch_index != UINT32_MAX, "parameter output store has a preceding structured EXEC conditional");
	std::istringstream branch_stream(lines[branch_index]);
	std::string branch_op;
	std::string condition;
	std::string active_target;
	std::string merge_target;
	branch_stream >> branch_op >> condition >> active_target >> merge_target;
	Require(branch_op == "OpBranchConditional" && !condition.empty() && !active_target.empty() && !merge_target.empty() &&
	            active_target != merge_target,
	        "parameter EXEC conditional has distinct active and merge successors");

	const auto condition_prefix = condition + " = OpINotEqual %bool ";
	uint32_t condition_index = UINT32_MAX;
	for (uint32_t line = 0; line < branch_index; ++line)
	{
		if (lines[line].compare(0, condition_prefix.size(), condition_prefix) == 0) { condition_index = line; }
	}
	Require(condition_index != UINT32_MAX, "parameter branch condition compares resolved EXEC with zero");
	std::istringstream condition_stream(lines[condition_index]);
	std::string condition_result;
	std::string equals;
	std::string compare;
	std::string boolean_type;
	std::string exec_value;
	std::string zero_value;
	condition_stream >> condition_result >> equals >> compare >> boolean_type >> exec_value >> zero_value;
	Require(condition_result == condition && equals == "=" && compare == "OpINotEqual" && boolean_type == "%bool" &&
	            zero_value == "%uint_0",
	        "parameter branch is taken only when resolved EXEC is nonzero");
	const auto exec_load = exec_value + " = OpLoad %uint %exec_lane_lo";
	Require(std::find(lines.begin(), lines.begin() + condition_index, exec_load) != lines.begin() + condition_index,
	        "parameter branch reads the resolved per-invocation EXEC lane");

	const auto active_label = active_target + " = OpLabel";
	const auto merge_label = merge_target + " = OpLabel";
	const auto active_it = std::find(lines.begin() + branch_index + 1u, store_it, active_label);
	const auto merge_it = std::find(store_it + 1u, lines.end(), merge_label);
	Require(active_it != store_it && merge_it != lines.end(), "parameter source loads and store are inside the active successor before merge");
	const auto active_index = static_cast<uint32_t>(std::distance(lines.begin(), active_it));
	const auto merge_index = static_cast<uint32_t>(std::distance(lines.begin(), merge_it));
	const auto selection_merge = "OpSelectionMerge " + merge_target + " None";
	Require(branch_index > 0u && lines[branch_index - 1u] == selection_merge,
	        "inactive parameter branch targets the structured merge directly");
	for (uint32_t line = active_index + 1u; line < merge_index; ++line)
	{
		Require(lines[line].find("OpKill") == std::string::npos && lines[line].find("OpReturn") == std::string::npos,
		        "inactive parameter control flow rejoins without kill or return");
	}
	Require(std::find(lines.begin() + active_index + 1u, lines.begin() + merge_index,
	                  "OpBranch " + merge_target) != lines.begin() + merge_index,
	        "active parameter path reaches the merge for later guest instructions");

	for (uint32_t source = 0; source < 4u; ++source)
	{
		const auto load = "%t" + std::to_string(source) + "_" + std::to_string(index) + " = OpLoad %float %v" +
		                  std::to_string(kRegisters[source]);
		const auto load_it = std::find(lines.begin(), lines.end(), load);
		Require(load_it != lines.end(), "parameter source load is emitted for each physical VGPR");
		const auto load_index = static_cast<uint32_t>(std::distance(lines.begin(), load_it));
		Require(load_index > active_index && load_index < merge_index, "all parameter source loads are dominated by the active EXEC branch");
	}
	const auto construct = "%t4_" + std::to_string(index) + " = OpCompositeConstruct %v4float %t0_" + std::to_string(index) +
	                       " %t1_" + std::to_string(index) + " %t2_" + std::to_string(index) + " %t3_" + std::to_string(index);
	const auto construct_it = std::find(lines.begin(), lines.end(), construct);
	Require(construct_it != lines.end(), "parameter source values are assembled into the output vector");
	const auto construct_index = static_cast<uint32_t>(std::distance(lines.begin(), construct_it));
	Require(construct_index > active_index && construct_index < merge_index,
	        "parameter vector construction is dominated by the active EXEC branch");
	Require(store_index > active_index && store_index < merge_index, "parameter store is dominated by the active EXEC branch");

	uint32_t probe_instructions = 0u;
	const std::string probe_prefix = "%vertex_param0_probe_";
	const auto probe_suffix = "_" + std::to_string(index);
	for (uint32_t line = 0; line < lines.size(); ++line)
	{
		bool has_this_export_probe = false;
		for (auto probe_id = lines[line].find(probe_prefix); probe_id != std::string::npos;
		     probe_id = lines[line].find(probe_prefix, probe_id + probe_prefix.size()))
		{
			const auto token_end = lines[line].find(' ', probe_id);
			const auto token_length = (token_end == std::string::npos ? lines[line].size() : token_end) - probe_id;
			const auto probe_token = lines[line].substr(probe_id, token_length);
			if (probe_token.size() >= probe_suffix.size() &&
			    probe_token.compare(probe_token.size() - probe_suffix.size(), probe_suffix.size(), probe_suffix) == 0)
			{
				has_this_export_probe = true;
				break;
			}
		}
		if (has_this_export_probe)
		{
			Require(line > active_index && line < merge_index, "every Param0 clip-probe operation is dominated by the active EXEC branch");
			++probe_instructions;
		}
	}
	Require((probe_instructions != 0u) == require_clip_probe, "optional Param0 clip-probe operations match the configured fixture");
	return merge_index;
}

uint32_t RequirePartialParameterStoreGuard(const std::string& source, uint32_t index, uint32_t parameter, uint8_t enable,
	                                          bool require_clip_probe)
{
	std::istringstream input(Instructions(String8(source.c_str())));
	std::vector<std::string> lines;
	for (std::string line; std::getline(input, line);) { lines.push_back(line); }

	const auto exec_load = "%parameter_exec_" + std::to_string(index) + " = OpLoad %uint %exec_lane_lo";
	const auto exec_test = "%parameter_active_" + std::to_string(index) + " = OpINotEqual %bool %parameter_exec_" +
	                       std::to_string(index) + " %uint_0";
	const auto selection_merge = "OpSelectionMerge %parameter_merge_" + std::to_string(index) + " None";
	const auto conditional_branch = "OpBranchConditional %parameter_active_" + std::to_string(index) + " %parameter_export_" +
	                                std::to_string(index) + " %parameter_merge_" + std::to_string(index);
	const auto active_label = "%parameter_export_" + std::to_string(index) + " = OpLabel";
	const auto merge_label = "%parameter_merge_" + std::to_string(index) + " = OpLabel";
	const auto exec_load_it = std::find(lines.begin(), lines.end(), exec_load);
	const auto exec_test_it = std::find(lines.begin(), lines.end(), exec_test);
	const auto branch_it = std::find(lines.begin(), lines.end(), conditional_branch);
	Require(exec_load_it != lines.end() && exec_test_it != lines.end() && branch_it != lines.end() && branch_it != lines.begin() &&
	            *(branch_it - 1) == selection_merge,
	        "partial parameter store has a structured EXEC != 0 branch");
	Require(exec_load_it < exec_test_it && exec_test_it < branch_it,
	        "partial parameter guard reads the resolved per-invocation EXEC lane");
	const auto active_it = std::find(branch_it + 1, lines.end(), active_label);
	const auto merge_it = std::find(active_it == lines.end() ? branch_it + 1 : active_it + 1, lines.end(), merge_label);
	Require(active_it != lines.end() && merge_it != lines.end(), "partial parameter active block reaches its structured merge");
	Require(std::find(active_it + 1, merge_it, "OpBranch %parameter_merge_" + std::to_string(index)) != merge_it,
	        "partial parameter active path reaches the merge for later guest instructions");
	const auto active_index = static_cast<uint32_t>(std::distance(lines.begin(), active_it));
	const auto merge_index = static_cast<uint32_t>(std::distance(lines.begin(), merge_it));

	for (uint32_t component = 0; component < 4u; ++component)
	{
		const auto load = "%t" + std::to_string(component) + "_" + std::to_string(index) + " = OpLoad %float %v" +
		                  std::to_string(kRegisters[component]);
		const auto access = "%exp_component_" + std::to_string(component) + "_" + std::to_string(index) +
		                    " = OpAccessChain %_ptr_Output_float %param" + std::to_string(parameter) + " %uint_" +
		                    std::to_string(component);
		const auto store = "OpStore %exp_component_" + std::to_string(component) + "_" + std::to_string(index) + " %t" +
		                   std::to_string(component) + "_" + std::to_string(index);
		const bool enabled = (enable & (1u << component)) != 0u;
		const auto load_it = std::find(lines.begin(), lines.end(), load);
		const auto access_it = std::find(lines.begin(), lines.end(), access);
		const auto store_it = std::find(lines.begin(), lines.end(), store);
		Require((load_it != lines.end()) == enabled && (access_it != lines.end()) == enabled && (store_it != lines.end()) == enabled,
		        "PARAM EN controls source loads and component stores with the matching physical source slot");
		if (enabled)
		{
			Require(load_it > active_it && load_it < merge_it && access_it > active_it && access_it < merge_it && store_it > active_it &&
				            store_it < merge_it,
				        "every partial PARAM source load and component store is dominated by active EXEC");
		}
	}

	for (const auto& line: lines)
	{
		Require(line.find("OpLoad") == std::string::npos || line.find("%param" + std::to_string(parameter)) == std::string::npos,
		        "partial PARAM does not read the output vector");
		Require(line != "OpStore %param" + std::to_string(parameter) + " %t4_" + std::to_string(index),
		        "partial PARAM does not overwrite the complete output vector");
	}

	uint32_t probe_instructions = 0u;
	const std::string probe_prefix = "%vertex_param0_probe_";
	const auto probe_suffix = "_" + std::to_string(index);
	for (uint32_t line = 0; line < lines.size(); ++line)
	{
		bool has_this_export_probe = false;
		for (auto probe_id = lines[line].find(probe_prefix); probe_id != std::string::npos;
		     probe_id = lines[line].find(probe_prefix, probe_id + probe_prefix.size()))
		{
			const auto token_end = lines[line].find(' ', probe_id);
			const auto token_length = (token_end == std::string::npos ? lines[line].size() : token_end) - probe_id;
			const auto probe_token = lines[line].substr(probe_id, token_length);
			if (probe_token.size() >= probe_suffix.size() &&
			    probe_token.compare(probe_token.size() - probe_suffix.size(), probe_suffix.size(), probe_suffix) == 0)
			{
				has_this_export_probe = true;
				break;
			}
		}
		if (has_this_export_probe)
		{
			Require(line > active_index && line < merge_index, "every Param0 probe operation is dominated by active EXEC");
			++probe_instructions;
		}
	}
	Require((probe_instructions != 0u) == require_clip_probe, "Param0 probe is emitted only when both XY components are enabled");
	return merge_index;
}

ShaderInstruction HandConstructed(const ExportCase& test)
{
	ShaderInstruction inst {};
	inst.pc = 0x30u;
	inst.raw_word = test.word;
	inst.type = ShaderInstructionType::Exp;
	inst.format = test.format;
	inst.src_num = test.sources;
	inst.exp_enable_mask = test.enable;
	inst.exp_control = test.control;
	for (int source = 0; source < test.sources; ++source) { inst.src[source] = Vgpr(kRegisters[source + test.first_physical]); }
	return inst;
}

enum class NonFinalPositionMutation
{
	EnableMask,
	SourceCount,
	SourceShape,
	RawControl,
};

[[noreturn]] void RejectMalformedNonFinalPosition(NonFinalPositionMutation mutation)
{
	Initialize();
	auto program = ParsePositionSequence({kPos0NoDone, kPos1XDone, kParam0});
	auto& pos0  = program.code.GetInstructions()[program.export_index];
	Require(pos0.format == kPos0NoDoneFormat && pos0.raw_word == kPos0NoDone.word && pos0.exp_enable_mask == 15u &&
	            pos0.exp_control == 0u && pos0.src_num == 4,
	        "the real parser establishes the valid non-final POS0 tuple before mutation");
	switch (mutation)
	{
		case NonFinalPositionMutation::EnableMask:
			pos0.exp_enable_mask = 7u;
			pos0.raw_word = (pos0.raw_word & ~0xfu) | 7u;
			break;
		case NonFinalPositionMutation::SourceCount:
			pos0.src_num = 3;
			pos0.src[3]  = {};
			break;
		case NonFinalPositionMutation::SourceShape:
			pos0.src[2].type        = ShaderOperandType::Sgpr;
			pos0.src[2].register_id = 34;
			break;
		case NonFinalPositionMutation::RawControl: pos0.raw_word |= 0x800u; break;
	}
	Require(ShaderInstructionLoweringPreconditions(pos0), "malformed export tuple remains within generic lowering preconditions");
	ShaderVertexInputInfo vertex {};
	vertex.position1_usage = ShaderVertexPosition1Usage::ClipDistance0;
	(void)SpirvGenerateSource(program.code, &vertex, nullptr, nullptr);
	std::_Exit(0);
}

[[noreturn]] void RejectStaleTail(const ExportCase& test)
{
	Initialize();
	auto inst = HandConstructed(test);
	Require(ShaderInstructionLoweringPreconditions(inst), "clean hand-constructed control is admitted");
	inst.src[test.sources] = Vgpr(kRegisters[test.sources]);
	Require(!ShaderInstructionLoweringPreconditions(inst), "one hand-constructed stale tail is rejected");
	ShaderInstruction end {};
	end.pc = 0x38u;
	end.type = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	ShaderCode code;
	code.SetType(test.stage);
	code.GetInstructions().Add(inst);
	code.GetInstructions().Add(end);
	ShaderVertexInputInfo vertex {};
	vertex.gs_prolog = true;
	vertex.position1_usage = ShaderVertexPosition1Usage::RenderTargetLayer;
	ShaderPixelInputInfo pixel {};
	pixel.target_output_mode[0] = 4;
	(void)SpirvGenerateSource(code, test.stage == ShaderType::Vertex ? &vertex : nullptr,
	                          test.stage == ShaderType::Pixel ? &pixel : nullptr, nullptr);
	std::_Exit(0);
}

} // namespace

TEST(EmulatorShaderExport, ParsesReducedNonColorExportsWithExactEmptyTails)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (const auto& test: {kPrim, kPos1, kPos1Done, kPixelZ, kPixelZNoDone, kNull, kPixelZNull}) { (void)Parse(test); }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ParsesPos1XEnableBeforeFinalPositionExport)
{
	EXPECT_EXIT(
	    ([] {
		    Initialize();
		    g_case = "vertex Gen5 target 13 EN=1 before final POS0";
		    std::vector<uint32_t> words;
		    for (unsigned source = 0; source < 4; ++source)
		    {
			    words.push_back(0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u));
			    words.push_back(kValues[source]);
		    }
		    const uint32_t pos1_word = ExportWord(13u, 1u, 0u);
		    words.insert(words.end(), {pos1_word, kSourceWord, kPos0.word, kSourceWord, kEndpgm});

		    ShaderCode code;
		    code.SetType(ShaderType::Vertex);
		    Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code),
		            "complete bounded POS1-X program parses");
		    const auto& instructions = code.GetInstructions();
		    Require(instructions.Size() == 7u, "POS1, final POS0, and S_ENDPGM remain separate instructions");

		    const auto& pos1 = instructions.At(4);
		    Require(pos1.type == ShaderInstructionType::Exp && pos1.format == ShaderInstructionFormat::Pos1Vsrc0OffOffOff &&
		                pos1.pc == 32u && pos1.raw_word == pos1_word &&
		                pos1.exp_enable_mask == 1u && pos1.exp_control == 0u && pos1.src_num == 1 &&
		                ExactOperand(pos1.src[0], Vgpr(kRegisters[0])),
		            "EN=1 preserves its physical X source, raw word, and control bits");
		    for (int source = 1; source < 4; ++source)
		    {
			    Require(pos1.src[source].type == ShaderOperandType::Unknown && pos1.src[source].size == 0 &&
			                ExactOperand(pos1.src[source], {}),
			            "disabled POS1 source tail is canonical");
		    }

		    const auto& pos0 = instructions.At(5);
		    Require(pos0.type == ShaderInstructionType::Exp &&
			            pos0.format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done && pos0.pc == 40u &&
			            pos0.raw_word == kPos0.word && pos0.exp_enable_mask == 0x0fu && pos0.exp_control == 2u && pos0.src_num == 4,
			        "following full DONE POS0 is retained");
		    const auto& end = instructions.At(6);
		    Require(end.type == ShaderInstructionType::SEndpgm && end.format == ShaderInstructionFormat::Empty &&
			            end.raw_word == kEndpgm && end.pc == 48u,
		            "following S_ENDPGM is retained");
		    const auto usage = ShaderDecodeVertexPosition1Usage(4u << 4u, (1u << 22u) | 1u, true);
		    Require(usage == ShaderVertexPosition1Usage::ClipDistance0, "output state establishes first-plane clipping");
		    for (bool gs_prolog: {false, true})
		    {
			    for (bool exec_zero: {false, true})
			    {
				    const auto program = Parse(kPos1X, false, true, exec_zero);
				    ShaderVertexInputInfo input {};
				    input.gs_prolog = gs_prolog;
				    input.position1_usage = usage;
				    const auto source = Validate(program, &input, nullptr);
				    const auto index = program.export_index;
				    Has(source, "OpCapability ClipDistance");
				    Has(source, "OpMemberDecorate %gl_PerVertex 2 BuiltIn ClipDistance");
				    Has(source, String8::FromPrintf(
				        "%%clip_active_%u = OpINotEqual %%bool %%clip_exec_%u %%uint_0\n"
				        "OpSelectionMerge %%clip_merge_%u None\n"
				        "OpBranchConditional %%clip_active_%u %%clip_export_%u %%clip_merge_%u\n"
				        "%%clip_export_%u = OpLabel\n"
				        "%%clip_source_%u = OpLoad %%float %%v17\n"
				        "%%clip_target_%u = OpAccessChain %%_ptr_Output_float %%outPerVertex %%int_2 %%int_0\n"
				        "OpStore %%clip_target_%u %%clip_source_%u\n"
				        "OpBranch %%clip_merge_%u\n%%clip_merge_%u = OpLabel",
				        index, index, index, index, index, index, index, index, index, index, index, index, index));
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesPositionSequenceWithDoneOnFinalTypedPositionExport)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    const auto program = ParsePositionSequence({kPos0NoDone, kPos1XDone, kParam0});
		    const auto& instructions = program.code.GetInstructions();
		    const uint32_t pos0_index = program.export_index;
		    const uint32_t pos1_index = pos0_index + 1u;
		    const uint32_t param_index = pos1_index + 1u;
		    CheckExport(instructions.At(pos0_index), kPos0NoDone, 32u);
		    CheckExport(instructions.At(pos1_index), kPos1XDone, 40u);
		    CheckExport(instructions.At(param_index), kParam0, 48u);
		    Require(instructions.At(pos0_index).raw_word == 0xf80000cfu && instructions.At(pos0_index).exp_control == 0u &&
		                instructions.At(pos1_index).raw_word == 0xf80008d1u && instructions.At(pos1_index).exp_control == 2u &&
		                instructions.At(param_index).raw_word == 0xf800020fu && instructions.At(param_index).exp_control == 0u,
		            "POS0 no-DONE, POS1.X DONE, and the post-DONE parameter keep their raw control order");

		    const auto usage = ShaderDecodeVertexPosition1Usage(4u << 4u, (1u << 22u) | 1u, true);
		    Require(usage == ShaderVertexPosition1Usage::ClipDistance0, "output state establishes only supported clip distance zero");
		    ShaderVertexInputInfo input {};
		    input.position1_usage = usage;
		    const auto source = Validate(program, &input, nullptr);
		    RequirePositionStoreGuard(source, pos0_index);
		    Has(source, "%t0_4 = OpLoad %float %v17\n%t1_4 = OpLoad %float %v61\n"
		                "%t2_4 = OpLoad %float %v173\n%t3_4 = OpLoad %float %v239\n"
		                "%t4_4 = OpCompositeConstruct %v4float %t0_4 %t1_4 %t2_4 %t3_4\n"
		                "%t5_4 = OpAccessChain %_ptr_Output_v4float %outPerVertex %int_per_vertex_0\nOpStore %t5_4 %t4_4");
		    Has(source, String8::FromPrintf(
		        "%%clip_active_%u = OpINotEqual %%bool %%clip_exec_%u %%uint_0\n"
		        "OpSelectionMerge %%clip_merge_%u None\n"
		        "OpBranchConditional %%clip_active_%u %%clip_export_%u %%clip_merge_%u\n"
		        "%%clip_export_%u = OpLabel\n"
		        "%%clip_source_%u = OpLoad %%float %%v17\n"
		        "%%clip_target_%u = OpAccessChain %%_ptr_Output_float %%outPerVertex %%int_2 %%int_0\n"
		        "OpStore %%clip_target_%u %%clip_source_%u\n"
		        "OpBranch %%clip_merge_%u\n%%clip_merge_%u = OpLabel",
		        pos1_index, pos1_index, pos1_index, pos1_index, pos1_index, pos1_index, pos1_index, pos1_index,
		        pos1_index, pos1_index, pos1_index, pos1_index, pos1_index));
		    Has(source, String8::FromPrintf("%%t0_%u = OpLoad %%float %%v17\n%%t1_%u = OpLoad %%float %%v61\n"
		                                   "%%t2_%u = OpLoad %%float %%v173\n%%t3_%u = OpLoad %%float %%v239\n"
		                                   "%%t4_%u = OpCompositeConstruct %%v4float %%t0_%u %%t1_%u %%t2_%u %%t3_%u\n"
		                                   "OpStore %%param0 %%t4_%u",
		                                   param_index, param_index, param_index, param_index, param_index,
		                                   param_index, param_index, param_index, param_index, param_index));
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, RejectsMalformedNonFinalPositionExportTuples)
{
	for (const auto mutation: {NonFinalPositionMutation::EnableMask, NonFinalPositionMutation::SourceCount,
	                           NonFinalPositionMutation::SourceShape, NonFinalPositionMutation::RawControl})
	{
		ASSERT_EXIT(RejectMalformedNonFinalPosition(mutation), ::testing::ExitedWithCode(kRejectedExit), "pc=0x00000020");
	}
}

TEST(EmulatorShaderExport, CompressedMrtKeepsBothPhysicalSlotsForPartialEn)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (uint32_t target = 0; target < 8; ++target)
		    {
			    for (uint8_t control: {4u, 5u, 6u, 7u})
			    {
				    for (uint8_t enable: {3u, 12u, 15u}) { (void)Parse(Mrt(target, enable, control)); }
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, NullMrtClearsAllPhysicalSourcesAndKeepsVmDoneCompr)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (uint32_t target = 0; target < 8; ++target)
		    {
			    (void)Parse(Mrt(target, 0, 7));
			    if (target != 0u) { (void)Parse(Mrt(target, 0, 6)); }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, FullAndPartialFourSlotFormatsPreservePhysicalPositions)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    (void)Parse(kPos0);
		    for (uint32_t target = 0; target < 8; ++target)
		    {
			    for (uint8_t enable: {5u, 8u, 15u}) { (void)Parse(Mrt(target, enable, 3)); }
		    }
		    for (uint32_t target: {0x20u, 0x3fu})
		    {
			    for (uint8_t enable: {1u, 5u, 8u, 15u})
			    {
				    const auto format = target == 0x20u ? ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3 :
				                                         ShaderInstructionFormat::Param31Vsrc0Vsrc1Vsrc2Vsrc3;
				    (void)Parse({ShaderType::Vertex, ExportWord(target, enable, 0), format, 4, enable, 0});
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, LegacyReducedExportsAlsoHaveCanonicalTails)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize(false);
		    for (const auto& test: {kPrim, kPixelZ, Mrt(0, 15, 7), Mrt(3, 3, 4), Mrt(3, 0, 6)}) { (void)Parse(test); }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesCurrentlyAdmittedPrimGsPrologModule)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    const auto program = Parse(kPrim, true);
		    ShaderVertexInputInfo input {};
		    input.gs_prolog = true;
		    const auto source = Validate(program, &input, nullptr);
		    Has(source, "OpStore %v5 %vertex_index");
		    Has(source, "OpStore %v8 %instance_index");
		    // One host subgroup is one guest wave: s3 counts every lane up to the highest live one.
		    Has(source, "%ngg_wave_info = OpBitwiseOr %uint %ngg_wave_primitives %ngg_wave_lanes\nOpStore %s3 %ngg_wave_info");
		    Has(source, "%t0_4 = OpLoad %float %v17\n%t1_4 = OpLoad %float %v61\n"
		                "%t2_4 = OpLoad %float %v173\n%t3_4 = OpLoad %float %v239\n"
		                "%t4_4 = OpCompositeConstruct %v4float %t0_4 %t1_4 %t2_4 %t3_4\n"
		                "%t5_4 = OpAccessChain %_ptr_Output_v4float %outPerVertex %int_per_vertex_0\nOpStore %t5_4 %t4_4");
		    // PRIM's existing emitter emits no primitive operation. This validates
		    // only its admitted gs_prolog module, not merged geometry execution.
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, PositionStoreAndClipProbeAreGuardedByResolvedExec)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (bool gs_prolog: {false, true})
		    {
			    for (bool exec_zero: {false, true})
			    {
				    const auto program = Parse(kPos0, false, false, exec_zero);
				    const auto& instructions = program.code.GetInstructions();
				    if (exec_zero)
				    {
					    Require(program.export_index == 5u && instructions.At(4).raw_word == kExecZero,
					            "zero-EXEC instruction precedes the final POS0 export");
				    } else
				    {
					    Require(program.export_index == 4u, "full-EXEC fixture reaches the final POS0 export directly");
				    }
				    ShaderVertexInputInfo input {};
				    input.gs_prolog = gs_prolog;
				    const auto source = Validate(program, &input, nullptr);
				    RequirePositionStoreGuard(source, program.export_index);
			    }
		    }

		    const auto probed = Parse(kPos0);
		    ShaderVertexInputInfo probe_input {};
		    probe_input.clip_probe.enabled = true;
		    probe_input.clip_probe_descriptor_set = 1u;
		    const auto probe_source = Validate(probed, &probe_input, nullptr);
		    RequirePositionStoreGuard(probe_source, probed.export_index, true);
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ParameterStoresAndParam0ProbeAreGuardedByResolvedExec)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (const auto& parameter: {kParam0, kParam31})
		    {
			    const uint32_t parameter_index = parameter.format == kParam0.format ? 0u : 31u;
			    const bool     probe_enabled   = parameter_index == 0u;
			    for (bool gs_prolog: {false, true})
			    {
				    const auto program = ParseExecParameterSequence(parameter);
				    ShaderVertexInputInfo input {};
				    input.gs_prolog = gs_prolog;
				    input.clip_probe.enabled = probe_enabled;
				    input.clip_probe_descriptor_set = probe_enabled ? 1u : 0u;
				    const auto source = Validate(program, &input, nullptr);

				    RequireParameterStoreGuard(source, 4u, parameter_index, probe_enabled);
				    const auto inactive_merge = RequireParameterStoreGuard(source, 6u, parameter_index, probe_enabled);
				    const auto restored_merge = RequireParameterStoreGuard(source, 8u, parameter_index, probe_enabled);
				    RequirePositionStoreGuard(source, 9u);

				    std::istringstream source_stream(Instructions(String8(source.c_str())));
				    std::vector<std::string> lines;
				    for (std::string line; std::getline(source_stream, line);) { lines.push_back(line); }
				    const auto restore = std::find(lines.begin(), lines.end(), "OpStore %exec_lo %t0_7");
				    Require(restore != lines.end() && inactive_merge < static_cast<uint32_t>(std::distance(lines.begin(), restore)),
				            "inactive export merge precedes scalar EXEC restoration");
				    const auto later_store = std::find(lines.begin(), lines.end(),
				                                      "OpStore %param" + std::to_string(parameter_index) + " %t4_8");
				    Require(later_store != lines.end() && restore < later_store &&
				                restored_merge > static_cast<uint32_t>(std::distance(lines.begin(), restore)),
				            "a later active parameter export follows EXEC restoration and reaches its merge");
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, PartialParameterEnStoresOnlyEnabledComponents)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (const uint32_t target: {0x20u, 0x3fu})
		    {
			    const uint32_t parameter_index = target == 0x20u ? 0u : 31u;
			    for (uint8_t enable = 1u; enable <= 15u; ++enable)
			    {
				    const auto format = target == 0x20u ? ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3 :
					                                         ShaderInstructionFormat::Param31Vsrc0Vsrc1Vsrc2Vsrc3;
				    const ExportCase parameter {ShaderType::Vertex, ExportWord(target, enable, 0), format, 4, enable, 0};
				    const auto program = ParseParameterThenScalar(parameter);

				    ShaderVertexInputInfo input {};
				    input.clip_probe.enabled = target == 0x20u;
				    input.clip_probe_descriptor_set = input.clip_probe.enabled ? 1u : 0u;
				    const auto source = Validate(program, &input, nullptr);
				    const bool probe_enabled = target == 0x20u && (enable & 0x03u) == 0x03u;
				    const auto merge_index = enable == 15u
				                                ? RequireParameterStoreGuard(source, 4u, parameter_index, probe_enabled)
				                                : RequirePartialParameterStoreGuard(source, 4u, parameter_index, enable, probe_enabled);

				    std::istringstream source_stream(Instructions(String8(source.c_str())));
				    std::vector<std::string> lines;
				    for (std::string line; std::getline(source_stream, line);) { lines.push_back(line); }
				    const auto scalar_exec_store = std::find(lines.begin(), lines.end(), "OpStore %exec_lo %t0_5");
				    Require(scalar_exec_store != lines.end() &&
				                merge_index < static_cast<uint32_t>(std::distance(lines.begin(), scalar_exec_store)),
				            "the scalar EXEC operation after PARAM remains reachable after the parameter merge");
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesPos1LayerFromPhysicalZForBothDoneValues)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (const auto& test: {kPos1, kPos1Done})
		    {
			    for (bool gs_prolog: {false, true})
			    {
				    const bool done = test.control == 2u;
				    // Intermediate POS1 precedes the final POS0. Final POS1 is a
				    // standalone layer-output module with S_ENDPGM, avoiding two
				    // DONE position exports; validation is not a rasterization proof.
				    const auto program = Parse(test, false, !done);
				    ShaderVertexInputInfo input {};
				    input.gs_prolog = gs_prolog;
				    input.position1_usage = ShaderVertexPosition1Usage::RenderTargetLayer;
				    const auto source = Validate(program, &input, nullptr);
				    const auto index = program.export_index;
				    Has(source, "OpCapability ShaderLayer");
				    Has(source, "OpDecorate %gl_Layer BuiltIn Layer");
				    Has(source, String8::FromPrintf("%%layer_source_%u = OpLoad %%float %%v173\n"
				                                   "%%layer_bits_%u = OpBitcast %%uint %%layer_source_%u\n"
				                                   "%%layer_unsigned_%u = OpBitwiseAnd %%uint %%layer_bits_%u %%uint_0x000007ff\n"
				                                   "%%layer_signed_%u = OpBitcast %%int %%layer_unsigned_%u\n"
				                                   "OpStore %%gl_Layer %%layer_signed_%u", index, index, index, index, index, index, index, index));
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesPixelZSourceAndDepthControlFlow)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    const auto program = Parse(kPixelZ);
		    ShaderPixelInputInfo input {};
		    const auto source = Validate(program, nullptr, &input);
		    Has(source, "OpExecutionMode %main DepthReplacing");
		    Has(source, "OpDecorate %fragDepth BuiltIn FragDepth");
		    Has(source, "OpBranchConditional %exp_exec_b_4 %exp_store_4 %exp_kill_4\n%exp_kill_4 = OpLabel\nOpKill\n"
		                "%exp_store_4 = OpLabel\n%t0_4 = OpLoad %float %v17\nOpStore %fragDepth %t0_4");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesNonFinalPixelZAheadOfColorExport)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    std::vector<uint32_t> words;
		    for (unsigned source = 0; source < 4; ++source)
		    {
			    words.push_back(0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u)); // v_mov_b32 vN, literal
			    words.push_back(kValues[source]);
		    }
		    const auto color = Mrt(0u, 15u, 3u);
		    words.insert(words.end(), {kPixelZNoDone.word, kSourceWord, color.word, kSourceWord, kEndpgm});
		    Program program;
		    program.code.SetType(ShaderType::Pixel);
		    Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &program.code),
		            "complete bounded parser input");
		    CheckExport(program.code.GetInstructions().At(4), kPixelZNoDone, 32u);
		    CheckExport(program.code.GetInstructions().At(5), color, 40u);

		    ShaderPixelInputInfo input {};
		    input.target_output_mode[0] = 9u;
		    const auto source = Validate(program, nullptr, &input);
		    Has(source, "OpExecutionMode %main DepthReplacing");
		    Has(source, "%exp_exec_u_4 = OpLoad %uint %exec_lane_lo\n"
		                "%exp_exec_b_4 = OpINotEqual %bool %exp_exec_u_4 %uint_0\n"
		                "OpSelectionMerge %exp_merge_4 None\n"
		                "OpBranchConditional %exp_exec_b_4 %exp_store_4 %exp_continue_4\n"
		                "%exp_continue_4 = OpLabel\nOpBranch %exp_merge_4\n"
		                "%exp_store_4 = OpLabel");
		    Has(source, "%t0_4 = OpLoad %float %v17\nOpStore %fragDepth %t0_4");
		    Require(source.find("OpStore %outColor") != std::string::npos, "the following color export still stores MRT0");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

Program ParseDepthWithDiscardTail(bool clear_exec)
{
	// Movs, s_cbranch_scc0 to the tail, MRTZ (no DONE), full MRT0 (DONE),
	// s_endpgm; tail: s_mov_b64 exec, 0 (or a NOP), MRTZ with no channel
	// (valid mask only, DONE), s_endpgm.
	std::vector<uint32_t> words;
	for (unsigned source = 0; source < 4; ++source)
	{
		words.push_back(0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u)); // v_mov_b32 vN, literal
		words.push_back(kValues[source]);
	}
	const auto color = Mrt(0u, 15u, 3u);
	words.insert(words.end(), {0xbf840005u, kPixelZNoDone.word, kSourceWord, color.word, kSourceWord, kEndpgm,
	                           clear_exec ? 0xbefe0480u : 0xbf800000u, kPixelZNull.word, kSourceWord, kEndpgm});
	Program program;
	program.code.SetType(ShaderType::Pixel);
	Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &program.code),
	        "complete bounded parser input");
	return program;
}

TEST(EmulatorShaderExport, PixelZWithDiscardTailWritesDepthOnEveryLiveExit)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    const auto program = ParseDepthWithDiscardTail(true);
		    Require(program.code.GetInstructions().At(9).format == ShaderInstructionFormat::NullVmDone, "MRTZ with no channel is a null export");
		    ShaderPixelInputInfo input {};
		    input.target_output_mode[0] = 9u;
		    const auto source = Validate(program, nullptr, &input);
		    Has(source, "OpExecutionMode %main DepthReplacing");
		    Has(source, "%t0_5 = OpLoad %float %v17\nOpStore %fragDepth %t0_5");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, PixelZRejectsTailThatKeepsLanesWithoutDepth)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    const auto program = ParseDepthWithDiscardTail(false);
		    ShaderPixelInputInfo input {};
		    input.target_output_mode[0] = 9u;
		    const auto assembly = SpirvGenerateSource(program.code, nullptr, &input, nullptr);
		    std::_Exit(assembly.FindIndex("OpKytyPixelDepthControlFlowRejected") != Core::STRING8_INVALID_INDEX ? 0 : 3);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, PixelZUnderClearedExecDoesNotDefineRestoredLanes)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    g_case = "Pixel-Z under zero EXEC before full EXEC restore";
		    const auto color = Mrt(0u, 15u, 2u);
		    std::vector<uint32_t> words {kExecZero, kPixelZNoDone.word, kSourceWord, kExecFull, color.word, kSourceWord, kEndpgm};
		    Program program;
		    program.code.SetType(ShaderType::Pixel);
		    Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &program.code),
		            "complete bounded EXEC-clear/Pixel-Z/EXEC-restore/color stream");
		    const auto& instructions = program.code.GetInstructions();
		    Require(instructions.Size() == 5u, "stream retains clear, Pixel-Z, restore, color, and end instructions");
		    Require(instructions.At(0).type == ShaderInstructionType::SMovB64 && instructions.At(0).raw_word == kExecZero,
		            "Pixel-Z is preceded by an EXEC clear");
		    CheckExport(instructions.At(1), kPixelZNoDone, 4u);
		    Require(instructions.At(2).type == ShaderInstructionType::SMovB64 && instructions.At(2).raw_word == kExecFull,
		            "full EXEC is restored before color export");
		    CheckExport(instructions.At(3), color, 16u);

		    ShaderPixelInputInfo input {};
		    input.target_output_mode[0] = 9u;
		    const auto source = SpirvGenerateSource(program.code, nullptr, &input, nullptr);
		    const bool rejected = source.FindIndex("OpKytyPixelDepthControlFlowRejected") != Core::STRING8_INVALID_INDEX;
		    const bool declares_depth_replacing = source.FindIndex("OpExecutionMode %main DepthReplacing") != Core::STRING8_INVALID_INDEX;
		    const bool stores_frag_depth = source.FindIndex("OpStore %fragDepth") != Core::STRING8_INVALID_INDEX;
		    if (!rejected || declares_depth_replacing || stores_frag_depth)
		    {
			    std::fprintf(stderr,
			                 "EXP fixture: pixel-depth admission mismatch: rejected=%s depth_replacing=%s frag_depth_store=%s\n",
			                 rejected ? "true" : "false", declares_depth_replacing ? "true" : "false", stores_frag_depth ? "true" : "false");
			    std::fflush(stderr);
			    std::_Exit(3);
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, InactiveNonFinalColorExportContinuesAfterExecRestore)
{
	ASSERT_EXIT(
	    (
	        []
	        {
		        Initialize();
		        const auto final_export = Mrt(0u, 15u, 2u);
		        for (const auto& nonfinal_export: {Mrt(1u, 3u, 4u), Mrt(1u, 5u, 0u)})
		        {
			        std::vector<uint32_t> words;
			        for (unsigned source = 0; source < 4; ++source)
			        {
				        words.push_back(0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u)); // v_mov_b32 vN, literal
				        words.push_back(kValues[source]);
			        }
			        words.insert(words.end(),
			                     {kExecZero, nonfinal_export.word, kSourceWord, kExecFull, final_export.word, kSourceWord, kEndpgm});
			        Program program;
			        program.code.SetType(ShaderType::Pixel);
			        Require(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &program.code),
			                "complete bounded parser input");
			        const auto& instructions = program.code.GetInstructions();
			        Require(instructions.At(4).type == ShaderInstructionType::SMovB64 && instructions.At(4).raw_word == kExecZero &&
			                    instructions.At(6).type == ShaderInstructionType::SMovB64 && instructions.At(6).raw_word == kExecFull,
			                "inactive export is followed by the guest's EXEC restore");
			        CheckExport(instructions.At(5), nonfinal_export, 36u);
			        CheckExport(instructions.At(7), final_export, 48u);

			        ShaderPixelInputInfo input {};
			        input.target_output_mode[0] = 9u;
			        input.target_output_mode[1] = (nonfinal_export.control & 4u) != 0u ? 4u : 9u;
			        const auto source           = Validate(program, nullptr, &input);
			        Has(source, "%t0_4 = OpBitcast %uint %int_0\n"
			                    "%t1_4 = OpBitcast %uint %uint_0\n"
			                    "OpStore %exec_lo %t0_4\nOpStore %exec_hi %t1_4");
			        Has(source, "%exp_exec_u_5 = OpLoad %uint %exec_lane_lo\n"
			                    "%exp_exec_b_5 = OpINotEqual %bool %exp_exec_u_5 %uint_0\n"
			                    "OpSelectionMerge %exp_merge_5 None\n"
			                    "OpBranchConditional %exp_exec_b_5 %exp_store_5 %exp_continue_5\n"
			                    "%exp_continue_5 = OpLabel\nOpBranch %exp_merge_5\n"
			                    "%exp_store_5 = OpLabel");
			        Require(source.find((nonfinal_export.control & 4u) != 0u
			                                ? "OpStore %outColor1 %t11_5"
			                                : "%exp_component_0_5 = OpAccessChain %_ptr_Output_float %outColor1 %uint_0") !=
			                    std::string::npos,
			                "the nonfinal export keeps its guarded color store");
			        Has(source, "%t0_6 = OpBitcast %uint %int_m1\n"
			                    "%t1_6 = OpBitcast %uint %uint_0xffffffff\n"
			                    "OpStore %exec_lo %t0_6\nOpStore %exec_hi %t1_6");
			        Has(source, "%exp_exec_u_7 = OpLoad %uint %exec_lane_lo\n"
			                    "%exp_exec_b_7 = OpINotEqual %bool %exp_exec_u_7 %uint_0\n"
			                    "OpSelectionMerge %exp_merge_7 None\n"
			                    "OpBranchConditional %exp_exec_b_7 %exp_store_7 %exp_kill_7\n"
			                    "%exp_kill_7 = OpLabel\nOpKill\n%exp_store_7 = OpLabel");
			        Has(source, "OpStore %outColor %t11_7");
			        const auto inactive_branch = source.find("OpBranchConditional %exp_exec_b_5 %exp_store_5 %exp_continue_5");
			        const auto exec_restore    = source.find("OpStore %exec_lo %t0_6", inactive_branch);
			        const auto terminal_branch = source.find("OpBranchConditional %exp_exec_b_7 %exp_store_7 %exp_kill_7", exec_restore);
			        Require(inactive_branch < exec_restore && exec_restore < terminal_branch,
			                "inactive nonfinal export rejoins before guest EXEC restoration and final export");
		        }
		        std::_Exit(0);
	        }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesCompressedMrtEnabledValueDataflow)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (uint32_t target = 0; target < 8; ++target)
		    {
			    for (uint8_t enable: {3u, 12u, 15u})
			    {
				    const auto program = Parse(Mrt(target, enable, target == 0u ? 7u : 4u));
				    ShaderPixelInputInfo input {};
				    input.target_output_mode[target] = 4;
			    input.target_output_number[target] = 4; // Number alone must not select the UINT16 interface.
				    const auto source = Validate(program, nullptr, &input);
				    Has(source, "%t1_4 = OpLoad %float %v17\n%t2_4 = OpBitcast %uint %t1_4\n"
				                "%t3_4 = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t2_4");
				    Has(source, "%t6_4 = OpLoad %float %v61\n%t7_4 = OpBitcast %uint %t6_4\n"
				                "%t8_4 = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t7_4");
				    const char* value[] = {"%t4_4", "%t5_4", "%t9_4", "%t10_4"};
				    const char* prior[] = {"%exp_old0_4", "%exp_old1_4", "%exp_old2_4", "%exp_old3_4"};
				    std::string expected = "%t11_4 = OpCompositeConstruct %v4float";
				    for (unsigned component = 0; component < 4; ++component)
				    {
					    expected += std::string(" ") + ((enable & (1u << component)) != 0u ? value[component] : prior[component]);
				    }
				    const auto output = target == 0u ? String8("outColor") : String8::FromPrintf("outColor%u", target);
				    Has(source, expected.c_str());
				    Has(source, String8::FromPrintf("OpStore %%%s %%t11_4", output.c_str()));
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesUnsigned16CompressedMrtInterfaceAndPartialComponents)
{
	ASSERT_EXIT(
	    (
	        []
	        {
		        Initialize();
		        for (const uint32_t target: {0u, 1u})
		        {
			        for (const uint8_t enable: {3u, 12u, 15u})
			        {
				        const auto           program = Parse(Mrt(target, enable, 7u));
				        ShaderPixelInputInfo input {};
				        input.target_output_mode[target]   = kShaderColorExportModeUint16Abgr;
				        input.target_output_number[target] = kShaderColorExportNumberUint;
				        input.target_output_order[target]  = 0u;
				        const auto source                  = Validate(program, nullptr, &input);
				        Has(source, "%_ptr_Output_v4uint = OpTypePointer Output %v4uint");
				        Has(source, target == 0u ? "%outColor = OpVariable %_ptr_Output_v4uint Output"
				                                 : "%outColor1 = OpVariable %_ptr_Output_v4uint Output");
				        if ((enable & 0x3u) != 0u)
				        {
					        Has(source, "%t1_4 = OpLoad %float %v17\n%t2_4 = OpBitcast %uint %t1_4");
					        Has(source, "%uint_r_4 = OpBitFieldUExtract %uint %t2_4 %uint_0 %uint_16\n"
					                    "%uint_g_4 = OpBitFieldUExtract %uint %t2_4 %uint_16 %uint_16");
				        } else
				        {
					        Require(source.find("%uint_r_4 = OpBitFieldUExtract") == std::string::npos &&
					                    source.find("%uint_g_4 = OpBitFieldUExtract") == std::string::npos,
					                "disabled low packed-half lanes are not loaded or extracted");
				        }
				        if ((enable & 0xcu) != 0u)
				        {
					        Has(source, "%t6_4 = OpLoad %float %v61\n%t7_4 = OpBitcast %uint %t6_4");
					        Has(source, "%uint_b_4 = OpBitFieldUExtract %uint %t7_4 %uint_0 %uint_16\n"
					                    "%uint_a_4 = OpBitFieldUExtract %uint %t7_4 %uint_16 %uint_16");
				        } else
				        {
					        Require(source.find("%uint_b_4 = OpBitFieldUExtract") == std::string::npos &&
					                    source.find("%uint_a_4 = OpBitFieldUExtract") == std::string::npos,
					                "disabled high packed-half lanes are not loaded or extracted");
				        }
				        const char* const values[] = {"%uint_r_4", "%uint_g_4", "%uint_b_4", "%uint_a_4"};
				        const auto        output   = target == 0u ? String8("outColor") : String8::FromPrintf("outColor%u", target);
				        if (enable == 0xfu)
				        {
					        Has(source, "%t11_4 = OpCompositeConstruct %v4uint %uint_r_4 %uint_g_4 %uint_b_4 %uint_a_4");
					        Has(source, String8::FromPrintf("OpStore %%%s %%t11_4", output.c_str()));
				        } else
				        {
					        for (uint32_t component = 0u; component < 4u; ++component)
					        {
						        const auto access =
						            String8::FromPrintf("%%exp_component_%u_4 = OpAccessChain %%_ptr_Output_uint %%%s %%uint_%u\n"
						                                "         OpStore %%exp_component_%u_4 %s",
						                                component, output.c_str(), component, component, values[component]);
						        if ((enable & (1u << component)) != 0u)
						        {
							        Has(source, access);
						        } else
						        {
							        Require(source.find(String8::FromPrintf("%%exp_component_%u_4 = OpAccessChain", component).c_str()) ==
							                    std::string::npos,
							                "partial EN stores only enabled output components");
						        }
					        }
					        Require(source.find("OpLoad %v4uint") == std::string::npos &&
					                    source.find("OpStore %outColor %t11_4") == std::string::npos &&
					                    source.find("OpStore %outColor1 %t11_4") == std::string::npos,
					                "partial EN does not read or overwrite disabled output components");
				        }
			        }
		        }
		        const auto           null_program = Parse(Mrt(0u, 0u, 7u));
		        ShaderPixelInputInfo null_input {};
		        null_input.target_output_mode[0]   = kShaderColorExportModeUint16Abgr;
		        null_input.target_output_number[0] = kShaderColorExportNumberUint;
		        null_input.target_output_order[0]  = 0u;
		        const auto null_source             = Validate(null_program, nullptr, &null_input);
		        Require(null_source.find("OpStore %outColor") == std::string::npos,
		                "valid UINT16 EN=0 export is a no-write terminator without a fabricated attachment write");
		        std::_Exit(0);
	        }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesNonterminalUnsigned16ExportsBeforeFinalMrt)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (uint8_t control: {4u, 5u})
		    {
			    for (uint8_t enable: {3u, 12u, 15u})
			    {
				    const auto program = ParseColorPair(Mrt(0u, enable, control), Mrt(1u, 15u, 7u));
				    ShaderPixelInputInfo input {};
				    for (uint32_t target = 0; target < 2u; ++target)
				    {
					    input.target_output_mode[target]   = kShaderColorExportModeUint16Abgr;
					    input.target_output_number[target] = kShaderColorExportNumberUint;
				    }
				    const auto source = Validate(program, nullptr, &input);
				    Has(source, "%exp_continue_4 = OpLabel\nOpBranch %exp_merge_4");
				    Has(source, "OpStore %outColor1 %t11_5");
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, RejectsUnsigned16CompressedMrtOutsideBoundedMetadataAndMasks)
{
	const auto diagnostic = String8::FromPrintf("shader emitter missing: stage=%u instruction=%u format=0x%016" PRIx64 " pc=0x00000020.*",
	                                            static_cast<unsigned>(ShaderType::Pixel), static_cast<unsigned>(ShaderInstructionType::Exp),
	                                            static_cast<uint64_t>(Mrt(0u, 3u, 7u).format));
	for (const auto invalid: {0u, 1u, 2u})
	{
		ASSERT_EXIT(
		    {
			    Initialize();
			    const auto           program = Parse(Mrt(0u, invalid == 2u ? 1u : 3u, 7u));
			    ShaderPixelInputInfo input {};
			    input.target_output_mode[0]   = kShaderColorExportModeUint16Abgr;
			    input.target_output_number[0] = invalid == 0u ? 3u : kShaderColorExportNumberUint;
			    input.target_output_order[0]  = invalid == 1u ? 1u : 0u;
			    (void)SpirvGenerateSource(program.code, nullptr, &input, nullptr);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(kRejectedExit), diagnostic.c_str());
	}
}

TEST(EmulatorShaderExport, ResolvesUnsigned16ColorAttachmentFormats)
{
	const auto r16    = ResolveRenderTextureFormat(2u, 4u, 0u);
	const auto rg16   = ResolveRenderTextureFormat(5u, 4u, 0u);
	const auto rgba16 = ResolveRenderTextureFormat(12u, 4u, 0u);
	EXPECT_EQ(r16.format, RenderTextureFormat::R16Uint);
	EXPECT_EQ(r16.bytes_per_element, 2u);
	EXPECT_EQ(rg16.format, RenderTextureFormat::R16G16Uint);
	EXPECT_EQ(rg16.bytes_per_element, 4u);
	EXPECT_EQ(rgba16.format, RenderTextureFormat::R16G16B16A16Uint);
	EXPECT_EQ(rgba16.bytes_per_element, 8u);
	EXPECT_EQ(VulkanResolveRenderTextureFormat(r16.format), VK_FORMAT_R16_UINT);
	EXPECT_EQ(VulkanResolveRenderTextureFormat(rg16.format), VK_FORMAT_R16G16_UINT);
}

TEST(EmulatorShaderExport, PreservesEmbeddedWritesAndSummarizesClassicPixelExports)
{
	ASSERT_EXIT(
	    (
	        []
	        {
		        Initialize(false);
		        HW::ShaderRegisters   embedded_registers {};
		        HW::PixelShaderInfo   embedded_shader {};
		        ShaderVertexInputInfo vertex_info {};
		        ShaderPixelInputInfo embedded_input {};
		        embedded_shader.ps_embedded = true;
		        ShaderGetInputInfoPS(&embedded_shader, &embedded_registers, &vertex_info, 0xffffffffu, &embedded_input, false);
		        Require(embedded_input.color_exports.enable[0] == 0u && !embedded_input.has_only_null_exports,
		                "embedded shader has no parsed export summary");
		        Require(State::ResolveColorWriteAgainstExport(0x0fu, embedded_input.color_exports.enable[0],
		                                                      !embedded_shader.ps_embedded) == 0x0fu,
		                "an unavailable embedded export summary preserves the requested color mask");

		        constexpr uint32_t kBinaryInfoOffsetDwords = 10u;
		        static_assert(sizeof(ShaderBinaryInfo) % sizeof(uint32_t) == 0u);
		        std::vector<uint32_t> classic_words(kBinaryInfoOffsetDwords + sizeof(ShaderBinaryInfo) / sizeof(uint32_t), 0u);
		        classic_words[0] = 0xbeeb03ffu;
		        classic_words[1] = 4u; // Binary info is at (word[1] + 1) * 2 dwords.
		        for (uint32_t source = 0u; source < 2u; ++source)
		        {
			        const uint32_t instruction = 2u + source * 2u;
			        classic_words[instruction]     = 0x7e0002ffu | (static_cast<uint32_t>(kRegisters[source]) << 17u);
			        classic_words[instruction + 1] = kValues[source];
		        }
		        const auto classic_export = Mrt(0u, 3u, 7u);
		        classic_words[6] = classic_export.word;
		        classic_words[7] = kSourceWord;
		        classic_words[8] = kEndpgm;
		        // The legacy usage table is one dword before the binary info and has
		        // zero slots; this is a valid resource-free usage section.
		        ShaderBinaryInfo header {};
		        header.length                     = 36u;
		        header.chunk_usage_base_offset_dw = 1u;
		        header.num_input_usage_slots      = 0u;
		        header.hash0                      = 0x12345678u;
		        header.crc32                      = 0x9abcdef0u;
		        std::memcpy(classic_words.data() + kBinaryInfoOffsetDwords, &header, sizeof(header));

		        const uint64_t guest_size = Core::VirtualMemory::GetPageSize();
		        const uint64_t guest_addr = Core::VirtualMemory::Alloc(0, guest_size, Core::VirtualMemory::Mode::ReadWrite);
		        Require(guest_addr != 0u, "allocate mapped classic shader memory");
		        Require(Core::VirtualMemory::CopyToGuest(guest_addr, classic_words.data(), classic_words.size() * sizeof(uint32_t)),
		                "copy classic binary and usage metadata to guest memory");
		        ShaderInit();
		        ShaderMappedData mapped {};
		        mapped.code_size_bytes = static_cast<uint32_t>(classic_words.size() * sizeof(uint32_t));
		        ShaderMapUserData(guest_addr, mapped);

		        HW::ShaderRegisters classic_registers {};
		        classic_registers.target_output_mode[0]   = 7u;
		        classic_registers.target_output_number[0] = 4u;
		        classic_registers.target_output_order[0]  = 0u;
		        HW::PixelShaderInfo classic_shader {};
		        classic_shader.ps_regs.data_addr = guest_addr;
		        ShaderPixelInputInfo classic_input {};
		        ShaderGetInputInfoPS(&classic_shader, &classic_registers, &vertex_info, 0xffffffffu, &classic_input, false);
		        Require(!classic_input.has_only_null_exports && classic_input.color_exports.enable[0] == 3u,
		                "classic shader summary comes from code parsed for input analysis");
		        Require(State::ResolveColorWriteAgainstExport(0x0fu, classic_input.color_exports.enable[0],
		                                                      !classic_shader.ps_embedded) == 0x0fu,
		                "a parsed classic color export preserves requested channels");
		        Require(State::ResolveColorWriteAgainstExport(0x0fu, classic_input.color_exports.enable[1],
		                                                      !classic_shader.ps_embedded) == 0u,
		                "a parsed classic program suppresses an absent MRT");
		        ShaderMapUserData(guest_addr, {});
		        Require(Core::VirtualMemory::Free(guest_addr), "free mapped classic shader memory");
		        std::_Exit(0);
	        }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesNullMaskAndMrtNoWriteDiscardContracts)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (bool exec_zero: {false, true})
		    {
			    const auto program = Parse(kNull, false, false, exec_zero);
			    ShaderPixelInputInfo input {};
			    const auto source = Validate(program, nullptr, &input);
			    const auto index = program.export_index;
			    Has(source, String8::FromPrintf("%%null_exec_lo_%u = OpLoad %%uint %%exec_lane_lo\n"
			                                   "%%null_exec_hi_%u = OpLoad %%uint %%exec_lane_hi\n"
			                                   "%%null_exec_%u = OpBitwiseOr %%uint %%null_exec_lo_%u %%null_exec_hi_%u",
			                                   index, index, index, index, index));
			    Has(source, String8::FromPrintf("OpBranchConditional %%null_valid_%u %%null_merge_%u %%null_kill_%u\n"
			                                   "%%null_kill_%u = OpLabel\nOpKill", index, index, index, index));
			    Require(source.find("%outColor") == std::string::npos, "NULL has no fabricated color interface or writes");
		    }
		    for (uint32_t target = 0; target < 8; ++target)
		    {
			    for (bool exec_zero: {false, true})
			    {
				    if (target == 0u && !exec_zero) { continue; } // Standalone MRT0 remains rejected below.
				    const auto program = Parse(Mrt(target, 0, target == 0u || exec_zero ? 7u : 6u), false, false, exec_zero);
				    ShaderPixelInputInfo input {};
				    input.target_output_mode[target] = 4;
				    const auto source = Validate(program, nullptr, &input);
				    Require((source.find("\nOpKill\n") != std::string::npos) == exec_zero, "MRT null tail keeps its discard/no-op distinction");
				    Require(source.find("\nOpStore %outColor") == std::string::npos, "null MRT never fabricates a color write");
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, ValidatesFullAndSparseMrtAndFullParameterDataflow)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (uint32_t target = 0; target < 8; ++target)
		    {
			    for (uint8_t enable: {5u, 8u, 15u})
			    {
				    const auto program = Parse(Mrt(target, enable, target == 0u ? 3u : 0u));
				    ShaderPixelInputInfo input {};
				    input.target_output_mode[target] = 9;
				    const auto source = Validate(program, nullptr, &input);
				    Has(source, "%t0_4 = OpLoad %float %v17\n%t1_4 = OpLoad %float %v61\n"
				                "%t2_4 = OpLoad %float %v173\n%t3_4 = OpLoad %float %v239\n"
				                "%t11_4 = OpCompositeConstruct %v4float %t0_4 %t1_4 %t2_4 %t3_4");
				    const auto output = target == 0u ? String8("outColor") : String8::FromPrintf("outColor%u", target);
				    if (enable == 15u)
				    {
					    Has(source, String8::FromPrintf("OpStore %%%s %%t11_4", output.c_str()));
				    } else
				    {
					    Require(source.find("\nOpStore %" + std::string(output.c_str()) + " ") == std::string::npos,
					            "sparse full export has no whole-vector store");
					    for (unsigned component = 0; component < 4; ++component)
					    {
						    const auto store = String8::FromPrintf("OpStore %%exp_component_%u_4 %%t%u_4", component, component);
						    if ((enable & (1u << component)) != 0u)
						    {
							    Has(source, String8::FromPrintf("%%exp_component_%u_4 = OpAccessChain %%_ptr_Output_float %%%s %%uint_%u\n%s",
							                                   component, output.c_str(), component, store.c_str()));
						    } else
						    {
							    Require(source.find("%exp_component_" + std::to_string(component) + "_4") == std::string::npos,
							            "disabled component does not get a write");
						    }
					    }
				    }
			    }
		    }
		    for (uint32_t target: {0x20u, 0x3fu})
		    {
			    const auto format = target == 0x20u ? ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3 :
			                                         ShaderInstructionFormat::Param31Vsrc0Vsrc1Vsrc2Vsrc3;
			    const auto program = Parse({ShaderType::Vertex, ExportWord(target, 15, 0), format, 4, 15, 0}, false, true);
			    ShaderVertexInputInfo input {};
			    const auto source = Validate(program, &input, nullptr);
			    Has(source, String8::FromPrintf("%%t0_4 = OpLoad %%float %%v17\n%%t1_4 = OpLoad %%float %%v61\n"
			                                   "%%t2_4 = OpLoad %%float %%v173\n%%t3_4 = OpLoad %%float %%v239\n"
			                                   "%%t4_4 = OpCompositeConstruct %%v4float %%t0_4 %%t1_4 %%t2_4 %%t3_4\n"
			                                   "OpStore %%param%u %%t4_4", target - 0x20u));
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, DualSourceSecondaryUsesLocation0Index1)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    for (const bool packed: {false, true})
		    {
			    // Guest MRT1 carries the secondary color; MRT0 stays the primary output.
			    const auto secondary = packed ? Mrt(1, 15, 4u) : Mrt(1, 15, 0u);
			    const auto primary   = packed ? Mrt(0, 15, 7u) : Mrt(0, 15, 3u);
			    const uint8_t mode   = packed ? 4 : 9;
			    for (const bool dual: {false, true})
			    {
				    const auto program = ParseColorPair(secondary, primary);
				    ShaderPixelInputInfo input {};
				    input.target_output_mode[0] = mode;
				    input.target_output_mode[1] = mode;
				    input.dual_source_blend     = dual;
				    const auto source = Validate(program, nullptr, &input);
				    if (dual)
				    {
					    Has(source, "OpDecorate %outColor1 Location 0");
					    Has(source, "OpDecorate %outColor1 Index 1");
					    Require(source.find("OpDecorate %outColor1 Location 1") == std::string::npos,
					            "secondary export has no Location 1 decoration");
				    } else
				    {
					    Has(source, "OpDecorate %outColor1 Location 1");
					    Require(source.find("OpDecorate %outColor1 Index") == std::string::npos,
					            "ordinary MRT1 has no dual-source index");
				    }
				    Require(source.find("\nOpStore %outColor1 ") != std::string::npos, "MRT1 store is preserved");
			    }
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderExport, DualSourceModuleRejectsTargetsBeyondTheSecondaryPair)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    const auto program = ParseColorPair(Mrt(1, 15, 0u), Mrt(0, 15, 3u));
		    ShaderPixelInputInfo input {};
		    input.target_output_mode[0] = 9;
		    input.target_output_mode[1] = 9;
		    input.target_output_mode[2] = 9;
		    input.dual_source_blend     = true;
		    (void)SpirvGenerateSource(program.code, nullptr, &input, nullptr);
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(kRejectedExit), ::testing::ContainsRegex("requires MRT0 and MRT1 color exports only"));
}

TEST(EmulatorShaderExport, DualSourceModuleRejectsMissingSecondaryMode)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    const auto program = ParseColorPair(Mrt(1, 15, 0u), Mrt(0, 15, 3u));
		    ShaderPixelInputInfo input {};
		    input.target_output_mode[0] = 9;
		    input.dual_source_blend     = true;
		    (void)SpirvGenerateSource(program.code, nullptr, &input, nullptr);
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(kRejectedExit), ::testing::ContainsRegex("requires MRT0 and MRT1 color exports only"));
}

TEST(EmulatorShaderExport, DualSourceModuleRejectsNullOnlyProgram)
{
	ASSERT_EXIT(
	    ([] {
		    Initialize();
		    const auto program = Parse(kNull);
		    ShaderPixelInputInfo input {};
		    input.target_output_mode[0] = 9;
		    input.target_output_mode[1] = 9;
		    input.dual_source_blend     = true;
		    (void)SpirvGenerateSource(program.code, nullptr, &input, nullptr);
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(kRejectedExit), ::testing::ContainsRegex("requires MRT0 and MRT1 color exports only"));
}

TEST(EmulatorShaderExport, HandConstructedStaleTailSlotsRemainRejected)
{
	for (const auto& test: {kPrim, kPos1, kPixelZ, kNull, Mrt(0, 15, 7), Mrt(3, 0, 6)})
	{
		const auto clean = HandConstructed(test);
		ASSERT_TRUE(ShaderInstructionLoweringPreconditions(clean));
		for (int source = clean.src_num; source < 4; ++source)
		{
			auto stale = clean;
			stale.src[source] = Vgpr(kRegisters[source]);
			EXPECT_FALSE(ShaderInstructionLoweringPreconditions(stale)) << "raw=" << test.word << " tail=" << source;
		}
	}
}

TEST(EmulatorShaderExport, StaleTailDiagnosticNamesExactExpPcAndRawOpcode)
{
	for (const auto& test: {kPrim, kPos1, kPixelZ, kNull, Mrt(0, 15, 7), Mrt(3, 0, 6)})
	{
		const auto diagnostic = String8::FromPrintf(
		    "shader emitter missing: stage=%u instruction=%u format=0x%016" PRIx64 " pc=0x00000030 "
		    "reason=lowering-preconditions src_num=%d mimg_address_num=0 sopp=0xff raw=0x%08" PRIx32,
		    static_cast<unsigned>(test.stage), static_cast<unsigned>(ShaderInstructionType::Exp), static_cast<uint64_t>(test.format),
		    test.sources, test.word);
		ASSERT_EXIT(RejectStaleTail(test), ::testing::ExitedWithCode(kRejectedExit), diagnostic.c_str());
	}
}

TEST(EmulatorShaderExport, RetainsExistingParserControlAndStageRejections)
{
	struct Rejected { ShaderType stage; uint32_t target; uint8_t enable; uint8_t control; bool next_gen; };
	const Rejected cases[] = {
	    {ShaderType::Pixel, 0, 0, 6, true}, {ShaderType::Pixel, 0, 15, 0, true},
	    {ShaderType::Pixel, 8, 1, 2, true}, {ShaderType::Pixel, 8, 3, 3, true}, {ShaderType::Pixel, 8, 1, 7, true},
	    {ShaderType::Pixel, 9, 1, 3, true}, {ShaderType::Pixel, 9, 0, 3, false}, {ShaderType::Vertex, 9, 0, 3, true},
	    {ShaderType::Pixel, 13, 4, 0, true}, {ShaderType::Vertex, 13, 4, 1, true}, {ShaderType::Vertex, 13, 4, 0, false},
	    {ShaderType::Pixel, 13, 1, 0, true}, {ShaderType::Vertex, 13, 1, 1, true}, {ShaderType::Vertex, 13, 1, 3, true},
	    {ShaderType::Vertex, 13, 1, 4, true}, {ShaderType::Vertex, 13, 1, 6, true},
	    {ShaderType::Vertex, 13, 0, 2, true}, {ShaderType::Vertex, 13, 2, 2, true},
	    {ShaderType::Vertex, 13, 15, 2, true}, {ShaderType::Vertex, 13, 1, 0, false},
	    {ShaderType::Pixel, 12, 15, 0, true}, {ShaderType::Vertex, 12, 15, 1, true},
	    {ShaderType::Vertex, 12, 15, 4, true}, {ShaderType::Vertex, 12, 14, 0, true},
	    {ShaderType::Vertex, 12, 15, 0, false},
	    {ShaderType::Vertex, 20, 1, 0, true}, {ShaderType::Vertex, 20, 3, 2, true},
	    {ShaderType::Vertex, 0x20u, 0, 0, true}, {ShaderType::Vertex, 0x20u, 15, 1, true},
	    {ShaderType::Vertex, 0x20u, 15, 2, true}, {ShaderType::Vertex, 0x20u, 15, 4, true},
	    {ShaderType::Vertex, 0x3fu, 15, 7, true}};
	for (const auto& test: cases)
	{
		const auto diagnostic = String8::FromPrintf("unknown exp target: 0x%02x done=%u compr=%u vm=%u en=0x%x at addr 0x00000020",
		                                            test.target, (test.control >> 1u) & 1u, (test.control >> 2u) & 1u,
		                                            test.control & 1u, static_cast<unsigned>(test.enable));
		ASSERT_EXIT(
		    {
			    Initialize(test.next_gen);
			    (void)Parse({test.stage, ExportWord(test.target, test.enable, test.control), ShaderInstructionFormat::Unknown,
			                 0, test.enable, test.control});
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(kRejectedExit), diagnostic.c_str());
	}
}

TEST(EmulatorShaderExport, RetainsGsPrologLayerMetadataAndStandaloneMrt0Gates)
{
	for (const auto& test: {kPrim, kPos1, kPos1X, Mrt(0, 0, 7)})
	{
		const auto diagnostic = String8::FromPrintf("shader emitter missing: stage=%u instruction=%u format=0x%016" PRIx64
		                                            " pc=0x00000020 sampled=0/0/0 inst=.*sopp=0xff raw=0x%08" PRIx32,
		                                            static_cast<unsigned>(test.stage), static_cast<unsigned>(ShaderInstructionType::Exp),
		                                            static_cast<uint64_t>(test.format), test.word);
		ASSERT_EXIT(
		    {
			    Initialize();
			    const auto program = Parse(test);
			    ShaderVertexInputInfo vertex {}; // No GS prolog or layer routing.
			    ShaderPixelInputInfo pixel {};
			    pixel.target_output_mode[0] = 4;
			    (void)SpirvGenerateSource(program.code, test.stage == ShaderType::Vertex ? &vertex : nullptr,
			                              test.stage == ShaderType::Pixel ? &pixel : nullptr, nullptr);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(kRejectedExit), diagnostic.c_str());
	}
}

TEST(EmulatorShaderExport, FinalClipDistanceStillRequiresSupportedOutputMetadata)
{
	const auto diagnostic = String8::FromPrintf("shader emitter missing: stage=%u instruction=%u format=0x%016" PRIx64
	                                            " pc=0x00000028 sampled=0/0/0 inst=.*sopp=0xff raw=0x%08" PRIx32,
	                                            static_cast<unsigned>(ShaderType::Vertex),
	                                            static_cast<unsigned>(ShaderInstructionType::Exp),
	                                            static_cast<uint64_t>(kPos1XDone.format), kPos1XDone.word);
	ASSERT_EXIT(
	    {
		    Initialize();
		    const auto program = ParsePositionSequence({kPos0NoDone, kPos1XDone, kParam0});
		    ShaderVertexInputInfo vertex {}; // ClipDistance0 metadata is deliberately absent.
		    (void)SpirvGenerateSource(program.code, &vertex, nullptr, nullptr);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(kRejectedExit), diagnostic.c_str());
}

UT_END();
