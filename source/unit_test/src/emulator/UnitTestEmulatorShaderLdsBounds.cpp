#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveLds.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

UT_BEGIN(EmulatorShaderLdsBounds);

using namespace Libs::Graphics;

namespace {

std::string g_fixture_case;

void Initialize()
{
	g_fixture_case.clear();
	// Production EXIT prints the diagnostic to stdout. Redirect only this
	// death-test child so gtest can assert the actual instruction/PC/reason.
	std::fflush(stdout);
#if defined(_WIN32)
	if (::_dup2(::_fileno(stderr), ::_fileno(stdout)) < 0) { std::_Exit(6); }
#else
	if (::dup2(::fileno(stderr), ::fileno(stdout)) < 0) { std::_Exit(6); }
#endif
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
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

void CheckTerminator(const ShaderCode& code, uint32_t pc)
{
	if (code.GetInstructions().Size() != 2 || code.GetInstructions().At(0).pc != 0)
	{
		std::fprintf(stderr, "fixture instruction/PC mismatch\n");
		std::_Exit(4);
	}
	const auto& end = code.GetInstructions().At(1);
	if (end.type != ShaderInstructionType::SEndpgm || end.pc != pc || !ShaderInstructionLoweringPreconditions(end) ||
	    ShaderClassifyComputeWaveInstruction(end) != ShaderComputeWaveInstructionKind::End)
	{
		std::fprintf(stderr, "fixture terminal rejected: type=%u pc=%u\n", static_cast<unsigned>(end.type), end.pc);
		std::_Exit(4);
	}
}

String8 Generate(uint32_t opcode, uint32_t registers, uint16_t offset, bool paired, uint32_t lds_dwords = 4,
                 uint32_t threads = 64)
{
	const uint32_t words[] = {0xd8000000u | (opcode << 18u) | offset, registers, 0xbf810000u};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
	CheckTerminator(code, 8);
	ShaderComputeInputInfo input {};
	input.threads_num[0] = threads;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.lds_dwords = lds_dwords;
	if (paired) { input.wave_layout = {ShaderComputeWaveStrategy::Paired64On32, {threads, 1, 1}, {threads / 2, 1, 1}, 64, 32, 2, threads / 64, lds_dwords}; }
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Vector<uint32_t> binary;
	String8 error;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		std::fprintf(stderr,
		             "LDS fixture module failure: pc=0x00000000 opcode=0x%02x registers=0x%08x offset=0x%04x "
		             "strategy=%s lds_dwords=%u threads=%u\ncase: %s\n"
		             "condition: ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty()\n%s\n",
		             opcode, registers, static_cast<unsigned>(offset), paired ? "paired64-on32" : "native", lds_dwords, threads,
		             g_fixture_case.c_str(), error.IsEmpty() ? "<empty toolchain error>" : error.c_str());
		std::fflush(stderr);
		std::_Exit(3);
	}
	return source;
}

struct State
{
	std::vector<uint32_t> lds {0x12345678u, 0x87654321u, 0xfedcba98u, 0x80000010u};
	std::vector<uint32_t> spill;
	std::unordered_map<std::string, uint32_t> registers;
	std::unordered_map<std::string, uint32_t> inputs;
	unsigned accesses = 0;
};

[[noreturn]] void Fail(const std::string& reason)
{
	std::fprintf(stderr, "LDS fixture failure: %s\ncase: %s\n", reason.c_str(), g_fixture_case.c_str());
	std::fflush(stderr);
	std::_Exit(5);
}

uint32_t ParseUint(const std::string& text)
{
	char* end = nullptr;
	errno = 0;
	const auto value = std::strtoul(text.c_str(), &end, 0);
	if (errno != 0 || end == text.c_str() || *end != '\0' || text.front() == '-' || value > UINT32_MAX)
	{
		Fail("invalid uint constant: " + text);
	}
	return static_cast<uint32_t>(value);
}

using Tokens = std::vector<std::string>;

std::vector<Tokens> Tokenize(const std::string& text)
{
	std::vector<Tokens> lines;
	std::istringstream input(text);
	for (std::string line; std::getline(input, line);)
	{
		std::istringstream words(line);
		Tokens tokens;
		for (std::string word; words >> word;) { tokens.push_back(word); }
		if (!tokens.empty()) { lines.push_back(tokens); }
	}
	return lines;
}

// Execute the production lane body, not a copy of the intended bounds formula.
// Unknown operations, undefined SSA values and even an evaluated OOB pointer
// fail this checker. The surrounding prolog/dispatcher is validated separately.
void Execute(const String8& source, const std::string& bank, State* state, bool addtid = false)
{
	const std::string full(source.c_str());
	const std::string tag = "0" + bank;
	const auto begin = full.find(addtid ? "%addtid_exec_" + tag + (bank.empty() ? " =" : "_word =") : "%lds_exec_" + tag + " =");
	const std::string end_label = (addtid ? "%addtid_merge_" : "%lds_exec_merge_") + tag + " = OpLabel";
	const auto end = full.find(end_label, begin);
	if (begin == std::string::npos || end == std::string::npos) { Fail("missing LDS lane body"); }
	auto lines = Tokenize(full.substr(begin, end + end_label.size() - begin));
	auto values = state->inputs;
	struct Pointer { bool spill; uint32_t index; };
	std::unordered_map<std::string, Pointer> pointers;
	std::unordered_map<std::string, size_t> labels;
	for (const auto& line: Tokenize(full))
	{
		if (line.size() == 5 && line[1] == "=" && line[2] == "OpConstant" && line[3] == "%uint")
		{
			values[line[0]] = ParseUint(line[4]);
		}
	}
	values["%wave_generic_exec_0" + bank] = state->registers.at("%exec_lo");
	for (size_t i = 0; i < lines.size(); ++i)
	{
		if (lines[i].size() == 3 && lines[i][2] == "OpLabel") { labels[lines[i][0]] = i; }
	}
	const auto load = [&](const std::string& pointer)
	{
		if (pointers.count(pointer) == 0) { return state->registers.at(pointer); }
		const auto& p = pointers.at(pointer);
		return (p.spill ? state->spill : state->lds).at(p.index);
	};
	const auto store = [&](const std::string& pointer, uint32_t value)
	{
		if (pointers.count(pointer) != 0)
		{
			const auto& p = pointers.at(pointer);
			(p.spill ? state->spill : state->lds).at(p.index) = value;
		}
		else { state->registers[pointer] = value; }
	};
	std::string block = "%entry";
	std::string predecessor;
	for (size_t pc = 0, steps = 0; pc < lines.size(); ++pc)
	{
		if (++steps > 4096) { Fail("unbounded lane body"); }
		const auto& t = lines[pc];
		if (t[0] == "OpSelectionMerge") { continue; }
		if (t[0] == "OpBranch" || t[0] == "OpBranchConditional")
		{
			predecessor = block;
			pc = labels.at(t[0] == "OpBranch" ? t[1] : t[values.at(t[1]) != 0 ? 2 : 3]) - 1;
			continue;
		}
		if (t[0] == "OpStore") { store(t[1], values.at(t[2])); continue; }
		if (t.size() < 3 || t[1] != "=") { Fail("unsupported statement"); }
		const auto& op = t[2];
		if (op == "OpLabel") { block = t[0]; continue; }
		if (op == "OpLoad") { values[t[0]] = load(t[4]); continue; }
		if (op == "OpPhi")
		{
			bool found = false;
			for (size_t i = 4; i + 1 < t.size(); i += 2)
			{
				if (t[i + 1] == predecessor) { values[t[0]] = values.at(t[i]); found = true; break; }
			}
			if (!found) { Fail("missing Phi predecessor"); }
			continue;
		}
		if (op == "OpAccessChain")
		{
			const bool spill = t[4] == "%lds_addtid";
			if ((!spill && t[4] != "%lds") || values.at(t[5]) >= (spill ? state->spill : state->lds).size())
			{
				Fail("evaluated OOB LDS/spill pointer");
			}
			pointers[t[0]] = {spill, values.at(t[5])};
			++state->accesses;
			continue;
		}
		if (op.rfind("OpAtomic", 0) == 0)
		{
			const uint32_t old = load(t[4]);
			const uint32_t data = values.at(t[7]);
			uint32_t result = 0;
			if (op == "OpAtomicIAdd") { result = old + data; }
			else if (op == "OpAtomicISub") { result = old - data; }
			else if (op == "OpAtomicAnd") { result = old & data; }
			else if (op == "OpAtomicOr") { result = old | data; }
			else if (op == "OpAtomicXor") { result = old ^ data; }
			else if (op == "OpAtomicExchange") { result = data; }
			else if (op == "OpAtomicUMin") { result = std::min(old, data); }
			else if (op == "OpAtomicUMax") { result = std::max(old, data); }
			else if (op == "OpAtomicSMin") { result = static_cast<uint32_t>(std::min(static_cast<int32_t>(old), static_cast<int32_t>(data))); }
			else if (op == "OpAtomicSMax") { result = static_cast<uint32_t>(std::max(static_cast<int32_t>(old), static_cast<int32_t>(data))); }
			else { Fail("unsupported atomic"); }
			store(t[4], result);
			values[t[0]] = old;
			continue;
		}
		const uint32_t a = values.at(t[4]);
		if (op == "OpBitcast" || op == "OpCopyObject") { values[t[0]] = a; continue; }
		const uint32_t b = values.at(t[5]);
		uint32_t result = 0;
		if (op == "OpIAdd") { result = a + b; }
		else if (op == "OpBitwiseAnd") { result = a & b; }
		else if (op == "OpShiftRightLogical" && b < 32) { result = a >> b; }
		else if (op == "OpShiftLeftLogical" && b < 32) { result = a << b; }
		else if (op == "OpUGreaterThanEqual") { result = a >= b; }
		else if (op == "OpULessThan") { result = a < b; }
		else if (op == "OpLogicalAnd") { result = a != 0 && b != 0; }
		else if (op == "OpIEqual") { result = a == b; }
		else if (op == "OpINotEqual") { result = a != b; }
		else { Fail("unsupported lane operation: " + op); }
		values[t[0]] = result;
	}
}

State MakeState(uint32_t address, bool active, const std::string& bank)
{
	State state;
	state.registers["%exec_lo"] = active ? 1 : 0;
	state.registers["%exec_lane_lo"] = active ? 1 : 0;
	for (int reg = 0; reg < 8; ++reg) { state.registers["%v" + std::to_string(reg) + bank] = 0xa0000000u + reg; }
	state.registers["%v0" + bank] = address;
	return state;
}

void CheckCondition(bool condition, const char* expression, int line)
{
	if (!condition)
	{
		std::fprintf(stderr, "LDS fixture check failed at %s:%d: %s\ncase: %s\n", __FILE__, line, expression, g_fixture_case.c_str());
		std::fflush(stderr);
		std::_Exit(4);
	}
}

#define Check(condition) CheckCondition((condition), #condition, __LINE__)

void RunWidths()
{
	Initialize();
	const uint32_t reads[] = {0x36u, 0x76u, 0xfeu, 0xffu};
	const uint32_t writes[] = {0x0du, 0x4du, 0xdeu, 0xdfu};
	for (bool paired: {false, true})
	{
		for (int count = 1; count <= 4; ++count)
		{
			for (bool write: {false, true})
			{
				// Reads deliberately overwrite the address VGPR as their first result.
				const auto source = Generate(write ? writes[count - 1] : reads[count - 1], write ? 0x100u : 0u, 0, paired);
				for (const std::string bank: paired ? std::vector<std::string>{"_low", "_high"} : std::vector<std::string>{""})
				{
					for (bool active: {false, true})
					{
						for (uint32_t address: {0u, static_cast<uint32_t>((4 - count) * 4), 16u, 0xfffffffcu})
						{
							auto state = MakeState(address, active, bank);
							const auto before = state;
							Execute(source, bank, &state);
							const bool valid = active && address < 16;
							Check(state.accesses == (valid ? static_cast<unsigned>(count) : 0u));
							for (int word = 0; word < count; ++word)
							{
								if (write && valid) { Check(state.lds[address / 4 + word] == before.registers.at("%v" + std::to_string(word + 1) + bank)); }
								if (!write && active) { Check(state.registers.at("%v" + std::to_string(word) + bank) == (valid ? before.lds[address / 4 + word] : 0u)); }
							}
							if (!write || !valid) { Check(state.lds == before.lds); }
							if (!active) { Check(state.registers == before.registers); }
						}
						// Partial spans and unaligned addresses exercise host safety only;
						// this test does not specify their undefined hardware data values.
						for (uint32_t address: {1u, 12u, 13u})
						{
							auto state = MakeState(address, active, bank);
							Execute(source, bank, &state);
						}
					}
				}
			}
		}
	}
}

void RunRead2AndOffsets()
{
	Initialize();
	struct Read2Case { uint16_t offset; unsigned first; unsigned second; };
	// Index 4 denotes an OOB result. The in-range pairs pin DWORD scaling and
	// exercise the second read after the first result overwrites the address VGPR.
	const Read2Case cases[] = {{0x0500u, 0, 4}, {0x0005u, 4, 0}, {0x0301u, 1, 3}, {0x0103u, 3, 1}};
	for (bool paired: {false, true})
	{
		for (const auto& test: cases)
		{
			const auto source = Generate(0x37u, 0u, test.offset, paired);
			for (const std::string bank: paired ? std::vector<std::string>{"_low", "_high"} : std::vector<std::string>{""})
			{
				for (bool active: {false, true})
				{
					auto state = MakeState(0, active, bank);
					const auto before = state;
					Execute(source, bank, &state);
					Check(state.accesses == (active ? (test.first < 4u) + (test.second < 4u) : 0u));
					Check(state.lds == before.lds);
					if (active)
					{
						Check(state.registers.at("%v0" + bank) == (test.first < 4u ? before.lds[test.first] : 0u));
						Check(state.registers.at("%v1" + bank) == (test.second < 4u ? before.lds[test.second] : 0u));
					} else
					{
						Check(state.registers == before.registers);
					}
				}
			}
		}
		for (uint16_t offset: {4u, 0x100u})
		{
			const auto source = Generate(0x36u, 0u, offset, paired);
			for (const std::string bank: paired ? std::vector<std::string>{"_low", "_high"} : std::vector<std::string>{""})
			{
				for (uint32_t address: {0u, 0xffffff00u})
				{
					auto state = MakeState(address, true, bank);
					const auto before = state;
					Execute(source, bank, &state);
					const bool valid = address == 0u && offset == 4u;
					Check(state.accesses == (valid ? 1u : 0u) && state.lds == before.lds);
					Check(state.registers.at("%v0" + bank) == (valid ? before.lds[1] : 0u));
				}
			}
		}
	}
}

void RunAtomics()
{
	Initialize();
	for (bool paired: {false, true})
	{
		struct AtomicCase { uint32_t opcode; uint32_t result; };
		// Independent ISA results for LDS=0x80000010, DATA=0xa0000001.
		const AtomicCase cases[] = {{0, 0x20000011u}, {1, 0xe000000fu}, {5, 0x80000010u}, {6, 0xa0000001u},
		                            {7, 0x80000010u}, {8, 0xa0000001u}, {9, 0x80000000u}, {10, 0xa0000011u},
		                            {11, 0x20000011u}, {0x20, 0x20000011u}, {0x2d, 0xa0000001u}};
		for (const auto& test: cases)
		{
			const uint32_t opcode = test.opcode;
			const bool returns = opcode == 0x20u || opcode == 0x2du;
			const auto source = Generate(opcode, returns ? 0x04000100u : 0x00000100u, 0, paired);
			for (const std::string bank: paired ? std::vector<std::string>{"_low", "_high"} : std::vector<std::string>{""})
			{
				for (bool active: {false, true})
				{
					for (uint32_t address: {12u, 16u, 0xfffffffcu})
					{
						auto state = MakeState(address, active, bank);
						const auto before = state;
						Execute(source, bank, &state);
						const bool valid = active && address == 12u;
						Check(state.accesses == (valid ? 1u : 0u));
						if (!valid) { Check(state.lds == before.lds); }
						if (!active) { Check(state.registers == before.registers); }
						if (returns && active) { Check(state.registers.at("%v4" + bank) == (valid ? before.lds[3] : 0u)); }
						if (valid) { Check(state.lds[3] == test.result); }
					}
				}
			}
		}
	}
}

void RunBuffer(uint32_t control, uint32_t second_flags, bool paired, bool next_gen = true, int corrupt = 0)
{
	Initialize();
	Config::SetNextGen(next_gen);
	const uint32_t words[] = {control, 0x8004052bu | second_flags, 0xbf810000u};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
	CheckTerminator(code, 8);
	const bool typed = (control >> 26u) == 0x3au;
	const auto expected_type = !typed ? ShaderInstructionType::BufferLoadDword
	                                 : (((control >> 16u) & 7u) == 0 ? ShaderInstructionType::TBufferLoadFormatX
	                                    : (((control >> 16u) & 7u) == 1 ? ShaderInstructionType::TBufferLoadFormatXy
	                                                                  : ShaderInstructionType::TBufferLoadFormatXyzw));
	Check(code.GetInstructions().At(0).type == expected_type);
	if (corrupt == 1) { code.GetInstructions()[0].mtbuf_format = 0xffu; }
	if (corrupt == 2) { code.GetInstructions()[0].mtbuf_components = 3; }
	if (corrupt == 3) { code.GetInstructions()[0].mtbuf_format_is_gen5 = !next_gen; }
	Check(ShaderInstructionLoweringPreconditions(code.GetInstructions().At(0)) ==
	      ((code.GetInstructions().At(0).buffer_flags & ~0x0au) == 0));
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = input.threads_num[2] = 1;
	if (paired) { input.wave_layout = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0}; }
	input.bind.storage_buffers.buffers_num = 1;
	input.bind.storage_buffers.start_register[0] = 16;
	input.bind.storage_buffers.sources[0] = ShaderStorageBindingSource::MetadataSharp;
	input.bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadOnly;
	input.bind.storage_buffers.accesses[0] = (control >> 26u) == 0x3au ? ShaderStorageAccess::Typed : ShaderStorageAccess::Raw;
	input.bind.storage_buffers.code_available[0] = true;
	input.bind.storage_buffers.exact_matches[0] = true;
	input.bind.storage_buffers.raw_vmem_oob_guarded[0] = true;
	input.bind.storage_buffers.buffers[0].fields[0] = 0x1000u;
	input.bind.storage_buffers.buffers[0].fields[1] = 16u << 16u;
	input.bind.storage_buffers.buffers[0].fields[2] = 4u;
	input.bind.push_constant_size = 16;
	ShaderCalcBindingIndices(&input.bind);
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	if (next_gen && (control >> 26u) == 0x3au)
	{
		const auto format = (control >> 19u) & 0x7fu;
		Check(std::string(source.c_str()).find("OpStore %temp_int_5 %int_" + std::to_string(format)) != std::string::npos);
	}
	Vector<uint32_t> binary;
	String8 error;
	std::_Exit(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty() ? 0 : 3);
}

