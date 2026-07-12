#include "Emulator/Dialog.h"

#include "Kyty/Core/String.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <atomic>
#include <cstdio>

#ifdef KYTY_EMU_ENABLED

// NOLINTNEXTLINE(modernize-concat-nested-namespaces)
namespace Kyty::Libs::Dialog {

namespace CommonDialog {

LIB_NAME("CommonDialog", "CommonDialog");

static std::atomic<bool> g_system_initialized {false};

int KYTY_SYSV_ABI CommonDialogInitialize()
{
	PRINT_NAME();

	if (g_system_initialized.load(std::memory_order_acquire))
	{
		return ERROR_ALREADY_SYSTEM_INITIALIZED;
	}

	g_system_initialized.store(true, std::memory_order_release);
	return OK;
}

bool CommonDialogIsSystemInitialized()
{
	return g_system_initialized.load(std::memory_order_acquire);
}

} // namespace CommonDialog

namespace SaveDataDialog {

LIB_NAME("SaveDataDialog", "SaveDataDialog");

static std::atomic<int> g_status {CommonDialog::STATUS_NONE};

int KYTY_SYSV_ABI SaveDataDialogInitialize()
{
	PRINT_NAME();

	// sceSaveDataDialogInitialize requires the common-dialog system to be up.
	if (!CommonDialog::CommonDialogIsSystemInitialized())
	{
		return CommonDialog::ERROR_NOT_SYSTEM_INITIALIZED;
	}

	const int status = g_status.load(std::memory_order_acquire);
	if (status != CommonDialog::STATUS_NONE)
	{
		return CommonDialog::ERROR_ALREADY_INITIALIZED;
	}

	g_status.store(CommonDialog::STATUS_INITIALIZED, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogOpen(const SaveDataDialogParam* param)
{
	PRINT_NAME();

	if (param == nullptr)
	{
		return CommonDialog::ERROR_ARG_NULL;
	}

	const int status = g_status.load(std::memory_order_acquire);
	if (status != CommonDialog::STATUS_INITIALIZED && status != CommonDialog::STATUS_FINISHED)
	{
		return CommonDialog::ERROR_INVALID_STATE;
	}

	// Observed guest envelopes use BaseParam.size == 0x30 and a non-zero mode.
	// Reject clearly invalid envelopes; deeper mode payload decoding is deferred.
	if (param->base_size != 0x30 || param->mode == 0)
	{
		printf("\t base_size = 0x%016" PRIx64 " mode = %u (rejected)\n", param->base_size, param->mode);
		return CommonDialog::ERROR_PARAM_INVALID;
	}

	printf("\t base_size = 0x%016" PRIx64 "\n", param->base_size);
	printf("\t size      = %u\n", param->size);
	printf("\t mode      = %u\n", param->mode);
	printf("\t disp_type = %u\n", param->disp_type);

	// Host has no SCE dialog compositor. Complete immediately so polling loops
	// that wait for FINISHED can proceed; guest-visible status is FINISHED.
	g_status.store(CommonDialog::STATUS_FINISHED, std::memory_order_release);
	return OK;
}

// sceSaveDataDialogGetStatus — read-only status (does not drive the dialog).
// NID ERKzksauAJA on SaveDataDialog / SaveDataDialog.native.
int KYTY_SYSV_ABI SaveDataDialogGetStatus()
{
	PRINT_NAME();

	return g_status.load(std::memory_order_acquire);
}

int KYTY_SYSV_ABI SaveDataDialogUpdateStatus()
{
	PRINT_NAME();

	return g_status.load(std::memory_order_acquire);
}

int KYTY_SYSV_ABI SaveDataDialogTerminate()
{
	PRINT_NAME();

	const int status = g_status.load(std::memory_order_acquire);
	if (status == CommonDialog::STATUS_NONE)
	{
		return CommonDialog::ERROR_NOT_INITIALIZED;
	}

	g_status.store(CommonDialog::STATUS_NONE, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogProgressBarSetValue(int target, uint32_t rate)
{
	PRINT_NAME();

	printf("\t target = %d\n", target);
	printf("\t rate   = %u\n", rate);

	const int status = g_status.load(std::memory_order_acquire);
	if (status != CommonDialog::STATUS_RUNNING && status != CommonDialog::STATUS_FINISHED)
	{
		return CommonDialog::ERROR_NOT_RUNNING;
	}

	return OK;
}

} // namespace SaveDataDialog

} // namespace Kyty::Libs::Dialog

#endif // KYTY_EMU_ENABLED
