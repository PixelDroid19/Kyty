#include "Emulator/Controller.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <cstdlib>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Controller {

LIB_NAME("Pad", "Pad");

struct PadData
{
	uint32_t buttons;
	uint8_t  left_stick_x;
	uint8_t  left_stick_y;
	uint8_t  right_stick_x;
	uint8_t  right_stick_y;
	uint8_t  analog_buttons_l2;
	uint8_t  analog_buttons_r2;
	uint8_t  padding[2];
	float    orientation_x;
	float    orientation_y;
	float    orientation_z;
	float    orientation_w;
	float    acceleration_x;
	float    acceleration_y;
	float    acceleration_z;
	float    angular_velocity_x;
	float    angular_velocity_y;
	float    angular_velocity_z;
	uint8_t  touch_data_touch_num;
	uint8_t  touch_data_reserve[3];
	uint32_t touch_data_reserve1;
	uint16_t touch_data_touch0_x;
	uint16_t touch_data_touch0_y;
	uint8_t  touch_data_touch0_id;
	uint8_t  touch_data_touch0_reserve[3];
	uint16_t touch_data_touch1_x;
	uint16_t touch_data_touch1_y;
	uint8_t  touch_data_touch1_id;
	uint8_t  touch_data_touch1_reserve[3];
	bool     connected;
	uint64_t timestamp;
	uint32_t extension_unit_data_extension_unit_id;
	uint8_t  extension_unit_data_reserve[1];
	uint8_t  extension_unit_data_data_length;
	uint8_t  extension_unit_data_data[10];
	uint8_t  connected_count;
	uint8_t  reserve[2];
	uint8_t  device_unique_data_len;
	uint8_t  device_unique_data[12];
};

struct PadControllerInformation
{
	float    touch_pixel_density;
	uint16_t touch_resolution_x;
	uint16_t touch_resolution_y;
	uint8_t  stick_dead_zone_left;
	uint8_t  stick_dead_zone_right;
	uint8_t  connection_type;
	uint8_t  connected_count;
	bool     connected;
	int      device_class;
};

struct PadVibrationParam
{
	uint8_t large_motor;
	uint8_t small_motor;
};

struct ControllerState
{
	uint64_t time                                  = 0;
	uint32_t buttons                               = 0;
	int      axes[static_cast<int>(Axis::AxisMax)] = {128, 128, 128, 128, 0, 0};
};

class GameController
{
public:
	GameController()          = default;
	virtual ~GameController() = default;

	KYTY_CLASS_NO_COPY(GameController);

	void Connect(int id);
	void Disconnect(int id);
	void Button(int id, uint32_t button, bool down);
	void Axis(int id, Axis axis, int value);
	void GetConnectionInfo(bool* flag, int* count);
	void ReadState(ControllerState* state, bool* flag, int* count);
	int  ReadStates(ControllerState* states, int states_num, bool* flag, int* count);

private:
	static constexpr uint32_t STATES_MAX = 64;

	struct StatePrivate
	{
		bool obtained = false;
	};

	void                          CheckActive();
	[[nodiscard]] ControllerState GetLastState() const;
	void                          AddState(const ControllerState& state);

	Core::Mutex     m_mutex;
	Vector<int>     m_connected_ids;
	int             m_active_id       = -1;
	bool            m_connected       = false;
	int             m_connected_count = 0;
	ControllerState m_states[STATES_MAX];
	StatePrivate    m_private[STATES_MAX];
	ControllerState m_last_state;
	uint32_t        m_states_num  = 0;
	uint32_t        m_first_state = 0;
};

static GameController* g_controller = nullptr;

KYTY_SUBSYSTEM_INIT(Controller)
{
	EXIT_IF(g_controller != nullptr);

	g_controller = new GameController;
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Controller) {}

KYTY_SUBSYSTEM_DESTROY(Controller) {}

void GameController::Connect(int id)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_connected_ids.Contains(id));

	m_connected_ids.Add(id);

	CheckActive();
}

void GameController::Disconnect(int id)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(!m_connected_ids.Contains(id));

	m_connected_ids.Remove(id);

	CheckActive();
}