void RunMubuf(uint32_t first_flags, uint32_t second_flags, bool paired)
{
	RunBuffer(0xe0302000u | first_flags, second_flags, paired);
}

ShaderCode ParsePair(uint32_t control, uint32_t registers, bool next_gen)
{
	Config::SetNextGen(next_gen);
	const uint32_t words[] = {control, registers, 0xbf810000u};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	Check(ShaderTryParseBounded(words, sizeof(words), &code));
	Check(code.GetInstructions().Size() == 2 && code.GetInstructions().At(1).pc == 8);
	CheckTerminator(code, 8);
	return code;
}

void RunParserMetadata()
{
	Initialize();
	for (bool next_gen: {false, true})
	{
		struct Flags { uint32_t first; uint32_t second; uint8_t gen5; uint8_t legacy; };
		const Flags flags[] = {{0, 0, 0, 0}, {0, 1u << 22u, 2, 2}, {0, 1u << 23u, 4, 4},
		                       {1u << 15u, 0, 8, 128}, {1u << 15u, 3u << 22u, 14, 134}};
		for (const auto& flag: flags)
		{
			const auto code = ParsePair(0xe0302234u | flag.first, 0x8504052bu | flag.second, next_gen);
			const auto& inst = code.GetInstructions().At(0);
			Check(inst.type == ShaderInstructionType::BufferLoadDword && inst.buffer_flags == (next_gen ? flag.gen5 : flag.legacy));
			Check(inst.buffer_idxen && !inst.buffer_offen && inst.src[1].register_id == 16 && inst.src[1].size == 4);
			Check(inst.buffer_imm_offset == (next_gen ? 0x234u : 0u) && inst.src[2].constant.u == (next_gen ? 5u : 0x239u));
			Check(ShaderInstructionLoweringPreconditions(inst) == ((inst.buffer_flags & ~0x0au) == 0));
		}
		for (const auto& flag: {Flags {1u << 16u, 0, 1, 1}, Flags {1u << 17u, 0, 128, 128}, Flags {0, 1u << 21u, 128, 128}})
		{
			const auto code = ParsePair(0xe0302000u | flag.first, 0x8004052bu | flag.second, next_gen);
			Check(code.GetInstructions().At(0).buffer_flags == flag.gen5);
			Check(!ShaderInstructionLoweringPreconditions(code.GetInstructions().At(0)));
		}
		struct Format { uint32_t opcode; uint8_t gen5; uint8_t legacy; uint8_t components; ShaderInstructionType type; };
		const Format formats[] = {{0, 22, 0x74, 1, ShaderInstructionType::TBufferLoadFormatX},
		                          {1, 64, 0x7b, 2, ShaderInstructionType::TBufferLoadFormatXy},
		                          {3, 77, 0x7e, 4, ShaderInstructionType::TBufferLoadFormatXyzw}};
		for (const auto& format: formats)
		{
			for (uint32_t address_mode = 0; address_mode < 4; ++address_mode)
			{
				for (const auto& flag: flags)
				{
					const uint32_t encoded = next_gen ? format.gen5 : format.legacy;
					const auto code = ParsePair(0xe8000234u | (encoded << 19u) | (format.opcode << 16u) |
					                               (address_mode << 12u) | flag.first, 0x8504052bu | flag.second, next_gen);
					const auto& inst = code.GetInstructions().At(0);
					Check(inst.type == format.type && inst.mtbuf_format == encoded && inst.mtbuf_components == format.components);
					Check(inst.mtbuf_format_is_gen5 == next_gen && inst.dst.size == format.components);
					Check(inst.buffer_flags == (next_gen ? flag.gen5 : flag.legacy));
					Check(inst.buffer_idxen == ((address_mode & 2u) != 0) && inst.buffer_offen == ((address_mode & 1u) != 0));
					Check(inst.src_num == 3 && inst.src[0].size == (address_mode == 3u ? 2 : 1));
					Check(inst.buffer_imm_offset == 0x234u && inst.src[2].constant.u == 5u);
					Check(ShaderInstructionLoweringPreconditions(inst) == ((inst.buffer_flags & ~0x0au) == 0));
				}
			}
		}
		if (!next_gen)
		{
			const auto code = ParsePair(0xeba02000u, 0x8024052bu, false);
			Check(code.GetInstructions().At(0).buffer_flags == 128 && !ShaderInstructionLoweringPreconditions(code.GetInstructions().At(0)));
		}
		// Literal S_OFFSET consumes its own word and remains distinct from the
		// typed-buffer immediate, including in the legacy encoding.
		const uint32_t literal_words[] = {0xe80023d0u | ((next_gen ? 22u : 0x74u) << 19u), 0xff04052bu, 0x1234u, 0xbf810000u};
		ShaderCode literal;
		literal.SetType(ShaderType::Compute);
		Check(ShaderTryParseBounded(literal_words, sizeof(literal_words), &literal));
		CheckTerminator(literal, 12);
		const auto& inst = literal.GetInstructions().At(0);
		Check(inst.src[2].type == ShaderOperandType::LiteralConstant && inst.src[2].constant.u == 0x1234u);
		Check(inst.buffer_imm_offset == 0x3d0u);
	}
}

void RunRejectedMtbuf(uint32_t opcode, uint32_t format, bool next_gen)
{
	Initialize();
	const auto code = ParsePair(0xe8002000u | (format << 19u) | ((opcode & 7u) << 16u),
	                            0x8004052bu | ((opcode >> 3u) << 21u), next_gen);
	(void) code;
	std::_Exit(0);
}

void RunCanonicalSourceTails()
{
	Initialize();
	const auto validate = [](const ShaderCode& code)
	{
		ShaderComputeInputInfo input {};
		input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		input.wave_layout = {ShaderComputeWaveStrategy::Native, {1, 1, 1}, {1, 1, 1}, 32, 32, 1, 1, 0};
		// Explicit synthetic FP state for the parser-to-emitter FP16 controls.
		input.float_mode = 0xc0u;
		input.fp_mode_known = true;
		input.fp16_overflow_known = true;
		const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		Vector<uint32_t> binary;
		String8 error;
		if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty()) { Fail(error.c_str()); }
	};
	const uint32_t terminal[] = {0xbf810000u};
	ShaderCode end_code;
	end_code.SetType(ShaderType::Compute);
	Check(ShaderTryParseBounded(terminal, sizeof(terminal), &end_code));
	const auto end = end_code.GetInstructions().At(0);
	Check(end.type == ShaderInstructionType::SEndpgm && end.src_num == 0 && ShaderInstructionLoweringPreconditions(end));
	validate(end_code);
	for (int corrupt = 0; corrupt < 5; ++corrupt)
	{
		auto invalid = end;
		if (corrupt == 0) { invalid.src[0].constant.u = 1; }
		if (corrupt == 1) { invalid.src[0] = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 1}; }
		if (corrupt == 2) { invalid.src[1] = end.src[0]; }
		if (corrupt == 3) { invalid.src_num = 1; }
		if (corrupt == 4) { invalid.raw_word |= 1u; }
		Check(!ShaderInstructionLoweringPreconditions(invalid));
	}
	struct Case { uint32_t control; uint32_t sources; int count; ShaderInstructionType type; };
	const Case cases[] = {{0xd7650002u, 126u | (128u << 9u), 2, ShaderInstructionType::VMbcntLoU32B32},
	                      {0xd7660002u, 127u | (128u << 9u), 2, ShaderInstructionType::VMbcntHiU32B32},
	                      {0xd5d50010u, 256u, 1, ShaderInstructionType::VSqrtF16},
	                      {0xd5320010u, 256u | (257u << 9u), 2, ShaderInstructionType::VAddF16},
	                      {0xd74b0010u, 256u | (257u << 9u) | (258u << 18u), 3, ShaderInstructionType::VFmaF16}};
	for (const auto& test: cases)
	{
		const auto code = ParsePair(test.control, test.sources, true);
		const auto& inst = code.GetInstructions().At(0);
		Check(inst.type == test.type && inst.src_num == test.count && inst.pc == 0 && ShaderInstructionLoweringPreconditions(inst));
		for (int source = test.count; source < 4; ++source)
		{
			Check(inst.src[source].type == ShaderOperandType::Unknown && inst.src[source].size == 0);
		}
		auto malformed = inst;
		malformed.src[test.count] = inst.src[0];
		Check(!ShaderInstructionLoweringPreconditions(malformed));
		validate(code);
	}
	const uint32_t words[] = {0xd5d50010u, 255u, 0x3c003c00u, 0xbf810000u};
	ShaderCode literal;
	literal.SetType(ShaderType::Compute);
	Check(ShaderTryParseBounded(words, sizeof(words), &literal));
	CheckTerminator(literal, 12);
	Check(literal.GetInstructions().At(0).src[0].constant.u == 0x3c003c00u &&
	      ShaderInstructionLoweringPreconditions(literal.GetInstructions().At(0)));
	validate(literal);
}

