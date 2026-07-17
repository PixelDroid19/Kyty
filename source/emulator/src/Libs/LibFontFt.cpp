#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("FontFt", 1, "FontFt", 1, 1);

namespace FontFt {

// Opaque selection tokens returned to sceFontCreateLibrary* as FontLibrarySelection /
// FontRendererSelection. Guest only compares/passes the pointer; content is HLE-private.
struct SelectionState
{
	int value;
	int kind;
};

// sceFontSelectLibraryFt — NID oM+XCzVG3oM. Returns a stable selection handle.
static void* KYTY_SYSV_ABI FontSelectLibraryFt(int value)
{
	PRINT_NAME();
	printf("\t value = %d\n", value);
	static SelectionState s_selection {};
	s_selection.value = value;
	s_selection.kind  = 0;
	return &s_selection;
}

// sceFontSelectRendererFt — NID Xx974EW-QFY.
static void* KYTY_SYSV_ABI FontSelectRendererFt(int value)
{
	PRINT_NAME();
	printf("\t value = %d\n", value);
	static SelectionState s_selection {};
	s_selection.value = value;
	s_selection.kind  = 1;
	return &s_selection;
}

} // namespace FontFt

LIB_DEFINE(InitFontFt_1)
{
	LIB_FUNC("oM+XCzVG3oM", FontFt::FontSelectLibraryFt);
	LIB_FUNC("Xx974EW-QFY", FontFt::FontSelectRendererFt);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