void GameController::CheckActive()
{
	bool reset          = false;
	bool next_connected = false;
	int  next_active    = CONTROLLER_KEYBOARD_ID;

	// A real SDL controller owns axes and its own buttons when present. The
	// keyboard remains an independent button source and is merged in Button().
	for (uint32_t i = 0; i < m_connected_ids.Size(); i++)
	{
		const int id = m_connected_ids.At(i);
		if (id != CONTROLLER_KEYBOARD_ID)
		{
			next_active    = id;
			next_connected = true;
			break;
		}
	}

	if (!next_connected && m_connected_ids.Contains(CONTROLLER_KEYBOARD_ID))
	{
		next_connected = true;
	}

	if (m_connected != next_connected || (next_connected && m_active_id != next_active))
	{
		m_active_id = next_active;
		m_connected = next_connected;
		if (next_connected)
		{
			m_connected_count++;
		}
		reset = true;
	}

	if (reset)
	{
		m_states_num = 0;
		m_last_state = ControllerState();
	}
}

ControllerState GameController::GetLastState() const
{
	if (m_states_num == 0)
	{
		return m_last_state;
	}

	auto last = (m_first_state + m_states_num - 1) % STATES_MAX;

	return m_states[last];
}

void GameController::AddState(const ControllerState& state)
{
	if (m_states_num >= STATES_MAX)
	{
		m_states_num  = STATES_MAX - 1;
		m_first_state = (m_first_state + 1) % STATES_MAX;
	}

	auto index = (m_first_state + m_states_num) % STATES_MAX;

	m_states[index] = state;
	m_last_state    = state;

	m_private[index].obtained = false;

	m_states_num++;
}

void GameController::Button(int id, uint32_t button, bool down)
{
	Core::LockGuard lock(m_mutex);

	if (m_active_id == id || id == CONTROLLER_KEYBOARD_ID)
	{
		auto state = GetLastState();

		state.time = LibKernel::KernelGetProcessTime();

		if (down)
		{
			state.buttons |= button;
		} else
		{
			state.buttons &= ~button;
		}

		AddState(state);
	}
}

void GameController::Axis(int id, Controller::Axis axis, int value)
{
	Core::LockGuard lock(m_mutex);

	if (m_active_id == id)
	{
		auto state = GetLastState();

		state.time = LibKernel::KernelGetProcessTime();

		int axis_id = static_cast<int>(axis);

		EXIT_IF(axis_id < 0 || axis_id >= static_cast<int>(Controller::Axis::AxisMax));

		state.axes[axis_id] = value;

		if (axis == Controller::Axis::TriggerLeft)
		{
			if (value > 0)
			{
				state.buttons |= PAD_BUTTON_L2;
			} else
			{
				state.buttons &= ~PAD_BUTTON_L2;
			}
		}

		if (axis == Controller::Axis::TriggerRight)
		{
			if (value > 0)
			{
				state.buttons |= PAD_BUTTON_R2;
			} else
			{
				state.buttons &= ~PAD_BUTTON_R2;
			}
		}

		AddState(state);
	}
}

void GameController::GetConnectionInfo(bool* flag, int* count)
{
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);

	Core::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
}

void GameController::ReadState(ControllerState* state, bool* flag, int* count)
{
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(state == nullptr);

	Core::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
	*state = GetLastState();
}

int GameController::ReadStates(ControllerState* states, int states_num, bool* flag, int* count)
{
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(states == nullptr);
	EXIT_IF(states_num < 1 || states_num > STATES_MAX);

	Core::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;

	int ret_num = 0;

	if (m_connected)
	{
		if (m_states_num == 0)
		{
			ret_num   = 1;
			states[0] = m_last_state;
		} else
		{
			for (uint32_t i = 0; i < m_states_num; i++)
			{
				if (ret_num >= states_num)
				{
					break;
				}
				auto index = (m_first_state + i) % STATES_MAX;
				if (!m_private[index].obtained)
				{
					m_private[index].obtained = true;

					states[ret_num++] = m_states[index];
				}
			}
		}
	}

	return ret_num;
}

void ControllerConnect(int id)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Connect(id);
}

void ControllerDisconnect(int id)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Disconnect(id);
}

void ControllerButton(int id, uint32_t button, bool down)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Button(id, button, down);
}

void ControllerAxis(int id, Axis axis, int value)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Axis(id, axis, value);
}

// SCE_PAD_ERROR_* (public Sony codes; used for ordinary guest failures, not EXIT).
constexpr int PAD_ERROR_INVALID_HANDLE     = static_cast<int>(0x80920003u);
constexpr int PAD_ERROR_NOT_INITIALIZED    = static_cast<int>(0x80920005u);
constexpr int PAD_ERROR_DEVICE_NO_HANDLE   = static_cast<int>(0x80920008u);

// Virtual keyboard/gamepad id: always connected after PadOpen so titles see a pad
// without requiring a physical SDL controller (playability on hosts with only a keyboard).
constexpr int kPrimaryHandle = 1;

static bool g_pad_initialized = false;
static bool g_pad_opened      = false;
// Counts PadReadState calls since last PadOpen (many titles Open then Read twice per tick).
static int  g_reads_since_open = 0;

int KYTY_SYSV_ABI PadInit()
{
	PRINT_NAME();

	g_pad_initialized = true;

	return OK;
}

int KYTY_SYSV_ABI PadOpen(int user_id, int type, int index, const void* param)
{
	PRINT_NAME();

	if (!g_pad_initialized)
	{
		return PAD_ERROR_NOT_INITIALIZED;
	}

	// user_id -1: no logged-in user → no handle (same contract as SCE docs / other HLEs).
	if (user_id == -1)
	{
		return PAD_ERROR_DEVICE_NO_HANDLE;
	}

	EXIT_NOT_IMPLEMENTED(user_id != 1);
	EXIT_NOT_IMPLEMENTED(type != 0);
	EXIT_NOT_IMPLEMENTED(index != 0);
	EXIT_NOT_IMPLEMENTED(param != nullptr);

	// Ensure a virtual pad is present for keyboard input and for connected=true.
	EXIT_IF(g_controller == nullptr);
	bool connected       = false;
	int  connected_count = 0;
	g_controller->GetConnectionInfo(&connected, &connected_count);
	if (!connected)
	{
		g_controller->Connect(CONTROLLER_KEYBOARD_ID);
	}

	// Real OS returns ALREADY_OPENED on re-open; guest must use GetHandle for the prior handle.
	// Observed Gen5 titles poll Open each frame and then ReadState with that handle, so return
	// the live handle instead of ALREADY_OPENED for this Open → ReadState pattern.
	g_reads_since_open = 0;

	if (g_pad_opened)
	{
		return kPrimaryHandle;
	}

	g_pad_opened = true;

	return kPrimaryHandle;
}

int KYTY_SYSV_ABI PadGetHandle(int user_id, int type, int index)
{
	PRINT_NAME();

	if (!g_pad_initialized)
	{
		return PAD_ERROR_NOT_INITIALIZED;
	}
	if (user_id == -1)
	{
		return PAD_ERROR_DEVICE_NO_HANDLE;
	}
	if (!g_pad_opened || user_id != 1 || type != 0 || index != 0)
	{
		return PAD_ERROR_DEVICE_NO_HANDLE;
	}

	return kPrimaryHandle;
}

int KYTY_SYSV_ABI PadSetMotionSensorState(int handle, bool enable)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle != 1);

	printf("\t enable = %s\n", (enable ? "true" : "false"));

	return OK;
}

