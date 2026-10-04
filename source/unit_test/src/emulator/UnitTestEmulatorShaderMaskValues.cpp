#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvInternal.h"
#include "../../../emulator/src/Graphics/ShaderSpirvTemplates.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

UT_BEGIN(EmulatorShaderMaskValues);

using namespace Libs::Graphics;

namespace {

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "mask fixture: %s\n", message.c_str());
	std::_Exit(10);
}

ShaderOperand Reg(ShaderOperandType type, int id = 0, int size = 1)
{
	ShaderOperand result {};
	result.type = type;
	result.register_id = id;
	result.size = size;
	return result;
}

ShaderOperand Literal(uint32_t bits, int size = 0)
{
	auto result = Reg(ShaderOperandType::LiteralConstant, 0, size);
	result.constant.u = bits;
	return result;
}

ShaderInstruction Inst(ShaderInstructionType type, ShaderInstructionFormat::Format format, ShaderOperand dst,
                       std::initializer_list<ShaderOperand> sources)
{
	ShaderInstruction inst {};
	inst.type = type;
	inst.format = format;
	inst.dst = dst;
	for (const auto& source: sources) { inst.src[inst.src_num++] = source; }
	return inst;
}

ShaderInstruction Move(ShaderOperand dst, ShaderOperand src)
{
	return Inst(dst.size == 2 ? ShaderInstructionType::SMovB64 : ShaderInstructionType::SMovB32,
	            dst.size == 2 ? ShaderInstructionFormat::Sdst2Ssrc02 : ShaderInstructionFormat::SVdstSVsrc0, dst, {src});
}

ShaderInstruction Count(bool high, ShaderOperand mask, ShaderOperand accumulator = Literal(0))
{
	return Inst(high ? ShaderInstructionType::VMbcntHiU32B32 : ShaderInstructionType::VMbcntLoU32B32,
	            ShaderInstructionFormat::SVdstSVsrc0SVsrc1, Reg(ShaderOperandType::Vgpr, 2), {mask, accumulator});
}

ShaderInstruction ReadFirst()
{
	return Inst(ShaderInstructionType::VReadfirstlaneB32, ShaderInstructionFormat::SVdstSVsrc0,
	            Reg(ShaderOperandType::Sgpr, 0), {Reg(ShaderOperandType::Vgpr, 0)});
}

uint32_t Population(uint32_t bits)
{
	uint32_t count = 0;
	for (; bits != 0; bits &= bits - 1u) { ++count; }
	return count;
}

uint32_t First(uint64_t mask)
{
	if (mask == 0) { return 0; }
	uint32_t bit = 0;
	while ((mask & (uint64_t {1} << bit)) == 0) { ++bit; }
	return bit;
}

// A bounded SIMD evaluator of the actual production-emitted instruction
// regions. Values are raw register bits, including float-typed VGPR storage.
// Unsupported opcodes, missing definitions and out-of-domain shifts fail the
// test. No replacement implementation of a guest opcode is evaluated here.
class Wave
{
public:
	using Value = std::array<uint32_t, 4>;
	using Values = std::vector<Value>;

	explicit Wave(uint32_t width, bool paired = false, bool pixel = false): lanes(paired ? 32 : width), paired(paired), pixel(pixel)
	{
		input.threads_num[0] = width;
		input.threads_num[1] = input.threads_num[2] = 1;
		input.wave_layout = {paired ? ShaderComputeWaveStrategy::Paired64On32 : ShaderComputeWaveStrategy::Native,
		                     {width, 1, 1}, {lanes, 1, 1}, width, lanes, paired ? 2u : 1u, 1, 0};
		ps.required_subgroup_size = width;
		ps.native_wave.guest_wave_size = width;
		if (pixel) { spirv.SetPsInputInfo(&ps); } else { spirv.SetCsInputInfo(&input); }
		for (uint32_t c = 0; c < 256; ++c) { spirv.AddConstantUint(c); }
		spirv.AddConstantUint(0x108u);
		spirv.AddConstantUint(0xffffffffu);
		spirv.AddConstantUint(0xfffffffcu);
		for (uint32_t shift = 0; shift < 32; shift += 4) { spirv.AddConstantUint(15u << shift); }
		for (uint32_t r = 0; r < 32; ++r)
		{
			Set("%s" + std::to_string(r), 0);
			Set("%v" + std::to_string(r), 99);
			Set("%v" + std::to_string(r) + "_low", 99);
			Set("%v" + std::to_string(r) + "_high", 99);
		}
		Set("%gl_SubgroupInvocationID", 0);
		Set("%wave_lane_id", 0);
		Set("%vcc_lo", 0);
		Set("%vcc_hi", 0);
		Set("%scc", 0);
		active.assign(lanes, true);
		for (uint32_t lane = 0; lane < lanes; ++lane)
		{
			values["%gl_SubgroupInvocationID"][lane][0] = lane;
			values["%wave_lane_id"][lane][0] = lane;
			values["%v0"][lane][0] = lane + 1000;
			values["%v0_low"][lane][0] = lane + 1000;
			values["%v0_high"][lane][0] = lane + 1032;
		}
		ShaderCode code;
		code.SetType(pixel ? ShaderType::Pixel : ShaderType::Compute);
		spirv.SetCode(code);
		SetExec(width == 64 ? UINT64_MAX : UINT32_MAX);
	}

	void Set(const std::string& id, uint32_t value) { values[id] = Values(lanes, Value {value, 0, 0, 0}); }
	uint32_t Get(const std::string& id, uint32_t lane = 0) { return Read(id).at(lane)[0]; }
	uint64_t Exec() { return Get("%exec_lo") | (uint64_t {Get("%exec_hi")} << 32u); }
	void SetExec(uint64_t mask)
	{
		Set("%exec_lo", static_cast<uint32_t>(mask));
		Set("%exec_hi", static_cast<uint32_t>(mask >> 32u));
		if (!paired) { Run(spirv.NativeExecRefresh("test").c_str()); }
	}

	String8 Emit(const ShaderInstruction& instruction)
	{
		if (!ShaderInstructionLoweringPreconditions(instruction)) { Fail("shared precondition rejected fixture"); }
		ShaderCode code;
		code.SetType(pixel ? ShaderType::Pixel : ShaderType::Compute);
		code.GetInstructions().Add(instruction);
		spirv.SetCode(code);
		for (int s = 0; s < instruction.src_num; ++s)
		{
			if (operand_is_constant(instruction.src[s])) { spirv.AddConstant(instruction.src[s]); }
		}
		const auto* func = RecompFunc(instruction.type, instruction.format);
		if (func == nullptr) { Fail("no production emitter"); }
		String8 source;
		bool emitted = false;
		const auto kind = paired ? ShaderClassifyComputeWaveInstruction(instruction) : ShaderComputeWaveInstructionKind::Unsupported;
		if (paired && kind == ShaderComputeWaveInstructionKind::Unsupported) { Fail("paired admission rejected fixture"); }
		if (kind == ShaderComputeWaveInstructionKind::WaveCount)
		{
			emitted = spirv.EmitComputeWaveMbcnt(instruction, 0, &source);
		} else if (kind == ShaderComputeWaveInstructionKind::BankedDpp)
		{
			emitted = spirv.EmitComputeWaveDppInstruction(instruction, 0, &source);
		} else if (kind == ShaderComputeWaveInstructionKind::BankedGeneric || kind == ShaderComputeWaveInstructionKind::BankedGenericLds)
		{
			emitted = spirv.EmitComputeWaveGenericInstruction(func, instruction, 0, &source);
			if (emitted && kind == ShaderComputeWaveInstructionKind::BankedGenericLds)
			{
				source += "OpControlBarrier %uint_3 %uint_3 %uint_0x00000108\n";
			}
		} else if (kind == ShaderComputeWaveInstructionKind::BankedGenericCompare)
		{
			emitted = spirv.EmitComputeWaveGenericCompare(func, instruction, 0, &source);
		} else if (kind == ShaderComputeWaveInstructionKind::BankedAlu)
		{
			emitted = spirv.EmitComputeWaveAluInstruction(instruction, 0, &source);
		} else if (kind == ShaderComputeWaveInstructionKind::BankedCarry)
		{
			emitted = spirv.EmitComputeWaveCarryInstruction(instruction, 0, &source);
		} else if (kind == ShaderComputeWaveInstructionKind::BankedVector || kind == ShaderComputeWaveInstructionKind::WaveLane ||
		           kind == ShaderComputeWaveInstructionKind::BankedSdwaExtract)
		{
			emitted = spirv.EmitComputeWaveLaneInstruction(instruction, 0, &source);
		} else
		{
			emitted = func->func(0, code, &source, &spirv, func->param, func->scc_check);
		}
		if (!emitted) { Fail("production emitter rejected fixture"); }
		return spirv.ResolveMaskAccesses(instruction, 0, source);
	}

	void Execute(const ShaderInstruction& instruction) { Run(Emit(instruction).c_str()); }

	void BranchCondition(ShaderInstructionType type)
	{
		const auto* func = RecompFunc(type, ShaderInstructionFormat::Label);
		if (func == nullptr) { Fail("no production mask branch"); }
		const auto inst = Inst(type, ShaderInstructionFormat::Label, {}, {Literal(0)});
		const auto condition = (String8(func->param[0]) + "\n" + func->param[1]).ReplaceStr("<index>", "0");
		Run(spirv.ResolveMaskAccesses(inst, 0, condition).c_str());
	}

