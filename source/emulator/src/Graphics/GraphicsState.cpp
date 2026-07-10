#include "Emulator/Graphics/GraphicsState.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/Pm4.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::State {

void SetGenericScissorTl(HW::Context& context, uint32_t value)
{
	const auto& viewport = context.GetScreenViewport();
	const int left = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, TL_X)));
	const int top  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, TL_Y)));
	const bool window_offset_disable = KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, WINDOW_OFFSET_DISABLE) != 0;

	context.SetGenericScissor(left, top, viewport.generic_scissor_right, viewport.generic_scissor_bottom, !window_offset_disable);
}

void SetGenericScissorBr(HW::Context& context, uint32_t value)
{
	const auto& viewport = context.GetScreenViewport();
	const int right  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_BR, BR_X)));
	const int bottom = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_BR, BR_Y)));

	context.SetGenericScissor(viewport.generic_scissor_left, viewport.generic_scissor_top, right, bottom,
	                          viewport.generic_scissor_window_offset_enable);
}

void SetModeControl(HW::Context& context, uint32_t value)
{
	HW::ModeControl mode;

	mode.cull_front               = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, CULL_FRONT) != 0;
	mode.cull_back                = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, CULL_BACK) != 0;
	mode.face                     = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, FACE) != 0;
	mode.poly_mode                = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLY_MODE);
	mode.polymode_front_ptype     = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLYMODE_FRONT_PTYPE);
	mode.polymode_back_ptype      = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLYMODE_BACK_PTYPE);
	mode.poly_offset_front_enable = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLY_OFFSET_FRONT_ENABLE) != 0;
	mode.poly_offset_back_enable  = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLY_OFFSET_BACK_ENABLE) != 0;
	mode.vtx_window_offset_enable = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, VTX_WINDOW_OFFSET_ENABLE) != 0;
	mode.provoking_vtx_last       = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, PROVOKING_VTX_LAST) != 0;
	mode.persp_corr_dis           = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, PERSP_CORR_DIS) != 0;

	context.SetModeControl(mode);
}

void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value)
{
	EXIT_IF(slot >= 8);

	HW::BlendControl blend;

	blend.color_srcblend  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_SRCBLEND);
	blend.color_comb_fcn  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_COMB_FCN);
	blend.color_destblend = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_DESTBLEND);
	blend.alpha_srcblend  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_SRCBLEND);
	blend.alpha_comb_fcn  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_COMB_FCN);
	blend.alpha_destblend = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_DESTBLEND);
	blend.separate_alpha_blend = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, SEPARATE_ALPHA_BLEND) != 0;
	blend.enable               = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ENABLE) != 0;

	context.SetBlendControl(slot, blend);
}

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED
