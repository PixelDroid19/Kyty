#include "Emulator/Hle/VideoOutRegistration.h"
#include "Emulator/Libs/Libs.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Hle {

LIB_DEFINE(InitVideoOut_1)
{
	VideoOutRegistration::InitVideoOut_1(s);
}

} // namespace Kyty::Hle

#endif // KYTY_EMU_ENABLED
