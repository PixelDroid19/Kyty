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
	add("uquVH4-Du78", reinterpret_cast<uint64_t>(VideoOut::VideoOutClose), U"VideoOut::VideoOutClose");
	add("6kPnj51T62Y", reinterpret_cast<uint64_t>(VideoOut::VideoOutGetResolutionStatus), U"VideoOut::VideoOutGetResolutionStatus");
	add("i6-sR91Wt-4", reinterpret_cast<uint64_t>(VideoOut::VideoOutSetBufferAttribute), U"VideoOut::VideoOutSetBufferAttribute");
	add("HXzjK9yI30k", reinterpret_cast<uint64_t>(VideoOut::VideoOutAddFlipEvent), U"VideoOut::VideoOutAddFlipEvent");
	add("Xru92wHJRmg", reinterpret_cast<uint64_t>(VideoOut::VideoOutAddVblankEvent), U"VideoOut::VideoOutAddVblankEvent");
	add("w3BY+tAEiQY", reinterpret_cast<uint64_t>(VideoOut::VideoOutRegisterBuffers), U"VideoOut::VideoOutRegisterBuffers");
	add("U46NwOiJpys", reinterpret_cast<uint64_t>(VideoOut::VideoOutSubmitFlip), U"VideoOut::VideoOutSubmitFlip");
	add("SbU3dwp80lQ", reinterpret_cast<uint64_t>(VideoOut::VideoOutGetFlipStatus), U"VideoOut::VideoOutGetFlipStatus");
	add("1FZBKy8HeNU", reinterpret_cast<uint64_t>(VideoOut::VideoOutGetVblankStatus), U"VideoOut::VideoOutGetVblankStatus");
	add("MTxxrOCeSig", reinterpret_cast<uint64_t>(VideoOut::VideoOutSetWindowModeMargins), U"VideoOut::VideoOutSetWindowModeMargins");
}

namespace LibGen4 {

LIB_VERSION("VideoOut", 1, "VideoOut", 0, 0);

LIB_DEFINE(InitVideoOut_1)
{
	PRINT_NAME_ENABLE(true);

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
	LIB_FUNC("zgXifHT9ErY", VideoOut::VideoOutIsFlipPending);
	LIB_FUNC("utPrVdxio-8", VideoOut::VideoOutGetOutputStatus);
	LIB_FUNC("DYhhWbJSeRg", VideoOut::VideoOutColorSettingsSetGamma);
	LIB_FUNC("pv9CI5VC+R0", VideoOut::VideoOutAdjustColor);
	LIB_FUNC("HuViW4HnrOw", VideoOut::VideoOutSubmitChangeBufferAttribute2);
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
