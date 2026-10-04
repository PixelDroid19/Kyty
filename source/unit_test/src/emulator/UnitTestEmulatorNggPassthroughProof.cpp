#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderNggPassthroughProof.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "NggPassthroughFixture.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <vector>

UT_BEGIN(EmulatorNggPassthroughProof);

using namespace Libs::Graphics;

namespace {

using namespace NggFixture;
using Type = ShaderInstructionType;
using Reject = ShaderNggPassthroughRejection;
using Kind = ShaderNggPassthroughStepKind;

uint32_t Find(const ShaderCode& code, Type type, unsigned occurrence = 0)
{
	for (uint32_t i = 0; i < code.GetInstructions().Size(); ++i)
	{
		if (code.GetInstructions().At(i).type == type)
		{
			if (occurrence == 0) { return i; }
			--occurrence;
		}
	}
	Check(false, "requested instruction exists");
	return 0;
}

ShaderOperand Register(ShaderOperandType type, int id = 0, int size = 1)
{
	ShaderOperand op;
	op.type = type;
	op.register_id = id;
	op.size = size;
	return op;
}

ShaderOperand Literal(uint32_t value)
{
	auto op = Register(ShaderOperandType::LiteralConstant, 0, 0);
	op.constant.u = value;
	return op;
}

ShaderNggPassthroughProof Accept(const ShaderCode& code, ShaderNggPassthroughCounts counts = {64, 3, 1})
{
	auto result = ShaderAnalyzeNggPassthrough(code, counts);
	if (!result.proven)
	{
		std::fprintf(stderr, "refused index=%u pc=%u: %s\n", result.rejection_index, result.rejection_pc, result.reason.c_str());
	}
	Check(result.proven && result.rejection == Reject::None && result.reason.IsEmpty(), "count-specific proof succeeds");
	Check(result.primitive_forwarding_proved && result.independent_vertex_transforms_proved, "both required proof obligations hold");
	Check(result.rejection_index == UINT32_MAX && result.rejection_pc == UINT32_MAX, "no fictitious failure PC zero");
	Check(result.instruction_count == code.GetInstructions().Size() && result.steps.size() == result.instruction_count,
	      "entire bounded program was analyzed");
	Check(result.vertex_mask == Mask(counts.es_vertex_count) && result.primitive_mask == Mask(counts.gs_primitive_count), "exact masks");
	Check(result.allocation_payload == ((counts.gs_primitive_count << 12u) | counts.es_vertex_count), "exact allocation payload");
	Check(result.steps.back().kind == Kind::End && result.steps.back().retain, "terminator is retained");
	return result;
}

void Refuse(const ShaderCode& code, Reject rejection, uint32_t index, ShaderNggPassthroughCounts counts = {64, 3, 1})
{
	const auto result = ShaderAnalyzeNggPassthrough(code, counts);
	if (result.proven || result.rejection != rejection || result.rejection_index != index)
	{
		std::fprintf(stderr, "expected reject=%u index=%u, got proven=%u reject=%u index=%u: %s\n",
		             static_cast<unsigned>(rejection), index, static_cast<unsigned>(result.proven),
		             static_cast<unsigned>(result.rejection), result.rejection_index, result.reason.c_str());
	}
	Check(!result.proven && !result.primitive_forwarding_proved && !result.independent_vertex_transforms_proved,
	      "a diagnostic prefix never becomes a proof");
	Check(result.rejection == rejection && result.rejection_index == index && !result.reason.IsEmpty(), "precise structured refusal");
	if (index != UINT32_MAX) { Check(result.rejection_pc == code.GetInstructions().At(index).pc, "original refusal PC preserved"); }
}

ShaderCode WithPrefix(const Words& prefix)
{
	auto words = Fixture();
	words.insert(words.begin(), prefix.begin(), prefix.end());
	return Parse(words);
}

// Insertion/removal controls are performed on encoded words, then reparsed.
// Deliberate malformed-IR controls below start from this same valid parser path.
ShaderCode InsertBefore(const ShaderCode& original, const Words& words, uint32_t index, const Words& inserted)
{
	auto changed = words;
	const auto offset = original.GetInstructions().At(index).pc / 4u;
	changed.insert(changed.begin() + offset, inserted.begin(), inserted.end());
	return Parse(changed);
}

} // namespace