void RunUnusedVop3(uint32_t control, uint32_t sources)
{
	Initialize();
	(void) ParsePair(control, sources, true);
	std::_Exit(0);
}

void RunFullAtomicOffsets()
{
	Initialize();
	struct Atomic { uint32_t opcode; uint32_t result; };
	const Atomic cases[] = {{0, 0x20000011u}, {1, 0xe000000fu}, {5, 0x80000010u}, {6, 0xa0000001u},
	                        {7, 0x80000010u}, {8, 0xa0000001u}, {9, 0x80000000u}, {10, 0xa0000011u},
	                        {11, 0x20000011u}, {0x20, 0x20000011u}, {0x2d, 0xa0000001u}};
	for (const auto& test: cases)
	{
		const bool returns = test.opcode == 0x20u || test.opcode == 0x2du;
		const uint32_t registers = returns ? 0x04000100u : 0x00000100u;
		for (uint16_t offset: {0x100u, 0xfffcu})
		{
			const auto code = ParsePair(0xd8000000u | (test.opcode << 18u) | offset, registers, true);
			Check(code.GetInstructions().At(0).ds_offset == offset && ShaderLdsMemoryInstructionSupported(code.GetInstructions().At(0)));
		}
		for (bool paired: {false, true})
		{
			const auto source = Generate(test.opcode, registers, 0x100u, paired, 68);
			for (const std::string bank: paired ? std::vector<std::string>{"_low", "_high"} : std::vector<std::string>{""})
			{
				for (bool active: {false, true})
				{
					for (uint32_t address: {12u, 16u, 0xffffff00u})
					{
						auto state = MakeState(address, active, bank);
						state.lds.resize(68, 0x80000010u);
						const auto before = state;
						Execute(source, bank, &state);
						const bool valid = active && address == 12u;
						auto expected = before.lds;
						if (valid) { expected[67] = test.result; }
						Check(state.lds == expected && state.accesses == (valid ? 1u : 0u));
						if (!active) { Check(state.registers == before.registers); }
						if (returns && active) { Check(state.registers.at("%v4" + bank) == (valid ? 0x80000010u : 0u)); }
					}
				}
			}
		}
	}
}