int KYTY_SYSV_ABI PadGetControllerInformation(int handle, PadControllerInformation* info)
{
	PRINT_NAME();

	EXIT_IF(g_controller == nullptr);

	int  connected_count = 0;
	bool connected       = false;

	g_controller->GetConnectionInfo(&connected, &connected_count);

	EXIT_NOT_IMPLEMENTED(handle != 1);
	EXIT_NOT_IMPLEMENTED(info == nullptr);

	info->touch_pixel_density   = 44.86f;
	info->touch_resolution_x    = 1920;
	info->touch_resolution_y    = 943;
	info->stick_dead_zone_left  = controller_get_axis(-32768, 32767, 8000) - 128;
	info->stick_dead_zone_right = controller_get_axis(-32768, 32767, 8000) - 128;
	info->connection_type       = 0;
	// After PadOpen the virtual pad is always present for keyboard hosts.
	info->connected_count       = (connected_count > 0 ? (connected_count > 255 ? 255 : connected_count) : 1);
	info->connected             = true;
	info->device_class          = 0;

	return OK;
}

int KYTY_SYSV_ABI PadReadState(int handle, PadData* data)
{
	PRINT_NAME();

	EXIT_IF(g_controller == nullptr);

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState state;

	g_controller->ReadState(&state, &connected, &connected_count);

	EXIT_NOT_IMPLEMENTED(handle != 1);
	EXIT_NOT_IMPLEMENTED(data == nullptr);

	// Optional diagnostic (KYTY_AUTO_CROSS=1): synthesize button edges so multi-
	// screen splash/title flows can advance without a physical pad. Continuous
	// hold only yields one rising edge — later logos (e.g. after Motion Twin)
	// never see a new press. Observed pattern is PadOpen + two PadReadState per
	// tick: release on the first read and press on the second so both
	// within-tick (curr & ~prev) and frame-to-frame edge detectors fire.
	uint32_t auto_buttons = 0;
	g_reads_since_open++;
	if (const char* auto_cross = std::getenv("KYTY_AUTO_CROSS"); auto_cross != nullptr && auto_cross[0] == '1')
	{
		static uint64_t first_read = 0;
		const uint64_t  now        = LibKernel::KernelGetProcessTime();
		if (first_read == 0)
		{
			first_read = now;
		}
		const uint64_t elapsed = now - first_read;
		// Warm up 2s, then 1s period: 250ms press window, 750ms release.
		if (elapsed > 2'000'000ull)
		{
			const uint64_t period = 1'000'000ull;
			const uint64_t phase  = (elapsed - 2'000'000ull) % period;
			const bool     press  = phase < 250'000ull;
			// Second read of the open cycle carries the press; first stays released.
			if (press && g_reads_since_open >= 2)
			{
				auto_buttons = PAD_BUTTON_CROSS;
				// Every 4th period also pulse Options|TouchPad for UIs that
				// only listen on those (still only on the pressed sample).
				const uint64_t period_index = (elapsed - 2'000'000ull) / period;
				if ((period_index % 4ull) == 3ull)
				{
					auto_buttons |= PAD_BUTTON_OPTIONS | PAD_BUTTON_TOUCH_PAD;
				}
			}
		}
	}

	const uint64_t now_ts = LibKernel::KernelGetProcessTime();

	data->buttons                = state.buttons | auto_buttons;
	data->left_stick_x           = state.axes[static_cast<int>(Axis::LeftX)];
	data->left_stick_y           = state.axes[static_cast<int>(Axis::LeftY)];
	data->right_stick_x          = state.axes[static_cast<int>(Axis::RightX)];
	data->right_stick_y          = state.axes[static_cast<int>(Axis::RightY)];
	data->analog_buttons_l2      = state.axes[static_cast<int>(Axis::TriggerLeft)];
	data->analog_buttons_r2      = state.axes[static_cast<int>(Axis::TriggerRight)];
	data->orientation_x          = 0.0f;
	data->orientation_y          = 0.0f;
	data->orientation_z          = 0.0f;
	data->orientation_w          = 1.0f;
	// Resting DualSense gravity (m/s^2); zero accel can look like a disconnected IMU sample.
	data->acceleration_x         = 0.0f;
	data->acceleration_y         = 0.0f;
	data->acceleration_z         = 9.8f;
	data->angular_velocity_x     = 0.0f;
	data->angular_velocity_y     = 0.0f;
	data->angular_velocity_z     = 0.0f;
	data->touch_data_touch_num   = 0;
	data->touch_data_touch0_x    = 0;
	data->touch_data_touch0_y    = 0;
	data->touch_data_touch0_id   = 1;
	data->touch_data_touch1_x    = 0;
	data->touch_data_touch1_y    = 0;
	data->touch_data_touch1_id   = 2;
	data->connected              = true;
	// Always refresh timestamp so held samples still look live to edge/time logic.
	data->timestamp              = now_ts;
	data->connected_count        = (connected_count > 0 ? connected_count : 1);
	data->device_unique_data_len = 0;
	return OK;
}

