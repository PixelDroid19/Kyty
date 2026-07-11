#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

// libSceAmpr (Advanced Memory / APR command buffers). Gen5-only imports.
// Record sizes from public export naming / measure APIs (reimplemented locally).
LIB_VERSION("Ampr", 1, "Ampr", 1, 1);

namespace Ampr {

// Fixed Ampr command-record sizes used by measure APIs (guest sizes buffers).
constexpr uint64_t kReadFileRecordSize       = 0x30;
constexpr uint64_t kKernelEventQueueRecordSize = 0x30;
constexpr uint64_t kWriteAddressRecordSize   = 0x20;

static KYTY_SYSV_ABI uint64_t AmprMeasureCommandSizeReadFile()
{
	PRINT_NAME();
	printf("\t size = 0x%016" PRIx64 "\n", kReadFileRecordSize);
	return kReadFileRecordSize;
}

static KYTY_SYSV_ABI uint64_t AmprMeasureCommandSizeWriteKernelEventQueue0400()
{
	PRINT_NAME();
	printf("\t size = 0x%016" PRIx64 "\n", kKernelEventQueueRecordSize);
	return kKernelEventQueueRecordSize;
}

static KYTY_SYSV_ABI uint64_t AmprMeasureCommandSizeWriteAddressOnCompletion()
{
	PRINT_NAME();
	printf("\t size = 0x%016" PRIx64 "\n", kWriteAddressRecordSize);
	return kWriteAddressRecordSize;
}

} // namespace Ampr

LIB_DEFINE(InitAmpr_1)
{
	LIB_FUNC("vWU-odnS+fU", Ampr::AmprMeasureCommandSizeReadFile);
	LIB_FUNC("sSAUCCU1dv4", Ampr::AmprMeasureCommandSizeWriteKernelEventQueue0400);
	LIB_FUNC("C+IEj+BsAFM", Ampr::AmprMeasureCommandSizeWriteAddressOnCompletion);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