TEST(EmulatorNggPassthroughProof, ProvesOnlySuppliedCountsAcrossBothArchitecturalWidths)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(Fixture());
		for (uint32_t width: {32u, 64u})
		{
			for (uint32_t vertices: {1u, 3u, 31u, 32u, 33u, 63u, 64u})
			{
				for (uint32_t primitives: {1u, 2u, 31u, 32u, 33u, 63u, 64u})
				{
					if (vertices > width || primitives > width) { continue; }
					const auto result = Accept(code, {width, vertices, primitives});
					Check(result.requires_layer_output_contract && result.parameter_mask == 1u, "layer/parameter obligations retained");
					Check(result.required_initial_vgprs.count() == 3 && result.required_initial_vgprs.test(0) &&
					          result.required_initial_vgprs.test(5) && result.required_initial_vgprs.test(8), "only real launch inputs required");
					Check(result.scalar_dependencies.test(3), "count dependency recorded");
				}
			}
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, RetainsEveryIndependentVertexInstructionAndExportByOriginalIndex)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto code = Parse(Fixture());
		code.SetHash0(0x5192u);
		code.SetCrc32(0x17a39u);
		const auto before = code.DbgDump();
		const auto result = Accept(code);
		Check(code.DbgDump() == before, "analyzer does not rewrite input IR");
		std::vector<uint32_t> expected;
		const auto first = Find(code, Type::VMovB32);
		for (uint32_t i = first; i < code.GetInstructions().Size(); ++i) { expected.push_back(i); }
		Check(result.retained_instruction_indices == expected, "exact whole vertex residue, including all exports and end");
		Check(result.steps[result.primitive_export_index].kind == Kind::PrimitiveForward &&
		          !result.steps[result.primitive_export_index].retain, "PRIM is a forwarding obligation, not silently omitted");
		for (const auto index: expected)
		{
			Check(result.steps[index].pc == code.GetInstructions().At(index).pc, "source PCs survive residualization plan");
			Check(result.steps[index].scalar_source_mask == 0u, "no retained numeric scalar reads");
			if (result.steps[index].kind != Kind::End)
			{
				Check(result.steps[index].active_mask_known && result.steps[index].active_mask == 7u, "exact ES activity on all residue");
			}
		}
		// PC relocation and identity metadata are not recognition patterns.
		for (uint32_t i = 0; i < code.GetInstructions().Size(); ++i) { code.GetInstructions()[i].pc += 0x400u; }
		code.SetHash0(0);
		code.SetCrc32(UINT32_MAX);
		Check(Accept(code).retained_instruction_indices == expected, "proof depends on semantics, not PC/hash identity");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, UnknownUpperSystemBitsRemainUnknownThroughDeadScalarArithmetic)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(Fixture());
		const auto result = Accept(code);
		const auto& input = result.steps[0].scalar_sources[0];
		Check(input.known_mask == 0xffffu && input.value == 0x103u && input.initial_dependencies.test(3), "partial s3 knowledge only");
		const auto shift = Find(code, Type::SLshrB32);
		Check(result.steps[shift].scalar_result.known_mask == 0xff0000ffu, "high system bits survive as unknown shifted middle bits");
		const auto subtract = Find(code, Type::SSubI32, 1);
		const auto& partial = result.steps[subtract].scalar_result;
		Check((partial.known_mask & 63u) == 63u && partial.known_mask != 0xffffffffu && (partial.value & 63u) == 61u,
		      "subtraction proves low shift bits without making a scalar representative");
		const auto mask = Find(code, Type::SLshrB64, 1);
		Check(result.steps[mask].scalar_result.known_mask == UINT64_MAX && result.steps[mask].scalar_result.value == 7u,
		      "64-bit mask is derived from only six known count bits");
		Words prefix;
		Sop1(prefix, 3, 42, 71); // Unknown SGPR copied to dead state is harmless.
		Sop2(prefix, 3, 44, Inline(0), 42);
		const auto dead = Accept(WithPrefix(prefix));
		Check(dead.steps[0].scalar_result.known_mask == 0u && dead.steps[0].scalar_result.initial_dependencies.test(71),
		      "unprovided scalar is not silently zero initialized");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, NumericScalarInputsAreRefusedEvenWhenAllBitsAreKnown)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto original = Parse(Fixture());
		const auto first = Find(original, Type::VMovB32);
		for (auto source: {Register(ShaderOperandType::Sgpr, 3), Register(ShaderOperandType::Sgpr, 20),
		                   Register(ShaderOperandType::Sgpr, 22), Register(ShaderOperandType::Sgpr, 71),
		                   Register(ShaderOperandType::ExecLo), Register(ShaderOperandType::ExecHi),
		                   Register(ShaderOperandType::VccLo), Register(ShaderOperandType::VccHi),
		                   Register(ShaderOperandType::Scc), Register(ShaderOperandType::ExecZ),
		                   Register(ShaderOperandType::VccZ), Register(ShaderOperandType::M0)})
		{
			auto code = original;
			code.GetInstructions()[first].src[0] = source;
			Check(ShaderInstructionLoweringPreconditions(code.GetInstructions().At(first)), "numeric read is valid IR, not malformed fixture");
			Refuse(code, Reject::ScalarVectorInput, first);
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, Wave32IgnoresHighExecActivityWithoutErasingItsNumericState)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto low = Parse(Fixture(3, 1, true));
		const auto result = Accept(low, {32, 3, 1});
		const auto first = Find(low, Type::VMovB32);
		Check(result.steps[first].exec_before.known_mask == 0xffffffffu, "initial EXEC_HI stays unknown under low-word writes");
		Refuse(low, Reject::UnknownExec, Find(low, Type::Exp), {64, 3, 1});
		const auto words = Fixture();
		const auto original = Parse(words);
		Words high;
		Sop1(high, 3, 127, 255, 0x935ac671u);
		const auto changed = InsertBefore(original, words, Find(original, Type::VMovB32), high);
		const auto preserved = Accept(changed, {32, 3, 1});
		const auto vector_index = Find(changed, Type::VMovB32);
		Check(preserved.steps[vector_index].exec_before.value == 0x935ac67100000007ull &&
		          preserved.steps[vector_index].exec_before.known_mask == UINT64_MAX, "known numeric high EXEC word is preserved");
		Refuse(changed, Reject::ExecMaskMismatch, vector_index, {64, 3, 1});
		auto leak = changed;
		leak.GetInstructions()[vector_index].src[0] = Register(ShaderOperandType::ExecHi);
		Refuse(leak, Reject::ScalarVectorInput, vector_index, {32, 3, 1});
		Sop1(high, 3, 42, 127); // numeric high-word copy into an ordinary SGPR
		auto saved = InsertBefore(original, words, Find(original, Type::VMovB32), high);
		const auto saved_index = Find(saved, Type::VMovB32);
		const auto saved_proof = Accept(saved, {32, 3, 1});
		Check(saved_proof.steps[saved_index - 1u].scalar_result.value == 0x935ac671u &&
		          saved_proof.steps[saved_index - 1u].scalar_result.known_mask == 0xffffffffu, "scalar copy retains the numeric high word");
		saved.GetInstructions()[saved_index].src[0] = Register(ShaderOperandType::Sgpr, 42);
		Refuse(saved, Reject::ScalarVectorInput, saved_index, {32, 3, 1});
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, AllocationPayloadAndMessageSequenceMustBeExact)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto words = Fixture();
		const auto original = Parse(words);
		const auto allocation = Find(original, Type::SSendmsg);
		for (uint32_t payload: {0u, 3u, 0x1002u, 0x2003u, 0x10001003u})
		{
			Words change;
			Sop1(change, 3, 124, 255, payload);
			const auto code = InsertBefore(original, words, allocation, change);
			Refuse(code, Reject::AllocationMismatch, Find(code, Type::SSendmsg));
		}
		Words unknown;
		Sop1(unknown, 3, 124, 3); // Low bits known, high bits unknown: not a complete M0 payload.
		const auto code = InsertBefore(original, words, allocation, unknown);
		Refuse(code, Reject::UnknownAllocation, Find(code, Type::SSendmsg));
		for (uint32_t message: {0u, 2u, 3u, 7u, 0x109u})
		{
			auto wrong = original;
			wrong.GetInstructions()[allocation].src[0] = Literal(message);
			Refuse(wrong, Reject::UnsupportedControl, allocation);
		}
		auto absent = original;
		absent.GetInstructions().RemoveAt(allocation);
		Refuse(absent, Reject::AllocationSequence, Find(absent, Type::Exp));
		Words again;
		Sopp(again, 16, 9);
		const auto twice = InsertBefore(original, words, allocation, again);
		Refuse(twice, Reject::AllocationSequence, Find(twice, Type::SSendmsg, 1));
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, EveryVectorAndExportNeedsItsProvedCountMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto original = Parse(Fixture());
		const auto primitive_shift = Find(original, Type::SLshrB64);
		const auto vertex_shift = Find(original, Type::SLshrB64, 1);
		for (auto pair: {std::pair<uint32_t, uint32_t> {primitive_shift, Find(original, Type::Exp)},
		                std::pair<uint32_t, uint32_t> {vertex_shift, Find(original, Type::VMovB32)}})
		{
			auto wrong = original;
			wrong.GetInstructions()[pair.first].src[1] = Literal(60); // four active lanes, wrong for both supplied counts
			Refuse(wrong, Reject::ExecMaskMismatch, pair.second);
			auto unknown = original;
			unknown.GetInstructions()[pair.first].src[1] = Register(ShaderOperandType::Sgpr, 70);
			Refuse(unknown, Reject::UnknownExec, pair.second);
		}
		Words vector;
		Vop1(vector, 1, 40, Inline(0));
		Refuse(WithPrefix(vector), Reject::UnknownExec, 0);
		// A mask that is a strict subset of the ES mask is insufficient even
		// when the destination will later be overwritten.
		auto narrow = original;
		narrow.GetInstructions()[vertex_shift].src[1] = Literal(63);
		Refuse(narrow, Reject::ExecMaskMismatch, Find(narrow, Type::VMovB32));
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, PrimitiveInputMustBeForwardedOnceBeforeItIsOverwritten)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto words = Fixture();
		const auto original = Parse(words);
		const auto primitive = Find(original, Type::Exp);
		auto different = original;
		different.GetInstructions()[primitive].src[0] = Register(ShaderOperandType::Vgpr, 5);
		Refuse(different, Reject::PrimitiveSourceModified, primitive);
		Words overwrite;
		Sop1(overwrite, 4, 126, Inline(-1));
		Vop1(overwrite, 1, 0, Inline(0));
		const auto modified = InsertBefore(original, words, primitive, overwrite);
		Refuse(modified, Reject::PrimitiveSourceModified, Find(modified, Type::VMovB32), {64, 64, 1});
		Words second;
		Exp(second, 20, 1, true, 0, 11, 13, 15);
		const auto duplicate = InsertBefore(original, words, primitive, second);
		Refuse(duplicate, Reject::DuplicateExport, Find(duplicate, Type::Exp, 1));
		auto missing = original;
		missing.GetInstructions().RemoveAt(primitive);
		Refuse(missing, Reject::AllocationSequence, Find(missing, Type::Exp));
		// Packed primitive data is not a vertex input supplied by a residual
		// compiler, even where the two planned lane domains happen to overlap.
		auto escape = original;
		escape.GetInstructions()[Find(escape, Type::VMovB32)].src[0] = Register(ShaderOperandType::Vgpr, 0);
		Refuse(escape, Reject::PrimitiveVectorInput, Find(escape, Type::VMovB32), {64, 1, 1});
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, ExportWaitsProtectExecAndAsynchronousSourceReads)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto words = Fixture();
		const auto original = Parse(words);
		const auto wait = Find(original, Type::SWaitcnt);
		auto absent = original;
		absent.GetInstructions().RemoveAt(wait);
		Refuse(absent, Reject::ExportDependency, Find(absent, Type::SLshrB64, 1));
		auto not_drained = original;
		not_drained.GetInstructions()[wait].src[0] = Literal(0xff1fu); // expcnt(1) cannot release PRIM
		Refuse(not_drained, Reject::ExportDependency, Find(not_drained, Type::SLshrB64, 1));
		Words overwrite;
		Vop1(overwrite, 1, 17, Inline(0));
		const auto pending = InsertBefore(original, words, Find(original, Type::SEndpgm), overwrite);
		Refuse(pending, Reject::ExportDependency, Find(pending, Type::SEndpgm) - 1u);
		Words waited;
		Sopp(waited, 12, 0);
		Vop1(waited, 1, 17, Inline(0));
		Accept(InsertBefore(original, words, Find(original, Type::SEndpgm), waited));
		// Mutating v0 after a drained primitive is permitted and is retained.
		Words change_primitive;
		Vop1(change_primitive, 1, 0, Inline(4));
		Accept(InsertBefore(original, words, Find(original, Type::VMovB32), change_primitive));
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, LateUndefinedInputAndOutputShapesFailClosed)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto original = Parse(Fixture());
		const auto position = Find(original, Type::Exp, 2);
		const auto layer = Find(original, Type::Exp, 1);
		const auto parameter = Find(original, Type::Exp, 3);
		for (const auto index: {layer, position, parameter})
		{
			auto undefined = original;
			undefined.GetInstructions()[index].src[0] = Register(ShaderOperandType::Vgpr, 73);
			Refuse(undefined, Reject::UndefinedVectorInput, index);
		}
		for (uint8_t mask: {uint8_t {0}, uint8_t {1}, uint8_t {7}, uint8_t {14}})
		{
			auto partial = original;
			partial.GetInstructions()[parameter].exp_enable_mask = mask;
			Refuse(partial, Reject::InvalidExport, parameter);
			partial = original;
			partial.GetInstructions()[position].exp_enable_mask = mask;
			Refuse(partial, Reject::InvalidExport, position);
		}
		for (uint8_t control: {uint8_t {0}, uint8_t {3}, uint8_t {6}, uint8_t {255}})
		{
			auto changed = original;
			changed.GetInstructions()[position].exp_control = control;
			Refuse(changed, Reject::InvalidExport, position);
		}
		auto missing = original;
		missing.GetInstructions().RemoveAt(position);
		Refuse(missing, Reject::MissingPosition, Find(missing, Type::SEndpgm));
		Words duplicate;
		Exp(duplicate, 12, 15, true, 17, 19, 23, 25);
		const auto twice = InsertBefore(original, Fixture(), parameter, duplicate);
		Refuse(twice, Reject::DuplicateExport, Find(twice, Type::Exp, 3));
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, SchedulingHintsAreDistinguishedFromReservedAndObservableControls)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto original = Parse(Fixture());
		const auto hint = Find(original, Type::SInstPrefetch);
		const auto nop = Find(original, Type::SInstPrefetch, 1);
		for (uint32_t mode: {0u, 4u, 0xffffu})
		{
			auto invalid = original;
			invalid.GetInstructions()[hint].src[0] = Literal(mode);
			Refuse(invalid, Reject::UnsupportedControl, hint);
		}
		for (uint8_t alias: {uint8_t {22}, uint8_t {23}, uint8_t {255}})
		{
			auto invalid = original;
			invalid.GetInstructions()[nop].sopp_opcode = alias;
			Refuse(invalid, Reject::UnsupportedControl, nop);
		}
		auto reserved_wait = original;
		const auto wait = Find(original, Type::SWaitcnt);
		reserved_wait.GetInstructions()[wait].src[0] = Literal(0xffffu);
		Refuse(reserved_wait, Reject::UnsupportedControl, wait);
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, VectorNopIsAnInertSchedulingHintInEitherEncoding)
{
	ASSERT_EXIT(([] {
		Initialize();
		// v_nop (VOP1) and its VOP3 form lead a shipped vertex shader.
		const auto code = WithPrefix({0x7e000000u, 0xd5800000u, 0x00000000u});
		const auto proof = Accept(code);
		for (unsigned occurrence = 0; occurrence < 2u; occurrence++)
		{
			const auto index = Find(code, Type::VNop, occurrence);
			Check(proof.steps[index].kind == Kind::SchedulingHint && !proof.steps[index].retain, "v_nop is proved inert and dropped");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, ScalarShiftMaskingAndSignedOverflowHaveIndependentValueControls)
{
	ASSERT_EXIT(([] {
		Initialize();
		for (uint32_t a: {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
		{
			for (uint32_t b: {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
			{
				Words prefix;
				Sop1(prefix, 3, 38, 255, a);
				Sop2(prefix, 3, 40, 38, 255, b);
				Sop1(prefix, 3, 42, 253); // capture SCC numerically in dead scalar state
				const auto result = Accept(WithPrefix(prefix));
				const int64_t sa = a <= 0x7fffffffu ? static_cast<int64_t>(a) : static_cast<int64_t>(a) - 0x100000000ll;
				const int64_t sb = b <= 0x7fffffffu ? static_cast<int64_t>(b) : static_cast<int64_t>(b) - 0x100000000ll;
				const uint32_t overflow = sa - sb < -0x80000000ll || sa - sb > 0x7fffffffll ? 1u : 0u;
				Check(result.steps[1].scalar_result.value == static_cast<uint32_t>(a - b) &&
				          result.steps[1].scalar_result.known_mask == 0xffffffffu, "modulo-32 scalar subtraction");
				Check(result.steps[1].scc_after.value == overflow && result.steps[1].scc_after.known_mask == 0xffffffffu &&
				          result.steps[2].scalar_result.value == overflow, "signed overflow, not borrow, and move preserves SCC");
			}
		}
		for (uint32_t shift: {0u, 1u, 31u, 32u, 33u, 63u, 64u, 65u, UINT32_MAX})
		{
			Words prefix;
			Sop1(prefix, 3, 36, 255, 0x12345678u);
			Sop1(prefix, 3, 37, 255, 0x87654321u);
			Sop2(prefix, 30, 38, 36, 255, shift);
			Sop2(prefix, 32, 39, 36, 255, shift);
			Sop2(prefix, 33, 40, 36, 255, shift);
			const auto result = Accept(WithPrefix(prefix));
			Check(result.steps[2].scalar_result.value == static_cast<uint32_t>(0x12345678u << (shift & 31u)), "LSHL_B32 masks five bits");
			Check(result.steps[3].scalar_result.value == (0x12345678u >> (shift & 31u)), "LSHR_B32 masks five bits");
			Check(result.steps[4].scalar_result.value == (0x8765432112345678ull >> (shift & 63u)) &&
			          result.steps[4].scalar_result.known_mask == UINT64_MAX, "LSHR_B64 preserves both numeric words and masks six bits");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, ScalarBitfieldOffsetsWidthsAndPartialKnownBitOperationsAreConservative)
{
	ASSERT_EXIT(([] {
		Initialize();
		for (uint32_t width: {0u, 1u, 8u, 31u, 32u})
		{
			for (uint32_t offset: {0u, 1u, 15u, 31u, 63u})
			{
				Words prefix;
				Sop1(prefix, 3, 36, 255, 0xa596c33cu);
				Sop2(prefix, 39, 38, 36, 255, (width << 16u) | offset);
				const auto result = Accept(WithPrefix(prefix));
				const uint32_t expected = static_cast<uint32_t>((uint64_t {0xa596c33cu} >> (offset & 31u)) & Mask(width));
				Check(result.steps[1].scalar_result.value == expected && result.steps[1].scalar_result.known_mask == 0xffffffffu,
				      "BFE offset uses five bits and extraction zero fills/truncates at a word");
			}
		}
		Words prefix;
		Sop2(prefix, 14, 36, 71, Inline(0)); // unknown & 0 is known zero
		Sop2(prefix, 16, 38, 71, Inline(-1)); // unknown | -1 is all known ones
		const auto result = Accept(WithPrefix(prefix));
		Check(result.steps[0].scalar_result.value == 0u && result.steps[0].scalar_result.known_mask == 0xffffffffu,
		      "AND can kill unknown scalar bits");
		Check(result.steps[1].scalar_result.value == 0xffffffffu && result.steps[1].scalar_result.known_mask == 0xffffffffu,
		      "OR can determine unknown scalar bits");
		Words wide;
		Sop2(wide, 39, 36, 3, 255, 33u << 16u);
		Refuse(WithPrefix(wide), Reject::UnsupportedControl, 0);
		Words unknown_control;
		Sop2(unknown_control, 39, 36, 3, 71);
		Refuse(WithPrefix(unknown_control), Reject::UnsupportedControl, 0);
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, RejectsCrossLaneMemoryBranchesAndContinuations)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto original = Parse(Fixture());
		const auto first = Find(original, Type::VMovB32);
		for (const auto type: {Type::VReadfirstlaneB32, Type::VReadlaneB32, Type::VWritelaneB32, Type::VPermlane16B32,
		                      Type::VMbcntLoU32B32, Type::VCndmaskB32, Type::VCmpxEqU32, Type::SLoadDword,
		                      Type::BufferLoadDword, Type::BufferStoreDword, Type::BufferAtomicAdd, Type::ImageLoad,
		                      Type::DsReadB32, Type::DsWriteB32, Type::Unknown})
		{
			auto code = original;
			code.GetInstructions()[first].type = type;
			// Invalid memory flag sentinels are rejected by the shared gate;
			// otherwise the closed semantic subset rejects the instruction.
			const auto expected = ShaderInstructionLoweringPreconditions(code.GetInstructions().At(first))
			                          ? Reject::UnsupportedInstruction : Reject::MalformedInstruction;
			Refuse(code, expected, first);
		}
		Words branch;
		Sopp(branch, 4, 0); // a real decoded conditional branch to the following instruction
		Refuse(WithPrefix(branch), Reject::ControlFlow, 0);
		auto continuation = original;
		continuation.SetContinuationPc(0x800u);
		Refuse(continuation, Reject::ControlFlow, UINT32_MAX);
		const auto refusal = ShaderAnalyzeNggPassthrough(continuation, {64, 3, 1});
		Check(refusal.rejection_pc == 0x800u, "continuation source location is not lost");
		auto dpp = original;
		dpp.GetInstructions()[first].src[0].dpp = true;
		dpp.GetInstructions()[first].src[0].dpp_ctrl = 0x111u;
		dpp.GetInstructions()[first].src[0].dpp_row_mask = 15;
		dpp.GetInstructions()[first].src[0].dpp_bank_mask = 15;
		Refuse(dpp, Reject::MalformedInstruction, first);
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, RealDecodedLaneMemoryAndAtomicInstructionsCannotBeDropped)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words lane;
		Vop1(lane, 2, 38, 256 + 5); // v_readfirstlane_b32 s38, v5
		const auto read_lane = WithPrefix(lane);
		Check(read_lane.GetInstructions().At(0).type == Type::VReadfirstlaneB32, "real lane-read decode");
		Refuse(read_lane, Reject::UnsupportedInstruction, 0);
		// Real DS atomic with complete encoding metadata, not a changed opcode
		// name on a move. It passes the common lowering gate but cannot vanish
		// during residualization, even if its result is not used.
		const auto atomic = WithPrefix({0xd8000000u, (8u << 8u) | 5u}); // ds_add_u32 v5, v8
		Check(atomic.GetInstructions().At(0).type == Type::DsAddU32, "real LDS atomic decode");
		Refuse(atomic, Reject::UnsupportedInstruction, 0);
		Words barrier;
		Sopp(barrier, 10);
		Refuse(WithPrefix(barrier), Reject::UnsupportedInstruction, 0);
		// A branch with a perfectly known predicate still changes the proof
		// domain; straight-line analysis does not interpret its chosen arm.
		Words branch;
		Sop2(branch, 14, 36, 3, Inline(0));
		Sopp(branch, 4, 0);
		Refuse(WithPrefix(branch), Reject::ControlFlow, 1);
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, AllFullParameterTargetsAndOptionalLayerHaveExplicitObligations)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto words = Fixture();
		const auto original = Parse(words);
		const auto parameter = Find(original, Type::Exp, 3);
		const auto offset = original.GetInstructions().At(parameter).pc / 4u;
		for (uint32_t target = 0; target < 32; ++target)
		{
			auto changed = words;
			changed[offset] = (changed[offset] & ~0x3f0u) | ((32u + target) << 4u);
			const auto result = Accept(Parse(changed));
			Check(result.parameter_mask == (1u << target), "exact original parameter target preserved");
			changed[offset] = (changed[offset] & ~15u) | 7u;
			const auto partial = Parse(changed);
			Refuse(partial, Reject::InvalidExport, Find(partial, Type::Exp, 3));
		}
		auto without_layer = original;
		without_layer.GetInstructions().RemoveAt(Find(without_layer, Type::Exp, 1));
		const auto result = Accept(without_layer);
		Check(!result.requires_layer_output_contract && result.layer_export_index == UINT32_MAX, "no invented layer export");
		Words duplicate;
		Exp(duplicate, 32, 15, false, 25, 23, 19, 17);
		const auto twice = InsertBefore(original, words, Find(original, Type::SEndpgm), duplicate);
		Refuse(twice, Reject::DuplicateExport, Find(twice, Type::Exp, 4));
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, SharedPreconditionsAndCompleteProgramBoundsRemainMandatory)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto words = Fixture();
		const auto original = Parse(words);
		const auto primitive = Find(original, Type::Exp);
		for (unsigned tail = 1; tail < 4; ++tail)
		{
			auto stale = original;
			stale.GetInstructions()[primitive].src[tail] = Register(ShaderOperandType::Vgpr, 191);
			Check(!ShaderInstructionLoweringPreconditions(stale.GetInstructions().At(primitive)), "shared validator rejects live operand tail");
			Refuse(stale, Reject::MalformedInstruction, primitive);
		}
		auto malformed = original;
		const auto first = Find(original, Type::VMovB32);
		malformed.GetInstructions()[first].src_num = 0;
		Refuse(malformed, Reject::MalformedInstruction, first);
		malformed = original;
		malformed.GetInstructions()[first].dst.register_id = 256;
		Refuse(malformed, Reject::MalformedInstruction, first);
		malformed = original;
		malformed.GetInstructions()[first].src[0].size = 2;
		Refuse(malformed, Reject::MalformedInstruction, first);
		auto truncated_words = words;
		truncated_words.pop_back();
		ShaderCode truncated;
		truncated.SetType(ShaderType::Vertex);
		Check(!ShaderTryParseBounded(truncated_words.data(), static_cast<uint32_t>(truncated_words.size() * 4u), &truncated),
		      "bounded parser reports truncation before terminator");
		Refuse(truncated, Reject::MissingEnd, truncated.GetInstructions().Size() - 1u);
		const uint32_t literal_word[] = {0xbea403ffu}; // s_mov_b32 s36, missing literal
		ShaderCode missing_literal;
		missing_literal.SetType(ShaderType::Vertex);
		Check(!ShaderTryParseBounded(literal_word, sizeof(literal_word), &missing_literal), "missing literal remains parser failure");
		Check(!ShaderAnalyzeNggPassthrough(missing_literal, {64, 3, 1}).proven, "partial decoder instruction is not sufficient");
		auto suffix = original;
		auto extra = original.GetInstructions().At(first);
		extra.pc = original.GetInstructions().At(Find(original, Type::SEndpgm)).pc + 4u;
		suffix.GetInstructions().Add(extra);
		Refuse(suffix, Reject::ControlFlow, Find(suffix, Type::SEndpgm));
		const auto count = original.GetInstructions().Size();
		Words prefix(ShaderNggPassthroughInstructionLimit - count, 0xbf800000u);
		const auto maximum = WithPrefix(prefix);
		Check(Accept(maximum).instruction_count == ShaderNggPassthroughInstructionLimit, "exact bound is fully analyzed");
		prefix.push_back(0xbf800000u);
		const auto excess = WithPrefix(prefix);
		Refuse(excess, Reject::InstructionLimit, UINT32_MAX);
		Check(ShaderAnalyzeNggPassthrough(excess, {64, 3, 1}).steps.empty(), "overbound program is rejected before any partial walk");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorNggPassthroughProof, InvalidCountsWidthsAndStagesHaveNoImplicitDefaults)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = Parse(Fixture());
		for (uint32_t width: {0u, 1u, 31u, 33u, 63u, 65u, UINT32_MAX})
		{
			Refuse(code, Reject::InvalidWaveSize, UINT32_MAX, {width, 3, 1});
		}
		for (uint32_t width: {32u, 64u})
		{
			for (uint32_t invalid: {0u, width + 1u, 255u, UINT32_MAX})
			{
				Refuse(code, Reject::InvalidCounts, UINT32_MAX, {width, invalid, 1});
				Refuse(code, Reject::InvalidCounts, UINT32_MAX, {width, 1, invalid});
			}
		}
		for (const auto type: {ShaderType::Unknown, ShaderType::Pixel, ShaderType::Compute, ShaderType::Fetch})
		{
			auto wrong = code;
			wrong.SetType(type);
			Refuse(wrong, Reject::UnsupportedStage, UINT32_MAX);
		}
		auto embedded = code;
		embedded.SetVsEmbedded(true);
		Refuse(embedded, Reject::UnsupportedStage, UINT32_MAX);
		ShaderCode empty;
		empty.SetType(ShaderType::Vertex);
		Refuse(empty, Reject::MissingEnd, UINT32_MAX);
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

UT_END();
