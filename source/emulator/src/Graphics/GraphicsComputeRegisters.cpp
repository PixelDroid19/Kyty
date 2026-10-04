#include "GraphicsComputeRegisters.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

GraphicsFp16OverflowMode GraphicsDecodeFp16Overflow(uint32_t value, GraphicsFp16OverflowStage stage)
{
	uint32_t mask = 0;
	switch (stage)
	{
		case GraphicsFp16OverflowStage::Compute: mask = 1u << 26u; break;
		case GraphicsFp16OverflowStage::Vertex:
		case GraphicsFp16OverflowStage::Geometry: mask = 1u << 31u; break;
		case GraphicsFp16OverflowStage::Pixel: mask = 1u << 29u; break;
		default: return {};
	}
	// Preserve the candidate raw bit even where its semantics are unknown.
	// GFX6..8.1 register definitions do not identify these bits as FP16_OVFL.
	return {(value & mask) != 0, Config::IsInitialized() && Config::GetGuestPlatform() == GuestPlatform::Ps5};
}

// Shared decoders for the packed shader-setup packet and individual
// COMPUTE_PGM_RSRC* register writes from guest-built command buffers.
void decode_compute_pgm_rsrc1(HW::CsStageRegisters& regs, uint32_t value)
{
	regs.vgprs = (value >> Pm4::COMPUTE_PGM_RSRC1_VGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_VGPRS_MASK;
	regs.sgprs = (value >> Pm4::COMPUTE_PGM_RSRC1_SGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_SGPRS_MASK;
	regs.bulky = (value >> Pm4::COMPUTE_PGM_RSRC1_BULKY_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_BULKY_MASK;
	// COMPUTE_PGM_RSRC1: FLOAT_MODE[19:12], DX10_CLAMP[21], IEEE_MODE[23].
	// Keep the initial wave modes from this exact write, including mode zero.
	regs.float_mode    = static_cast<uint8_t>((value >> 12u) & 0xffu);
	regs.dx10_clamp    = (value & (1u << 21u)) != 0;
	regs.ieee_mode     = (value & (1u << 23u)) != 0;
	regs.fp_mode_known = true;
	const auto overflow = GraphicsDecodeFp16Overflow(value, GraphicsFp16OverflowStage::Compute);
	regs.fp16_overflow       = overflow.enabled;
	regs.fp16_overflow_known = overflow.known;
}

void decode_compute_pgm_rsrc2(HW::CsStageRegisters& regs, uint32_t value)
{
	regs.scratch_en     = (value >> Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_MASK;
	regs.user_sgpr      = (value >> Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_MASK;
	regs.tgid_x_en      = (value >> Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_MASK;
	regs.tgid_y_en      = (value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_MASK;
	regs.tgid_z_en      = (value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_MASK;
	regs.tg_size_en     = (value >> Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_MASK;
	regs.tidig_comp_cnt = (value >> Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_MASK;
	regs.lds_size       = (value >> Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_MASK;
}

bool GraphicsDecodeComputeResourceLimits(HW::CsStageRegisters* regs, uint32_t cmd_offset, const uint32_t* values, uint32_t value_count)
{
	if (regs == nullptr || values == nullptr || cmd_offset != Pm4::COMPUTE_RESOURCE_LIMITS || value_count != 1)
	{
		return false;
	}

	regs->SetResourceLimits(values[0]);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
