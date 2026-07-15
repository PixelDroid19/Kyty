#include "Emulator/Common.h"
#include "Emulator/Dialog.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace LibCommonDialog {

LIB_VERSION("CommonDialog", 1, "CommonDialog", 1, 1);

namespace CommonDialog = Dialog::CommonDialog;

LIB_DEFINE(InitDialog_1_CommonDialog)
{
	LIB_FUNC("uoUpLGNkygk", CommonDialog::CommonDialogInitialize);
}

} // namespace LibCommonDialog

namespace LibSaveDataDialog {

// Standard and .native library names are both exported by Gen5 titles.
LIB_VERSION("SaveDataDialog", 1, "SaveDataDialog", 1, 1);

namespace SaveDataDialog = Dialog::SaveDataDialog;

static void RegisterSaveDataDialogFuncs(Loader::SymbolDatabase* s)
{
	// sceSaveDataDialogInitialize / Open — required before UpdateStatus flows.
	LIB_FUNC("s9e3+YpRnzw", SaveDataDialog::SaveDataDialogInitialize);
	LIB_FUNC("4tPhsP6FpDI", SaveDataDialog::SaveDataDialogOpen);
	// sceSaveDataDialogGetStatus
	LIB_FUNC("ERKzksauAJA", SaveDataDialog::SaveDataDialogGetStatus);
	LIB_FUNC("KK3Bdg1RWK0", SaveDataDialog::SaveDataDialogUpdateStatus);
	LIB_FUNC("YuH2FA7azqQ", SaveDataDialog::SaveDataDialogTerminate);
	LIB_FUNC("hay1CfTmLyA", SaveDataDialog::SaveDataDialogProgressBarSetValue);
}

LIB_DEFINE(InitDialog_1_SaveDataDialog)
{
	RegisterSaveDataDialogFuncs(s);
}

} // namespace LibSaveDataDialog

namespace LibSaveDataDialogNative {

LIB_VERSION("SaveDataDialog.native", 1, "SaveDataDialog", 1, 1);

namespace SaveDataDialog = Dialog::SaveDataDialog;

LIB_DEFINE(InitDialog_1_SaveDataDialogNative)
{
	// Same HLE contracts as SaveDataDialog; distinct library id for native PS5.
	LIB_FUNC("s9e3+YpRnzw", SaveDataDialog::SaveDataDialogInitialize);
	LIB_FUNC("4tPhsP6FpDI", SaveDataDialog::SaveDataDialogOpen);
	LIB_FUNC("ERKzksauAJA", SaveDataDialog::SaveDataDialogGetStatus);
	LIB_FUNC("KK3Bdg1RWK0", SaveDataDialog::SaveDataDialogUpdateStatus);
	LIB_FUNC("YuH2FA7azqQ", SaveDataDialog::SaveDataDialogTerminate);
	LIB_FUNC("hay1CfTmLyA", SaveDataDialog::SaveDataDialogProgressBarSetValue);
}

} // namespace LibSaveDataDialogNative

namespace LibMsgDialog {

LIB_VERSION("MsgDialog", 1, "MsgDialog", 1, 1);

namespace MsgDialog = Dialog::MsgDialog;

static void RegisterMsgDialogFuncs(Loader::SymbolDatabase* s)
{
	// NIDs from public PS5-3.20_Libs libSceMsgDialog.c sprx_dlsym table.
	LIB_FUNC("lDqxaY1UbEo", MsgDialog::MsgDialogInitialize);
	LIB_FUNC("b06Hh0DPEaE", MsgDialog::MsgDialogOpen);
	LIB_FUNC("Lr8ovHH9l6A", MsgDialog::MsgDialogGetResult);
	LIB_FUNC("CWVW78Qc3fI", MsgDialog::MsgDialogGetStatus);
	LIB_FUNC("6fIC3XKt2k0", MsgDialog::MsgDialogUpdateStatus);
	LIB_FUNC("HTrcDKlFKuM", MsgDialog::MsgDialogClose);
	LIB_FUNC("ePw-kqZmelo", MsgDialog::MsgDialogTerminate);
}

LIB_DEFINE(InitDialog_1_MsgDialog)
{
	RegisterMsgDialogFuncs(s);
}

} // namespace LibMsgDialog

namespace LibMsgDialogNative {

LIB_VERSION("MsgDialog.native", 1, "MsgDialog", 1, 1);

namespace MsgDialog = Dialog::MsgDialog;

LIB_DEFINE(InitDialog_1_MsgDialogNative)
{
	// Gen5 imports MsgDialog.native_v1 (Astro Bot post-window path).
	LIB_FUNC("lDqxaY1UbEo", MsgDialog::MsgDialogInitialize);
	LIB_FUNC("b06Hh0DPEaE", MsgDialog::MsgDialogOpen);
	LIB_FUNC("Lr8ovHH9l6A", MsgDialog::MsgDialogGetResult);
	LIB_FUNC("CWVW78Qc3fI", MsgDialog::MsgDialogGetStatus);
	LIB_FUNC("6fIC3XKt2k0", MsgDialog::MsgDialogUpdateStatus);
	LIB_FUNC("HTrcDKlFKuM", MsgDialog::MsgDialogClose);
	LIB_FUNC("ePw-kqZmelo", MsgDialog::MsgDialogTerminate);
}

} // namespace LibMsgDialogNative

LIB_DEFINE(InitDialog_1)
{
	LibCommonDialog::InitDialog_1_CommonDialog(s);
	LibSaveDataDialog::InitDialog_1_SaveDataDialog(s);
	LibSaveDataDialogNative::InitDialog_1_SaveDataDialogNative(s);
	LibMsgDialog::InitDialog_1_MsgDialog(s);
	LibMsgDialogNative::InitDialog_1_MsgDialogNative(s);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
