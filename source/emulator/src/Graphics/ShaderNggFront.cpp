#include "Emulator/Graphics/ShaderNggFront.h"

#include "Emulator/Graphics/ShaderNggFrontBody.h"
#include "Emulator/Log.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static bool IsGuestWaveSize(uint32_t guest_wave_size)
{
	return guest_wave_size == 32u || guest_wave_size == 64u;
}

static ShaderNggFrontProof Refusal(const ShaderNggPassthroughCounts& counts, const ShaderNggPassthroughProof& proof)
{
	ShaderNggFrontProof front;
	front.refused_counts = counts;
	front.rejection      = proof.rejection;
	front.refusal_pc     = proof.rejection_pc;
	front.reason         = proof.reason;
	return front;
}

static ShaderNggFrontProof ProveWholeProgram(const ShaderCode& code, uint32_t guest_wave_size)
{
	for (uint32_t vertices = 1; vertices <= guest_wave_size; ++vertices)
	{
		for (uint32_t primitives = 1; primitives <= guest_wave_size; ++primitives)
		{
			const ShaderNggPassthroughCounts counts {guest_wave_size, vertices, primitives};
			const auto                       proof = ShaderAnalyzeNggPassthrough(code, counts);
			if (!proof.proven) { return Refusal(counts, proof); }
		}
	}
	ShaderNggFrontProof front;
	front.lane_local = true;
	return front;
}

// The scalar prologue is proved exactly for every launch; what follows it is proved
// lane local once, from the scalar words the prologue leaves launch dependent.
static ShaderNggFrontProof ProvePrologueAndBody(const ShaderCode& code, uint32_t guest_wave_size)
{
	uint32_t                    end = ShaderNggPassthroughNoInstruction;
	ShaderNggScalarDependencies wave_scalars;
	for (uint32_t vertices = 1; vertices <= guest_wave_size; ++vertices)
	{
		for (uint32_t primitives = 1; primitives <= guest_wave_size; ++primitives)
		{
			const ShaderNggPassthroughCounts counts {guest_wave_size, vertices, primitives};
			const auto                       proof = ShaderAnalyzeNggPassthroughPrologue(code, counts);
			if (!proof.prologue_proven) { return Refusal(counts, proof); }
			if (end != ShaderNggPassthroughNoInstruction && end != proof.prologue_end_index)
			{
				ShaderNggFrontProof front;
				front.refused_counts = counts;
				front.rejection      = ShaderNggPassthroughRejection::ExecMaskMismatch;
				front.reason         = "the prologue ends at a different instruction for another launch";
				return front;
			}
			end = proof.prologue_end_index;
			wave_scalars |= proof.wave_dependent_scalars;
		}
	}
	const auto body = ShaderProveNggFrontBodyLaneLocal(code, end + 1u, wave_scalars);
	ShaderNggFrontProof front;
	front.lane_local = body.lane_local;
	if (!body.lane_local)
	{
		front.refused_counts.guest_wave_size = guest_wave_size;
		front.rejection                      = ShaderNggPassthroughRejection::UnsupportedInstruction;
		front.refusal_pc                     = body.refused_pc;
		front.reason                         = body.reason != nullptr ? body.reason : "body is not lane local";
		// The refused instruction and the three before it: enough context to classify the gap.
		const auto& instructions = code.GetInstructions();
		for (uint32_t index = 0; index < instructions.Size(); ++index)
		{
			if (instructions.At(index).pc != body.refused_pc) { continue; }
			for (uint32_t shown = index >= 3u ? index - 3u : 0u; shown <= index; ++shown)
			{
				front.reason += Kyty::Core::String8(shown == index ? " AT [" : " [") + ShaderCode::DbgInstructionToStr(instructions.At(shown)) + "]";
			}
		}
	}
	return front;
}

ShaderNggFrontProof ShaderProveNggFrontLaneLocal(const ShaderCode& code, uint32_t guest_wave_size)
{
	if (!IsGuestWaveSize(guest_wave_size))
	{
		ShaderNggFrontProof front;
		front.refused_counts.guest_wave_size = guest_wave_size;
		front.rejection                      = ShaderNggPassthroughRejection::InvalidWaveSize;
		front.reason                         = "guest wave size must be explicitly 32 or 64";
		return front;
	}
	const auto whole = ProveWholeProgram(code, guest_wave_size);
	return whole.lane_local ? whole : ProvePrologueAndBody(code, guest_wave_size);
}

bool ShaderNggFrontVerdict::LaneLocal(const ShaderCode& code, uint32_t guest_wave_size) const
{
	if (!IsGuestWaveSize(guest_wave_size)) { return false; }
	Slot& slot = guest_wave_size == 32u ? m_wave32 : m_wave64;
	std::call_once(slot.once, [&]
	{
		const auto front = ShaderProveNggFrontLaneLocal(code, guest_wave_size);
		slot.lane_local  = front.lane_local;
		if (!front.lane_local)
		{
			slot.refusal = Kyty::Core::String8::FromPrintf(
			    "ngg_front_refusal{wave=%u vertices=%u primitives=%u pc=0x%x reason=%s}", guest_wave_size,
			    front.refused_counts.es_vertex_count, front.refused_counts.gs_primitive_count, front.refusal_pc, front.reason.c_str());
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "NGG passthrough front is not proven width-neutral: %s\n", slot.refusal.c_str());
		}
	});
	return slot.lane_local;
}

const Kyty::Core::String8& ShaderNggFrontVerdict::Refusal(uint32_t guest_wave_size) const
{
	static const Kyty::Core::String8 none;
	return guest_wave_size == 32u ? m_wave32.refusal : guest_wave_size == 64u ? m_wave64.refusal : none;
}

static bool StagesAreNggPassthrough(const GraphicsGeRawRegister* stages)
{
	if (stages == nullptr || !stages->known) { return false; }
	const auto decoded = GraphicsDecodeGeStages(*stages, GraphicsGeGeneration::Gfx103);
	return decoded.raw_unknown_bits == 0 && decoded.kind == GraphicsGeStageKind::NggPassthrough;
}

bool ShaderApplyNggFrontProof(ShaderNativeWaveInfo* wave, const ShaderCode& code, const GraphicsGeRawRegister* stages,
                              const ShaderNggFrontVerdict& verdict)
{
	const bool needs_exact = ShaderUsesNativeWaveState(code);
	const bool classified  = wave != nullptr && wave->refusal_reason == nullptr && wave->proof == ShaderNativeWaveProof::ExactSubgroup;
	if (!needs_exact || !classified || !StagesAreNggPassthrough(stages)) { return needs_exact; }
	if (!verdict.LaneLocal(code, wave->guest_wave_size)) { return true; }
	wave->proof = ShaderNativeWaveProof::LaneLocal;
	return false;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
