#include "Emulator/Common.h"
#include "Emulator/Dialog.h"
#include "Emulator/Libs/LibraryRegistration.h"
#include "Emulator/Libs/Libs.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace LibCommonDialog {

LIB_VERSION("CommonDialog", 1, "CommonDialog", 1, 1);

namespace CommonDialog = Dialog::CommonDialog;

LIB_DEFINE(InitDialog_1_CommonDialog)
{
	LIB_FUNC("uoUpLGNkygk", CommonDialog::CommonDialogInitialize);
	LIB_FUNC("BQ3tey0JmQM", CommonDialog::CommonDialogIsUsed);
}

} // namespace LibCommonDialog

namespace LibImeDialog {

LIB_VERSION("ImeDialog", 1, "ImeDialog", 1, 1);

namespace ImeDialog = Dialog::ImeDialog;

LIB_DEFINE(InitDialog_1_ImeDialog)
{
	LIB_FUNC("IADmD4tScBY", ImeDialog::ImeDialogGetStatus);
}

} // namespace LibImeDialog

namespace LibErrorDialog {

LIB_VERSION("ErrorDialog", 1, "ErrorDialog", 1, 1);

namespace ErrorDialog = Dialog::ErrorDialog;

LIB_DEFINE(InitDialog_1_ErrorDialog)
{
	LIB_FUNC("I88KChlynSs", ErrorDialog::ErrorDialogInitialize);
	LIB_FUNC("M2ZF-ClLhgY", ErrorDialog::ErrorDialogOpen);
	LIB_FUNC("t2FvHRXzgqk", ErrorDialog::ErrorDialogGetStatus);
	LIB_FUNC("WWiGuh9XfgQ", ErrorDialog::ErrorDialogUpdateStatus);
	LIB_FUNC("ekXHb1kDBl0", ErrorDialog::ErrorDialogClose);
	LIB_FUNC("9XAxK2PMwk8", ErrorDialog::ErrorDialogTerminate);
}

} // namespace LibErrorDialog

