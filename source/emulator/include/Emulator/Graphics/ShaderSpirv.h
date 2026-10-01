#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSPIRV_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSPIRV_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String8.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class ShaderCode;
struct ShaderVertexInputInfo;
struct ShaderPixelInputInfo;
struct ShaderComputeInputInfo;

// Host-only input contract for one logical wave64 per compute workgroup.
// Each input lane contains flags (allocated=1, initial EXEC=2, covered=4),
// raw VGPR words, then 16 words per attribute: four native interpolated
// values and twelve raw parameter words in P10/P20/P0 order.
// Each output lane contains pixel validity, a 32-bit MRT component mask and
// 32 raw FP32 words. Export VM updates validity independently of color data.
// The caller must supply captured values and enable linear compute derivatives
// plus required full subgroups of 32; this interface does not select a draw path.
// If user_sgpr_count is specified from the PS resource register, each input
// wave starts with its raw parameter-state SGPR word, followed by 64 lanes.
// That word and the parameter triples must describe the same parameter cache.
// Virtualized uses a normalized selector into the host's primitive-owned
// captured block, without a raw wave header. It requires proof that the initial
// word reaches M0 alone and that M0 never escapes into guest data or control.
enum class ShaderFragmentParameterState
{
	Captured,
	Virtualized
};

struct ShaderFragmentComputeInfo
{
	uint32_t                     descriptor_set     = 1;
	uint32_t                     input_binding      = 0;
	uint32_t                     output_binding     = 1;
	uint32_t                     initial_vgpr_count = 0;
	uint32_t                     user_sgpr_count    = UINT32_MAX;
	ShaderFragmentParameterState parameter_state    = ShaderFragmentParameterState::Captured;

	[[nodiscard]] uint32_t HeaderWords() const
	{
		return user_sgpr_count != UINT32_MAX && parameter_state == ShaderFragmentParameterState::Captured ? 1u : 0u;
	}
};

// Highest interpolant the program reads, or the register count when larger.
// Native, capture and shade generators must agree on it for one program.
uint32_t SpirvResolvePixelParameterCount(const ShaderCode& code, uint32_t register_count);
String8 SpirvGenerateSource(const ShaderCode& code, const ShaderVertexInputInfo* vs_input_info, const ShaderPixelInputInfo* ps_input_info,
                            const ShaderComputeInputInfo* cs_input_info);
String8 SpirvGenerateFragmentComputeSource(const ShaderCode& code, const ShaderPixelInputInfo& ps_input_info,
                                           const ShaderFragmentComputeInfo& transport);
String8 SpirvGetEmbeddedVs(uint32_t id);
String8 SpirvGetEmbeddedPs(uint32_t id);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSPIRV_H_ */
