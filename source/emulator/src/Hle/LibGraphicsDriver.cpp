#include "Emulator/Hle/GraphicsRegistration.h"
#include "Emulator/Libs/Libs.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Hle {

LIB_DEFINE(InitGraphicsDriver_1)
{
	GraphicsRegistration::InitGraphicsDriver_1(s);
}

} // namespace Kyty::Hle

#endif // KYTY_EMU_ENABLED
