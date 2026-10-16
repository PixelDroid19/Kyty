#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderFragmentMaskFlow.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "NggPassthroughFixture.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

UT_BEGIN(EmulatorFragmentMaskFlow);

using namespace Libs::Graphics;

namespace {

using namespace NggFixture;

class ScopedShaderValidation final: public Config::ConfigSource
{
public:
	ScopedShaderValidation(): m_previous(Config::ShaderValidationEnabled()) { Config::Load(*this); }
	~ScopedShaderValidation() override
	{
		m_enabled = m_previous;
		Config::Load(*this);
	}
	bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
	int64_t GetInteger(const Core::String&) const override { return 0; }
	bool GetBool(const Core::String&) const override { return m_enabled; }
	Core::String GetString(const Core::String&) const override { return {}; }

private:
	bool m_previous;
	bool m_enabled = true;
};

// One entry per source instruction so a variant can insert, drop or reorder whole
// instructions. Independently authored generic RDNA2 encodings of the pattern a
// compiler emits for "flip the result when a uniform constant says so" in a Wave64
// pixel shader; registers and values are synthetic.
enum Slot : size_t
{
	kPrefetch,
	kM0,
	kLoad,
	kSave,
	kWqm,
	kWait,
	kCompare,
	kSelect,
	kCopy,
	kBlend,
	kRestore,
	kZero,
	kExport,
	kEnd,
	kSlotCount,
};

constexpr uint32_t kVccLo   = 106u;
constexpr uint32_t kM0Reg   = 124u;
constexpr uint32_t kExecLo  = 126u;
constexpr uint32_t kVgpr    = 256u;

std::vector<Words> Baseline()
{
	std::vector<Words> slots(kSlotCount);
	Sopp(slots[kPrefetch], 32, 1);                         // s_inst_prefetch
	Sop1(slots[kM0], 3, kM0Reg, 16);                       // s_mov_b32 m0, s16
	slots[kLoad] = {0xf4201a86u, 4u};                      // s_buffer_load_dword vcc_lo, s[12:15], 4
	Sop1(slots[kSave], 4, 12, kExecLo);                    // s_mov_b64 s[12:13], exec
	Sop1(slots[kWqm], 10, kExecLo, kExecLo);               // s_wqm_b64 exec, exec
	Sopp(slots[kWait], 12, 0xc07f);                        // s_waitcnt lgkmcnt(0)
	slots[kCompare] = Sopc(6, Inline(1), kVccLo);          // s_cmp_eq_u32 1, vcc_lo
	Sop2(slots[kSelect], 11, kVccLo, kExecLo, Inline(0));  // s_cselect_b64 vcc, exec, 0
	Vop1(slots[kCopy], 1, 1, kVgpr + 0);                   // v_mov_b32 v1, v0
	Vop2(slots[kBlend], 1, 0, kVgpr + 0, 1);               // v_cndmask_b32 v0, v0, v1, vcc
	Sop1(slots[kRestore], 4, kExecLo, 12);                 // s_mov_b64 exec, s[12:13]
	Vop1(slots[kZero], 1, 2, Inline(0));                   // v_mov_b32 v2, 0
	slots[kExport] = {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)}; // exp mrt0 v2,v2,v2,v2 vm done
	Sopp(slots[kEnd], 1);                                  // s_endpgm
	return slots;
}

ShaderCode ParsePixel(const std::vector<Words>& slots)
{
	Words words;
	for (const auto& slot: slots) { words.insert(words.end(), slot.begin(), slot.end()); }
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	Check(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code), "bounded complete parse");
	for (const auto& inst: code.GetInstructions())
	{
		Check(ShaderInstructionLoweringPreconditions(inst), "every parsed instruction passes the shared preconditions");
	}
	return code;
}

uint32_t Find(const ShaderCode& code, ShaderInstructionType type)
{
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		if (code.GetInstructions().At(index).type == type) { return index; }
	}
	Check(false, "requested instruction exists");
	return 0;
}

bool Contains(const char* text, const char* fragment)
{
	return text != nullptr && std::strstr(text, fragment) != nullptr;
}

void ExpectNotLaneLocal(const ShaderCode& code, const char* why)
{
	for (uint32_t width: {32u, 64u})
	{
		const auto wave = ShaderAnalyzeNativeWave(code, width);
		Check(wave.proof != ShaderNativeWaveProof::LaneLocal, why);
	}
}

void ExpectNativeRefusal(const ShaderCode& code, const char* why)
{
	for (uint32_t width: {32u, 64u})
	{
		const auto wave = ShaderAnalyzeNativeWave(code, width);
		Check(wave.refusal_reason != nullptr, why);
	}
}

Words DsAddtidWrite(uint32_t data, uint16_t offset)
{
	return {0xd8000000u | (0xb0u << 18u) | offset, data << 8u};
}

Words DsAddtidRead(uint32_t destination, uint16_t offset)
{
	return {0xd8000000u | (0xb1u << 18u) | offset, destination << 24u};
}

Words DsWriteB32(uint32_t address, uint32_t data, uint16_t offset)
{
	return {0xd8000000u | (0x0du << 18u) | offset, (data << 8u) | address};
}

Words VAddF32FromScalarMask()
{
	// VOP3 v_add_f32 v10, exec_lo, 0: a valid tuple that reads the mask as a number.
	return {0xd0000000u | (0x103u << 16u) | 10u, kExecLo | (Inline(0) << 9u)};
}

Words Mrt0Export(uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha)
{
	return {0xf800180fu, red | (green << 8u) | (blue << 16u) | (alpha << 24u)};
}

Words VInterpP1(uint32_t destination, uint32_t source, uint32_t attribute, uint32_t channel)
{
	return {0xc8000000u | (destination << 18u) | (attribute << 10u) | (channel << 8u) | source};
}

Words UniformVccLowCompare()
{
	// SDWA VOPC compares two scalar operands and writes the architectural VCC pair.
	// Both source selectors are SGPRs; the default DWORD selects preserve the inputs.
	return {0x7c000000u | (0xc5u << 17u) | (1u << 9u) | 249u,
	        20u | (6u << 16u) | (1u << 23u) | (6u << 24u) | (1u << 31u)};
}

std::vector<Words> VccBranchWithOneVaryingWord(bool varying_high_word)
{
	std::vector<Words> slots;
	slots.push_back(UniformVccLowCompare());
	Words save_low;
	Sop1(save_low, 3, 30, kVccLo); // preserve the low comparison mask in s30
	slots.push_back(save_low);
	if (varying_high_word)
	{
		// gfx1030 VOPC SDWA: v_cmp_ne_u32 vcc_hi, 0, v0.
		slots.push_back({0x7d8a00f9u, 0x0686eb80u});
	}
	Words restore_low;
	Sop1(restore_low, 3, kVccLo, 30); // only VCC_LO is copied from a uniform mask
	slots.push_back(restore_low);
	Words branch;
	Sopp(branch, 7, 1); // skip the scalar write when the VCC branch is taken
	slots.push_back(branch);
	Words scalar_write;
	Sop1(scalar_write, 3, 20, Inline(1));
	slots.push_back(scalar_write);
	Words scalar_read;
	Vop1(scalar_read, 1, 3, 20); // read after the join is safe only if the full VCC branch is uniform
	slots.push_back(scalar_read);
	Words zero;
	Vop1(zero, 1, 2, Inline(0));
	slots.push_back(zero);
	slots.push_back(Mrt0Export(2, 2, 2, 2));
	Words end;
	Sopp(end, 1);
	slots.push_back(end);
	return slots;
}

struct AddtidOp
{
	Words words;
	bool  read = false;
};

enum class AddtidMutation
{
	None,
	AliasPlanes,
	ReadBeforeWrite,
	UnknownM0,
	UnalignedM0,
	AlignedM0Offset,
	HighM0Address,
	ClobberedM0,
	WidenExecBetweenStoreAndRead,
	MixedDs,
	BranchIntoInterval,
	BranchOverStores,
	BranchSkipsM0,
	Wave64PlaneOverlap,
	NoWqmReadBeforeWrite,
	ScalarMaskAsNumber,
};

