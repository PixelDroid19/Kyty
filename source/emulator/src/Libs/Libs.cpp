#include "Emulator/Libs/Libs.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace LibcInternal {
LIB_DEFINE(InitLibcInternal_1);
} // namespace LibcInternal

LIB_DEFINE(InitLibC_1);

LIB_DEFINE(InitAppContent_1);
LIB_DEFINE(InitAudio_1);
LIB_DEFINE(InitDbgAddressSanitizer_1);
LIB_DEFINE(InitDebug_1);
LIB_DEFINE(InitDialog_1);
LIB_DEFINE(InitDiscMap_1);
LIB_DEFINE(InitGraphicsDriver_1);
LIB_DEFINE(InitLibKernel_1);
LIB_DEFINE(InitNet_1);
LIB_DEFINE(InitPad_1);
LIB_DEFINE(InitPlayGo_1);
LIB_DEFINE(InitSaveData_1);
namespace SaveDataNative {
LIB_DEFINE(InitSaveDataNative_1);
} // namespace SaveDataNative
namespace NpUniversalDataSystem {
LIB_DEFINE(InitNpUniversalDataSystem_1);
} // namespace NpUniversalDataSystem
namespace NpGameIntent {
LIB_DEFINE(InitNpGameIntent_1);
} // namespace NpGameIntent
namespace NpEntitlementAccess {
LIB_DEFINE(InitNpEntitlementAccess_1);
} // namespace NpEntitlementAccess
namespace NpManager {
LIB_DEFINE(InitNpManager_1);
} // namespace NpManager
namespace NpTrophy2 {
LIB_DEFINE(InitNpTrophy2_1);
} // namespace NpTrophy2
namespace NpManager {
LIB_DEFINE(InitNpManager_1);
} // namespace NpManager
LIB_DEFINE(InitSysmodule_1);
LIB_DEFINE(InitSystemService_1);
LIB_DEFINE(InitUserService_1);
LIB_DEFINE(InitVideoOut_1);
LIB_DEFINE(InitUlt_1);
LIB_DEFINE(InitCes_1);
LIB_DEFINE(InitShare_1);
LIB_DEFINE(InitAmpr_1);
LIB_DEFINE(InitFiber_1);
LIB_DEFINE(InitRudp_1);
LIB_DEFINE(InitNpCppWebApi_1);
LIB_DEFINE(InitJson2_1);
LIB_DEFINE(InitFont_1);
LIB_DEFINE(InitFontFt_1);
LIB_DEFINE(InitIme_1);
LIB_DEFINE(InitMouse_1);
LIB_DEFINE(InitKeyboard_1);
LIB_DEFINE(InitContentExport_1);
LIB_DEFINE(InitAcm_1);
LIB_DEFINE(InitRtc_1);
LIB_DEFINE(InitRandom_1);
LIB_DEFINE(InitCoredump_1);

