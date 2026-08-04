#ifndef EMULATOR_SRC_GRAPHICS_GRAPHICSCOMPUTEREGISTERS_H_
#define EMULATOR_SRC_GRAPHICS_GRAPHICSCOMPUTEREGISTERS_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace HW {
struct CsStageRegisters;
}

void decode_compute_pgm_rsrc1(HW::CsStageRegisters& regs, uint32_t value);
void decode_compute_pgm_rsrc2(HW::CsStageRegisters& regs, uint32_t value);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_GRAPHICSCOMPUTEREGISTERS_H_ */