std::vector<Words> AddtidProgram(AddtidMutation mutation)
{
	if (mutation == AddtidMutation::NoWqmReadBeforeWrite)
	{
		std::vector<Words> slots;
		Words             m0;
		Sop1(m0, 3, kM0Reg, Inline(0));
		slots.push_back(m0);
		slots.push_back(DsAddtidRead(20u, 0u));
		slots.push_back(DsAddtidWrite(10u, 0u));
		slots.push_back(Mrt0Export(20u, 20u, 20u, 20u));
		Words end;
		Sopp(end, 1);
		slots.push_back(end);
		return slots;
	}

	auto slots = Baseline();
	slots[kZero].clear(); // leave the reloaded values live through the MRT0 export
	slots[kExport] = Mrt0Export(20u, 21u, 22u, 22u);
	const uint32_t m0_source = mutation == AddtidMutation::UnknownM0 ? 16u
	                           : mutation == AddtidMutation::UnalignedM0 ? Inline(1)
	                                                                    : Inline(0);
	if (mutation == AddtidMutation::AlignedM0Offset)
	{
		Sop1(slots[kM0], 3, kM0Reg, 255u, 256u);
	} else if (mutation == AddtidMutation::HighM0Address)
	{
		Sop1(slots[kM0], 3, kM0Reg, 255u, 0xfffffffcu);
	} else
	{
		Sop1(slots[kM0], 3, kM0Reg, m0_source); // exact M0=0 for the accepted fixture
	}

	const uint16_t offsets[] = {0u, mutation == AddtidMutation::AliasPlanes       ? 4u
	                                  : mutation == AddtidMutation::Wave64PlaneOverlap ? 0x80u
	                                                                                   : 0x100u,
	                            0x200u};
	std::vector<AddtidOp> operations;
	if (mutation == AddtidMutation::ScalarMaskAsNumber) { operations.push_back({VAddF32FromScalarMask(), false}); }
	for (uint32_t plane = 0; plane < 3u; ++plane)
	{
		if (mutation == AddtidMutation::ScalarMaskAsNumber && plane == 0u) { continue; }
		Words seed;
		Vop1(seed, 1, 10u + plane, Inline(static_cast<int>(plane + 1u)));
		operations.push_back({seed, false});
	}
	if (mutation == AddtidMutation::ReadBeforeWrite)
	{
		operations.push_back({DsAddtidRead(20u, offsets[0]), true});
	}
	for (uint32_t plane = 0; plane < 3u; ++plane)
	{
		const auto write = mutation == AddtidMutation::MixedDs && plane == 0u ? DsWriteB32(30u, 10u, offsets[plane])
		                                                                        : DsAddtidWrite(10u + plane, offsets[plane]);
		operations.push_back({write, false});
	}
	if (mutation == AddtidMutation::ClobberedM0)
	{
		Words clobber;
		Sop1(clobber, 3, kM0Reg, Inline(4));
		operations.push_back({clobber, false});
	}
	for (uint32_t plane = mutation == AddtidMutation::ReadBeforeWrite ? 1u : 0u;
	     plane < 3u; ++plane)
	{
		operations.push_back({DsAddtidRead(20u + plane, offsets[plane]), true});
	}

	if (mutation == AddtidMutation::BranchIntoInterval)
	{
		uint32_t words_before_read = 0;
		for (const auto& operation: operations)
		{
			if (operation.read) { break; }
			words_before_read += static_cast<uint32_t>(operation.words.size());
		}
		Words branch;
		Sopp(branch, 2, words_before_read); // jump directly into the read half, bypassing every store
		operations.insert(operations.begin(), {branch, false});
	}

	if (mutation == AddtidMutation::BranchOverStores)
	{
		auto     first_store = operations.begin();
		uint32_t store_words = 0;
		while (first_store->words[0] >> 26u != 0x36u) { ++first_store; } // DS encoding
		for (auto op = first_store; op != operations.end() && !op->read; ++op) { store_words += static_cast<uint32_t>(op->words.size()); }
		Words branch;
		Sopp(branch, 4, store_words); // s_cbranch_scc0 on a uniform compare: one path skips every store
		first_store = operations.insert(first_store, {branch, false});
		operations.insert(first_store, {Sopc(6, 16, Inline(0)), false}); // s_cmp_eq_u32 s16, 0
	}

	if (mutation == AddtidMutation::WidenExecBetweenStoreAndRead)
	{
		std::vector<Words> stores;
		std::vector<Words> reads;
		for (const auto& operation: operations)
		{
			(operation.read ? reads : stores).push_back(operation.words);
		}
		const uint32_t wait_index = kWait + static_cast<uint32_t>(stores.size());
		slots.insert(slots.begin() + kWqm, stores.begin(), stores.end()); // live EXEC stores precede WQM
		slots.insert(slots.begin() + wait_index + 1u, reads.begin(), reads.end()); // reads run after WQM widens EXEC
	} else
	{
		std::vector<Words> encoded;
		for (const auto& operation: operations) { encoded.push_back(operation.words); }
		slots.insert(slots.begin() + kWait + 1u, encoded.begin(), encoded.end());
	}
	if (mutation == AddtidMutation::BranchSkipsM0)
	{
		Words branch;
		Sopp(branch, 4, static_cast<uint32_t>(slots[kM0].size())); // s_cbranch_scc0 over M0=0: the paths disagree on M0
		slots.insert(slots.begin() + kM0, {Sopc(6, 16, Inline(0)), branch});
	}
	return slots;
}

std::vector<Words> AddtidVinterpProgram(bool restore_origin)
{
	std::vector<Words> slots;
	Words             setup_parameter_m0;
	Sop1(setup_parameter_m0, 3, kM0Reg, 30); // m0 = the caller-provided pixel LDS parameter
	slots.push_back(setup_parameter_m0);
	Words save_parameter_m0;
	Sop1(save_parameter_m0, 3, 0, kM0Reg); // s0 = the original parameter base
	slots.push_back(save_parameter_m0);
	Words reuse_parameter_sgpr;
	Sop1(reuse_parameter_sgpr, 3, 30, Inline(8)); // reusing the incoming parameter SGPR must not erase s0's origin
	slots.push_back(reuse_parameter_sgpr);
	Words set_scratch_m0;
	Sop1(set_scratch_m0, 3, kM0Reg, Inline(0));
	slots.push_back(set_scratch_m0);
	Words save_exec;
	Sop1(save_exec, 4, 12, kExecLo);
	slots.push_back(save_exec);
	Words enter_wqm;
	Sop1(enter_wqm, 10, kExecLo, kExecLo);
	slots.push_back(enter_wqm);
	Words wait;
	Sopp(wait, 12, 0xc07f);
	slots.push_back(wait);
	for (uint32_t plane = 0; plane < 3u; ++plane)
	{
		Words seed;
		Vop1(seed, 1, 10u + plane, Inline(static_cast<int>(plane + 1u)));
		slots.push_back(seed);
	}
	const uint16_t offsets[] = {0u, 0x100u, 0x200u};
	for (uint32_t plane = 0; plane < 3u; ++plane) { slots.push_back(DsAddtidWrite(10u + plane, offsets[plane])); }
	for (uint32_t plane = 0; plane < 3u; ++plane) { slots.push_back(DsAddtidRead(20u + plane, offsets[plane])); }
	Words restore_m0;
	if (restore_origin)
	{
		Sop1(restore_m0, 3, kM0Reg, 0);
	} else
	{
		Sop1(restore_m0, 3, kM0Reg, Inline(4));
	}
	slots.push_back(restore_m0);
	slots.push_back(VInterpP1(23u, 0u, 0u, 0u));
	Words restore_exec;
	Sop1(restore_exec, 4, kExecLo, 12);
	slots.push_back(restore_exec);
	slots.push_back(Mrt0Export(23u, 23u, 23u, 23u));
	Words end;
	Sopp(end, 1);
	slots.push_back(end);
	return slots;
}

std::vector<Words> AddtidEmitterProgram()
{
	std::vector<Words> slots;
	Words             m0;
	Sop1(m0, 3, kM0Reg, Inline(0));
	slots.push_back(m0);
	Words save_exec;
	Sop1(save_exec, 4, 12, kExecLo);
	slots.push_back(save_exec);
	Words enter_wqm;
	Sop1(enter_wqm, 10, kExecLo, kExecLo);
	slots.push_back(enter_wqm);
	Words wait;
	Sopp(wait, 12, 0xc07f);
	slots.push_back(wait);
	for (uint32_t plane = 0; plane < 3u; ++plane)
	{
		Words seed;
		Vop1(seed, 1, 10u + plane, Inline(static_cast<int>(plane + 1u)));
		slots.push_back(seed);
	}
	const uint16_t offsets[] = {0u, 0x100u, 0x200u};
	for (uint32_t plane = 0; plane < 3u; ++plane) { slots.push_back(DsAddtidWrite(10u + plane, offsets[plane])); }
	for (uint32_t plane = 0; plane < 3u; ++plane) { slots.push_back(DsAddtidRead(20u + plane, offsets[plane])); }
	Words restore_exec;
	Sop1(restore_exec, 4, kExecLo, 12);
	slots.push_back(restore_exec);
	slots.push_back(Mrt0Export(20u, 21u, 22u, 22u));
	Words end;
	Sopp(end, 1);
	slots.push_back(end);
	return slots;
}

// The shape a compiler emits under register pressure: the parameter M0 survives a
// uniform join, is saved around a spill store, observed by an interpolation, and a
// uniform loop runs before M0 is pointed at the slot again for the reload.
std::vector<Words> AddtidSpillAcrossControlFlowProgram()
{
	std::vector<Words> slots;
	Words              head;
	Sop1(head, 3, kM0Reg, 30);              // s_mov_b32 m0, s30 (the pixel LDS parameter)
	Sop1(head, 4, 12, kExecLo);             // s_mov_b64 s[12:13], exec
	Sop1(head, 10, kExecLo, kExecLo);       // s_wqm_b64 exec, exec
	Append(head, Sopc(6, 16, Inline(0)));   // s_cmp_eq_u32 s16, 0
	Sopp(head, 4, 1);                       // s_cbranch_scc0 over one instruction: a uniform join
	Vop1(head, 1, 11, Inline(1));           // v_mov_b32 v11, 1
	Vop1(head, 1, 10, Inline(2));           // join: v_mov_b32 v10, 2
	Sop1(head, 3, 0, kM0Reg);               // s_mov_b32 s0, m0
	Sop1(head, 3, kM0Reg, Inline(0));       // s_mov_b32 m0, 0
	Append(head, DsAddtidWrite(10u, 0u));
	Sop1(head, 3, kM0Reg, 0);               // s_mov_b32 m0, s0
	Append(head, VInterpP1(23u, 0u, 0u, 0u));
	const size_t loop = head.size();
	Sop2(head, 0, 21, 21, Inline(1));       // loop: s_add_u32 s21, s21, 1
	Vop2(head, 3, 11, kVgpr + 11, 10);      // v_add_f32 v11, v11, v10
	Append(head, Sopc(0x4, 21, 20));
	Sopp(head, 5, static_cast<uint32_t>(loop - (head.size() + 1u)) & 0xffffu); // s_cbranch_scc1 loop
	Sop1(head, 3, kM0Reg, Inline(0));       // s_mov_b32 m0, 0
	Append(head, DsAddtidRead(20u, 0u));
	Sop1(head, 4, kExecLo, 12);             // s_mov_b64 exec, s[12:13]
	Append(head, Mrt0Export(20u, 23u, 11u, 11u));
	Sopp(head, 1);
	slots.push_back(head);
	return slots;
}

