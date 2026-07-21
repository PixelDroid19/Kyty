#ifndef EMULATOR_INCLUDE_EMULATOR_DIALOG_H_
#define EMULATOR_INCLUDE_EMULATOR_DIALOG_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Dialog {

namespace CommonDialog {

// sceCommonDialog status values (shared by save-data dialog UpdateStatus).
constexpr int STATUS_NONE        = 0;
constexpr int STATUS_INITIALIZED = 1;
constexpr int STATUS_RUNNING     = 2;
constexpr int STATUS_FINISHED    = 3;

// sceCommonDialog error codes (psdevwiki facility 0x80B8).
constexpr int ERROR_NOT_SYSTEM_INITIALIZED     = static_cast<int32_t>(0x80B80001u);
constexpr int ERROR_ALREADY_SYSTEM_INITIALIZED = static_cast<int32_t>(0x80B80002u);
constexpr int ERROR_NOT_INITIALIZED            = static_cast<int32_t>(0x80B80003u);
constexpr int ERROR_ALREADY_INITIALIZED        = static_cast<int32_t>(0x80B80004u);
constexpr int ERROR_NOT_FINISHED               = static_cast<int32_t>(0x80B80005u);
constexpr int ERROR_INVALID_STATE              = static_cast<int32_t>(0x80B80006u);
constexpr int ERROR_BUSY                       = static_cast<int32_t>(0x80B80008u);
constexpr int ERROR_PARAM_INVALID              = static_cast<int32_t>(0x80B8000Au);
constexpr int ERROR_NOT_RUNNING                = static_cast<int32_t>(0x80B8000Bu);
constexpr int ERROR_ARG_NULL                   = static_cast<int32_t>(0x80B8000Du);

int KYTY_SYSV_ABI CommonDialogInitialize();
bool              CommonDialogIsSystemInitialized();
int KYTY_SYSV_ABI CommonDialogIsUsed();

} // namespace CommonDialog

namespace ErrorDialog {

int KYTY_SYSV_ABI ErrorDialogInitialize();
int KYTY_SYSV_ABI ErrorDialogOpen(const void* param);
int KYTY_SYSV_ABI ErrorDialogGetStatus();
int KYTY_SYSV_ABI ErrorDialogUpdateStatus();
int KYTY_SYSV_ABI ErrorDialogClose();
int KYTY_SYSV_ABI ErrorDialogTerminate();

} // namespace ErrorDialog

namespace SaveDataDialog {

// Observed OrbisSaveDataDialogParam envelope (BaseParam + mode fields).
// BaseParam.size is 0x30; outer size and mode follow. Full nested mode
// payloads are not decoded until a workload depends on them.
struct SaveDataDialogParam
{
	// CommonDialog::BaseParam
	uint64_t base_size;
	uint8_t  base_reserved[36];
	uint32_t base_magic;
	// SaveDataDialogParam
	uint32_t size;
	uint32_t mode;
	uint32_t disp_type;
	// Remainder of the structure is mode-specific and not required for
	// open/finish acceptance; keep the known head only for validation.
};

int KYTY_SYSV_ABI SaveDataDialogInitialize();
int KYTY_SYSV_ABI SaveDataDialogOpen(const SaveDataDialogParam* param);
int KYTY_SYSV_ABI SaveDataDialogGetStatus();
int KYTY_SYSV_ABI SaveDataDialogUpdateStatus();
int KYTY_SYSV_ABI SaveDataDialogTerminate();
int KYTY_SYSV_ABI SaveDataDialogClose();
int KYTY_SYSV_ABI SaveDataDialogGetResult(void* result);
int KYTY_SYSV_ABI SaveDataDialogIsReadyToDisplay(int* ready);
int KYTY_SYSV_ABI SaveDataDialogProgressBarInc(int target, uint32_t delta);
int KYTY_SYSV_ABI SaveDataDialogProgressBarSetValue(int target, uint32_t rate);

} // namespace SaveDataDialog

namespace MsgDialog {

struct MsgDialogResult
{
	uint32_t size;
	uint32_t button_id;
};

struct MsgDialogParam
{
	uint64_t base_size;
	uint8_t  base_reserved[36];
	uint32_t base_magic;
	uint32_t size;
	uint32_t mode;
};

int KYTY_SYSV_ABI MsgDialogInitialize();
int KYTY_SYSV_ABI MsgDialogOpen(const MsgDialogParam* param);
int KYTY_SYSV_ABI MsgDialogGetStatus();
int KYTY_SYSV_ABI MsgDialogUpdateStatus();
int KYTY_SYSV_ABI MsgDialogGetResult(MsgDialogResult* result);
int KYTY_SYSV_ABI MsgDialogClose();
int KYTY_SYSV_ABI MsgDialogTerminate();
int KYTY_SYSV_ABI MsgDialogProgressBarInc(int target, uint32_t delta);
int KYTY_SYSV_ABI MsgDialogProgressBarSetMsg(int target, const char* msg);
int KYTY_SYSV_ABI MsgDialogProgressBarSetValue(int target, uint32_t value);

} // namespace MsgDialog

} // namespace Kyty::Libs::Dialog

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_DIALOG_H_ */