void RunAddtid(bool shared)
{
	Initialize();
	// 1024 is the existing Function-array capacity, not a new guest LDS domain.
	const uint32_t bound = shared ? 128u : 1024u;
	for (bool paired: {false, true})
	{
		for (bool write: {false, true})
		{
			for (uint16_t offset: {0u, 0x100u})
			{
				const std::string module_case = String8::FromPrintf(
				    "ADDTID storage=%s strategy=%s operation=%s offset=0x%04x bound_dwords=%u threads=128",
				    shared ? "shared" : "private", paired ? "paired64-on32" : "native", write ? "write" : "read",
				    static_cast<unsigned>(offset), bound).c_str();
				g_fixture_case = module_case + " phase=module";
				const auto source = Generate(write ? 0xb0u : 0xb1u, write ? 0x100u : 0x04000000u, offset, paired, shared ? bound : 0u, 128);
				for (uint32_t physical: paired ? std::vector<uint32_t>{0, 31, 32, 63} : std::vector<uint32_t>{0, 31, 32, 63, 64, 127})
				{
					for (const std::string bank: paired ? std::vector<std::string>{"_low", "_high"} : std::vector<std::string>{""})
					{
						const uint32_t logical = paired ? (physical / 32u) * 64u + physical % 32u + (bank == "_high" ? 32u : 0u) : physical;
						const uint32_t tid = shared || paired ? logical % 64u : 0u;
						struct Address { uint32_t m0; uint32_t word; };
						const Address addresses[] = {{0, tid + offset / 4u}, {0xffff0004u, tid + offset / 4u + 1u},
						                             {(bound - tid - 1u) * 4u - offset, bound - 1u},
						                             {(bound - tid) * 4u - offset, bound}, {0xfffcu, bound}};
						for (const auto& address: addresses)
						{
							for (bool active: {false, true})
							{
								auto state = MakeState(0, active, bank);
								state.lds.resize(shared ? bound : 0u);
								state.spill.resize(shared ? 0u : bound);
								auto& memory = shared ? state.lds : state.spill;
								for (uint32_t i = 0; i < bound; ++i) { memory[i] = 0xc0100000u + i; }
								state.registers["%m0"] = address.m0;
								state.registers["%gl_LocalInvocationIndex"] = logical;
								state.inputs["%wave_lane_id"] = physical % 32u;
								state.inputs["%wave_logical_low"] = (physical / 32u) * 64u + physical % 32u;
								state.inputs["%wave_logical_high"] = state.inputs["%wave_logical_low"] + 32u;
								if (paired)
								{
									const uint32_t bit = 1u << (physical % 32u);
									state.registers[bank == "_low" ? "%exec_lo" : "%exec_hi"] = active ? bit : ~bit;
									state.registers[bank == "_low" ? "%exec_hi" : "%exec_lo"] = active ? 0u : bit;
								}
								g_fixture_case = module_case + String8::FromPrintf(
								    " phase=execute physical=%u logical=%u bank=%s tid=%u m0=0x%08x oracle_word=%u active=%u "
								    "exec_lo=0x%08x exec_hi=0x%08x",
								    physical, logical, bank.empty() ? "native" : bank.c_str(), tid, address.m0, address.word,
								    static_cast<unsigned>(active), state.registers.at("%exec_lo"),
								    paired ? state.registers.at("%exec_hi") : 0u).c_str();
								const auto before = state;
								Execute(source, bank, &state, true);
								const bool valid = active && address.word < bound;
								g_fixture_case += String8::FromPrintf(" phase=check accesses=%u expected_accesses=%u v4=0x%08x old_v4=0x%08x",
								                                     state.accesses, valid ? 1u : 0u, state.registers.at("%v4" + bank),
								                                     before.registers.at("%v4" + bank)).c_str();
								Check(state.accesses == (valid ? 1u : 0u));
								auto expected = shared ? before.lds : before.spill;
								if (write && valid) { expected[address.word] = before.registers.at("%v1" + bank); }
								if (memory != expected)
								{
									const auto difference = std::mismatch(memory.begin(), memory.end(), expected.begin());
									g_fixture_case += String8::FromPrintf(" memory_word=%u actual=0x%08x expected=0x%08x",
									                                     static_cast<unsigned>(difference.first - memory.begin()),
									                                     *difference.first, *difference.second).c_str();
								}
								Check(memory == expected);
								if (!write && active) { Check(state.registers.at("%v4" + bank) == (valid ? expected[address.word] : 0u)); }
								if (!active || write) { Check(state.registers == before.registers); }
							}
						}
					}
				}
			}
		}
	}
}