ShaderCode ParseAddtidPixel(const std::vector<Words>& words, uint32_t extra_dwords = 256u)
{
	auto code = ParsePixel(words);
	code.SetPixelLdsAllocation(extra_dwords, 30);
	return code;
}

ShaderCode ParseAddtidPixel(AddtidMutation mutation, uint32_t extra_dwords = 256u)
{
	return ParseAddtidPixel(AddtidProgram(mutation), extra_dwords);
}

} // namespace

TEST(EmulatorFragmentMaskFlow, ProvesStableWqmAddtidPlanesForBothNativeWaveSizes)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = ParseAddtidPixel(AddtidMutation::None);
		const auto& instructions = code.GetInstructions();
		uint32_t writes = 0;
		uint32_t reads = 0;
		uint32_t wqm_count = 0;
		bool     reads_started = false;
		bool     exact_m0_zero = false;
		bool     wqm_seen = false;
		for (uint32_t index = 0; index < instructions.Size(); ++index)
		{
			const auto& inst = instructions.At(index);
			if (inst.dst.type == ShaderOperandType::M0 && inst.src_num == 1 &&
			    inst.src[0].type == ShaderOperandType::IntegerInlineConstant && inst.src[0].constant.i == 0)
			{
				exact_m0_zero = true;
			}
			if (inst.type == ShaderInstructionType::SWqmB64)
			{
				Check(exact_m0_zero, "M0=0 is established before WQM");
				++wqm_count;
				wqm_seen = true;
			}
			if (inst.type == ShaderInstructionType::DsWriteAddtidB32)
			{
				Check(wqm_seen, "the private-slot stores run after WQM starts");
				Check(!reads_started, "all private-slot stores precede their reads");
				++writes;
			}
			if (inst.type == ShaderInstructionType::DsReadAddtidB32)
			{
				reads_started = true;
				++reads;
			}
		}
		Check(exact_m0_zero && wqm_count == 1u && writes == 3u && reads == 3u,
		      "the complete three-slot fixture uses M0=0 inside one stable WQM region");
		const auto& export_inst = instructions.At(Find(code, ShaderInstructionType::Exp));
		Check(export_inst.src_num == 4 && export_inst.src[0].register_id == 20 && export_inst.src[1].register_id == 21 &&
		          export_inst.src[2].register_id == 22 && export_inst.src[3].register_id == 22,
		      "the ADD_TID reloads feed the exported MRT0 components");
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(flow.lane_local && flow.reason == nullptr, "paired private slots preserve lane-local masks");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal,
			      "the proved fragment pattern is native-wave lane local");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, RequiresEnoughDeclaredPixelLdsForTheAddtidSpan)
{
	ASSERT_EXIT(([] {
		Initialize();
		struct Case
		{
			uint32_t extra_dwords;
			bool     admitted;
		};
		const Case cases[] = {{0u, false}, {128u, false}, {256u, true}};
		for (const auto& test: cases)
		{
			const auto code = ParseAddtidPixel(AddtidMutation::None, test.extra_dwords);
			Check(code.GetPixelExtraLdsDwords() == test.extra_dwords && code.GetPixelParameterSgpr() == 30,
			      "the fixture carries the declared PS LDS metadata");
			const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
			Check(flow.lane_local == test.admitted, "the full three-plane span must fit its declared allocation");
			for (uint32_t width: {32u, 64u})
			{
				const auto wave = ShaderAnalyzeNativeWave(code, width);
				if (test.admitted)
				{
					Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal,
					      "the full allocation admits the three-plane proof");
				} else
				{
					Check(wave.refusal_reason != nullptr, "an undersized allocation carries an explicit refusal");
				}
			}
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AcceptsAlignedM0OffsetInsideTheDeclaredPixelLds)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = ParseAddtidPixel(AddtidMutation::AlignedM0Offset);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(flow.lane_local && flow.reason == nullptr,
		      "aligned nonzero M0 keeps all three wave64 spill planes inside the declared allocation");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal,
			      "the bounded aligned M0 offset preserves the proof for either supported native width");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, RestoresThePixelParameterM0BeforeInterpolation)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto restored = ParseAddtidPixel(AddtidVinterpProgram(true));
		Check(restored.GetPixelParameterSgpr() == 30, "the fixture identifies the implicit pixel LDS parameter");
		Check(Find(restored, ShaderInstructionType::VInterpP1F32) < Find(restored, ShaderInstructionType::Exp),
		      "the interpolation follows the ADD_TID spill and restore");
		const auto accepted = ShaderAnalyzeFragmentMaskFlow(restored, 64u);
		Check(accepted.lane_local && accepted.reason == nullptr,
		      "VInterp observes the restored parameter M0 after the private spill");

		const auto clobbered = ParseAddtidPixel(AddtidVinterpProgram(false));
		const auto rejected = ShaderAnalyzeFragmentMaskFlow(clobbered, 64u);
		Check(!rejected.lane_local, "numeric M0 overlapping the private spill is refused before VInterp");
		ExpectNativeRefusal(clobbered, "VInterp with scratch M0 has an explicit native-wave refusal");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, EmitsAndValidatesADeclaredPixelAddtidSpill)
{
	ASSERT_EXIT(([] {
		Initialize();
		ScopedShaderValidation validation;
		Check(Config::ShaderValidationEnabled(), "SPIR-V validation is explicitly enabled at the toolchain boundary");
		const auto code = ParseAddtidPixel(AddtidEmitterProgram());
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(flow.lane_local && flow.reason == nullptr, "the emitter fixture proves stable private spill slots");
		ShaderPixelInputInfo input {};
		input.native_wave = ShaderAnalyzeNativeWave(code, 64u);
		Check(input.native_wave.refusal_reason == nullptr, "the emitter consumes the native-wave proof");
		input.required_subgroup_size = 64u;
		const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
		Check(!source.IsEmpty(), "the fragment ADD_TID program emits GLSL-like SPIR-V source");
		Vector<uint32_t> binary;
		String8          error;
		Check(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty(), error.c_str());
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, RejectsAddtidPatternsWithoutAnExactPrivateSlotProof)
{
	ASSERT_EXIT(([] {
		Initialize();
		struct Case
		{
			AddtidMutation mutation;
			const char*    reason;
		};
		const Case cases[] = {
		    {AddtidMutation::AliasPlanes, "overlapping private slots are refused"},
		    {AddtidMutation::ReadBeforeWrite, "a read before its matching store is refused"},
			{AddtidMutation::UnknownM0, "an unknown M0 base is refused"},
			{AddtidMutation::UnalignedM0, "an unaligned M0 base is refused"},
			{AddtidMutation::HighM0Address, "an aligned high M0 address outside the allocation is refused without wraparound"},
			{AddtidMutation::ClobberedM0, "an M0 clobber inside the interval is refused"},
		    {AddtidMutation::MixedDs, "a mixed shared DS access is refused"},
			{AddtidMutation::BranchIntoInterval, "a branch into the read half is refused"},
			{AddtidMutation::BranchOverStores, "a uniform branch over the stores leaves the reload without one"},
			{AddtidMutation::BranchSkipsM0, "paths that disagree on M0 leave the spill base unknown"},
			{AddtidMutation::Wave64PlaneOverlap, "planes 128 bytes apart still overlap for a wave64 spill"},
		    {AddtidMutation::NoWqmReadBeforeWrite, "a no-WQM read before store is refused despite monotone masks"},
		};
		for (const auto& test: cases)
		{
			const auto code = ParseAddtidPixel(test.mutation);
			const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
			Check(!flow.lane_local && Contains(flow.reason, "memory write"), test.reason);
			ExpectNativeRefusal(code, test.reason);
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ProvesAddtidSlotsAcrossJoinsLoopsAndWidenedExec)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto across = ParseAddtidPixel(AddtidSpillAcrossControlFlowProgram());
		const auto flow   = ShaderAnalyzeFragmentMaskFlow(across, 64u);
		Check(flow.lane_local && flow.reason == nullptr,
		      "M0 provenance survives a join both paths agree on, and the reload follows its store across a loop");
		// A lane outside the store's EXEC reads undefined bytes from guest LDS and from its private array alike.
		const auto widened = ParseAddtidPixel(AddtidMutation::WidenExecBetweenStoreAndRead);
		Check(ShaderAnalyzeFragmentMaskFlow(widened, 64u).lane_local, "a slot stays private to its lane under a wider EXEC");
		for (uint32_t width: {32u, 64u})
		{
			Check(ShaderAnalyzeNativeWave(across, width).proof == ShaderNativeWaveProof::LaneLocal,
			      "the spill across control flow is native-wave lane local");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, RefusesNumericMaskBeforeTheValueCanBeSpilledThroughAddtid)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = ParseAddtidPixel(AddtidMutation::ScalarMaskAsNumber);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(!flow.lane_local && Contains(flow.reason, "mask observed as a number"),
		      "a mask cannot become a numeric VGPR value before the LDS spill");
		ExpectNativeRefusal(code, "a numeric mask value is never native-wave lane local");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AdmitsTheWqmBracketedUniformSelectAsLaneLocal)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = ParsePixel(Baseline());
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(flow.lane_local && flow.reason == nullptr, "every mask observation is a lane bit or mask algebra");
		const auto wqm     = Find(code, ShaderInstructionType::SWqmB64);
		uint32_t closed = 0;
		for (uint32_t index = 0; index < flow.closed_exec_write.size(); ++index) { closed += flow.closed_exec_write[index] ? 1u : 0u; }
		Check(closed == 2u && flow.closed_exec_write[wqm], "exactly the WQM entry and its restore are closed");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ANumericObservationOfTheMaskIsRefused)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = Baseline();
		Sop2(slots[kWait], 0, 0, kExecLo, Inline(1)); // s_add_u32 s0, exec_lo, 1: the lane set as a number
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(!flow.lane_local && Contains(flow.reason, "mask observed as a number"), "refused as a numeric use");
		ExpectNotLaneLocal(code, "a numeric mask observation is never lane local");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// VOP3 v_writelane_b32 / v_readlane_b32: VDST holds the VGPR (write) or the SGPR (read).
void LaneMove(Words& words, uint32_t opcode, uint32_t dst, uint32_t src0, uint32_t lane)
{
	words.push_back(0xd4000000u | (opcode << 16u) | dst);
	words.push_back(src0 | (lane << 9u));
}

// The saved EXEC spilled through two lanes of v3 and read back before the restore,
// as a compiler spills a scalar pair under register pressure.
std::vector<Words> SpilledSaveSlots(bool read_back_as_number)
{
	auto slots = Baseline();
	LaneMove(slots[kSave], 0x361, 3, 12, Inline(1)); // v_writelane_b32 v3, s12, 1
	LaneMove(slots[kSave], 0x361, 3, 13, Inline(2)); // v_writelane_b32 v3, s13, 2
	slots[kRestore].clear();
	LaneMove(slots[kRestore], 0x360, 40, kVgpr + 3, Inline(1)); // v_readlane_b32 s40, v3, 1
	LaneMove(slots[kRestore], 0x360, 41, kVgpr + 3, Inline(2)); // v_readlane_b32 s41, v3, 2
	if (read_back_as_number)
	{
		Vop1(slots[kRestore], 1, 4, 40); // v_mov_b32 v4, s40
	}
	Sop1(slots[kRestore], 4, kExecLo, 40); // s_mov_b64 exec, s[40:41]
	return slots;
}

TEST(EmulatorFragmentMaskFlow, AMaskSpilledThroughAVgprLaneStaysAMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto spilled = ShaderAnalyzeFragmentMaskFlow(ParsePixel(SpilledSaveSlots(false)), 64u);
		if (!spilled.lane_local) { std::fprintf(stderr, "refused: %s\n", spilled.reason); }
		Check(spilled.lane_local && spilled.reason == nullptr, "a static spill moves the saved EXEC like a scalar copy");

		const auto observed = ShaderAnalyzeFragmentMaskFlow(ParsePixel(SpilledSaveSlots(true)), 64u);
		Check(!observed.lane_local && Contains(observed.reason, "mask observed as a number"),
		      "the read-back word is still a mask");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ASelectUnderAMaskDerivedSccIsRefused)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = Baseline();
		slots[kCompare].clear(); // SCC still holds the S_WQM_B64 result: exec != 0
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(!flow.lane_local && Contains(flow.reason, "SCC derived from a mask"), "refused as a lane-dependent choice");
		ExpectNotLaneLocal(code, "a select under a mask-derived SCC is never lane local");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ANumericValueCannotBecomeTheExecutionMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = Baseline();
		slots[kRestore].clear();
		Sop1(slots[kRestore], 3, kExecLo, 20); // s_mov_b32 exec_lo, s20
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(!flow.lane_local && Contains(flow.reason, "EXEC assigned a numeric value"), "refused as a numeric EXEC");
		ExpectNotLaneLocal(code, "a numeric EXEC is never lane local");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// VCC enters undefined (treated as a mask) and the load replaces only its low word with a number. Scalar ALU may
// carry such a half-mask pair like a diverged word, as long as nothing observes the result.
std::vector<Words> MixedPairCarried()
{
	auto slots = Baseline();
	slots[kSelect].clear();
	Sop2(slots[kSelect], 15, kVccLo, 0, kVccLo); // s_and_b64 vcc, s[0:1], vcc
	return slots;
}

TEST(EmulatorFragmentMaskFlow, AdmitsAHalfMaskPairThatScalarAluCarriesButNothingObserves)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = MixedPairCarried();
		slots[kBlend].clear();
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(flow.lane_local && flow.reason == nullptr, "an unobserved half-mask pair is not a mask observation");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AHalfMaskPairCarriedByScalarAluIsStillRefusedWhereObserved)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto lane_bit = ParsePixel(MixedPairCarried()); // v_cndmask reads the carried pair as lane bits
		const auto by_lane  = ShaderAnalyzeFragmentMaskFlow(lane_bit, 64u);
		Check(!by_lane.lane_local && Contains(by_lane.reason, "mask pair partly overwritten by a number"), "a vector read refuses");
		ExpectNotLaneLocal(lane_bit, "a lane read of a half-mask pair is never lane local");
		auto slots = MixedPairCarried();
		slots[kBlend].clear();
		Sop1(slots[kBlend], 4, kExecLo, kVccLo); // s_mov_b64 exec, vcc
		const auto exec    = ParsePixel(slots);
		const auto by_exec = ShaderAnalyzeFragmentMaskFlow(exec, 64u);
		Check(!by_exec.lane_local && Contains(by_exec.reason, "partly overwritten by a number"), "EXEC from it refuses");
		slots = MixedPairCarried();
		slots[kBlend].clear();
		Sop2(slots[kBlend], 11, 20, Inline(1), Inline(0)); // s_cselect_b64 s[20:21], 1, 0: SCC came from the pair
		const auto select    = ParsePixel(slots);
		const auto by_select = ShaderAnalyzeFragmentMaskFlow(select, 64u);
		Check(!by_select.lane_local && Contains(by_select.reason, "SCC derived from a mask"), "its SCC selects nothing");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ADescriptorReusedAsTheSaveAreaIsAMaskOnlyAfterTheSave)
{
	ASSERT_EXIT(([] {
		Initialize();
		// Save first, then load through the very registers that now hold the mask.
		auto slots = Baseline();
		std::swap(slots[kLoad], slots[kSave]);
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(!flow.lane_local && Contains(flow.reason, "mask"), "a mask used as a descriptor is refused");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AWrapperWithoutItsRestoreStaysOutsideAnyClosedRegion)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto slots = Baseline();
		slots[kRestore].clear();
		const auto code = ParsePixel(slots);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(!flow.lane_local && Contains(flow.reason, "helper lanes"), "exporting while still widened is refused");
		ExpectNotLaneLocal(code, "an unrestored EXEC widening is never lane local");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AClobberedSaveAreaOrAnExportInsideBreaksTheWrapper)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto clobbered = Baseline();
		Sop1(clobbered[kWait], 3, 12, 20); // s_mov_b32 s12, s20 after the widening
		const auto clobbered_code = ParsePixel(clobbered);
		const auto clobbered_flow = ShaderAnalyzeFragmentMaskFlow(clobbered_code, 64u);
		Check(!clobbered_flow.lane_local && Contains(clobbered_flow.reason, "partly overwritten"),
		      "a save area half replaced by a number is not a mask");
		ExpectNotLaneLocal(clobbered_code, "an unverified restore is never lane local");

		auto exported = Baseline();
		std::swap(exported[kExport], exported[kRestore]); // export while helper lanes are still active
		const auto exported_code = ParsePixel(exported);
		const auto exported_flow = ShaderAnalyzeFragmentMaskFlow(exported_code, 64u);
		Check(!exported_flow.lane_local && Contains(exported_flow.reason, "helper lanes"),
		      "an export while EXEC includes helper lanes is refused");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AMemoryWriteMakesHelperLanesObservable)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto code = ParsePixel(Baseline());
		// A buffer store before the export: the guest performs it for live lanes only.
		ShaderInstruction store {};
		store.type = ShaderInstructionType::BufferStoreDword;
		store.pc   = code.GetInstructions().At(Find(code, ShaderInstructionType::Exp)).pc;
		code.GetInstructions().InsertAt(Find(code, ShaderInstructionType::Exp), store);
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(!flow.lane_local && Contains(flow.reason, "memory write"), "a memory write is refused");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// A divergent block: lanes selected by a per-lane compare run a scalar load and a vector
// use; the others skip it. `after_join` is appended after the join.
std::vector<Words> DivergentProgram(const Words& after_join)
{
	std::vector<Words> slots;
	Words head;
	Sop1(head, 4, 4, kExecLo);                                        // s_mov_b64 s[4:5], exec
	Append(head, Vopc(0xc5, Inline(0), 0u));                          // v_cmp_ne_u32 vcc, 0, v0 (per lane)
	Sop1(head, 0x24, 2, kVccLo);                                      // s_and_saveexec_b64 s[2:3], vcc
	Sopp(head, 8, 4);                                                 // s_cbranch_execz over the next four dwords
	head.insert(head.end(), {0xf4200508u, 0xfa000004u});              // s_buffer_load_dword s20, s[16:19], 4
	Sopp(head, 12, 0xc07f);                                           // s_waitcnt lgkmcnt(0)
	Vop1(head, 1, 1, 20);                                             // v_mov_b32 v1, s20
	Sop1(head, 4, kExecLo, 2);                                        // join: s_mov_b64 exec, s[2:3]
	Append(head, after_join);
	Vop1(head, 1, 2, Inline(0));                                      // v_mov_b32 v2, 0
	head.insert(head.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(head, 1);
	slots.push_back(head);
	return slots;
}

TEST(EmulatorFragmentMaskFlow, AdmitsDivergentBlocksWhoseScalarsAreNotReadAfterTheJoin)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto program = ParsePixel(DivergentProgram({}));
		const auto flow    = ShaderAnalyzeFragmentMaskFlow(program, 64u);
		Check(flow.lane_local, "a scalar used only inside its divergent block is lane local");
		Check(flow.closed_exec_write[Find(program, ShaderInstructionType::SAndSaveexecB64)], "narrowing by a lane bit is accounted for");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(program, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		// A packed compare after control flow is still this lane's own bit.
		Words compare_after;
		Append(compare_after, Vopc(0xc5, Inline(0), 0u)); // v_cmp_ne_u32 vcc, 0, v0
		const auto after = ParsePixel(DivergentProgram(compare_after));
		const auto wave  = ShaderAnalyzeNativeWave(after, 64u);
		Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "packed compare after a branch is lane local");
		// Redefining the scalar after the join ends its divergence.
		Words reload;
		reload.insert(reload.end(), {0xf4200508u, 0xfa000004u});
		Sopp(reload, 12, 0xc07f);
		Vop1(reload, 1, 3, 20);
		Check(ShaderAnalyzeFragmentMaskFlow(ParsePixel(DivergentProgram(reload)), 64u).lane_local, "a redefined scalar is clean again");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ADivergentBlocksScalarCannotBeReadByAVectorAfterTheJoin)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words leak;
		Vop1(leak, 1, 3, 20); // v_mov_b32 v3, s20: skipped lanes still hold the old s20 on the host
		const auto flow = ShaderAnalyzeFragmentMaskFlow(ParsePixel(DivergentProgram(leak)), 64u);
		Check(!flow.lane_local && Contains(flow.reason, "divergent control flow"), "the divergent scalar is refused");
		ExpectNotLaneLocal(ParsePixel(DivergentProgram(leak)), "a divergent scalar read after the join is never lane local");
		// Carried through scalar ALU it is still divergent, and a scalar load address is a sink too.
		Words carried;
		Sop2(carried, 0, 21, 20, Inline(1));                         // s_add_u32 s21, s20, 1
		carried.insert(carried.end(), {0xf4200508u | (22u << 6u), 21u << 25u});       // s_buffer_load_dword s22, s[16:19], s21
		const auto by_address = ShaderAnalyzeFragmentMaskFlow(ParsePixel(DivergentProgram(carried)), 64u);
		Check(!by_address.lane_local && Contains(by_address.reason, "divergent control flow"), "divergence survives scalar ALU");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, UniformBranchesNeedNoDivergenceRuleAndMaskedSccIsRefused)
{
	ASSERT_EXIT(([] {
		Initialize();
		// SCC from a compare of scalar numbers: every lane takes the same path, so a
		// scalar written in the skipped block may be read afterwards.
		std::vector<Words> uniform(1);
		uniform[0].insert(uniform[0].end(), {0xf4200508u, 0xfa000004u});          // s_buffer_load_dword s20, s[16:19], 4
		Sopp(uniform[0], 12, 0xc07f);
		Append(uniform[0], Sopc(6, 20, Inline(0)));                               // s_cmp_eq_u32 s20, 0
		Sopp(uniform[0], 5, 2);                                                   // s_cbranch_scc1 over the next two dwords
		uniform[0].insert(uniform[0].end(), {0xf4200508u | (21u << 6u), 0xfa000008u}); // s_buffer_load_dword s21, s[16:19], 8
		Vop1(uniform[0], 1, 3, 21);                                               // join: v_mov_b32 v3, s21
		Vop1(uniform[0], 1, 2, Inline(0));
		uniform[0].insert(uniform[0].end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(uniform[0], 1);
		Check(ShaderAnalyzeFragmentMaskFlow(ParsePixel(uniform), 64u).lane_local, "a uniform branch needs no divergence rule");

		// The same branch on the SCC an S_WQM_B64 left behind depends on the lane set.
		std::vector<Words> masked(1);
		Sop1(masked[0], 4, 4, kExecLo);
		Sop1(masked[0], 10, kExecLo, kExecLo);
		Sopp(masked[0], 5, 1);
		Vop1(masked[0], 1, 3, Inline(0));
		Sop1(masked[0], 4, kExecLo, 4);
		Vop1(masked[0], 1, 2, Inline(0));
		masked[0].insert(masked[0].end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(masked[0], 1);
		const auto by_scc = ShaderAnalyzeFragmentMaskFlow(ParsePixel(masked), 64u);
		Check(!by_scc.lane_local && Contains(by_scc.reason, "SCC branch"), "a branch on a mask-derived SCC is refused");

		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, VccBranchRequiresUniformityOfTheWholePair)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto full_pair = ParsePixel(VccBranchWithOneVaryingWord(false));
		const auto uniform = ShaderAnalyzeFragmentMaskFlow(full_pair, 64u);
		Check(uniform.lane_local, "a full-pair uniform compare still supports a uniform VCC branch");

		const auto low_only = ParsePixel(VccBranchWithOneVaryingWord(true));
		const auto flow = ShaderAnalyzeFragmentMaskFlow(low_only, 64u);
		Check(!flow.lane_local && Contains(flow.reason, "divergent control flow"),
		      "a uniform VCC_LO copy cannot promote a varying VCC_HI word at the join");
		Check(ShaderAnalyzeNativeWave(low_only, 64u).proof != ShaderNativeWaveProof::LaneLocal,
		      "wave64 admission sees the varying word in the VCC branch pair");
		// A wave32 VCCZ reflects VCC_LO only, so the varying VCC_HI is an ordinary SGPR there.
		Check(ShaderAnalyzeFragmentMaskFlow(low_only, 32u).lane_local, "wave32 branches on the uniform VCC_LO");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// A wave32 ignores the upper half of VCC (RDNA2 ISA 2.1, 3.x): a compare into VCC leaves VCC_HI, which the
// program uses as an ordinary SGPR across it. A wave64 compare overwrites VCC_HI with lane bits.
std::vector<Words> NumberInVccHiAcrossCompare()
{
	std::vector<Words> slots;
	Words number;
	Sop1(number, 3, kVccLo + 1u, 20); // s_mov_b32 vcc_hi, s20
	slots.push_back(number);
	slots.push_back({0x7c000000u | (194u << 17u) | Inline(3)}); // v_cmp_eq_u32 vcc, 3, v0
	Words read;
	Vop1(read, 1, 3, kVccLo + 1u); // v_mov_b32 v3, vcc_hi
	slots.push_back(read);
	Words blend;
	Vop2(blend, 1, 0, kVgpr + 0, 1); // v_cndmask_b32 v0, v0, v1, vcc
	slots.push_back(blend);
	slots.push_back(Mrt0Export(0, 3, 0, 3));
	Words end;
	Sopp(end, 1);
	slots.push_back(end);
	return slots;
}

TEST(EmulatorFragmentMaskFlow, Wave32CompareKeepsTheNumberInVccHi)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code   = ParsePixel(NumberInVccHiAcrossCompare());
		const auto wave32 = ShaderAnalyzeFragmentMaskFlow(code, 32u);
		Check(wave32.lane_local && wave32.reason == nullptr, "wave32: VCC_HI is still the number after the compare");
		const auto wave64 = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(!wave64.lane_local && Contains(wave64.reason, "mask observed as a number"), "wave64: the compare wrote VCC_HI");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// "Does any lane hold the condition": a per-lane compare into VCC, S_CMP_LG_U64 VCC, 0 and an SCC branch
// over a block that every lane of the wave runs when one does. `consumer` follows the compare; `wqm` runs the
// compare with the helper lanes of each quad enabled and restores EXEC before the export.
std::vector<Words> AnyLaneProgram(const Words& consumer, bool wqm = false)
{
	std::vector<Words> slots(1);
	auto& words = slots[0];
	if (wqm)
	{
		Sop1(words, 4, 6, kExecLo);                              // s_mov_b64 s[6:7], exec
		Sop1(words, 10, kExecLo, kExecLo);                       // s_wqm_b64 exec, exec
	}
	Append(words, Vopc(0xc5, Inline(0), 0u));                    // v_cmp_ne_u32 vcc, 0, v0 (per lane)
	Append(words, Sopc(0x13, kVccLo, Inline(0)));                // s_cmp_lg_u64 vcc, 0
	Append(words, consumer);
	if (wqm) { Sop1(words, 4, kExecLo, 6); }                     // s_mov_b64 exec, s[6:7]
	Vop1(words, 1, 2, Inline(0));                                // v_mov_b32 v2, 0
	words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(words, 1);
	return slots;
}

TEST(EmulatorFragmentMaskFlow, AnAnyLaneTestBranchesLikeTheVccBranch)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words skip;
		Sopp(skip, 4, 3);                                            // s_cbranch_scc0 over the next three dwords
		skip.insert(skip.end(), {0xf4200508u, 0xfa000004u});         // s_buffer_load_dword s20, s[16:19], 4
		Vop1(skip, 1, 1, Inline(1));                                 // v_mov_b32 v1, 1
		const auto flow = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AnyLaneProgram(skip)), 64u);
		Check(flow.lane_local, "a forward branch on an any-lane test opens a divergent region");
		Check(ShaderAnalyzeFragmentMaskFlow(ParsePixel(AnyLaneProgram(skip, true)), 64u).lane_local,
		      "helper bits under WQM may answer the test, as they do for VCCZ");

		Words leak = skip;
		Vop1(leak, 1, 3, 20);                                        // join: v_mov_b32 v3, s20
		const auto leaked = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AnyLaneProgram(leak)), 64u);
		Check(!leaked.lane_local && Contains(leaked.reason, "divergent control flow"), "its region's scalars diverge at the join");

		Words select;
		Sop2(select, 10, 20, Inline(1), Inline(0));                  // s_cselect_b32 s20, 1, 0
		const auto selected = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AnyLaneProgram(select)), 64u);
		Check(!selected.lane_local && Contains(selected.reason, "SCC derived from a mask"), "any other SCC reader is refused");

		Words loop;
		Sopp(loop, 4, 0xfffdu);                                      // s_cbranch_scc0 back to the vector compare
		const auto looped = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AnyLaneProgram(loop)), 64u);
		Check(!looped.lane_local && Contains(looped.reason, "loop branch on an any-lane test"), "a back edge is refused");

		Words overwritten;
		Append(overwritten, Sopc(6, 20, Inline(0)));                 // s_cmp_eq_u32 s20, 0 replaces the test
		Sop1(overwritten, 4, 4, kVccLo);                             // s_mov_b64 s[4:5], vcc
		Sop2(overwritten, 15, 4, 4, Inline(1));                      // s_and_b64 s[4:5], s[4:5], 1: SCC is a mask again
		Append(overwritten, skip);
		const auto replaced = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AnyLaneProgram(overwritten)), 64u);
		Check(!replaced.lane_local && Contains(replaced.reason, "SCC branch"), "only the last SCC writer counts");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

std::vector<Words> AsSlots(const Words& words)
{
	return {words};
}

// A pixel loop as the guest compiler emits it for "iterate while a per-lane condition
// holds": lanes leave by narrowing EXEC, the wave leaves when none is left, EXEC comes
// back from its saved copy after the exit.
//   save; head: v_cmpx; s_cbranch_execz exit; body; s_branch head; exit: restore; after; export
Words LoopProgram(const Words& body, const Words& after)
{
	Words words;
	Sop1(words, 4, 4, kExecLo);                                  // s_mov_b64 s[4:5], exec
	const size_t head = words.size();
	Append(words, Vopc(0xd5, Inline(0), 0u));                    // v_cmpx_ne_u32 0, v0: lanes drop out
	const size_t test = words.size();
	Sopp(words, 8, 0);                                           // s_cbranch_execz exit (patched below)
	Append(words, body);
	const size_t back = words.size();
	Sopp(words, 2, static_cast<uint32_t>(head - (back + 1u)) & 0xffffu); // s_branch head
	words[test] |= static_cast<uint32_t>(words.size() - (test + 1u));
	Sop1(words, 4, kExecLo, 4);                                  // exit: s_mov_b64 exec, s[4:5]
	Append(words, after);
	Vop1(words, 1, 2, Inline(0));
	words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(words, 1);
	return words;
}

// The bottom-tested form: the body runs once per iteration and S_CBRANCH_EXECNZ repeats it.
Words BottomTestedLoop(const Words& body, const Words& after)
{
	Words words;
	Sop1(words, 4, 4, kExecLo);                                  // s_mov_b64 s[4:5], exec
	const size_t head = words.size();
	Append(words, Vopc(0xd5, Inline(0), 0u));                    // v_cmpx_ne_u32 0, v0
	Append(words, body);
	const size_t test = words.size();
	Sopp(words, 9, static_cast<uint32_t>(head - (test + 1u)) & 0xffffu); // s_cbranch_execnz head
	Sop1(words, 4, kExecLo, 4);                                  // s_mov_b64 exec, s[4:5]
	Append(words, after);
	Vop1(words, 1, 2, Inline(0));
	words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(words, 1);
	return words;
}

Words ScalarLoadOfS20()
{
	Words words;
	words.insert(words.end(), {0xf4200508u, 0xfa000004u});       // s_buffer_load_dword s20, s[16:19], 4
	Sopp(words, 12, 0xc07f);                                     // s_waitcnt lgkmcnt(0)
	return words;
}

ShaderFragmentMaskFlow FlowOf(const Words& words)
{
	return ShaderAnalyzeFragmentMaskFlow(ParsePixel(AsSlots(words)), 64u);
}

// A number spilled to lane 1 of v3, read back into s21 and used by a vector.
Words NumberThroughSpillSlot()
{
	Words words = ScalarLoadOfS20();
	LaneMove(words, 0x361, 3, 20, Inline(1));        // v_writelane_b32 v3, s20, 1
	LaneMove(words, 0x360, 21, kVgpr + 3, Inline(1)); // v_readlane_b32 s21, v3, 1
	Vop1(words, 1, 1, 21);                            // v_mov_b32 v1, s21
	return words;
}

// The saved EXEC spilled to the same slot: a compiler reuses a slot once its word is dead.
Words SavedExecThroughSpillSlot()
{
	Words words;
	Sop1(words, 4, 12, kExecLo);                      // s_mov_b64 s[12:13], exec
	LaneMove(words, 0x361, 3, 12, Inline(1));         // v_writelane_b32 v3, s12, 1
	LaneMove(words, 0x361, 3, 13, Inline(2));         // v_writelane_b32 v3, s13, 2
	LaneMove(words, 0x360, 40, kVgpr + 3, Inline(1)); // v_readlane_b32 s40, v3, 1
	LaneMove(words, 0x360, 41, kVgpr + 3, Inline(2)); // v_readlane_b32 s41, v3, 2
	Sop1(words, 4, kExecLo, 40);                      // s_mov_b64 exec, s[40:41]
	return words;
}

TEST(EmulatorFragmentMaskFlow, ASpillSlotCarriesTheWordLastSpilledOnEachPath)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words reused = NumberThroughSpillSlot();
		Append(reused, SavedExecThroughSpillSlot());
		Vop1(reused, 1, 2, Inline(0)); // v_mov_b32 v2, 0
		reused.insert(reused.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(reused, 1);
		const auto later_mask = FlowOf(reused);
		if (!later_mask.lane_local) { std::fprintf(stderr, "refused: %s\n", later_mask.reason); }
		Check(later_mask.lane_local, "a mask spilled after the number was read does not reach that read");

		// Through the back edge the mask reaches the read again.
		Words carried;
		LaneMove(carried, 0x360, 21, kVgpr + 3, Inline(1)); // v_readlane_b32 s21, v3, 1
		Vop1(carried, 1, 1, 21);                             // v_mov_b32 v1, s21
		Sop1(carried, 4, 12, kExecLo);                       // s_mov_b64 s[12:13], exec
		LaneMove(carried, 0x361, 3, 12, Inline(1));          // v_writelane_b32 v3, s12, 1
		Words looped = ScalarLoadOfS20();
		LaneMove(looped, 0x361, 3, 20, Inline(1));           // v_writelane_b32 v3, s20, 1
		Append(looped, LoopProgram(carried, {}));
		const auto loop_carried = FlowOf(looped);
		Check(!loop_carried.lane_local && Contains(loop_carried.reason, "mask observed as a number"),
		      "a mask spilled later in a loop body reaches the read at the head");

		// A slot written under divergent control flow differs between lanes after the join.
		Words region = ScalarLoadOfS20();
		LaneMove(region, 0x361, 3, 20, Inline(1));           // v_writelane_b32 v3, s20, 1
		Append(region, Vopc(0xc5, Inline(0), 0u));           // v_cmp_ne_u32 vcc, 0, v0 (per lane)
		Sop1(region, 0x24, 2, kVccLo);                       // s_and_saveexec_b64 s[2:3], vcc
		Sopp(region, 8, 5);                                  // s_cbranch_execz over the next five dwords
		region.insert(region.end(), {0xf4200508u | (22u << 6u), 0xfa000008u}); // s_buffer_load_dword s22, s[16:19], 8
		Sopp(region, 12, 0xc07f);                            // s_waitcnt lgkmcnt(0)
		LaneMove(region, 0x361, 3, 22, Inline(1));           // v_writelane_b32 v3, s22, 1
		Sop1(region, 4, kExecLo, 2);                         // join: s_mov_b64 exec, s[2:3]
		LaneMove(region, 0x360, 21, kVgpr + 3, Inline(1));   // v_readlane_b32 s21, v3, 1
		Vop1(region, 1, 1, 21);                              // v_mov_b32 v1, s21
		Vop1(region, 1, 2, Inline(0));                       // v_mov_b32 v2, 0
		region.insert(region.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(region, 1);
		const auto divergent = FlowOf(region);
		Check(!divergent.lane_local && Contains(divergent.reason, "divergent control flow"),
		      "a slot spilled in a divergent block is refused after the join");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AdmitsALoopWhoseLanesLeaveByNarrowingTheMask)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words body;
		Vop1(body, 1, 1, 256u + 0u);                             // v_mov_b32 v1, v0
		for (const auto& words: {LoopProgram(body, {}), BottomTestedLoop(body, {})})
		{
			const auto code = ParsePixel(AsSlots(words));
			const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
			Check(flow.lane_local && flow.reason == nullptr, "narrowing EXEC inside a loop is mask algebra");
			Check(flow.closed_exec_write[Find(code, ShaderInstructionType::VCmpxNeU32)], "the narrowing compare is accounted for");
			for (uint32_t width: {32u, 64u})
			{
				const auto wave = ShaderAnalyzeNativeWave(code, width);
				Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
			}
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AScalarWrittenInADivergentLoopIsDivergentAfterTheExit)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words inside = ScalarLoadOfS20();
		Vop1(inside, 1, 1, 20);                                  // v_mov_b32 v1, s20: a live lane sees its own iterations
		Check(FlowOf(LoopProgram(inside, {})).lane_local, "a scalar used only inside the loop is lane local");
		Check(FlowOf(BottomTestedLoop(inside, {})).lane_local, "so is the bottom-tested form");

		// Lanes that left early ran a different number of iterations on the host than on the guest.
		Words leak;
		Vop1(leak, 1, 3, 20);                                    // v_mov_b32 v3, s20 after the exit
		for (const auto& words: {LoopProgram(inside, leak), BottomTestedLoop(inside, leak)})
		{
			const auto flow = FlowOf(words);
			Check(!flow.lane_local && Contains(flow.reason, "divergent control flow"), "the loop's scalar is divergent after the exit");
		}
		// Redefined after the exit it is clean again.
		Words reload = ScalarLoadOfS20();
		Vop1(reload, 1, 3, 20);
		Check(FlowOf(LoopProgram(inside, reload)).lane_local, "a redefined scalar is clean again");

		// A counter that advances per iteration is coherent for the lanes still running.
		Words counter;
		Sop2(counter, 0, 21, 21, Inline(1));                     // s_add_u32 s21, s21, 1
		Vop1(counter, 1, 1, 21);                                 // v_mov_b32 v1, s21
		Check(FlowOf(LoopProgram(counter, {})).lane_local, "a loop-carried scalar is read by the lanes that produced it");
		Words after_counter;
		Vop1(after_counter, 1, 3, 21);
		const auto flow = FlowOf(LoopProgram(counter, after_counter));
		Check(!flow.lane_local && Contains(flow.reason, "divergent control flow"), "the counter is divergent after the exit");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AdmitsThePlainWqmBracketWhenTheWholeProgramPassAlreadyAcceptsIt)
{
	ASSERT_EXIT(([] {
		Initialize();
		// s_mov_b64 vcc, exec; s_wqm_b64 exec, exec; <derivative work>; s_mov_b64 exec, vcc; export.
		// No SCC, no select: the whole-program pass accepts it without the flow proof, and the
		// widening write must still be accounted for by the flow proof.
		Words words;
		Sop1(words, 4, kVccLo, kExecLo);
		Sop1(words, 10, kExecLo, kExecLo);
		Vop1(words, 1, 1, 256u + 0u);
		Sop1(words, 4, kExecLo, kVccLo);
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
		const auto code = ParsePixel(AsSlots(words));
		Check(ShaderAnalyzeFragmentMaskFlow(code, 64u).lane_local, "the bracket is a closed widen/restore of the entry mask");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		// The restore is what makes the export narrow: without it the export sees helper lanes.
		Words open_bracket;
		Sop1(open_bracket, 4, kVccLo, kExecLo);
		Sop1(open_bracket, 10, kExecLo, kExecLo);
		Vop1(open_bracket, 1, 2, Inline(0));
		open_bracket.insert(open_bracket.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(open_bracket, 1);
		const auto open = ParsePixel(AsSlots(open_bracket));
		Check(!ShaderAnalyzeFragmentMaskFlow(open, 64u).lane_local, "an export under the widened mask is refused");
		ExpectNotLaneLocal(open, "and so is the stage");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, AUniformLoopNeedsNoDivergenceRule)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words words = ScalarLoadOfS20();                         // the trip count is a scalar number
		const size_t head = words.size();
		Sop2(words, 0, 21, 21, Inline(1));                       // head: s_add_u32 s21, s21, 1
		Append(words, Sopc(0x4, 21, 20));                        // s_cmp_lg_u32 s21, s20
		Sopp(words, 5, static_cast<uint32_t>(head - (words.size() + 1u)) & 0xffffu); // s_cbranch_scc1 head
		Vop1(words, 1, 3, 21);                                   // the counter is the same on every lane
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
		Check(FlowOf(words).lane_local, "every lane runs the same iterations, so the counter needs no divergence rule");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ABackEdgeCarriesWhatTheLoopBodyDoesToTheLoopHead)
{
	ASSERT_EXIT(([] {
		Initialize();
		// Iteration one reads s20 as a number; iteration two reads the mask the body saved into it.
		Words words = ScalarLoadOfS20();
		const size_t head = words.size();
		Vop1(words, 1, 1, 20);                                   // head: v_mov_b32 v1, s20
		Sop1(words, 4, 20, kExecLo);                             // s_mov_b64 s[20:21], exec
		Append(words, Sopc(0x6, 22, Inline(0)));                 // s_cmp_eq_u32 s22, 0 (a clean number)
		Sopp(words, 5, static_cast<uint32_t>(head - (words.size() + 1u)) & 0xffffu); // s_cbranch_scc1 head
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
		const auto flow = FlowOf(words);
		Check(!flow.lane_local && Contains(flow.reason, "mask observed as a number"), "the second iteration observes the mask");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, ALoopOnAMaskDerivedSccIsRefusedButADerivativeFetchInsideIsNot)
{
	ASSERT_EXIT(([] {
		Initialize();
		Words masked;
		Sop1(masked, 4, 4, kExecLo);
		Sop1(masked, 10, kExecLo, kExecLo);                      // s_wqm_b64 leaves SCC = "any lane"
		const size_t head = masked.size();
		Vop1(masked, 1, 3, Inline(0));
		Sopp(masked, 5, static_cast<uint32_t>(head - (masked.size() + 1u)) & 0xffffu); // s_cbranch_scc1 head
		Sop1(masked, 4, kExecLo, 4);
		Vop1(masked, 1, 2, Inline(0));
		masked.insert(masked.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(masked, 1);
		const auto by_scc = FlowOf(masked);
		Check(!by_scc.lane_local && Contains(by_scc.reason, "SCC branch"), "the trip count would depend on the lane set");

		Words body;
		Vop1(body, 1, 1, 256u + 0u);
		// The bottom-tested loop opens no branch region: only the loop itself is divergent. Its lane-dependent
		// branches take their quad's vote, so the quad runs every iteration as one.
		for (const auto& words: {LoopProgram(body, {}), BottomTestedLoop(body, {})})
		{
			auto inside = ParsePixel(AsSlots(words));
			ShaderInstruction sample {};
			sample.type = ShaderInstructionType::ImageSample;
			const auto mov = Find(inside, ShaderInstructionType::VMovB32);
			sample.pc = inside.GetInstructions().At(mov).pc - 1u;
			inside.GetInstructions().InsertAt(mov, sample);
			const auto derivative = ShaderAnalyzeFragmentMaskFlow(inside, 64u);
			Check(derivative.lane_local, "implicit derivatives inside a divergent loop see their whole quad");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}


// Discard: widen to quads, kill lanes by a per-lane compare, early-out to an inert tail
// when no lane survives, re-widen from the survivors, narrow again before the export.
Words DiscardProgram(bool inert_tail, bool restore_before_export, bool wide_survivors)
{
	Words words;
	Sop1(words, 4, 12, kExecLo);                                 // s_mov_b64 s[12:13], exec (live lanes)
	Sop1(words, 10, kExecLo, kExecLo);                           // s_wqm_b64 exec, exec
	Append(words, Vopc(0xc5, Inline(0), 0u));                    // v_cmp_ne_u32 vcc, 0, v0 (kill condition)
	if (wide_survivors)
	{
		Sop1(words, 8, 12, kVccLo);                              // s_not_b64 s[12:13], vcc: a complement includes helpers
	} else
	{
		Sop2(words, 0x15, 12, 12, kVccLo);                       // s_andn2_b64 s[12:13], s[12:13], vcc
	}
	Sopp(words, 4, restore_before_export ? 7 : 6);               // s_cbranch_scc0 to the tail
	Sop1(words, 10, kExecLo, 12);                                // s_wqm_b64 exec, s[12:13]
	Vop1(words, 1, 1, 256u + 0u);                                // v_mov_b32 v1, v0 (derivative work)
	if (restore_before_export) { Sop1(words, 4, kExecLo, 12); }  // s_mov_b64 exec, s[12:13]
	Vop1(words, 1, 2, Inline(0));
	words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(words, 1);
	if (inert_tail)
	{
		Sop1(words, 4, kExecLo, Inline(0));                      // s_mov_b64 exec, 0
		words.insert(words.end(), {0xf8001c00u, 0u});            // exp null mrt0, vm, done
		Sopp(words, 1);
	} else
	{
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
	}
	return words;
}

TEST(EmulatorFragmentMaskFlow, AdmitsTheDiscardPatternWithWideningAnInertTailAndANarrowExport)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = ParsePixel(AsSlots(DiscardProgram(true, true, false)));
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(flow.lane_local, "kill, early-out and re-widening are lane local");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// A depth/stencil-only pass kills lanes with the valid-mask export of the dedicated null target
// (`exp null, vm, done`): survivors are exported as EXEC, and a wave with none skips to a tail that
// exports an empty mask. Both tails kill the same lanes, so the wave-level shortcut is unobservable.
Words NullTargetKillProgram(bool exporting_tail)
{
	Words words;
	Append(words, Vopc(0xc5, Inline(0), 0u));                    // v_cmp_ne_u32 vcc, 0, v0 (kill condition)
	Sop2(words, 0x15, kVccLo, kExecLo, kVccLo);                  // s_andn2_b64 vcc, exec, vcc (survivors)
	Sopp(words, 4, 4);                                           // s_cbranch_scc0 to the tail
	Sop1(words, 4, kExecLo, kVccLo);                             // s_mov_b64 exec, vcc
	words.insert(words.end(), {0xf8001890u, 0u});                // exp null, vm, done
	Sopp(words, 1);
	Sop1(words, 4, kExecLo, Inline(0));                          // tail: s_mov_b64 exec, 0
	if (exporting_tail)
	{
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)}); // exp mrt0 ... done
	} else
	{
		words.insert(words.end(), {0xf8001890u, 0u});            // exp null, vm, done
	}
	Sopp(words, 1);
	return words;
}

TEST(EmulatorFragmentMaskFlow, AdmitsTheNullTargetKillPassWithItsInertTail)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto code = ParsePixel(AsSlots(NullTargetKillProgram(false)));
		Check(Find(code, ShaderInstructionType::Exp) < code.GetInstructions().Size(), "the program has its exports");
		Check(code.GetInstructions().At(Find(code, ShaderInstructionType::Exp)).format == ShaderInstructionFormat::NullVmDone,
		      "the valid-mask export decodes as the dedicated null target");
		const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
		Check(flow.lane_local, "survivors exported as EXEC and an empty-mask tail are lane local");
		for (uint32_t width: {32u, 64u})
		{
			const auto wave = ShaderAnalyzeNativeWave(code, width);
			Check(wave.refusal_reason == nullptr && wave.proof == ShaderNativeWaveProof::LaneLocal, "pixel stage is lane local");
		}
		// A tail that exports colour is not inert: skipping to it changes what the wave writes.
		const auto exporting = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AsSlots(NullTargetKillProgram(true))), 64u);
		Check(!exporting.lane_local && Contains(exporting.reason, "SCC branch on a mask"), "a tail with a colour export is observable");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, TheDiscardPatternNeedsItsInertTailItsNarrowingAndNarrowSurvivors)
{
	ASSERT_EXIT(([] {
		Initialize();
		const auto refused = [](const Words& words, const char* reason, const char* why) {
			const auto code = ParsePixel(AsSlots(words));
			const auto flow = ShaderAnalyzeFragmentMaskFlow(code, 64u);
			Check(!flow.lane_local && Contains(flow.reason, reason), why);
			ExpectNotLaneLocal(code, why);
		};
		refused(DiscardProgram(false, true, false), "SCC branch on a mask", "an early-out to a tail that exports is not unobservable");
		refused(DiscardProgram(true, false, false), "helper lanes", "exporting while widened");
		refused(DiscardProgram(true, true, true), "SCC branch on a mask", "survivors that may include helpers do not prove an empty wave");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// A kill inside a divergent block: lanes selected by a per-lane compare test a second condition and drop out of
// the live mask, ending in the inert tail once none survives. `kill_op` is the SOP2 opcode combining the live mask
// with that compare: S_ANDN2_B64 (0x15) clears only lanes inside the block, S_AND_B64 (0x0f) clears every other lane.
Words DivergentKillProgram(uint32_t kill_op)
{
	Words words;
	Sop1(words, 4, 12, kExecLo);                       // s_mov_b64 s[12:13], exec (live lanes)
	Sop1(words, 4, 14, kExecLo);                       // s_mov_b64 s[14:15], exec
	Append(words, Vopc(0xd5, Inline(0), 0u));          // v_cmpx_ne_u32 0, v0: the block's lanes
	Sopp(words, 8, 3);                                 // s_cbranch_execz over the block
	Append(words, Vopc(0xc5, Inline(1), 1u));          // v_cmp_ne_u32 vcc, 1, v1 (kill condition, block lanes only)
	Sop2(words, kill_op, 12, 12, kVccLo);              // s_andn2_b64 / s_and_b64 s[12:13], s[12:13], vcc
	Sopp(words, 4, 6);                                 // s_cbranch_scc0 to the inert tail
	Sop1(words, 4, kExecLo, 14);                       // join: s_mov_b64 exec, s[14:15]
	Vop1(words, 1, 2, Inline(0));
	Sop1(words, 4, kExecLo, 12);                       // s_mov_b64 exec, s[12:13]: the survivors export
	words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
	Sopp(words, 1);
	Sop1(words, 4, kExecLo, Inline(0));                // s_mov_b64 exec, 0
	words.insert(words.end(), {0xf8001c00u, 0u});      // exp null mrt0, vm, done
	Sopp(words, 1);
	return words;
}

TEST(EmulatorFragmentMaskFlow, AKillInsideADivergentBlockKeepsEveryOtherLanesLiveBit)
{
	ASSERT_EXIT(([] {
		Initialize();
		// The host skips the block for a subgroup with no lane in it, the guest runs it for the wave; ANDN2 with a
		// compare evaluated inside clears no bit outside, so the live mask is the same per lane either way.
		const auto kept = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AsSlots(DivergentKillProgram(0x15))), 64u);
		Check(kept.lane_local, "a kill confined to the block's lanes");
		// AND with the same compare clears every lane outside the block when the block runs: a diverged mask.
		const auto cleared = ShaderAnalyzeFragmentMaskFlow(ParsePixel(AsSlots(DivergentKillProgram(0x0f))), 64u);
		Check(!cleared.lane_local, "a mask the block may clear outside its lanes is diverged after the join");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, DerivativeFetchesAreAdmittedInsideQuadUniformBlocks)
{
	ASSERT_EXIT(([] {
		Initialize();
		auto outside = ParsePixel(DivergentProgram({}));
		auto inside  = ParsePixel(DivergentProgram({}));
		ShaderInstruction sample {};
		sample.type = ShaderInstructionType::ImageSample;
		sample.pc = inside.GetInstructions().At(Find(inside, ShaderInstructionType::VMovB32)).pc + 1u;
		inside.GetInstructions().InsertAt(Find(inside, ShaderInstructionType::VMovB32), sample);
		// The EXEC branch around the block is emitted quad-uniform, so the fetch sees its whole quad.
		Check(ShaderAnalyzeFragmentMaskFlow(inside, 64u).lane_local, "implicit derivatives inside a quad-uniform block");
		sample.pc = outside.GetInstructions().At(Find(outside, ShaderInstructionType::Exp)).pc + 1u;
		outside.GetInstructions().InsertAt(Find(outside, ShaderInstructionType::Exp), sample);
		Check(ShaderAnalyzeFragmentMaskFlow(outside, 64u).lane_local, "the same fetch outside any divergent block is fine");
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorFragmentMaskFlow, PixelExecBranchesTakeTheVoteOfARealFragmentOfTheQuad)
{
	ASSERT_EXIT(([] {
		Initialize();
		// s_mov_b64 s[4:5], exec; v_cmp_ne_u32 vcc, 0, v0; s_and_saveexec_b64 s[2:3], vcc; s_cbranch_execz over one
		// dword; v_mov_b32 v1, 0; s_mov_b64 exec, s[2:3]; v_mov_b32 v2, 0; export; s_endpgm.
		Words words;
		Sop1(words, 4, 4, kExecLo);
		Append(words, Vopc(0xc5, Inline(0), 0u));
		Sop1(words, 0x24, 2, kVccLo);
		Sopp(words, 8, 1);
		Vop1(words, 1, 1, Inline(0));
		Sop1(words, 4, kExecLo, 2);
		Vop1(words, 1, 2, Inline(0));
		words.insert(words.end(), {0xf800180fu, 2u | (2u << 8u) | (2u << 16u) | (2u << 24u)});
		Sopp(words, 1);
		const auto           code = ParsePixel(std::vector<Words> {words});
		ShaderPixelInputInfo input {};
		const auto           source = SpirvGenerateSource(code, nullptr, &input, nullptr);
		Check(!source.IsEmpty(), "complete module");
		// The packed-EXEC vote goes to cc_vote; every lane, helpers included, then reads cc_any from a real
		// fragment of its quad, so the quad takes the branch as one.
		Check(source.FindIndex("%cc_vote_") != Core::STRING8_INVALID_INDEX &&
		          source.FindIndex("_helper = OpLoad %bool %gl_HelperInvocation") != Core::STRING8_INVALID_INDEX &&
		          source.FindIndex("OpGroupNonUniformQuadBroadcast %bool %uint_3 %cc_vote_") != Core::STRING8_INVALID_INDEX,
		      "quad-uniform branch vote");
		Vector<uint32_t> binary;
		String8          error;
		Check(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty(), error.c_str());
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}



UT_END();
