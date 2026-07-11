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
	LIB_FUNC("KK3Bdg1RWK0", SaveDataDialog::SaveDataDialogUpdateStatus);
	LIB_FUNC("YuH2FA7azqQ", SaveDataDialog::SaveDataDialogTerminate);
	LIB_FUNC("hay1CfTmLyA", SaveDataDialog::SaveDataDialogProgressBarSetValue);
}

} // namespace LibSaveDataDialogNative

LIB_DEFINE(InitDialog_1)
{
	LibCommonDialog::InitDialog_1_CommonDialog(s);
	LibSaveDataDialog::InitDialog_1_SaveDataDialog(s);
	LibSaveDataDialogNative::InitDialog_1_SaveDataDialogNative(s);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