	Values Run(const std::string& source, const std::vector<Values>& parameters = {})
	{
		if (pixel)
		{
			Set("%gl_HelperInvocation", 0);
			for (uint32_t lane = 0; lane < lanes; ++lane) { values["%gl_HelperInvocation"][lane][0] = active[lane] ? 0u : 1u; }
		}
		std::istringstream stream(source);
		std::string line;
		size_t parameter = 0;
		uint32_t steps = 0;
		while (std::getline(stream, line))
		{
			if (++steps > 4096) { Fail("instruction region exceeds test bound"); }
			line = line.substr(0, line.find(';'));
			std::istringstream words(line);
			std::vector<std::string> t;
			for (std::string word; words >> word;) { t.push_back(word); }
			if (t.empty()) { continue; }
			if (t[0] == "OpStore") { values[t.at(1)] = Read(t.at(2)); continue; }
			if (t[0] == "OpReturnValue") { return Read(t.at(1)); }
			if (t[0] == "OpFunctionEnd") { continue; }
			if (t.size() < 3 || t[1] != "=") { Fail("unsupported emitted statement: " + line); }
			const auto& op = t[2];
			if (op == "OpFunction" || op == "OpLabel") { continue; }
			if (op == "OpFunctionParameter") { values[t[0]] = parameters.at(parameter++); continue; }
			if (op == "OpFunctionCall")
			{
				if (t.at(4) != "%wqm") { Fail("unsupported emitted helper"); }
				values[t[0]] = Run(FUNC_WQM, {Read(t.at(5)), Read(t.at(6)), Read(t.at(7))});
				continue;
			}
			Values result(lanes);
			for (uint32_t lane = 0; lane < lanes; ++lane)
			{
				auto word = [&](size_t i) { return Read(t.at(i)).at(lane)[0]; };
				auto& r = result[lane];
				if (!active[lane] && op.rfind("OpGroupNonUniform", 0) == 0 && op != "OpGroupNonUniformQuadBroadcast")
				{
					// Helpers may be inactive for ordinary subgroup operations. Poison
					// their result, rather than assuming they receive the real lanes' value.
					r.fill(UINT32_MAX);
					continue;
				}
				if (op == "OpLoad" || op == "OpCopyObject" || op == "OpBitcast") { r = Read(t.at(4)).at(lane); }
				else if (op == "OpSelect") { r = Read(t.at(word(4) != 0 ? 5 : 6)).at(lane); }
				else if (op == "OpIAdd") { r[0] = word(4) + word(5); }
				else if (op == "OpISub") { r[0] = word(4) - word(5); }
				else if (op == "OpBitwiseAnd") { r[0] = word(4) & word(5); }
				else if (op == "OpBitwiseOr") { r[0] = word(4) | word(5); }
				else if (op == "OpBitwiseXor") { r[0] = word(4) ^ word(5); }
				else if (op == "OpNot") { r[0] = ~word(4); }
				else if (op == "OpIEqual") { r[0] = word(4) == word(5); }
				else if (op == "OpINotEqual") { r[0] = word(4) != word(5); }
				else if (op == "OpULessThan") { r[0] = word(4) < word(5); }
				else if (op == "OpUGreaterThanEqual") { r[0] = word(4) >= word(5); }
				else if (op == "OpLogicalAnd") { r[0] = word(4) != 0 && word(5) != 0; }
				else if (op == "OpLogicalOr") { r[0] = word(4) != 0 || word(5) != 0; }
				else if (op == "OpLogicalNot") { r[0] = word(4) == 0; }
				else if (op == "OpBitCount") { r[0] = Population(word(4)); }
				else if (op == "OpShiftLeftLogical" || op == "OpShiftRightLogical")
				{
					if (word(5) >= 32) { Fail("undefined shift"); }
					r[0] = op == "OpShiftLeftLogical" ? word(4) << word(5) : word(4) >> word(5);
				} else if (op == "OpBitFieldInsert")
				{
					const uint32_t offset = word(6), count = word(7);
					if (offset > 32 || count > 32 - offset) { Fail("undefined bitfield insert"); }
					const uint64_t mask = ((uint64_t {1} << count) - 1u) << offset;
					r[0] = static_cast<uint32_t>((word(4) & ~mask) | ((uint64_t {word(5)} << offset) & mask));
				} else if (op == "OpCompositeExtract") { r[0] = Read(t.at(4)).at(lane).at(std::stoul(t.at(5))); }
				else if (op == "OpCompositeInsert") { r = Read(t.at(5)).at(lane); r.at(std::stoul(t.at(6))) = word(4); }
				else if (op == "OpCompositeConstruct")
				{
					for (size_t i = 4; i < t.size(); ++i) { r.at(i - 4) = word(i); }
				} else if (op == "OpIAddCarry")
				{
					const uint64_t sum = uint64_t {word(4)} + word(5);
					r[0] = static_cast<uint32_t>(sum); r[1] = static_cast<uint32_t>(sum >> 32);
				} else if (op == "OpISubBorrow") { r[0] = word(4) - word(5); r[1] = word(4) < word(5); }
				else if (op == "OpFNegate") { r[0] = word(4) ^ 0x80000000u; }
				else if (op == "OpExtInst")
				{
					if (t.at(5) == "UMin") { r[0] = std::min(word(6), word(7)); }
					else if (t.at(5) == "FAbs") { r[0] = word(6) & 0x7fffffffu; }
					else if (t.at(5) == "FindILsb") { r[0] = word(6) == 0 ? UINT32_MAX : First(word(6)); }
					else { Fail("unsupported emitted extended operation"); }
				} else if (op == "OpGroupNonUniformBallot")
				{
					for (uint32_t peer = 0; peer < lanes; ++peer)
					{
						if (active[peer] && Read(t.at(5))[peer][0]) { r[peer / 32] |= 1u << (peer % 32); }
					}
				} else if (op == "OpGroupNonUniformBitwiseOr")
				{
					for (uint32_t peer = 0; peer < lanes; ++peer) { if (active[peer]) { r[0] |= Read(t.at(6))[peer][0]; } }
				} else if (op == "OpGroupNonUniformBallotFindLSB")
				{
					const auto mask = Read(t.at(5))[lane];
					const uint64_t bits = mask[0] | (uint64_t {mask[1]} << 32);
					// The paired emitter may compute an unused empty-word result;
					// choosing the maximal undefined value catches an unguarded fetch.
					r[0] = bits == 0 ? UINT32_MAX : First(bits);
				} else if (op == "OpGroupNonUniformBroadcast" || op == "OpGroupNonUniformShuffle" || op == "OpGroupNonUniformQuadBroadcast")
				{
					uint32_t peer = word(6);
					if (op == "OpGroupNonUniformQuadBroadcast") { peer += lane & ~3u; }
					if (peer >= lanes || (op != "OpGroupNonUniformQuadBroadcast" && !active[peer]))
					{
						Fail("fetch from an unavailable physical lane");
					}
					r = Read(t.at(5))[peer];
				} else { Fail("unsupported emitted operation: " + op); }
			}
			values[t[0]] = std::move(result);
		}
		return {};
	}

	uint32_t lanes;
	bool paired;
	bool pixel;
	Spirv spirv;
	ShaderComputeInputInfo input {};
	ShaderPixelInputInfo ps {};
	std::vector<bool> active;
	std::map<std::string, Values> values;

private:
	const Values& Read(const std::string& id)
	{
		if (values.count(id) == 0)
		{
			if (id.rfind("%uint_", 0) == 0) { Set(id, static_cast<uint32_t>(std::stoul(id.substr(6), nullptr, 0))); }
			else if (id == "%true") { Set(id, 1); }
			else if (id == "%false") { Set(id, 0); }
			else { Fail("undefined emitted ID: " + id); }
		}
		return values.at(id);
	}
};

bool ValidateCompleteModule(uint32_t width, bool paired, bool pixel)
{
	ShaderCode code;
	code.SetType(pixel ? ShaderType::Pixel : ShaderType::Compute);
	code.GetInstructions().Add(Move(Reg(ShaderOperandType::ExecLo, 0, 2), Literal(4, 2)));
	code.GetInstructions().Add(ReadFirst());
	code.GetInstructions().Add(Count(false, Reg(ShaderOperandType::ExecLo)));
	code.GetInstructions().Add(Inst(ShaderInstructionType::VCmpxNeU32, ShaderInstructionFormat::SmaskVsrc0Vsrc1,
	                               Reg(ShaderOperandType::VccLo, 0, 2), {Reg(ShaderOperandType::Vgpr, 0), Literal(0)}));
	code.GetInstructions().Add(Inst(ShaderInstructionType::SWqmB64, ShaderInstructionFormat::Sdst2Ssrc02,
	                               Reg(ShaderOperandType::ExecLo, 0, 2), {Reg(ShaderOperandType::ExecLo, 0, 2)}));
	if (!paired)
	{
		code.GetInstructions().Add(Move(Reg(ShaderOperandType::Sgpr, 8, 2), Reg(ShaderOperandType::ExecLo, 0, 2)));
		code.GetInstructions().Add(Inst(ShaderInstructionType::VCmpEqU64, ShaderInstructionFormat::Sdst2Ssrc02Ssrc12,
		                               Reg(ShaderOperandType::VccLo, 0, 2),
		                               {Reg(ShaderOperandType::ExecLo, 0, 2), Reg(ShaderOperandType::Sgpr, 8, 2)}));
	}
	code.GetInstructions().Add(Inst(ShaderInstructionType::SEndpgm, ShaderInstructionFormat::Empty, {}, {}));
	for (uint32_t i = 0; i < code.GetInstructions().Size(); ++i) { code.GetInstructions()[i].pc = i * 4; }
	ShaderComputeInputInfo input {};
	input.threads_num[0] = width; input.threads_num[1] = input.threads_num[2] = 1;
	input.wave_layout = {paired ? ShaderComputeWaveStrategy::Paired64On32 : ShaderComputeWaveStrategy::Native,
	                     {width, 1, 1}, {paired ? 32u : width, 1, 1}, width, paired ? 32u : width, paired ? 2u : 1u, 1, 0};
	ShaderPixelInputInfo ps {};
	ps.required_subgroup_size = width;
	ps.native_wave.guest_wave_size = width;
	const auto source = SpirvGenerateSource(code, nullptr, pixel ? &ps : nullptr, pixel ? nullptr : &input);
	Vector<uint32_t> binary;
	String8 error;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		std::fprintf(stderr, "mask module width=%u paired=%d pixel=%d: %s\n", width, paired, pixel, error.c_str());
		return false;
	}
	return true;
}

} // namespace

TEST(EmulatorShaderMaskValues, ScalarMasksSelectSparseHighAndEmptyReadfirstLanes)
{
	for (uint32_t width: {32u, 64u})
	{
		for (uint64_t mask: {uint64_t {0}, uint64_t {4}, uint64_t {0x80000000u}, uint64_t {1} << 32, uint64_t {1} << 63})
		{
			if (width == 32 && mask > UINT32_MAX) { continue; }
			Wave wave(width);
			wave.Execute(Move(Reg(ShaderOperandType::ExecLo, 0, 2), Literal(static_cast<uint32_t>(mask), 2)));
			wave.Execute(Move(Reg(ShaderOperandType::ExecHi), Literal(static_cast<uint32_t>(mask >> 32))));
			wave.Execute(Move(Reg(ShaderOperandType::Sgpr, 6, 2), Reg(ShaderOperandType::ExecLo, 0, 2)));
			wave.Execute(ReadFirst());
			wave.Execute(Count(false, Literal(UINT32_MAX)));
			for (uint32_t lane = 0; lane < width; ++lane)
			{
				EXPECT_EQ(wave.Get("%s0", lane), 1000u + First(mask));
				EXPECT_EQ(wave.Get("%s6", lane), static_cast<uint32_t>(mask));
				EXPECT_EQ(wave.Get("%s7", lane), static_cast<uint32_t>(mask >> 32));
				EXPECT_EQ(wave.Get("%v2", lane), (mask & (uint64_t {1} << lane)) != 0 ? std::min(lane, 32u) : 99u);
			}
			EXPECT_EQ(wave.Get("%execz"), mask == 0 ? 1u : 0u);
		}
	}
}

TEST(EmulatorShaderMaskValues, CompareSaveRestoreAndWaterfallUsePackedWords)
{
	for (uint32_t width: {32u, 64u})
	{
		Wave wave(width);
		const uint64_t initial = width == 32 ? 0x80000004u : 0x8000000180000004ull;
		wave.SetExec(initial);
		uint64_t pending = initial;
		uint32_t iterations = 0;
		while (wave.Exec() != 0 && ++iterations <= width)
		{
			wave.Execute(ReadFirst());
			EXPECT_EQ(wave.Get("%s0"), 1000u + First(pending));
			wave.Execute(Inst(ShaderInstructionType::VCmpEqU32, ShaderInstructionFormat::SmaskVsrc0Vsrc1,
			                  Reg(ShaderOperandType::VccLo, 0, 2), {Reg(ShaderOperandType::Sgpr, 0), Reg(ShaderOperandType::Vgpr, 0)}));
			const uint64_t selected = uint64_t {1} << First(pending);
			EXPECT_EQ(wave.Get("%vcc_lo"), static_cast<uint32_t>(selected));
			EXPECT_EQ(wave.Get("%vcc_hi"), static_cast<uint32_t>(selected >> 32));
			wave.Execute(Inst(ShaderInstructionType::SAndSaveexecB64, ShaderInstructionFormat::Sdst2Ssrc02,
			                  Reg(ShaderOperandType::Sgpr, 4, 2), {Reg(ShaderOperandType::VccLo, 0, 2)}));
			EXPECT_EQ(wave.Exec(), selected);
			wave.Execute(Move(Reg(ShaderOperandType::ExecLo, 0, 2), Reg(ShaderOperandType::Sgpr, 4, 2)));
			EXPECT_EQ(wave.Exec(), pending);
			wave.Execute(Inst(ShaderInstructionType::VCmpxNeU32, ShaderInstructionFormat::SmaskVsrc0Vsrc1,
			                  Reg(ShaderOperandType::VccLo, 0, 2), {Reg(ShaderOperandType::Sgpr, 0), Reg(ShaderOperandType::Vgpr, 0)}));
			pending &= ~selected;
			EXPECT_EQ(wave.Exec(), pending);
		}
		EXPECT_EQ(pending, 0u);
		EXPECT_LE(iterations, width);
	}
}

TEST(EmulatorShaderMaskValues, NumericMbcntUsesOwnLaneWordAndCorrectHalf)
{
	for (bool paired: {false, true})
	{
		Wave wave(64, paired);
		for (bool high: {false, true})
		{
			for (uint32_t pattern: {0u, 1u, 0x80000000u, 0xa5c39e71u, UINT32_MAX})
			{
				wave.Set("%v1", pattern); wave.Set("%v1_low", pattern); wave.Set("%v1_high", pattern);
				wave.Execute(Count(high, Reg(ShaderOperandType::Vgpr, 1), Literal(UINT32_MAX - 7u)));
				for (uint32_t lane = 0; lane < 64; ++lane)
				{
					const uint64_t prefix = (uint64_t {1} << lane) - 1u;
					const uint32_t half = static_cast<uint32_t>(high ? prefix >> 32u : prefix);
					const auto reg = paired ? (lane < 32 ? "%v2_low" : "%v2_high") : "%v2";
					EXPECT_EQ(wave.Get(reg, lane % wave.lanes), UINT32_MAX - 7u + Population(pattern & half));
				}
			}
		}
		for (uint32_t lane = 0; lane < 64; ++lane)
		{
			const auto reg = paired ? (lane < 32 ? "%v1_low" : "%v1_high") : "%v1";
			wave.values[reg][lane % wave.lanes][0] = 1u << (lane % 32);
		}
		wave.Execute(Count(false, Reg(ShaderOperandType::Vgpr, 1)));
		for (uint32_t lane = 0; lane < 64; ++lane)
		{
			const auto reg = paired ? (lane < 32 ? "%v2_low" : "%v2_high") : "%v2";
			EXPECT_EQ(wave.Get(reg, lane % wave.lanes), lane < 32 ? 0u : 1u);
		}
	}
	Wave wave(32);
	for (uint32_t lane = 0; lane < 32; ++lane) { wave.values["%v1"][lane][0] = 1u << lane; }
	wave.Execute(Count(false, Reg(ShaderOperandType::Vgpr, 1)));
	for (uint32_t lane = 0; lane < 32; ++lane) { EXPECT_EQ(wave.Get("%v2", lane), 0u); }
	wave.Execute(Count(true, Literal(UINT32_MAX), Literal(7)));
	for (uint32_t lane = 0; lane < 32; ++lane) { EXPECT_EQ(wave.Get("%v2", lane), 7u); }
}

TEST(EmulatorShaderMaskValues, NumericExecOperandsRemainWordsInNativeAndPairedPaths)
{
	constexpr uint64_t mask = 0x8000000100000004ull;
	for (bool paired: {false, true})
	{
		Wave wave(64, paired);
		wave.SetExec(mask);
		for (bool high: {false, true})
		{
			wave.Set("%v2", 99); wave.Set("%v2_low", 99); wave.Set("%v2_high", 99);
			wave.Execute(Count(high, Reg(high ? ShaderOperandType::ExecHi : ShaderOperandType::ExecLo), Literal(5)));
			for (uint32_t lane = 0; lane < 64; ++lane)
			{
				const uint64_t prefix = (uint64_t {1} << lane) - 1u;
				const uint32_t word = static_cast<uint32_t>(high ? mask >> 32u : mask);
				const uint32_t half = static_cast<uint32_t>(high ? prefix >> 32u : prefix);
				const auto reg = paired ? (lane < 32 ? "%v2_low" : "%v2_high") : "%v2";
				EXPECT_EQ(wave.Get(reg, lane % wave.lanes), (mask & (uint64_t {1} << lane)) != 0 ? 5u + Population(word & half) : 99u);
			}
		}
		const auto direct = Inst(ShaderInstructionType::VAndB32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1,
		                         Reg(ShaderOperandType::Vgpr, 2), {Reg(ShaderOperandType::ExecLo), Literal(UINT32_MAX)});
		if (paired)
		{
			// Numeric MBCNT above admits EXEC directly. Generic paired VALU
			// currently excludes it; never substitute a native emission here.
			EXPECT_EQ(ShaderClassifyComputeWaveInstruction(direct), ShaderComputeWaveInstructionKind::Unsupported);
			wave.Execute(Move(Reg(ShaderOperandType::Sgpr, 8, 2), Reg(ShaderOperandType::ExecLo, 0, 2)));
			wave.Execute(Inst(ShaderInstructionType::VAndB32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1,
			                  Reg(ShaderOperandType::Vgpr, 2), {Reg(ShaderOperandType::Sgpr, 8), Literal(UINT32_MAX)}));
		} else
		{
			wave.Execute(direct);
		}
		for (uint32_t lane: {2u, 32u, 63u})
		{
			const auto reg = paired ? (lane < 32 ? "%v2_low" : "%v2_high") : "%v2";
			EXPECT_EQ(wave.Get(reg, lane % wave.lanes), 4u);
		}
	}
}