void RunRejectedDs(uint32_t control, ShaderInstructionType expected, bool paired)
{
	Initialize();
	const uint32_t words[] = {control, 0x00000100u, 0xbf810000u};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
	CheckTerminator(code, 8);
	const auto& inst = code.GetInstructions().At(0);
	Check(inst.type == expected && inst.ds_encoding_control == control && !ShaderLdsMemoryInstructionSupported(inst));
	Check(ShaderInstructionLoweringPreconditions(inst));
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.lds_dwords = 4;
	if (paired) { input.wave_layout = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 4}; }
	if (paired)
	{
		const auto analysis = ShaderAnalyzeComputeWaveCode(code, input);
		Check(!analysis.supported && analysis.unsupported_pc == 0);
	}
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	std::_Exit(source.IsEmpty() ? 3 : 0);
}

void RunSdwa(uint32_t control, bool paired)
{
	Initialize();
	const uint32_t words[] = {0x7e0c02f9u, control, 0xbf810000u};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
	CheckTerminator(code, 8);
	Check(code.GetInstructions().At(0).type == ShaderInstructionType::VMovB32);
	Check(ShaderInstructionLoweringPreconditions(code.GetInstructions().At(0)) == (control == 0x00000616u));
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = input.threads_num[2] = 1;
	if (paired) { input.wave_layout = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0}; }
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Vector<uint32_t> binary;
	String8 error;
	std::_Exit(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty() ? 0 : 3);
}

void RunParsedInteger(bool next_gen, bool vop3, bool subtract, uint32_t sdst, bool paired, int corrupt)
{
	Initialize();
	Config::SetNextGen(next_gen);
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (vop3)
	{
		const uint32_t opcode = subtract ? 0x126u : 0x125u;
		const uint32_t control = 0xd0000000u | (opcode << (next_gen ? 16u : 17u)) | (sdst << 8u) | 2u;
		code = ParsePair(control, 0x00020300u, next_gen); // v0, v1, unused SRC2=0
	} else
	{
		const uint32_t words[] = {((subtract ? 0x26u : 0x25u) << 25u) | (2u << 17u) | (1u << 9u) | 256u, 0xbf810000u};
		Check(ShaderTryParseBounded(words, sizeof(words), &code));
	}
	auto& inst = code.GetInstructions()[0];
	CheckTerminator(code, vop3 ? 8u : 4u);
	Check(inst.type == (subtract ? ShaderInstructionType::VSubI32 : ShaderInstructionType::VAddI32));
	Check(inst.format == (next_gen ? ShaderInstructionFormat::SVdstSVsrc0SVsrc1 : ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1));
	switch (corrupt)
	{
		case 0: break;
		case 1: inst.dst2 = {.type = ShaderOperandType::Sgpr, .register_id = 103, .size = 2}; break;
		case 2: inst.dst2 = {.type = ShaderOperandType::ExecLo, .size = 2}; break;
		case 3: inst.dst2.size = 1; break;
		case 4: inst.src[0].negate = true; break;
		case 5: inst.dst.multiplier = 2.0f; break;
		case 6: inst.src_num = 1; break;
		case 7: inst.src_num = 5; break;
		case 8: inst.src[1].size = 2; break;
		case 9: inst.vop3_op_sel = 1; break;
		case 10: inst.vop3_omod = 1; break;
		case 11: inst.dst2.absolute = true; break;
		case 12: inst.src[2] = inst.src[0]; break;
		case 13: inst.dst.register_id = 256; break;
		default: Fail("unknown integer mutation");
	}
	Check(ShaderInstructionLoweringPreconditions(inst) == (corrupt == 0));
	if (!next_gen && corrupt == 0)
	{
		Check(ShaderClassifyComputeWaveInstruction(inst) == ShaderComputeWaveInstructionKind::Unsupported);
		Check(!ShaderComputeWaveGenericVectorSupported(inst));
	}
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = input.threads_num[2] = 1;
	if (paired) { input.wave_layout = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0}; }
	if (paired && !next_gen && corrupt == 0)
	{
		const auto analysis = ShaderAnalyzeComputeWaveCode(code, input);
		Check(!analysis.supported && analysis.unsupported_pc == 0 &&
		      analysis.reason == "paired instruction has an unsupported second destination");
	}
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	if (!next_gen)
	{
		Check(std::string(source.c_str()).find(subtract ? "OpISubBorrow" : "OpIAddCarry") != std::string::npos);
	}
	Vector<uint32_t> binary;
	String8 error;
	std::_Exit(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty() ? 0 : 3);
}

void RunIntegerTuple(int corrupt, bool paired)
{
	Initialize();
	ShaderInstruction instruction {};
	instruction.type = ShaderInstructionType::VAndB32;
	instruction.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	instruction.src_num = 2;
	instruction.dst = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	instruction.src[0] = {.type = ShaderOperandType::Vgpr, .register_id = 1, .size = 1};
	instruction.src[1] = {.type = ShaderOperandType::Sgpr, .register_id = 2, .size = 1};
	switch (corrupt)
	{
		case 0: break;
		case 1: instruction.dst.register_id = 256; break;
		case 2: instruction.dst.clamp = true; break;
		case 3: instruction.vop3_op_sel = 1; break;
		case 4: instruction.vop3_omod = 1; break;
		case 5: instruction.src_num = 5; break;
		case 6: instruction.src_num = 1; break;
		case 7: instruction.src[1].register_id = 103; instruction.src[1].size = 2; break;
		default: std::_Exit(2);
	}
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(instruction);
	ShaderInstruction end {};
	end.pc = 4;
	end.type = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);
	CheckTerminator(code, 4);
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = input.threads_num[2] = 1;
	if (paired) { input.wave_layout = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0}; }
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Vector<uint32_t> binary;
	String8 error;
	std::_Exit(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty() ? 0 : 3);
}

std::string EmitterRejection(ShaderInstructionType type, bool preconditions = false)
{
	return "shader emitter missing: stage=4 instruction=" + std::to_string(static_cast<unsigned>(type)) +
	       " .*pc=0x00000000 " + (preconditions ? "reason=lowering-preconditions" : "sampled=0/0/0 inst=");
}

std::string MtbufRejection(uint32_t opcode, bool format = false)
{
	std::ostringstream text;
	if (format) { text << "unsupported mtbuf format/component tuple: .*opcode = 0x0" << std::hex << opcode << ".*at addr 0x00000000"; }
	else { text << "unknown mtbuf opcode: 0x" << std::hex << opcode << " at addr 0x00000000"; }
	return text.str();
}

constexpr const char* kPairedCarryRejection = "paired-wave instruction contract unsupported: pc=0x00000000 reason="
                                            "paired instruction has an unsupported second destination";

#if defined(_WIN32)
constexpr int kRejectedExit = 321;
#else
constexpr int kRejectedExit = 65;
#endif

#undef Check

} // namespace