int KYTY_SYSV_ABI PadRead(int handle, PadData* data, int num)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(num < 1 || num > 64);
	EXIT_NOT_IMPLEMENTED(handle != 1);
	EXIT_NOT_IMPLEMENTED(data == nullptr);

	EXIT_IF(g_controller == nullptr);

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState states[64];

	int ret_num = g_controller->ReadStates(states, num, &connected, &connected_count);

	if (!connected)
	{
		ret_num = 1;
	}

	for (int i = 0; i < ret_num; i++)
	{
		data[i].buttons                = states[i].buttons;
		data[i].left_stick_x           = states[i].axes[static_cast<int>(Axis::LeftX)];
		data[i].left_stick_y           = states[i].axes[static_cast<int>(Axis::LeftY)];
		data[i].right_stick_x          = states[i].axes[static_cast<int>(Axis::RightX)];
		data[i].right_stick_y          = states[i].axes[static_cast<int>(Axis::RightY)];
		data[i].analog_buttons_l2      = states[i].axes[static_cast<int>(Axis::TriggerLeft)];
		data[i].analog_buttons_r2      = states[i].axes[static_cast<int>(Axis::TriggerRight)];
		data[i].orientation_x          = 0.0f;
		data[i].orientation_y          = 0.0f;
		data[i].orientation_z          = 0.0f;
		data[i].orientation_w          = 1.0f;
		data[i].acceleration_x         = 0.0f;
		data[i].acceleration_y         = 0.0f;
		data[i].acceleration_z         = 0.0f;
		data[i].angular_velocity_x     = 0.0f;
		data[i].angular_velocity_y     = 0.0f;
		data[i].angular_velocity_z     = 0.0f;
		data[i].touch_data_touch_num   = 0;
		data[i].touch_data_touch0_x    = 0;
		data[i].touch_data_touch0_y    = 0;
		data[i].touch_data_touch0_id   = 1;
		data[i].touch_data_touch1_x    = 0;
		data[i].touch_data_touch1_y    = 0;
		data[i].touch_data_touch1_id   = 2;
		data[i].connected              = connected;
		data[i].timestamp              = states[i].time;
		data[i].connected_count        = connected_count;
		data[i].device_unique_data_len = 0;
	}

	return ret_num;
}

int KYTY_SYSV_ABI PadSetVibrationMode(int handle, int mode)
{
	PRINT_NAME();

	if (handle != kPrimaryHandle)
	{
		return PAD_ERROR_INVALID_HANDLE;
	}

	printf("\t mode = %d\n", mode);

	return OK;
}

int KYTY_SYSV_ABI PadSetTriggerEffect(int handle, const void* param)
{
	PRINT_NAME();

	if (handle != kPrimaryHandle)
	{
		return PAD_ERROR_INVALID_HANDLE;
	}

	// DualSense adaptive-trigger program: accepted and ignored (no host hardware).
	(void)param;

	return OK;
}

int KYTY_SYSV_ABI PadSetVibration(int handle, const PadVibrationParam* param)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle != 1);

	printf("\t large_motor = %d\n", static_cast<int>(param->large_motor));
	printf("\t small_motor = %d\n", static_cast<int>(param->small_motor));

	return OK;
}

int KYTY_SYSV_ABI PadResetLightBar(int handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle != 1);

	return OK;
}

int KYTY_SYSV_ABI PadSetLightBar(int handle, const PadLightBarParam* param)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle != 1);
	EXIT_NOT_IMPLEMENTED(param == nullptr);

	return OK;
}

} // namespace Kyty::Libs::Controller

#endif // KYTY_EMU_ENABLED
