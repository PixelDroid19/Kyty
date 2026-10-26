#ifndef EMULATOR_INCLUDE_EMULATOR_CONTROLLERSTATE_H_
#define EMULATOR_INCLUDE_EMULATOR_CONTROLLERSTATE_H_

#include "Emulator/Controller.h"

#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Controller {

struct ControllerTouchPoint
{
	bool     down = false;
	uint8_t  id   = 0;
	uint16_t x    = 0;
	uint16_t y    = 0;
};

// Motion and touch of the active gamepad. A source without sensors reads as a
// motionless pad: identity orientation and gravity on +Z. The record reports
// acceleration in G, so gravity is about 1.0, never 9.8; that resting axis is
// the one this HLE always used, not verified against a physical pad.
struct ControllerSensors
{
	float                orientation[4]      = {0.0f, 0.0f, 0.0f, 1.0f};
	float                acceleration[3]     = {0.0f, 0.0f, 1.0f};
	float                angular_velocity[3] = {};
	ControllerTouchPoint touch[2];
};

struct ControllerState
{
	uint64_t          time                                  = 0;
	uint32_t          buttons                               = 0;
	int               axes[static_cast<int>(Axis::AxisMax)] = {128, 128, 128, 128, 0, 0};
	ControllerSensors sensors;
};

// Host event state and the bounded guest report history. Each admitted source
// owns its digital buttons independently of its trigger-axis contributions.
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
	void Motion(int id, MotionSensor sensor, float x, float y, float z, uint64_t timestamp_ns);
	void Touch(int id, int finger, bool down, float x, float y);
	void ResetOrientation();
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
	void                          AddSourceState();
	[[nodiscard]] ControllerState GetLastState() const;
	void                          AddState(const ControllerState& state);

	Core::Mutex     m_mutex;
	Vector<int>     m_connected_ids;
	int             m_active_id       = CONTROLLER_KEYBOARD_ID;
	bool            m_connected       = false;
	int             m_connected_count = 0;
	ControllerState m_keyboard_state;
	ControllerState m_physical_state;
	// Sensors are continuous, not transitions: every read reports the latest
	// sample instead of queueing one history entry per sensor event.
	ControllerSensors m_sensors;
	uint64_t          m_last_gyro_ns  = 0;
	uint8_t           m_last_touch_id = 0;
	ControllerState m_states[STATES_MAX];
	StatePrivate    m_private[STATES_MAX];
	ControllerState m_last_state;
	uint32_t        m_states_num  = 0;
	uint32_t        m_first_state = 0;
};

} // namespace Kyty::Libs::Controller

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_CONTROLLERSTATE_H_ */