namespace {

constexpr LibraryIdentity SAVE_DATA_DIALOG        = {"SaveDataDialog", 1, "SaveDataDialog", 1, 1};
constexpr LibraryIdentity SAVE_DATA_DIALOG_NATIVE = {"SaveDataDialog.native", 1, "SaveDataDialog", 1, 1};
constexpr LibraryIdentity MSG_DIALOG              = {"MsgDialog", 1, "MsgDialog", 1, 1};
constexpr LibraryIdentity MSG_DIALOG_NATIVE       = {"MsgDialog.native", 1, "MsgDialog", 1, 1};

void RegisterSaveDataDialogFunctions(Loader::SymbolDatabase* symbols, const LibraryIdentity& identity)
{
	using namespace Dialog::SaveDataDialog;
	RegisterLibraryFunction(symbols, identity, "s9e3+YpRnzw", SaveDataDialogInitialize,
	                        U"Dialog::SaveDataDialog::SaveDataDialogInitialize");
	RegisterLibraryFunction(symbols, identity, "4tPhsP6FpDI", SaveDataDialogOpen, U"Dialog::SaveDataDialog::SaveDataDialogOpen");
	RegisterLibraryFunction(symbols, identity, "ERKzksauAJA", SaveDataDialogGetStatus, U"Dialog::SaveDataDialog::SaveDataDialogGetStatus");
	RegisterLibraryFunction(symbols, identity, "KK3Bdg1RWK0", SaveDataDialogUpdateStatus,
	                        U"Dialog::SaveDataDialog::SaveDataDialogUpdateStatus");
	RegisterLibraryFunction(symbols, identity, "YuH2FA7azqQ", SaveDataDialogTerminate, U"Dialog::SaveDataDialog::SaveDataDialogTerminate");
	RegisterLibraryFunction(symbols, identity, "fH46Lag88XY", SaveDataDialogClose, U"Dialog::SaveDataDialog::SaveDataDialogClose");
	RegisterLibraryFunction(symbols, identity, "yEiJ-qqr6Cg", SaveDataDialogGetResult, U"Dialog::SaveDataDialog::SaveDataDialogGetResult");
	RegisterLibraryFunction(symbols, identity, "en7gNVnh878", SaveDataDialogIsReadyToDisplay,
	                        U"Dialog::SaveDataDialog::SaveDataDialogIsReadyToDisplay");
	RegisterLibraryFunction(symbols, identity, "V-uEeFKARJU", SaveDataDialogProgressBarInc,
	                        U"Dialog::SaveDataDialog::SaveDataDialogProgressBarInc");
	RegisterLibraryFunction(symbols, identity, "hay1CfTmLyA", SaveDataDialogProgressBarSetValue,
	                        U"Dialog::SaveDataDialog::SaveDataDialogProgressBarSetValue");
}

void RegisterMsgDialogFunctions(Loader::SymbolDatabase* symbols, const LibraryIdentity& identity)
{
	using namespace Dialog::MsgDialog;
	RegisterLibraryFunction(symbols, identity, "lDqxaY1UbEo", MsgDialogInitialize, U"Dialog::MsgDialog::MsgDialogInitialize");
	RegisterLibraryFunction(symbols, identity, "b06Hh0DPEaE", MsgDialogOpen, U"Dialog::MsgDialog::MsgDialogOpen");
	RegisterLibraryFunction(symbols, identity, "CWVW78Qc3fI", MsgDialogGetStatus, U"Dialog::MsgDialog::MsgDialogGetStatus");
	RegisterLibraryFunction(symbols, identity, "6fIC3XKt2k0", MsgDialogUpdateStatus, U"Dialog::MsgDialog::MsgDialogUpdateStatus");
	RegisterLibraryFunction(symbols, identity, "Lr8ovHH9l6A", MsgDialogGetResult, U"Dialog::MsgDialog::MsgDialogGetResult");
	RegisterLibraryFunction(symbols, identity, "HTrcDKlFKuM", MsgDialogClose, U"Dialog::MsgDialog::MsgDialogClose");
	RegisterLibraryFunction(symbols, identity, "ePw-kqZmelo", MsgDialogTerminate, U"Dialog::MsgDialog::MsgDialogTerminate");
	RegisterLibraryFunction(symbols, identity, "Gc5k1qcK4fs", MsgDialogProgressBarInc, U"Dialog::MsgDialog::MsgDialogProgressBarInc");
	RegisterLibraryFunction(symbols, identity, "6H-71OdrpXM", MsgDialogProgressBarSetMsg, U"Dialog::MsgDialog::MsgDialogProgressBarSetMsg");
	RegisterLibraryFunction(symbols, identity, "wTpfglkmv34", MsgDialogProgressBarSetValue,
	                        U"Dialog::MsgDialog::MsgDialogProgressBarSetValue");
}

} // namespace

namespace LibSaveDataDialog {

LIB_DEFINE(InitDialog_1_SaveDataDialog)
{
	RegisterSaveDataDialogFunctions(s, SAVE_DATA_DIALOG);
}

} // namespace LibSaveDataDialog

namespace LibSaveDataDialogNative {

LIB_DEFINE(InitDialog_1_SaveDataDialogNative)
{
	RegisterSaveDataDialogFunctions(s, SAVE_DATA_DIALOG_NATIVE);
}

} // namespace LibSaveDataDialogNative

namespace LibMsgDialog {

LIB_DEFINE(InitDialog_1_MsgDialog)
{
	RegisterMsgDialogFunctions(s, MSG_DIALOG);
}

} // namespace LibMsgDialog

namespace LibMsgDialogNative {

LIB_DEFINE(InitDialog_1_MsgDialogNative)
{
	RegisterMsgDialogFunctions(s, MSG_DIALOG_NATIVE);
}

} // namespace LibMsgDialogNative

LIB_DEFINE(InitDialog_1)
{
	LibCommonDialog::InitDialog_1_CommonDialog(s);
	LibImeDialog::InitDialog_1_ImeDialog(s);
	LibErrorDialog::InitDialog_1_ErrorDialog(s);
	LibSaveDataDialog::InitDialog_1_SaveDataDialog(s);
	LibSaveDataDialogNative::InitDialog_1_SaveDataDialogNative(s);
	LibMsgDialog::InitDialog_1_MsgDialog(s);
	LibMsgDialogNative::InitDialog_1_MsgDialogNative(s);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
