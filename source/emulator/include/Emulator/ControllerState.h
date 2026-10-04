#ifndef EMULATOR_INCLUDE_EMULATOR_CONTROLLERSTATE_H_
#define EMULATOR_INCLUDE_EMULATOR_CONTROLLERSTATE_H_

#include "Emulator/Controller.h"

#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Controller {

struct ControllerState
{
	uint64_t time                                  = 0;
	uint32_t buttons                               = 0;
	int      axes[static_cast<int>(Axis::AxisMax)] = {128, 128, 128, 128, 0, 0};
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
	ControllerState m_states[STATES_MAX];
	StatePrivate    m_private[STATES_MAX];
	ControllerState m_last_state;
	uint32_t        m_states_num  = 0;
	uint32_t        m_first_state = 0;
};

} // namespace Kyty::Libs::Controller

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_CONTROLLERSTATE_H_ */
