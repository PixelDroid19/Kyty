#include "Emulator/Common.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

static void InitVideoOutCommon(Loader::SymbolDatabase* symbols, int module_version_major, int module_version_minor)
{
	auto add = [symbols, module_version_major, module_version_minor](const char* nid, uint64_t function, const char32_t* debug_name) {
		Loader::SymbolResolve symbol {};
		symbol.name                 = String::FromUtf8(nid);
		symbol.library              = U"VideoOut";
		symbol.library_version      = 1;
		symbol.module               = U"VideoOut";
		symbol.module_version_major = module_version_major;
		symbol.module_version_minor = module_version_minor;
		symbol.type                 = Loader::SymbolType::Func;
		symbols->Add(symbol, function, debug_name);
	};

	add("Up36PTk687E", reinterpret_cast<uint64_t>(VideoOut::VideoOutOpen), U"VideoOut::VideoOutOpen");
	add("CBiu4mCE1DA", reinterpret_cast<uint64_t>(VideoOut::VideoOutSetFlipRate), U"VideoOut::VideoOutSetFlipRate");
}

namespace LibGen4 {

LIB_VERSION("VideoOut", 1, "VideoOut", 0, 0);

LIB_DEFINE(InitVideoOut_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("uquVH4-Du78", VideoOut::VideoOutClose);
	LIB_FUNC("6kPnj51T62Y", VideoOut::VideoOutGetResolutionStatus);
	LIB_FUNC("i6-sR91Wt-4", VideoOut::VideoOutSetBufferAttribute);
	LIB_FUNC("HXzjK9yI30k", VideoOut::VideoOutAddFlipEvent);
	LIB_FUNC("Xru92wHJRmg", VideoOut::VideoOutAddVblankEvent);
	LIB_FUNC("w3BY+tAEiQY", VideoOut::VideoOutRegisterBuffers);
	LIB_FUNC("U46NwOiJpys", VideoOut::VideoOutSubmitFlip);
	LIB_FUNC("SbU3dwp80lQ", VideoOut::VideoOutGetFlipStatus);
	LIB_FUNC("1FZBKy8HeNU", VideoOut::VideoOutGetVblankStatus);
	LIB_FUNC("MTxxrOCeSig", VideoOut::VideoOutSetWindowModeMargins);
	InitVideoOutCommon(s, 0, 0);
}

} // namespace LibGen4

namespace LibGen5 {

LIB_VERSION("VideoOut", 1, "VideoOut", 1, 1);

LIB_DEFINE(InitVideoOut_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("PjS5uASwcV8", VideoOut::VideoOutSetBufferAttribute2);
	LIB_FUNC("rKBUtgRrtbk", VideoOut::VideoOutRegisterBuffers2);
	InitVideoOutCommon(s, 1, 1);
}

} // namespace LibGen5

LIB_DEFINE(InitVideoOut_1)
{
	LibGen4::InitVideoOut_1(s);
	LibGen5::InitVideoOut_1(s);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