TEST(EmulatorShaderLdsBounds, ContiguousWidthsUseBoundedPointersAndSnapshotAliasedAddress)
{
	ASSERT_EXIT({ RunWidths(); std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLdsBounds, Read2ChecksIndependentAddressesAndRejectsAdditionOverflow)
{
	ASSERT_EXIT({ RunRead2AndOffsets(); std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLdsBounds, AtomicsDiscardOutOfBoundsAndReturnZeroOnlyToActiveLanes)
{
	ASSERT_EXIT({ RunAtomics(); std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLdsBounds, NativeAndPairedMubufRejectUnmodeledAndReservedFlags)
{
	for (bool paired: {false, true})
	{
		ASSERT_EXIT(RunMubuf(0, 0, paired), ::testing::ExitedWithCode(0), "");
		ASSERT_EXIT(RunMubuf(0, 1u << 22u, paired), ::testing::ExitedWithCode(0), ""); // SLC hint
		ASSERT_EXIT(RunMubuf(1u << 15u, 0, paired), ::testing::ExitedWithCode(0), ""); // DLC, ISA table 98
		for (uint32_t first: {1u << 16u, 1u << 17u})
		{
			EXPECT_EXIT(RunMubuf(first, 0, paired), ::testing::ExitedWithCode(kRejectedExit), EmitterRejection(ShaderInstructionType::BufferLoadDword, true));
		}
		for (uint32_t second: {1u << 23u, 1u << 21u})
		{
			EXPECT_EXIT(RunMubuf(0, second, paired), ::testing::ExitedWithCode(kRejectedExit), EmitterRejection(ShaderInstructionType::BufferLoadDword, true));
		}
	}
}

TEST(EmulatorShaderLdsBounds, NativeAndPairedSdwaRejectUnrepresentedControls)
{
	for (bool paired: {false, true})
	{
		ASSERT_EXIT(RunSdwa(0x00000616u, paired), ::testing::ExitedWithCode(0), "");
		for (uint32_t control: {0x00070616u, 0x00400616u, 0x01000616u, 0x00080616u, 0x00000016u})
		{
			EXPECT_EXIT(RunSdwa(control, paired), ::testing::ExitedWithCode(kRejectedExit), EmitterRejection(ShaderInstructionType::VMovB32, true));
		}
	}
}

TEST(EmulatorShaderLdsBounds, NativeMtbufPreservesHintsAndRejectsTfeAndD16Aliases)
{
	// Four native emitter tuples: X, XY, XYZW, XYZW with OFFEN+IDXEN.
	// RDNA2 table 96 puts DLC at bit15 and the high opcode bit at bit53.
	for (uint32_t control: {0xe8b02000u, 0xea012000u, 0xea6b2000u, 0xea6b3000u})
	{
		const auto expected = ((control >> 16u) & 7u) == 0 ? ShaderInstructionType::TBufferLoadFormatX
		                      : (((control >> 16u) & 7u) == 1 ? ShaderInstructionType::TBufferLoadFormatXy
		                                                    : ShaderInstructionType::TBufferLoadFormatXyzw);
		ASSERT_EXIT(RunBuffer(control, 0, false), ::testing::ExitedWithCode(0), "");
		ASSERT_EXIT(RunBuffer(control | (1u << 15u), 1u << 22u, false), ::testing::ExitedWithCode(0), "");
		EXPECT_EXIT(RunBuffer(control, 1u << 23u, false), ::testing::ExitedWithCode(kRejectedExit), EmitterRejection(expected, true));
		EXPECT_EXIT(RunBuffer(control, 1u << 21u, false), ::testing::ExitedWithCode(kRejectedExit), MtbufRejection(((control >> 16u) & 7u) | 8u));
		for (int corrupt = 1; corrupt <= 3; ++corrupt)
		{
			EXPECT_EXIT(RunBuffer(control, 0, false, true, corrupt), ::testing::ExitedWithCode(kRejectedExit), EmitterRejection(expected));
		}
	}
}

TEST(EmulatorShaderLdsBounds, ProductionBufferParsersRetainGenerationFlagsFormatsAndComponents)
{
	ASSERT_EXIT({ RunParserMetadata(); std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLdsBounds, MtbufRejectsEveryUnimplementedOpcodeAndFormatComponentAlias)
{
	for (uint32_t opcode = 0; opcode < 16; ++opcode)
	{
		if (opcode == 0 || opcode == 1 || opcode == 3) { continue; }
		EXPECT_EXIT(RunRejectedMtbuf(opcode, 22, true), ::testing::ExitedWithCode(kRejectedExit), MtbufRejection(opcode));
		if (opcode < 8)
		{
			EXPECT_EXIT(RunRejectedMtbuf(opcode, 0x74, false), ::testing::ExitedWithCode(kRejectedExit), MtbufRejection(opcode));
		}
	}
	for (bool next_gen: {false, true})
	{
		for (uint32_t opcode: {0u, 1u, 3u})
		{
			const uint32_t expected = next_gen ? (opcode == 0 ? 22u : (opcode == 1 ? 64u : 77u))
			                                  : (opcode == 0 ? 0x74u : (opcode == 1 ? 0x7bu : 0x7eu));
			for (uint32_t format: {0u, 22u, 64u, 77u, 0x74u, 0x7bu, 0x7eu})
			{
				if (format == expected) { continue; }
				EXPECT_EXIT(RunRejectedMtbuf(opcode, format, next_gen), ::testing::ExitedWithCode(kRejectedExit), MtbufRejection(opcode, true));
			}
		}
	}
}

TEST(EmulatorShaderLdsBounds, OrdinaryAtomicParsersAndEmittersPreserveHighOffsetBytes)
{
	ASSERT_EXIT({ RunFullAtomicOffsets(); std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLdsBounds, AddtidSharedLdsBranchesBeforePointersAndSeparatesPhysicalBanks)
{
	ASSERT_EXIT({ RunAddtid(true); std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLdsBounds, AddtidPrivateSpillKeepsItsExistingCapacityAndBankAddresses)
{
	ASSERT_EXIT({ RunAddtid(false); std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLdsBounds, ParsedLegacyCarryAddSubRemainNativeOnlyWithExactTuples)
{
	for (bool subtract: {false, true})
	{
		ASSERT_EXIT(RunParsedInteger(false, false, subtract, 106, false, 0), ::testing::ExitedWithCode(0), "");
		EXPECT_EXIT(RunParsedInteger(false, false, subtract, 106, true, 0), ::testing::ExitedWithCode(kRejectedExit), kPairedCarryRejection);
		// Explicit VOP3B destinations: VCC, even SGPR pair and odd VALU pair.
		for (uint32_t sdst: {106u, 2u, 3u})
		{
			ASSERT_EXIT(RunParsedInteger(false, true, subtract, sdst, false, 0), ::testing::ExitedWithCode(0), "");
			EXPECT_EXIT(RunParsedInteger(false, true, subtract, sdst, true, 0), ::testing::ExitedWithCode(kRejectedExit), kPairedCarryRejection);
		}
		for (int corrupt = 1; corrupt <= 13; ++corrupt)
		{
			EXPECT_EXIT(RunParsedInteger(false, false, subtract, 106, false, corrupt), ::testing::ExitedWithCode(kRejectedExit),
			            EmitterRejection(subtract ? ShaderInstructionType::VSubI32 : ShaderInstructionType::VAddI32, true));
		}
	}
}

TEST(EmulatorShaderLdsBounds, ParsedNextGenNoCarryTuplesRetainMalformedRejections)
{
	for (bool paired: {false, true})
	{
		for (bool subtract: {false, true})
		{
			ASSERT_EXIT(RunParsedInteger(true, false, subtract, 0, paired, 0), ::testing::ExitedWithCode(0), "");
			for (int corrupt = 1; corrupt <= 13; ++corrupt)
			{
				EXPECT_EXIT(RunParsedInteger(true, false, subtract, 0, paired, corrupt), ::testing::ExitedWithCode(kRejectedExit),
				            EmitterRejection(subtract ? ShaderInstructionType::VSubI32 : ShaderInstructionType::VAddI32, true));
			}
		}
	}
}

TEST(EmulatorShaderLdsBounds, NativeAndPairedRejectInvalidTuplesBeforeOperandDiscovery)
{
	for (bool paired: {false, true})
	{
		ASSERT_EXIT(RunIntegerTuple(0, paired), ::testing::ExitedWithCode(0), "");
		for (int corrupt = 1; corrupt <= 7; ++corrupt)
		{
			EXPECT_EXIT(RunIntegerTuple(corrupt, paired), ::testing::ExitedWithCode(kRejectedExit), EmitterRejection(ShaderInstructionType::VAndB32, true));
		}
	}
}

TEST(EmulatorShaderLdsBounds, NativeAndPairedDsRejectGdsReservedAndLossyAliases)
{
	struct Case { uint32_t control; ShaderInstructionType type; const char* name; };
	const Case cases[] = {{0xd8360000u, ShaderInstructionType::DsWriteB32, "DsWriteB32"},
	                      {0xd8350000u, ShaderInstructionType::DsWriteB32, "DsWriteB32"},
	                      {0xd8780000u, ShaderInstructionType::DsWriteB32, "DsWriteB32"},
	                      {0xd80c0000u, ShaderInstructionType::DsIncU32, "DsIncU32"},
	                      {0xd8100000u, ShaderInstructionType::DsDecU32, "DsDecU32"}};
	for (bool paired: {false, true})
	{
		for (const auto& test: cases)
		{
			const std::string reason = paired ? std::string("paired-wave instruction contract unsupported: pc=0x00000000 reason=instruction ") +
			                                        test.name + " is outside the paired compute-wave admission set"
			                                  : EmitterRejection(test.type);
			EXPECT_EXIT(RunRejectedDs(test.control, test.type, paired), ::testing::ExitedWithCode(kRejectedExit), reason);
		}
	}
}

TEST(EmulatorShaderLdsBounds, CanonicalEndAndVop3UsedSourcesPassWithoutLiveTailExceptions)
{
	ASSERT_EXIT({ RunCanonicalSourceTails(); std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderLdsBounds, Vop3UnusedSelectorsAndModifiersRejectAtTheirOwnPc)
{
	for (uint32_t sources: {256u | (1u << 9u), 256u | (255u << 9u), 256u | (1u << 30u)})
	{
		EXPECT_EXIT(RunUnusedVop3(0xd5d50010u, sources), ::testing::ExitedWithCode(kRejectedExit),
		            "unsupported vop3 unused source: opcode=0x1d5 pc=0x00000000 source=1");
	}
	EXPECT_EXIT(RunUnusedVop3(0xd5d50210u, 256u), ::testing::ExitedWithCode(kRejectedExit),
	            "unsupported vop3 unused source: opcode=0x1d5 pc=0x00000000 source=1");
	EXPECT_EXIT(RunUnusedVop3(0xd7650002u, 126u | (128u << 9u) | (1u << 18u)), ::testing::ExitedWithCode(kRejectedExit),
	            "unsupported vop3 unused source: opcode=0x365 pc=0x00000000 source=2");
}

UT_END();
