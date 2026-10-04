#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_FRONT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_FRONT_H_

#include "Emulator/Graphics/GraphicsGeState.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderNggPassthroughProof.h"

#include <cstdint>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// The fused ES-to-GS front runs one host invocation per guest vertex thread, and one host
// subgroup is one guest wave: EXEC holds the ballot of the live invocations and every
// invocation reads its lane bit at its subgroup index. The SPIR-V generator therefore
// initializes s3 ([7:0] vertices, [15:8] primitives, the RDNA2 wave-info layout) with
// n vertices and n primitives, n = highest live subgroup lane + 1, so EXEC after the
// prologue's vertex-count shift keeps every live invocation. Native admission bounds the
// host subgroup by the guest width for a lane-local proof, so 1 <= n <= guest width:
// one of the launches the proof below quantifies over.

struct ShaderNggFrontProof
{
	bool lane_local = false;
	// Filled on refusal: the first launch whose equivalence could not be proved.
	ShaderNggPassthroughCounts    refused_counts;
	ShaderNggPassthroughRejection rejection  = ShaderNggPassthroughRejection::None;
	uint32_t                      refusal_pc = ShaderNggPassthroughNoInstruction;
	Kyty::Core::String8           reason;
};

// Width-neutrality proof for a NGG passthrough front (ES fused with the GS prologue),
// quantified over every launch a wave of guest_wave_size lanes can carry: all ES
// vertex counts and all GS primitive counts in 1..guest_wave_size.
//
// Two routes, the first that succeeds wins:
//  1. A program made only of the wave-count prologue and pure lane ALU/exports is
//     proved as a whole by ShaderAnalyzeNggPassthrough for every launch.
//  2. Otherwise the prologue is proved for every launch (same analyzer, stopped where
//     EXEC becomes the vertex-count mask) and the remainder is proved once by
//     ShaderProveNggFrontBodyLaneLocal: scalar loads, wave-uniform branches, lane-bit
//     selects and vertex fetch that never read what the prologue left launch dependent.
// Per launch the prologue proof establishes that the allocation request matches the
// counts, that EXEC is exactly the low-lane mask of the relevant count, and that the
// primitive export forwards the hardware-supplied primitive unmodified. Therefore vertex
// thread i of any such wave computes what an isolated invocation with the same vertex
// inputs computes, whichever width the host subgroup has. The (n, n) launch the generator
// initializes (see above) is one of the cases, so the scalar prologue the host really
// executes is covered as well.
//
// This is not a renderer launch proof: the caller must establish the GE stage shape
// (NggPassthrough) and that the host assembles primitives itself.
[[nodiscard]] ShaderNggFrontProof ShaderProveNggFrontLaneLocal(const ShaderCode& code, uint32_t guest_wave_size);

// One verdict per immutable program and guest width, computed on first use. The
// proof enumerates up to guest_wave_size^2 launches, so it must not run per draw.
class ShaderNggFrontVerdict
{
public:
	[[nodiscard]] bool LaneLocal(const ShaderCode& code, uint32_t guest_wave_size) const;
	// Why the proof refused this program at this width (empty when it proved it or has not
	// run). For the strict-mode diagnostic; valid after LaneLocal() for the same width.
	[[nodiscard]] const Kyty::Core::String8& Refusal(uint32_t guest_wave_size) const;

private:
	struct Slot
	{
		std::once_flag      once;
		bool                lane_local = false;
		Kyty::Core::String8 refusal;
	};
	mutable Slot m_wave32;
	mutable Slot m_wave64;
};

// Replaces an ExactSubgroup verdict by LaneLocal when the GE stage state decodes as a
// NGG passthrough (complete known word, no unknown bits) and the front is proven above.
// Any other stage shape, including the merged ES/GS form, keeps the generic verdict.
// Returns whether the module still needs a native wave width.
[[nodiscard]] bool ShaderApplyNggFrontProof(ShaderNativeWaveInfo* wave, const ShaderCode& code, const GraphicsGeRawRegister* stages,
                                            const ShaderNggFrontVerdict& verdict);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_FRONT_H_