TEST(EmulatorShaderMaskValues, NativeU64CompareReadsNumericExecPair)
{
	const auto compare = Inst(ShaderInstructionType::VCmpEqU64, ShaderInstructionFormat::Sdst2Ssrc02Ssrc12,
	                          Reg(ShaderOperandType::VccLo, 0, 2),
	                          {Reg(ShaderOperandType::ExecLo, 0, 2), Reg(ShaderOperandType::Sgpr, 8, 2)});
	ASSERT_TRUE(ShaderInstructionLoweringPreconditions(compare));
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(compare), ShaderComputeWaveInstructionKind::Unsupported);
	for (uint32_t width: {32u, 64u})
	{
		for (uint64_t mask: {uint64_t {0}, uint64_t {4}, uint64_t {1} << 32, uint64_t {1} << 63, uint64_t {0x8000000100000004ull}})
		{
			Wave wave(width);
			wave.SetExec(mask);
			wave.Set("%vcc_hi", 0x87654321u);
			wave.Execute(Move(Reg(ShaderOperandType::Sgpr, 8, 2), Reg(ShaderOperandType::ExecLo, 0, 2)));
			wave.Execute(compare);
			for (uint32_t lane = 0; lane < width; ++lane)
			{
				EXPECT_EQ(wave.Get("%vcc_lo", lane), static_cast<uint32_t>(mask));
				EXPECT_EQ(wave.Get("%vcc_hi", lane), width == 32 ? 0x87654321u : static_cast<uint32_t>(mask >> 32u));
			}
		}
	}
}

TEST(EmulatorShaderMaskValues, LdsExecGatesConsumeLaneBitsAfterStrategyResolution)
{
	const auto read = Inst(ShaderInstructionType::DsReadB32, ShaderInstructionFormat::VdstVaddrOffset,
	                       Reg(ShaderOperandType::Vgpr, 2), {Reg(ShaderOperandType::Vgpr, 0)});
	const auto write = Inst(ShaderInstructionType::DsWriteB32, ShaderInstructionFormat::VaddrVdataOffset,
	                        {}, {Reg(ShaderOperandType::Vgpr, 0), Reg(ShaderOperandType::Vgpr, 1)});
	for (uint32_t width: {32u, 64u})
	{
		for (bool paired: {false, true})
		{
			if (paired && width != 64) { continue; }
			Wave wave(width, paired);
			wave.input.lds_dwords = 4;
			for (uint64_t mask: {uint64_t {0}, uint64_t {4}, uint64_t {1} << 32, uint64_t {1} << 63})
			{
				wave.Execute(Move(Reg(ShaderOperandType::ExecLo, 0, 2), Literal(static_cast<uint32_t>(mask), 2)));
				wave.Execute(Move(Reg(ShaderOperandType::ExecHi), Literal(static_cast<uint32_t>(mask >> 32u))));
				for (const auto& inst: {read, write})
				{
					const std::string emitted(wave.Emit(inst).c_str());
					for (uint32_t bank = 0; bank < (paired ? 2u : 1u); ++bank)
					{
						const std::string suffix = paired ? (bank == 0 ? "_low" : "_high") : "";
						const auto begin = paired ? emitted.find("%wave_generic_exec_0" + suffix + "_b_word =") : 0u;
						ASSERT_NE(begin, std::string::npos);
						const auto end = emitted.find("OpSelectionMerge", begin);
						ASSERT_NE(end, std::string::npos);
						// Evaluate the production predicate before memory control flow.
						// The LDS suite owns pointer, bounds and memory-value evaluation.
						wave.Run(emitted.substr(begin, end - begin));
						for (uint32_t lane = 0; lane < wave.lanes; ++lane)
						{
							EXPECT_EQ(wave.Get("%lds_active_0" + suffix, lane), (mask >> (lane + 32u * bank)) & 1u);
						}
					}
				}
			}
		}
	}
}

TEST(EmulatorShaderMaskValues, HalfWritesAndWave32MaskResultsPreserveAdjacentWords)
{
	Wave wide(64);
	wide.SetExec(0x8000000100000004ull);
	wide.Execute(Move(Reg(ShaderOperandType::ExecLo), Literal(8)));
	EXPECT_EQ(wide.Exec(), 0x8000000100000008ull);
	wide.Execute(Move(Reg(ShaderOperandType::ExecHi), Literal(0)));
	EXPECT_EQ(wide.Exec(), 8u);
	wide.Execute(Inst(ShaderInstructionType::SWqmB32, ShaderInstructionFormat::SVdstSVsrc0,
	                  Reg(ShaderOperandType::ExecLo), {Reg(ShaderOperandType::ExecLo)}));
	EXPECT_EQ(wide.Exec(), 15u);
	EXPECT_EQ(wide.Get("%scc"), 1u);

	Wave narrow(32);
	narrow.Set("%vcc_hi", 0x87654321u);
	narrow.Execute(Inst(ShaderInstructionType::VCmpEqU32, ShaderInstructionFormat::SmaskVsrc0Vsrc1,
	                    Reg(ShaderOperandType::VccLo, 0, 2), {Literal(1002), Reg(ShaderOperandType::Vgpr, 0)}));
	EXPECT_EQ(narrow.Get("%vcc_lo"), 4u);
	EXPECT_EQ(narrow.Get("%vcc_hi"), 0x87654321u);
	narrow.Execute(Move(Reg(ShaderOperandType::ExecHi), Literal(0x12345678u)));
	narrow.Execute(Inst(ShaderInstructionType::VCmpxEqU32, ShaderInstructionFormat::SmaskVsrc0Vsrc1,
	                    Reg(ShaderOperandType::VccLo, 0, 2), {Literal(0), Reg(ShaderOperandType::Vgpr, 0)}));
	EXPECT_EQ(narrow.Get("%exec_lo"), 0u);
	EXPECT_EQ(narrow.Get("%exec_hi"), 0x12345678u);
	EXPECT_EQ(narrow.Get("%execz"), 1u);
	narrow.Execute(Inst(ShaderInstructionType::VCmpxTruF32, ShaderInstructionFormat::SmaskVsrc0Vsrc1,
	                    Reg(ShaderOperandType::VccLo, 0, 2), {Reg(ShaderOperandType::Vgpr, 0), Reg(ShaderOperandType::Vgpr, 0)}));
	EXPECT_EQ(narrow.Get("%execz"), 1u);
}

TEST(EmulatorShaderMaskValues, ScalarVccAndSavedMaskSelectsUseArchitecturalBits)
{
	Wave wave(64);
	wave.Execute(Move(Reg(ShaderOperandType::VccLo, 0, 2), Literal(1, 2)));
	wave.Execute(Count(false, Reg(ShaderOperandType::VccLo)));
	for (uint32_t lane = 0; lane < 64; ++lane) { EXPECT_EQ(wave.Get("%v2", lane), lane == 0 ? 0u : 1u); }
	wave.Execute(Move(Reg(ShaderOperandType::VccHi), Literal(0x80000000u)));
	wave.Execute(Move(Reg(ShaderOperandType::Sgpr, 8, 2), Reg(ShaderOperandType::VccLo, 0, 2)));
	for (auto mask: {Reg(ShaderOperandType::VccLo, 0, 2), Reg(ShaderOperandType::Sgpr, 8, 2)})
	{
		wave.Execute(Inst(ShaderInstructionType::VCndmaskB32, ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2,
		                  Reg(ShaderOperandType::Vgpr, 2), {Literal(17), Literal(29), mask}));
		for (uint32_t lane = 0; lane < 64; ++lane) { EXPECT_EQ(wave.Get("%v2", lane), lane == 0 || lane == 63 ? 29u : 17u); }
	}
	wave.Execute(Inst(ShaderInstructionType::SWqmB64, ShaderInstructionFormat::Sdst2Ssrc02,
	                  Reg(ShaderOperandType::ExecLo, 0, 2), {Reg(ShaderOperandType::VccLo, 0, 2)}));
	EXPECT_EQ(wave.Exec(), 0xf00000000000000full);
}

TEST(EmulatorShaderMaskValues, MaskBranchesUseBothWordsAndRemainUniformInHelpers)
{
	for (uint32_t width: {32u, 64u})
	{
		Wave wave(width, false, true);
		wave.active[0] = false;
		for (uint64_t mask: {uint64_t {0}, uint64_t {4}, uint64_t {1} << 63})
		{
			wave.SetExec(mask);
			wave.Set("%vcc_lo", static_cast<uint32_t>(mask));
			wave.Set("%vcc_hi", static_cast<uint32_t>(mask >> 32u));
			const bool nonzero = width == 32 ? static_cast<uint32_t>(mask) != 0 : mask != 0;
			for (auto type: {ShaderInstructionType::SCbranchExecz, ShaderInstructionType::SCbranchVccz, ShaderInstructionType::SCbranchVccnz})
			{
				wave.BranchCondition(type);
				for (uint32_t lane = 0; lane < width; ++lane)
				{
					EXPECT_EQ(wave.Get("%cc_b_0", lane), type == ShaderInstructionType::SCbranchVccnz ? nonzero : !nonzero);
				}
			}
		}
	}
}

TEST(EmulatorShaderMaskValues, DppFiZeroFiltersSourceBeforeModifiersAndKeepsInactiveDestination)
{
	for (bool pixel: {false, true})
	{
		for (bool alias: {false, true})
		{
			Wave wave(32, false, pixel);
			wave.Set("%v0", 0x12345678u);
			auto source = Reg(ShaderOperandType::Vgpr, 0);
			source.dpp = true; source.dpp_row_mask = source.dpp_bank_mask = 15;
			const auto inst = Inst(ShaderInstructionType::VMovB32, ShaderInstructionFormat::SVdstSVsrc0,
			                       Reg(ShaderOperandType::Vgpr, alias ? 0 : 2), {source});
			wave.Execute(inst);
			EXPECT_EQ(wave.Get(alias ? "%v0" : "%v2", 1), 0x12345678u);
			wave.SetExec(2);
			wave.Execute(inst);
			EXPECT_EQ(wave.Get(alias ? "%v0" : "%v2", 1), 0u);
			EXPECT_EQ(wave.Get(alias ? "%v0" : "%v2", 0), 0x12345678u);
			auto negated = inst;
			negated.src[0].negate = true;
			wave.Execute(negated);
			EXPECT_EQ(wave.Get(alias ? "%v0" : "%v2", 1), 0x80000000u);
		}
	}
}

