#ifndef EMULATOR_SRC_GRAPHICS_SHADERSPIRVSELECTIONCUT_H_
#define EMULATOR_SRC_GRAPHICS_SHADERSPIRVSELECTIONCUT_H_

#include "Kyty/Core/String8.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct SpirvSelectionCutConfig
{
	uint32_t device_address_block  = 0;
	bool     vsharp_uniform_buffer = false;
};

// Cuts live ranges of loop-invariant local scalar variables across the
// highest-scoring loop-enclosing one-armed selection construct, using a runtime
// zero value derived from the aligned guest device-address table pointer in
// %vsharp. Returns true and fills *out_constants / *out_main_body when applied.
[[nodiscard]] bool SpirvCutSelectionLiveRanges(const String8& main_body, const SpirvSelectionCutConfig& config, String8* out_constants,
                                               String8* out_main_body);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERSPIRVSELECTIONCUT_H_ */
