#ifndef EMULATOR_INCLUDE_EMULATOR_CONTROLLER_H_
#define EMULATOR_INCLUDE_EMULATOR_CONTROLLER_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Ports/ControllerInputPort.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Controller {

// The guest pad ABI constants and axis ids live in the neutral input port;
// this HLE layer imports them so guest-facing code keeps the short names.
using ::Kyty::Emulator::Ports::CONTROLLER_KEYBOARD_ID;
using ::Kyty::Emulator::Ports::PAD_BUTTON_L3;
using ::Kyty::Emulator::Ports::PAD_BUTTON_R3;
using ::Kyty::Emulator::Ports::PAD_BUTTON_OPTIONS;
using ::Kyty::Emulator::Ports::PAD_BUTTON_UP;
using ::Kyty::Emulator::Ports::PAD_BUTTON_RIGHT;
using ::Kyty::Emulator::Ports::PAD_BUTTON_DOWN;
using ::Kyty::Emulator::Ports::PAD_BUTTON_LEFT;
using ::Kyty::Emulator::Ports::PAD_BUTTON_L2;
using ::Kyty::Emulator::Ports::PAD_BUTTON_R2;
using ::Kyty::Emulator::Ports::PAD_BUTTON_L1;
using ::Kyty::Emulator::Ports::PAD_BUTTON_R1;
using ::Kyty::Emulator::Ports::PAD_BUTTON_TRIANGLE;
using ::Kyty::Emulator::Ports::PAD_BUTTON_CIRCLE;
using ::Kyty::Emulator::Ports::PAD_BUTTON_CROSS;
using ::Kyty::Emulator::Ports::PAD_BUTTON_SQUARE;
using ::Kyty::Emulator::Ports::PAD_BUTTON_TOUCH_PAD;
using ::Kyty::Emulator::Ports::Axis;

class ControllerSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<ControllerSubsystem>::Instance(); }
	const char*       Id() override { return "Controller"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


struct PadControllerInformation;
struct PadData;
struct PadVibrationParam;
struct PadLightBarParam;

inline int controller_get_axis(int min, int max, int value)
{
	int v = (255 * (value - min)) / (max - min);
	return (v < 0 ? 0 : (v > 255 ? 255 : v));
}

void ControllerConnect(int id);
void ControllerDisconnect(int id);
void ControllerButton(int id, uint32_t button, bool down);
void ControllerAxis(int id, Axis axis, int value);

// Diagnostic agent overlay. OR-ed into PadReadState/PadRead separately from
// KYTY_AUTO_CROSS. Not gameplay acceptance.
bool AgentPadButtonFromName(const char* name, uint32_t* out_button);
bool AgentPadAxisFromName(const char* name, Axis* out_axis);
void AgentPadSetButton(uint32_t button, bool down);
void AgentPadSetAxis(Axis axis, uint8_t value);
bool AgentPadScheduleTap(uint32_t button);

// Present-addressed diagnostic taps are intentionally small and local. A
// successful schedule is retained by the emulator, not by the request socket.
inline constexpr uint32_t kAgentPadScheduledTapMax = 8u;

// Keep a diagnostic tap visible across a bounded pair of advancing guest
// samples. This makes paired-poll reproduction deterministic without tying the
// pulse to host timing or claiming gameplay input compatibility.
inline constexpr uint32_t kAgentPadTapPressSamples = 2u;

enum class AgentPadScheduleResult: uint8_t
{
	Scheduled,
	Invalid,
	NotFuture,
	ButtonHeld,
	TapPending,
	QueueFull,
	TargetConflict,
};

struct AgentPadScheduledTapStats
{
	uint32_t scheduled_taps           = 0;
	uint64_t next_target_present      = 0;
	uint64_t cancelled_scheduled_taps = 0;
};

struct AgentPadPresentResult
{
	uint32_t started                  = 0;
	uint32_t cancelled                = 0;
	uint64_t started_target_present   = 0;
	uint64_t cancelled_target_present = 0;
};

// Atomically validates against the latest real present and adds repeat exact
// targets: at_present + N * present_delta. Targets are fired only by
// AgentPadOnPresent(), never by wall-clock time.
AgentPadScheduleResult AgentPadScheduleTaps(uint32_t button, uint64_t at_present, uint32_t repeat,
                                             uint64_t present_delta);
AgentPadPresentResult  AgentPadOnPresent(uint64_t present);
void                   AgentPadUpdatePresentWatermark(uint64_t present);
void                   AgentPadGetScheduledTapStats(AgentPadScheduledTapStats* out);
void AgentPadClear();
void AgentPadGetState(uint32_t* buttons, uint8_t* axes);

// Counts guest pad samples that actually incorporated a non-neutral agent
// overlay. This is observability only: it never changes guest input state.
struct AgentPadReadStats
{
	uint64_t read_state_samples = 0;
	uint64_t read_samples       = 0;
	uint64_t delivered_taps     = 0;
	bool     tap_pending        = false;
};

void AgentPadGetReadStats(AgentPadReadStats* out);

// Applies one PadReadState-equivalent overlay sample (advances tap FSM).
// Test/observability helper; does not touch SDL or guest memory.
void AgentPadApplyReadStateSample(uint32_t* buttons);

int KYTY_SYSV_ABI PadInit();
int KYTY_SYSV_ABI PadOpen(int user_id, int type, int index, const void* param);
int KYTY_SYSV_ABI PadOpenExt(int user_id, int type, int index, const void* param);
int KYTY_SYSV_ABI PadClose(int handle);
int KYTY_SYSV_ABI PadGetHandle(int user_id, int type, int index);
int KYTY_SYSV_ABI PadSetMotionSensorState(int handle, bool enable);
int KYTY_SYSV_ABI PadSetTiltCorrectionState(int handle, bool enable);
int KYTY_SYSV_ABI PadSetAngularVelocityDeadbandState(int handle, bool enable);
int KYTY_SYSV_ABI PadResetOrientation(int handle);
int KYTY_SYSV_ABI PadGetControllerInformation(int handle, PadControllerInformation* info);
int KYTY_SYSV_ABI PadGetExtControllerInformation(int handle, void* info);
int KYTY_SYSV_ABI PadDeviceClassGetExtendedInformation(int handle, void* info);
int KYTY_SYSV_ABI PadDeviceClassParseData(int handle, const PadData* data, void* class_data);
int KYTY_SYSV_ABI PadReadState(int handle, PadData* data);
int KYTY_SYSV_ABI PadRead(int handle, PadData* data, int num);
int KYTY_SYSV_ABI PadSetVibration(int handle, const PadVibrationParam* param);
int KYTY_SYSV_ABI PadSetVibrationMode(int handle, int mode);
int KYTY_SYSV_ABI PadSetTriggerEffect(int handle, const void* param);
// scePadGetTriggerEffectState — report both adaptive triggers idle.
struct PadTriggerEffectStateInformation
{
	int32_t state[2];
};
int KYTY_SYSV_ABI PadGetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info);
int KYTY_SYSV_ABI PadResetLightBar(int handle);
int KYTY_SYSV_ABI PadSetLightBar(int handle, const PadLightBarParam* param);

} // namespace Kyty::Libs::Controller

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_CONTROLLER_H_ */