TEST(EmulatorShaderMaskValues, FragmentMaskPackingAndReadfirstRetainHelperValues)
{
	for (uint32_t real = 0; real < 4; ++real)
	{
		Wave wave(32, false, true);
		for (uint32_t lane = 0; lane < 4; ++lane) { wave.active[lane] = lane == real; }
		const uint32_t helper = (real + 1u) % 4u;
		wave.Execute(Inst(ShaderInstructionType::VCmpEqU32, ShaderInstructionFormat::SmaskVsrc0Vsrc1,
		                  Reg(ShaderOperandType::VccLo, 0, 2), {Literal(1000u + helper), Reg(ShaderOperandType::Vgpr, 0)}));
		for (uint32_t lane = 0; lane < 32; ++lane) { EXPECT_EQ(wave.Get("%vcc_lo", lane), 1u << helper); }
		wave.Execute(Move(Reg(ShaderOperandType::ExecLo, 0, 2), Reg(ShaderOperandType::VccLo, 0, 2)));
		wave.Execute(ReadFirst());
		for (uint32_t lane = 0; lane < 32; ++lane) { EXPECT_EQ(wave.Get("%s0", lane), 1000u + helper); }
	}
}

TEST(EmulatorShaderMaskValues, PairedDppFiltersInactiveSourcesAndPreservesAliasedDestination)
{
	for (bool alias: {false, true})
	{
		Wave wave(64, true);
		wave.Set("%v0_low", 0x12345678u); wave.Set("%v0_high", 0x87654321u);
		auto source = Reg(ShaderOperandType::Vgpr, 0);
		source.dpp = true; source.dpp_row_mask = source.dpp_bank_mask = 15;
		const auto inst = Inst(ShaderInstructionType::VMovB32, ShaderInstructionFormat::SVdstSVsrc0,
		                       Reg(ShaderOperandType::Vgpr, alias ? 0 : 2), {source});
		wave.Execute(inst);
		wave.SetExec(0x0000000200000002ull);
		wave.Execute(inst);
		EXPECT_EQ(wave.Get(alias ? "%v0_low" : "%v2_low", 1), 0u);
		EXPECT_EQ(wave.Get(alias ? "%v0_high" : "%v2_high", 1), 0u);
		EXPECT_EQ(wave.Get(alias ? "%v0_low" : "%v2_low", 0), 0x12345678u);
		EXPECT_EQ(wave.Get(alias ? "%v0_high" : "%v2_high", 0), 0x87654321u);
	}
}

TEST(EmulatorShaderMaskValues, CarryInputLoadsTheSelectedArchitecturalBit)
{
	for (uint32_t width: {32u, 64u})
	{
		Wave wave(width);
		wave.Set("%vcc_lo", 4u); wave.Set("%vcc_hi", 0x80000001u);
		wave.Execute(Move(Reg(ShaderOperandType::Sgpr, 8, 2), Reg(ShaderOperandType::VccLo, 0, 2)));
		for (const auto& mask: {Reg(ShaderOperandType::VccLo, 0, 2), Reg(ShaderOperandType::Sgpr, 8, 2)})
		{
			auto add = Inst(ShaderInstructionType::VAddCoCiU32, ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2,
			                Reg(ShaderOperandType::Vgpr, 2), {Literal(0), Literal(0), mask});
			add.dst2 = Reg(ShaderOperandType::VccLo, 0, 2);
			// Execute the emitter's complete input region before its arithmetic
			// helper. Carry-result packing is exercised separately below.
			const std::string emitted(wave.Emit(add).c_str());
			const auto call = emitted.find("%t_0 = OpFunctionCall");
			ASSERT_NE(call, std::string::npos);
			wave.Run(emitted.substr(0, call));
			for (uint32_t lane = 0; lane < width; ++lane)
			{
				EXPECT_EQ(wave.Get("%t2_0", lane), lane == 2 || lane == 32 || lane == 63 ? 1u : 0u);
			}
		}
	}
}

TEST(EmulatorShaderMaskValues, CarryResultsArePackedAndInactiveValuesAreRetained)
{
	Wave wave(64);
	wave.SetExec(0x8000000100000004ull);
	wave.Set("%v0", UINT32_MAX);
	auto add = Inst(ShaderInstructionType::VAddI32, ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1,
	                Reg(ShaderOperandType::Vgpr, 2), {Reg(ShaderOperandType::Vgpr, 0), Literal(1)});
	add.dst2 = Reg(ShaderOperandType::VccLo, 0, 2);
	wave.Execute(add);
	EXPECT_EQ(wave.Get("%vcc_lo"), 4u);
	EXPECT_EQ(wave.Get("%vcc_hi"), 0x80000001u);
	for (uint32_t lane = 0; lane < 64; ++lane)
	{
		EXPECT_EQ(wave.Get("%v2", lane), (wave.Exec() & (uint64_t {1} << lane)) != 0 ? 0u : 99u);
	}
}

TEST(EmulatorShaderMaskValues, PairedReadfirstMatchesNativeForSparseAndEmptyMasks)
{
	for (uint64_t mask: {uint64_t {0}, uint64_t {4}, uint64_t {1} << 31, uint64_t {1} << 32, uint64_t {1} << 63})
	{
		Wave wave(64, true);
		wave.SetExec(mask);
		wave.Execute(ReadFirst());
		for (uint32_t lane = 0; lane < 32; ++lane) { EXPECT_EQ(wave.Get("%s0", lane), 1000u + First(mask)); }
	}
}

TEST(EmulatorShaderMaskValues, CompleteProductionModuleAssemblesAndValidates)
{
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    class Validation final: public Config::ConfigSource
		    {
		    public:
			    bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t GetInteger(const Core::String&) const override { return 0; }
			    bool GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation;
		    Config::Load(validation);
		    std::_Exit(ValidateCompleteModule(32, false, false) && ValidateCompleteModule(64, false, false) &&
		                       ValidateCompleteModule(64, true, false) && ValidateCompleteModule(32, false, true) &&
		                       ValidateCompleteModule(64, false, true) ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
