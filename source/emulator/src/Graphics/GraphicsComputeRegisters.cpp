#include "GraphicsComputeRegisters.h"

#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Shared decoders for the packed shader-setup packet and individual
// COMPUTE_PGM_RSRC* register writes from guest-built command buffers.
void decode_compute_pgm_rsrc1(HW::CsStageRegisters& regs, uint32_t value)
{
	regs.vgprs = (value >> Pm4::COMPUTE_PGM_RSRC1_VGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_VGPRS_MASK;
	regs.sgprs = (value >> Pm4::COMPUTE_PGM_RSRC1_SGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_SGPRS_MASK;
	regs.bulky = (value >> Pm4::COMPUTE_PGM_RSRC1_BULKY_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_BULKY_MASK;
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
