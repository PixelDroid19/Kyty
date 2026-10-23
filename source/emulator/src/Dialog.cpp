#include "Emulator/Dialog.h"

#include "Kyty/Core/String.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/Ports/KeyboardTextPort.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

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

int KYTY_SYSV_ABI CommonDialogIsUsed()
{
	PRINT_NAME();
	return OK;
}

} // namespace CommonDialog

namespace ImeDialog {

LIB_NAME("ImeDialog", "ImeDialog");

// Results the system returns, as titles map them to SCE_IME_ERROR_* names.
constexpr int IME_ERROR_BUSY                    = static_cast<int>(0x80BC0001u);
constexpr int IME_ERROR_INVALID_MAX_TEXT_LENGTH = static_cast<int>(0x80BC0016u);
constexpr int IME_ERROR_INVALID_INPUT_TEXT_BUFFER = static_cast<int>(0x80BC0017u);
constexpr int IME_ERROR_INVALID_ADDRESS         = static_cast<int>(0x80BC0031u);

// USB HID usages that edit the text instead of typing a character.
constexpr uint16_t KEY_USAGE_ENTER        = 0x28;
constexpr uint16_t KEY_USAGE_BACKSPACE    = 0x2a;
constexpr uint16_t KEY_USAGE_KEYPAD_ENTER = 0x58;

struct ImeDialogState
{
	int            status     = STATUS_NONE;
	uint32_t       end_status = END_STATUS_OK;
	char16_t*      buffer     = nullptr;
	uint32_t       max_length = 0;
	std::u16string text;
};

static std::mutex     g_mutex;
static ImeDialogState g_dialog;

static void FinishLocked(uint32_t end_status)
{
	g_dialog.status     = STATUS_FINISHED;
	g_dialog.end_status = end_status;
	Emulator::Ports::KeyboardTextPort::SetDialogOpen(false);
}

// Applies the host keys typed since the last poll; Enter confirms the text.
static void ApplyHostKeysLocked()
{
	Emulator::Ports::KeyboardTextKey keys[Emulator::Ports::KeyboardTextPort::QUEUE_MAX];
	const size_t num     = Emulator::Ports::KeyboardTextPort::Drain(keys, Emulator::Ports::KeyboardTextPort::QUEUE_MAX);
	bool         changed = false;
	for (size_t i = 0; i < num && g_dialog.status == STATUS_RUNNING; i++)
	{
		const auto& key = keys[i];
		if (!key.down)
		{
			continue;
		}
		if (key.usage == KEY_USAGE_ENTER || key.usage == KEY_USAGE_KEYPAD_ENTER)
		{
			std::memcpy(g_dialog.buffer, g_dialog.text.data(), g_dialog.text.size() * sizeof(char16_t));
			g_dialog.buffer[g_dialog.text.size()] = u'\0';
			FinishLocked(END_STATUS_OK);
			return;
		}
		if (key.usage == KEY_USAGE_BACKSPACE)
		{
			if (!g_dialog.text.empty())
			{
				g_dialog.text.pop_back();
				changed = true;
			}
		} else if (key.character != 0 && g_dialog.text.size() < g_dialog.max_length)
		{
			g_dialog.text.push_back(static_cast<char16_t>(key.character));
			changed = true;
		}
	}
	if (changed)
	{
		Emulator::Ports::KeyboardTextPort::SetDialogText(g_dialog.text);
	}
}

int KYTY_SYSV_ABI ImeDialogInit(const ImeDialogParam* param, const void* /*extended*/)
{
	PRINT_NAME();
	if (param == nullptr)
	{
		return IME_ERROR_INVALID_ADDRESS;
	}
	if (param->input_text_buffer == nullptr)
	{
		return IME_ERROR_INVALID_INPUT_TEXT_BUFFER;
	}
	if (param->max_text_length == 0)
	{
		return IME_ERROR_INVALID_MAX_TEXT_LENGTH;
	}
	std::lock_guard lock(g_mutex);
	if (g_dialog.status != STATUS_NONE)
	{
		return IME_ERROR_BUSY;
	}
	// No on-screen keyboard exists: the host keyboard edits the text the title placed
	// in the buffer, and the dialog runs until the user confirms it.
	g_dialog            = {};
	g_dialog.status     = STATUS_RUNNING;
	g_dialog.buffer     = param->input_text_buffer;
	g_dialog.max_length = param->max_text_length;
	for (uint32_t i = 0; i < param->max_text_length && param->input_text_buffer[i] != u'\0'; i++)
	{
		g_dialog.text.push_back(param->input_text_buffer[i]);
	}
	Emulator::Ports::KeyboardTextPort::SetDialogOpen(true);
	Emulator::Ports::KeyboardTextPort::SetDialogText(g_dialog.text);
	return OK;
}

// Titles poll the status on their own thread, which is where the typed keys apply.
int KYTY_SYSV_ABI ImeDialogGetStatus()
{
	PRINT_NAME();
	std::lock_guard lock(g_mutex);
	if (g_dialog.status == STATUS_RUNNING)
	{
		ApplyHostKeysLocked();
	}
	return g_dialog.status;
}

int KYTY_SYSV_ABI ImeDialogGetResult(ImeDialogResult* result)
{
	PRINT_NAME();
	std::lock_guard lock(g_mutex);
	if (result == nullptr || g_dialog.status != STATUS_FINISHED)
	{
		EXIT("sceImeDialogGetResult: no finished dialog (result=%p status %d)\n", static_cast<void*>(result), g_dialog.status);
	}
	std::memset(result, 0, sizeof(*result));
	result->end_status = g_dialog.end_status;
	return OK;
}

int KYTY_SYSV_ABI ImeDialogAbort()
{
	PRINT_NAME();
	std::lock_guard lock(g_mutex);
	if (g_dialog.status == STATUS_NONE)
	{
		EXIT("sceImeDialogAbort: no dialog is open\n");
	}
	FinishLocked(END_STATUS_ABORTED);
	return OK;
}

int KYTY_SYSV_ABI ImeDialogTerm()
{
	PRINT_NAME();
	std::lock_guard lock(g_mutex);
	g_dialog = {};
	Emulator::Ports::KeyboardTextPort::SetDialogOpen(false);
	return OK;
}

} // namespace ImeDialog

namespace ErrorDialog {

LIB_NAME("ErrorDialog", "ErrorDialog");

static std::atomic<int> g_status {CommonDialog::STATUS_NONE};
static std::atomic<bool> g_initialized {false};

int KYTY_SYSV_ABI ErrorDialogInitialize()
{
	PRINT_NAME();
	if (g_initialized.exchange(true, std::memory_order_acq_rel))
	{
		return static_cast<int32_t>(0x80ED0001u);
	}
	g_status.store(CommonDialog::STATUS_INITIALIZED, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI ErrorDialogOpen(const void* param)
{
	PRINT_NAME();
	if (param == nullptr)
	{
		return static_cast<int32_t>(0x80ED0005u);
	}
	if (!g_initialized.load(std::memory_order_acquire))
	{
		return static_cast<int32_t>(0x80ED0002u);
	}
	g_status.store(CommonDialog::STATUS_FINISHED, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI ErrorDialogGetStatus()
{
	PRINT_NAME();
	return g_status.load(std::memory_order_acquire);
}

int KYTY_SYSV_ABI ErrorDialogUpdateStatus()
{
	PRINT_NAME();
	return g_status.load(std::memory_order_acquire);
}

int KYTY_SYSV_ABI ErrorDialogClose()
{
	PRINT_NAME();
	g_status.store(CommonDialog::STATUS_FINISHED, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI ErrorDialogTerminate()
{
	PRINT_NAME();
	g_status.store(CommonDialog::STATUS_NONE, std::memory_order_release);
	g_initialized.store(false, std::memory_order_release);
	return OK;
}

} // namespace ErrorDialog

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
		KYTY_LOG_DEBUG("\t base_size = 0x%016" PRIx64 " mode = %u (rejected)\n", param->base_size, param->mode);
		return CommonDialog::ERROR_PARAM_INVALID;
	}

	KYTY_LOG_DEBUG("\t base_size = 0x%016" PRIx64 "\n", param->base_size);
	KYTY_LOG_DEBUG("\t size      = %u\n", param->size);
	KYTY_LOG_DEBUG("\t mode      = %u\n", param->mode);
	KYTY_LOG_DEBUG("\t disp_type = %u\n", param->disp_type);

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

int KYTY_SYSV_ABI SaveDataDialogClose()
{
	PRINT_NAME();
	return SaveDataDialogTerminate();
}

int KYTY_SYSV_ABI SaveDataDialogGetResult(void* /*result*/)
{
	PRINT_NAME();
	if (g_status.load(std::memory_order_acquire) != CommonDialog::STATUS_FINISHED)
	{
		return CommonDialog::ERROR_NOT_FINISHED;
	}
	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogIsReadyToDisplay(int* ready)
{
	PRINT_NAME();
	if (ready == nullptr)
	{
		return CommonDialog::ERROR_ARG_NULL;
	}
	*ready = 1;
	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogProgressBarInc(int target, uint32_t delta)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t target = %d\n", target);
	KYTY_LOG_DEBUG("\t delta  = %u\n", delta);
	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogProgressBarSetValue(int target, uint32_t rate)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t target = %d\n", target);
	KYTY_LOG_DEBUG("\t rate   = %u\n", rate);

	const int status = g_status.load(std::memory_order_acquire);
	if (status != CommonDialog::STATUS_RUNNING && status != CommonDialog::STATUS_FINISHED)
	{
		return CommonDialog::ERROR_NOT_RUNNING;
	}

	return OK;
}

} // namespace SaveDataDialog

namespace MsgDialog {

LIB_NAME("MsgDialog", "MsgDialog");

static std::atomic<int> g_status {CommonDialog::STATUS_NONE};

int KYTY_SYSV_ABI MsgDialogInitialize()
{
	PRINT_NAME();

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

int KYTY_SYSV_ABI MsgDialogOpen(const MsgDialogParam* param)
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

	if (param->base_size != 0x30)
	{
		KYTY_LOG_DEBUG("\t base_size = 0x%016" PRIx64 " (rejected)\n", param->base_size);
		return CommonDialog::ERROR_PARAM_INVALID;
	}

	KYTY_LOG_DEBUG("\t base_size = 0x%016" PRIx64 "\n", param->base_size);
	KYTY_LOG_DEBUG("\t size      = %u\n", param->size);
	KYTY_LOG_DEBUG("\t mode      = %u\n", param->mode);

	g_status.store(CommonDialog::STATUS_FINISHED, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI MsgDialogGetStatus()
{
	PRINT_NAME();
	return g_status.load(std::memory_order_acquire);
}

int KYTY_SYSV_ABI MsgDialogUpdateStatus()
{
	PRINT_NAME();
	return g_status.load(std::memory_order_acquire);
}

int KYTY_SYSV_ABI MsgDialogGetResult(MsgDialogResult* result)
{
	PRINT_NAME();

	if (g_status.load(std::memory_order_acquire) != CommonDialog::STATUS_FINISHED)
	{
		return CommonDialog::ERROR_NOT_FINISHED;
	}
	if (result == nullptr)
	{
		return CommonDialog::ERROR_ARG_NULL;
	}

	result->size      = sizeof(MsgDialogResult);
	result->button_id = 0;
	return OK;
}

int KYTY_SYSV_ABI MsgDialogClose()
{
	PRINT_NAME();

	const int status = g_status.load(std::memory_order_acquire);
	if (status != CommonDialog::STATUS_RUNNING && status != CommonDialog::STATUS_FINISHED)
	{
		return CommonDialog::ERROR_NOT_RUNNING;
	}

	g_status.store(CommonDialog::STATUS_FINISHED, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI MsgDialogTerminate()
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

int KYTY_SYSV_ABI MsgDialogProgressBarInc(int target, uint32_t delta)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t target = %d\n", target);
	KYTY_LOG_DEBUG("\t delta  = %u\n", delta);
	return OK;
}

int KYTY_SYSV_ABI MsgDialogProgressBarSetMsg(int target, const char* msg)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t target = %d\n", target);
	KYTY_LOG_DEBUG("\t msg    = %s\n", msg != nullptr ? msg : "(null)");
	return OK;
}

int KYTY_SYSV_ABI MsgDialogProgressBarSetValue(int target, uint32_t value)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t target = %d\n", target);
	KYTY_LOG_DEBUG("\t value  = %u\n", value);
	return OK;
}

} // namespace MsgDialog

namespace SigninDialog {

LIB_NAME("SigninDialog", "SigninDialog");

static std::atomic<int> g_status {CommonDialog::STATUS_NONE};

int KYTY_SYSV_ABI SigninDialogInitialize()
{
	PRINT_NAME();

	if (!CommonDialog::CommonDialogIsSystemInitialized())
	{
		return CommonDialog::ERROR_NOT_SYSTEM_INITIALIZED;
	}

	int expected = CommonDialog::STATUS_NONE;
	if (!g_status.compare_exchange_strong(expected, CommonDialog::STATUS_INITIALIZED, std::memory_order_acq_rel))
	{
		return CommonDialog::ERROR_ALREADY_INITIALIZED;
	}
	return OK;
}

int KYTY_SYSV_ABI SigninDialogOpen(const SigninDialogParam* param)
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

	KYTY_LOG_DEBUG("\t size    = %u\n", param->size);
	KYTY_LOG_DEBUG("\t user_id = %d\n", param->user_id);

	g_status.store(CommonDialog::STATUS_FINISHED, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI SigninDialogGetStatus()
{
	PRINT_NAME();
	return g_status.load(std::memory_order_acquire);
}

int KYTY_SYSV_ABI SigninDialogUpdateStatus()
{
	PRINT_NAME();
	return g_status.load(std::memory_order_acquire);
}

int KYTY_SYSV_ABI SigninDialogGetResult(SigninDialogResult* result)
{
	PRINT_NAME();

	if (g_status.load(std::memory_order_acquire) != CommonDialog::STATUS_FINISHED)
	{
		return CommonDialog::ERROR_NOT_FINISHED;
	}
	if (result == nullptr)
	{
		return CommonDialog::ERROR_ARG_NULL;
	}

	*result        = SigninDialogResult {};
	result->result = RESULT_USER_CANCELED;
	return OK;
}

int KYTY_SYSV_ABI SigninDialogClose()
{
	PRINT_NAME();

	const int status = g_status.load(std::memory_order_acquire);
	if (status != CommonDialog::STATUS_RUNNING && status != CommonDialog::STATUS_FINISHED)
	{
		return CommonDialog::ERROR_NOT_RUNNING;
	}

	g_status.store(CommonDialog::STATUS_FINISHED, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI SigninDialogTerminate()
{
	PRINT_NAME();

	if (g_status.exchange(CommonDialog::STATUS_NONE, std::memory_order_acq_rel) == CommonDialog::STATUS_NONE)
	{
		return CommonDialog::ERROR_NOT_INITIALIZED;
	}
	return OK;
}

} // namespace SigninDialog

} // namespace Kyty::Libs::Dialog

#endif // KYTY_EMU_ENABLED