bool Init(const String& id, Loader::SymbolDatabase* s)
{
	LIB_CHECK(U"libAppContent_1", InitAppContent_1);
	LIB_CHECK(U"libAudio_1", InitAudio_1);
	LIB_CHECK(U"libc_1", InitLibC_1);
	LIB_CHECK(U"libc_internal_1", LibcInternal::InitLibcInternal_1);
	LIB_CHECK(U"libDbgAddressSanitizer_1", InitDbgAddressSanitizer_1);
	LIB_CHECK(U"libDebug_1", InitDebug_1);
	LIB_CHECK(U"libDialog_1", InitDialog_1);
	LIB_CHECK(U"libDiscMap_1", InitDiscMap_1);
	LIB_CHECK(U"libGraphicsDriver_1", InitGraphicsDriver_1);
	LIB_CHECK(U"libkernel_1", InitLibKernel_1);
	LIB_CHECK(U"libNet_1", InitNet_1);
	LIB_CHECK(U"libPad_1", InitPad_1);
	LIB_CHECK(U"libPlayGo_1", InitPlayGo_1);
	LIB_CHECK(U"libNpUniversalDataSystem_1", NpUniversalDataSystem::InitNpUniversalDataSystem_1);
	LIB_CHECK(U"libNpGameIntent_1", NpGameIntent::InitNpGameIntent_1);
	LIB_CHECK(U"libNpEntitlementAccess_1", NpEntitlementAccess::InitNpEntitlementAccess_1);
	LIB_CHECK(U"libNpManager_1", NpManager::InitNpManager_1);
	LIB_CHECK(U"libNpTrophy2_1", NpTrophy2::InitNpTrophy2_1);
	LIB_CHECK(U"libNpManager_1", NpManager::InitNpManager_1);
	if (id == U"libSaveData_1")
	{
		InitSaveData_1(s);
		SaveDataNative::InitSaveDataNative_1(s);
		return true;
	}
	LIB_CHECK(U"libSysmodule_1", InitSysmodule_1);
	LIB_CHECK(U"libSystemService_1", InitSystemService_1);
	LIB_CHECK(U"libUserService_1", InitUserService_1);
	LIB_CHECK(U"libVideoOut_1", InitVideoOut_1);
	LIB_CHECK(U"libUlt_1", InitUlt_1);
	LIB_CHECK(U"libCes_1", InitCes_1);
	LIB_CHECK(U"libShare_1", InitShare_1);
	LIB_CHECK(U"libAmpr_1", InitAmpr_1);
	LIB_CHECK(U"libFiber_1", InitFiber_1);
	LIB_CHECK(U"libRudp_1", InitRudp_1);
	LIB_CHECK(U"libNpCppWebApi_1", InitNpCppWebApi_1);
	LIB_CHECK(U"libJson2_1", InitJson2_1);
	LIB_CHECK(U"libFont_1", InitFont_1);
	LIB_CHECK(U"libFontFt_1", InitFontFt_1);
	LIB_CHECK(U"libIme_1", InitIme_1);
	LIB_CHECK(U"libMouse_1", InitMouse_1);
	LIB_CHECK(U"libKeyboard_1", InitKeyboard_1);
	LIB_CHECK(U"libContentExport_1", InitContentExport_1);
	LIB_CHECK(U"libAcm_1", InitAcm_1);
	LIB_CHECK(U"libSceRtc_1", InitRtc_1);
	LIB_CHECK(U"libSceRandom_1", InitRandom_1);
	LIB_CHECK(U"libSceCoredump_1", InitCoredump_1);

	return false;
}

void InitAll(Loader::SymbolDatabase* s)
{
	LIB_LOAD(InitAudio_1);
	LIB_LOAD(InitAppContent_1);
	LIB_LOAD(InitLibC_1);
	LIB_LOAD(LibcInternal::InitLibcInternal_1);
	LIB_LOAD(InitDbgAddressSanitizer_1);
	LIB_LOAD(InitDebug_1);
	LIB_LOAD(InitDialog_1);
	LIB_LOAD(InitDiscMap_1);
	LIB_LOAD(InitGraphicsDriver_1);
	LIB_LOAD(InitLibKernel_1);
	LIB_LOAD(InitNet_1);
	LIB_LOAD(InitPad_1);
	LIB_LOAD(InitPlayGo_1);
	LIB_LOAD(NpUniversalDataSystem::InitNpUniversalDataSystem_1);
	LIB_LOAD(NpGameIntent::InitNpGameIntent_1);
	LIB_LOAD(NpEntitlementAccess::InitNpEntitlementAccess_1);
	LIB_LOAD(NpManager::InitNpManager_1);
	LIB_LOAD(NpTrophy2::InitNpTrophy2_1);
	LIB_LOAD(NpManager::InitNpManager_1);
	LIB_LOAD(InitSaveData_1);
	LIB_LOAD(SaveDataNative::InitSaveDataNative_1);
	LIB_LOAD(InitSysmodule_1);
	LIB_LOAD(InitSystemService_1);
	LIB_LOAD(InitUserService_1);
	LIB_LOAD(InitVideoOut_1);
	LIB_LOAD(InitUlt_1);
	LIB_LOAD(InitCes_1);
	LIB_LOAD(InitShare_1);
	LIB_LOAD(InitAmpr_1);
	LIB_LOAD(InitFiber_1);
	LIB_LOAD(InitRudp_1);
	LIB_LOAD(InitNpCppWebApi_1);
	LIB_LOAD(InitJson2_1);
	LIB_LOAD(InitFont_1);
	LIB_LOAD(InitFontFt_1);
	LIB_LOAD(InitIme_1);
	LIB_LOAD(InitMouse_1);
	LIB_LOAD(InitKeyboard_1);
	LIB_LOAD(InitContentExport_1);
	LIB_LOAD(InitAcm_1);
	LIB_LOAD(InitRtc_1);
	LIB_LOAD(InitRandom_1);
	LIB_LOAD(InitCoredump_1);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
