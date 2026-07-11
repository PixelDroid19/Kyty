#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Ces", 1, "Ces", 1, 1);

namespace Ces {

static KYTY_SYSV_ABI void* CesUcsProfileInitSjis1997Cp932(void* sheet)
{
	PRINT_NAME();
	printf("\t sheet               = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(sheet));
	return sheet;
}

} // namespace Ces

LIB_DEFINE(InitCes_1)
{
	LIB_FUNC("ZiDCxUUGbec", Ces::CesUcsProfileInitSjis1997Cp932);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
