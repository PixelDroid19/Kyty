#ifndef EMULATOR_SRC_GRAPHICS_GRAPHICSCOMPUTEREGISTERS_H_
#define EMULATOR_SRC_GRAPHICS_GRAPHICSCOMPUTEREGISTERS_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace HW {
struct CsStageRegisters;
}

enum class GraphicsFp16OverflowStage { Compute, Vertex, Geometry, Pixel };

struct GraphicsFp16OverflowMode
{
	// A candidate raw bit until known; false is not an implicit guest policy.
	bool enabled = false;
	bool known   = false;
};

// Stage-specific GFX9+ RSRC1 locations. Only the identified Gen5/GFX10-family
// guest path establishes their meaning; legacy/unknown platforms stay unknown.
[[nodiscard]] GraphicsFp16OverflowMode GraphicsDecodeFp16Overflow(uint32_t value, GraphicsFp16OverflowStage stage);

// Packed setup, direct SET_SH_REG and indirect writes share this decoder.
// An RSRC1 write supplies the base FP controls and marks fp_mode_known;
// FP16_OVFL additionally carries generation-qualified fp16_overflow_known.
// Default-constructed register state does not establish an observed mode zero.
void decode_compute_pgm_rsrc1(HW::CsStageRegisters& regs, uint32_t value);
void decode_compute_pgm_rsrc2(HW::CsStageRegisters& regs, uint32_t value);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_GRAPHICSCOMPUTEREGISTERS_H_ */
