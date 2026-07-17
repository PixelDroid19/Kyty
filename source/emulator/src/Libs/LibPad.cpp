#include "Emulator/Common.h"
#include "Emulator/Controller.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Pad", 1, "Pad", 1, 1);

LIB_DEFINE(InitPad_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("hv1luiJrqQM", Controller::PadInit);
	LIB_FUNC("xk0AcarP3V4", Controller::PadOpen);
	// scePadGetHandle — returns the handle from PadOpen for the same user/type/index.
	LIB_FUNC("u1GRHp+oWoY", Controller::PadGetHandle);
	LIB_FUNC("clVvL4ZDntw", Controller::PadSetMotionSensorState);
	LIB_FUNC("vDLMoJLde8I", Controller::PadSetTiltCorrectionState);
	LIB_FUNC("r44mAxdSG+U", Controller::PadSetAngularVelocityDeadbandState);
	LIB_FUNC("gjP9-KQzoUk", Controller::PadGetControllerInformation);
	LIB_FUNC("YndgXqQVV7c", Controller::PadReadState);
	LIB_FUNC("q1cHNfGycLI", Controller::PadRead);
	LIB_FUNC("yFVnOdGxvZY", Controller::PadSetVibration);
	// scePadSetVibrationMode / scePadSetTriggerEffect — no-op success (PS5 dualsense extras).
	LIB_FUNC("W2G-yoyMF5U", Controller::PadSetVibrationMode);
	LIB_FUNC("2JgFB2n9oUM", Controller::PadSetTriggerEffect);
	// scePadGetTriggerEffectState — NID znaWI0gpuo8 (Astro after PlayGo).
	LIB_FUNC("znaWI0gpuo8", Controller::PadGetTriggerEffectState);
	LIB_FUNC("DscD1i9HX1w", Controller::PadResetLightBar);
	LIB_FUNC("RR4novUEENY", Controller::PadSetLightBar);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
