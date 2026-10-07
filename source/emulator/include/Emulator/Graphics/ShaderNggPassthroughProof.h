#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_PASSTHROUGH_PROOF_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_PASSTHROUGH_PROOF_H_

#include "Emulator/Graphics/Shader.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

constexpr uint32_t ShaderNggPassthroughInstructionLimit = 1024;
constexpr uint32_t ShaderNggPassthroughNoInstruction    = UINT32_MAX;

struct ShaderNggPassthroughCounts
{
	uint32_t guest_wave_size    = 0;
	uint32_t es_vertex_count    = 0;
	uint32_t gs_primitive_count = 0;
};

// Conservative dependencies on INITIAL scalar words: SGPR 0..103, VCC_LO,
// VCC_HI, EXEC_LO, EXEC_HI, M0, SCC, in that order. A dependency may remain
// after an operation makes its value known; it is not a liveness certificate.
using ShaderNggScalarDependencies = std::bitset<110>;

struct ShaderNggKnownBits
{
	// Only bits selected by known_mask have a value. Zero in an unknown bit
	// is storage normalization, NEVER a representative runtime value/seed.
	uint64_t value      = 0;
	uint64_t known_mask = 0;
	ShaderNggScalarDependencies initial_dependencies;
};

enum class ShaderNggPassthroughRejection
{
	None,
	InvalidWaveSize,
	InvalidCounts,
	UnsupportedStage,
	InstructionLimit,
	ControlFlow,
	MalformedInstruction,
	UnsupportedInstruction,
	UnsupportedScalarOperand,
	UnsupportedControl,
	UnknownAllocation,
	AllocationMismatch,
	AllocationSequence,
	UnknownExec,
	ExecMaskMismatch,
	ExportDependency,
	PrimitiveSourceModified,
	InvalidExport,
	DuplicateExport,
	UndefinedVectorInput,
	ScalarVectorInput,
	PrimitiveVectorInput,
	MissingEnd,
	MissingPrimitive,
	MissingPosition,
};

enum class ShaderNggPassthroughStepKind
{
	Scalar,
	SchedulingHint,
	Wait,
	AllocationRequest,
	PrimitiveForward,
	VertexAlu,
	PositionExport,
	LayerExport,
	ParameterExport,
	End,
};

struct ShaderNggPassthroughStep
{
	uint32_t instruction_index = 0;
	uint32_t pc                = 0;
	ShaderNggPassthroughStepKind kind = ShaderNggPassthroughStepKind::Scalar;
	// Source slots correspond to the original instruction, except that an
	// AllocationRequest records its implicit M0 payload in slot 0. Only slots
	// selected by scalar_source_mask are meaningful (including unknown values).
	std::array<ShaderNggKnownBits, 4> scalar_sources {};
	uint8_t scalar_source_mask = 0;
	ShaderNggKnownBits scalar_result;
	ShaderNggKnownBits exec_before;
	ShaderNggKnownBits exec_after;
	ShaderNggKnownBits scc_after;
	bool     active_mask_known = false;
	uint64_t active_mask       = 0;
	bool     retain           = false;
};

struct ShaderNggPassthroughProof
{
	bool proven = false;
	ShaderNggPassthroughCounts counts;
	ShaderNggPassthroughRejection rejection = ShaderNggPassthroughRejection::None;
	uint32_t rejection_pc    = ShaderNggPassthroughNoInstruction;
	uint32_t rejection_index = ShaderNggPassthroughNoInstruction;
	Kyty::Core::String8 reason;
	uint32_t instruction_count = 0;
	uint64_t vertex_mask       = 0;
	uint64_t primitive_mask    = 0;
	uint32_t allocation_payload = 0;
	uint32_t allocation_index = ShaderNggPassthroughNoInstruction;
	uint32_t primitive_export_index = ShaderNggPassthroughNoInstruction;
	uint32_t position_export_index  = ShaderNggPassthroughNoInstruction;
	uint32_t layer_export_index     = ShaderNggPassthroughNoInstruction;
	uint32_t parameter_mask = 0;
	bool primitive_forwarding_proved = false;
	bool independent_vertex_transforms_proved = false;
	// POS1's enabled Z word is defined. Interpreting that word as a layer still
	// requires the renderer's position-format/output-control contract.
	bool requires_layer_output_contract = false;
	// Prologue mode only (ShaderAnalyzeNggPassthroughPrologue). The exact scalar,
	// allocation and primitive-forwarding proof holds through instruction
	// prologue_end_index, the write after which EXEC equals the vertex-count mask.
	// `proven` stays false: the remainder of the program was not analysed here.
	bool prologue_proven = false;
	uint32_t prologue_end_index = ShaderNggPassthroughNoInstruction;
	// Scalar words (same order as ShaderNggScalarDependencies) whose value at the
	// end of the prologue depends on a wave-level initial word: SGPR 0..7, VCC, EXEC,
	// M0 or SCC. They differ between launches, so the rest of the program may not
	// read them as data before redefining them.
	ShaderNggScalarDependencies wave_dependent_scalars;
	std::bitset<256> required_initial_vgprs;
	ShaderNggScalarDependencies scalar_dependencies;
	// Both vectors are bounded by ShaderNggPassthroughInstructionLimit. On a
	// refusal they describe only a diagnostic prefix and MUST NOT be consumed.
	std::vector<ShaderNggPassthroughStep> steps;
	std::vector<uint32_t> retained_instruction_indices;
};

// Pure, count-specific conditional equivalence proof over a COMPLETE parsed
// Vertex ShaderCode (including its real terminator). This is neither a renderer
// launch/topology proof nor native-subgroup admission, and selects no physical
// lane mapping. The caller must establish the actual NGG-passthrough launch,
// input identities, counts, program completeness/generation and output modes.
//
// Initial knowledge is ONLY s3[15:0] = {GSPrimCount, ESVertCount}; all other
// scalar/EXEC bits are unknown. v0 is the original packed primitive on the
// primitive lanes; v5/v8 are symbolic vertex/instance IDs on the vertex lanes.
// No scalar representative or s3 seed is produced. The retained instructions
// read only defined per-vertex VGPRs, encoded constants and scalar words derived
// only from user data (never from SGPR 0..7, VCC, EXEC, M0 or SCC), and run with
// every ES vertex lane enabled; results on other enabled lanes are never read.
//
// A future compiler must retain those original ALU/export instructions (and
// their floating-point modes), and replace allocation/EXP PRIM with PROVED
// renderer primitive assembly, not omit the primitive operation. Scalar steps
// and waits are proof evidence, not a standalone executable residual program.
// Their removal is conditional on that missing renderer contract and on the
// absence of any external observation of terminal scalar state. No claim about
// host floating-point accuracy or a domain of other counts follows from proven.
[[nodiscard]] ShaderNggPassthroughProof ShaderAnalyzeNggPassthrough(const ShaderCode& code,
	                                                              const ShaderNggPassthroughCounts& counts);

// The same proof, stopped at the end of the wave-count prologue: it must contain the
// exact GS allocation request, the unmodified primitive forward, and the write that
// leaves EXEC equal to the vertex-count mask, all made of instructions in the proof's
// closed subset. Instructions after it (loads, branches, selects) are NOT examined;
// the caller must prove them lane local given wave_dependent_scalars.
[[nodiscard]] ShaderNggPassthroughProof ShaderAnalyzeNggPassthroughPrologue(const ShaderCode& code,
	                                                                        const ShaderNggPassthroughCounts& counts);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_PASSTHROUGH_PROOF_H_
