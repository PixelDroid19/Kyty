#include "GamepadInput.h"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QWindow>

#include <SDL3/SDL.h>

#include <algorithm>
#include <limits>

// Poll rate while a controller is attached and the launcher is in use.
constexpr int ACTIVE_INTERVAL_MS = 16;
// Poll rate otherwise, enough to notice a controller being plugged in.
constexpr int IDLE_INTERVAL_MS = 250;

constexpr qint64 REPEAT_DELAY_MS    = 400;
constexpr qint64 REPEAT_INTERVAL_MS = 110;
constexpr double STICK_DEADZONE     = 0.5;
// Holding the back button this long opens the power menu instead of going back.
constexpr qint64 HOLD_MS = 700;

namespace {

struct ButtonBinding
{
	SDL_GamepadButton button;
	int               key;
};

// Face buttons by position, which is Cross/Circle/Square/Triangle on a PlayStation controller.
constexpr ButtonBinding BUTTONS[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, Qt::Key_Return},
    {SDL_GAMEPAD_BUTTON_EAST, Qt::Key_Escape},
    {SDL_GAMEPAD_BUTTON_WEST, Qt::Key_E},
    {SDL_GAMEPAD_BUTTON_NORTH, Qt::Key_O},
    {SDL_GAMEPAD_BUTTON_START, Qt::Key_Menu},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Qt::Key_PageUp},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Qt::Key_PageDown},
};

// Direction order matches the arrays in GamepadInput.
constexpr int DIRECTION_KEYS[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};

static_assert(SDL_GAMEPAD_BUTTON_COUNT <= 32, "capture keeps one bit per button");

QString GuidOf(SDL_JoystickID id)
{
	char text[33] = {};
	SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), text, sizeof(text));
	return QString::fromLatin1(text);
}

double Axis(SDL_Gamepad* pad, SDL_GamepadAxis axis)
{
	return pad != nullptr ? std::max(-1.0, SDL_GetGamepadAxis(pad, axis) / 32767.0) : 0.0;
}

} // namespace

GamepadInput::GamepadInput(QObject* parent): QObject(parent)
{
	m_clock.start();
	connect(&m_timer, &QTimer::timeout, this, &GamepadInput::Poll);
	OpenSdl();
}

GamepadInput::~GamepadInput()
{
	CloseSdl();
}

void GamepadInput::OpenSdl()
{
	// SDL would otherwise take over SIGINT and SIGTERM and turn them into events nobody reads, so the launcher could not be stopped.
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	// SDL reports the controllers already plugged in as added devices on the first poll.
	m_sdl_ready = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
	UpdateInterval();
	if (m_sdl_ready)
	{
		m_timer.start();
	}
}

void GamepadInput::CloseSdl()
{
	m_timer.stop();
	CloseController();
	if (m_sdl_ready)
	{
		SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
	}
	m_sdl_ready = false;
}

void GamepadInput::SetWindow(QWindow* window)
{
	m_window = window;
}

void GamepadInput::SetPaused(bool paused)
{
	if (paused == m_paused)
	{
		return;
	}
	m_paused = paused;
	// While a game runs the launcher closes its controller handle and stops polling, so the game is the only reader.
	if (paused)
	{
		CloseSdl();
	} else
	{
		OpenSdl();
	}
}

void GamepadInput::UpdateInterval()
{
	m_timer.setInterval(m_pad != nullptr ? ACTIVE_INTERVAL_MS : IDLE_INTERVAL_MS);
}

void GamepadInput::Poll()
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		if (event.type != SDL_EVENT_GAMEPAD_ADDED && event.type != SDL_EVENT_GAMEPAD_REMOVED)
		{
			continue;
		}
		if (m_pad != nullptr && !SDL_GamepadConnected(m_pad))
		{
			CloseController();
		}
		// A new controller replaces the open one only when it is the preferred one and the open one is not.
		const bool better = event.type == SDL_EVENT_GAMEPAD_ADDED && m_pad != nullptr && !m_preferred.isEmpty() &&
		                    SameDevice(GuidOf(event.gdevice.which), m_preferred) &&
		                    !SameDevice(GuidOf(SDL_GetGamepadID(m_pad)), m_preferred);
		if (better)
		{
			CloseController();
		}
		if (m_pad == nullptr)
		{
			OpenPreferred();
		}
		emit DevicesChanged();
	}

	if (m_pad == nullptr)
	{
		return;
	}
	// Button and stick state is tracked even when input is not delivered, so a press held across a focus change does not fire late.
	// Decided before the capture, which may end in this poll with the captured button still down.
	const bool deliver = m_window != nullptr && m_window->isActive() && !m_capturing;
	if (m_capturing)
	{
		PollCapture();
	}
	const auto now     = m_clock.elapsed();

	for (int i = 0; i < BUTTON_COUNT; i++)
	{
		const bool down = SDL_GetGamepadButton(m_pad, BUTTONS[i].button);
		if (BUTTONS[i].key == Qt::Key_Escape)
		{
			PollBack(down, m_button_down[i], now, deliver);
		} else if (down && !m_button_down[i] && deliver)
		{
			Tap(BUTTONS[i].key);
		}
		m_button_down[i] = down;
	}

	const double x = m_stick_navigation ? SDL_GetGamepadAxis(m_pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0 : 0;
	const double y = m_stick_navigation ? SDL_GetGamepadAxis(m_pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0 : 0;

	const bool directions[DIRECTION_COUNT] = {
	    SDL_GetGamepadButton(m_pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -STICK_DEADZONE,
	    SDL_GetGamepadButton(m_pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > STICK_DEADZONE,
	    SDL_GetGamepadButton(m_pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -STICK_DEADZONE,
	    SDL_GetGamepadButton(m_pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > STICK_DEADZONE,
	};

	for (int d = 0; d < DIRECTION_COUNT; d++)
	{
		if (!directions[d])
		{
			m_direction_down[d] = false;
			continue;
		}
		if (!m_direction_down[d])
		{
			m_next_repeat[d] = deliver ? now + REPEAT_DELAY_MS : std::numeric_limits<qint64>::max();
			if (deliver)
			{
				Tap(DIRECTION_KEYS[d]);
			}
		} else if (now >= m_next_repeat[d])
		{
			m_next_repeat[d] = now + REPEAT_INTERVAL_MS;
			if (deliver)
			{
				Tap(DIRECTION_KEYS[d]);
			}
		}
		m_direction_down[d] = true;
	}
}

// Back goes on release, so a hold can become the power menu without going back first.
void GamepadInput::PollBack(bool down, bool was_down, qint64 now, bool deliver)
{
	if (down && !was_down)
	{
		m_back_down_at = now;
		// A press that was not delivered, such as a captured one, does not go back when it is released.
		m_back_held = !deliver;
		return;
	}
	if (down && !m_back_held && now - m_back_down_at >= HOLD_MS)
	{
		m_back_held = true;
		if (deliver)
		{
			Tap(Qt::Key_PowerOff);
		}
		return;
	}
	if (!down && was_down && !m_back_held && deliver)
	{
		Tap(Qt::Key_Escape);
	}
}

QStringList GamepadInput::ButtonLabels() const
{
	static const QStringList playstation = {QStringLiteral("✕"), QStringLiteral("○"), QStringLiteral("□"), QStringLiteral("△")};
	QStringList              labels;
	for (const auto button: {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH})
	{
		switch (m_pad != nullptr ? SDL_GetGamepadButtonLabel(m_pad, button) : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN)
		{
			case SDL_GAMEPAD_BUTTON_LABEL_A: labels << QStringLiteral("A"); break;
			case SDL_GAMEPAD_BUTTON_LABEL_B: labels << QStringLiteral("B"); break;
			case SDL_GAMEPAD_BUTTON_LABEL_X: labels << QStringLiteral("X"); break;
			case SDL_GAMEPAD_BUTTON_LABEL_Y: labels << QStringLiteral("Y"); break;
			case SDL_GAMEPAD_BUTTON_LABEL_CROSS: labels << QStringLiteral("✕"); break;
			case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: labels << QStringLiteral("○"); break;
			case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: labels << QStringLiteral("□"); break;
			case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: labels << QStringLiteral("△"); break;
			// No controller, or one SDL cannot name: the PlayStation symbol in that place.
			default: labels << playstation.at(labels.size()); break;
		}
	}
	switch (m_pad != nullptr ? SDL_GetGamepadType(m_pad) : SDL_GAMEPAD_TYPE_PS5)
	{
		case SDL_GAMEPAD_TYPE_XBOX360:
		case SDL_GAMEPAD_TYPE_XBOXONE: labels << QStringLiteral("LB") << QStringLiteral("RB"); break;
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: labels << QStringLiteral("L") << QStringLiteral("R"); break;
		default: labels << QStringLiteral("L1") << QStringLiteral("R1"); break;
	}
	return labels;
}

int GamepadInput::BatteryPercent() const
{
	if (m_pad == nullptr)
	{
		return -1;
	}
	int        percent = -1;
	const auto state   = SDL_GetGamepadPowerInfo(m_pad, &percent);
	const bool battery = state == SDL_POWERSTATE_ON_BATTERY || state == SDL_POWERSTATE_CHARGING || state == SDL_POWERSTATE_CHARGED;
	return battery ? percent : -1;
}

bool GamepadInput::Charging() const
{
	return m_pad != nullptr && SDL_GetGamepadPowerInfo(m_pad, nullptr) == SDL_POWERSTATE_CHARGING;
}

QVector<GamepadInput::Device> GamepadInput::Devices() const
{
	QVector<Device> devices;
	if (!m_sdl_ready)
	{
		return devices;
	}
	int   count = 0;
	auto* ids   = SDL_GetGamepads(&count);
	for (int i = 0; i < count; i++)
	{
		const char* name = SDL_GetGamepadNameForID(ids[i]);
		devices << Device {GuidOf(ids[i]), name != nullptr ? QString::fromUtf8(name) : QStringLiteral("Controller")};
	}
	SDL_free(ids);
	return devices;
}

void GamepadInput::SetPreferredDevice(const QString& guid)
{
	if (guid == m_preferred)
	{
		return;
	}
	m_preferred = guid;
	if (m_pad != nullptr && (guid.isEmpty() || SameDevice(GuidOf(SDL_GetGamepadID(m_pad)), guid)))
	{
		return;
	}
	const auto devices = Devices();
	const bool present = std::any_of(devices.begin(), devices.end(), [&](const Device& device) { return SameDevice(device.guid, guid); });
	if (m_pad == nullptr || present)
	{
		CloseController();
		OpenPreferred();
	}
}

QPointF GamepadInput::StickPosition(bool right) const
{
	return {Axis(m_pad, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX),
	        Axis(m_pad, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY)};
}

qreal GamepadInput::TriggerPosition(bool right) const
{
	return std::max(0.0, Axis(m_pad, right ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
}

QStringList GamepadInput::HeldButtons() const
{
	QStringList held;
	for (int b = 0; m_pad != nullptr && b < SDL_GAMEPAD_BUTTON_COUNT; b++)
	{
		const auto  button = static_cast<SDL_GamepadButton>(b);
		const char* name   = SDL_GetGamepadButton(m_pad, button) ? SDL_GetGamepadStringForButton(button) : nullptr;
		if (name != nullptr)
		{
			held << QString::fromLatin1(name);
		}
	}
	return held;
}

void GamepadInput::BeginCapture()
{
	m_capturing    = true;
	m_capture_down = 0;
	// Buttons already held, such as the one that opened the capture, are not captured.
	for (int b = 0; m_pad != nullptr && b < SDL_GAMEPAD_BUTTON_COUNT; b++)
	{
		m_capture_down |= SDL_GetGamepadButton(m_pad, static_cast<SDL_GamepadButton>(b)) ? 1u << b : 0u;
	}
}

void GamepadInput::EndCapture()
{
	m_capturing = false;
	// A direction held since before the capture must not start repeating now.
	for (int d = 0; d < DIRECTION_COUNT; d++)
	{
		if (m_direction_down[d])
		{
			m_next_repeat[d] = std::numeric_limits<qint64>::max();
		}
	}
}

void GamepadInput::PollCapture()
{
	quint32 down = 0;
	for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
	{
		down |= SDL_GetGamepadButton(m_pad, static_cast<SDL_GamepadButton>(b)) ? 1u << b : 0u;
	}
	const quint32 pressed = down & ~m_capture_down;
	m_capture_down        = down;
	for (int b = 0; pressed != 0 && b < SDL_GAMEPAD_BUTTON_COUNT; b++)
	{
		const char* name = (pressed & (1u << b)) != 0 ? SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b)) : nullptr;
		if (name != nullptr)
		{
			emit ButtonCaptured(QString::fromLatin1(name));
			return;
		}
	}
}

bool GamepadInput::SameDevice(const QString& a, const QString& b)
{
	if (a.size() != b.size())
	{
		return false;
	}
	for (int i = 0; i < a.size(); i++)
	{
		if ((i < 4 || i >= 8) && a.at(i).toLower() != b.at(i).toLower())
		{
			return false;
		}
	}
	return true;
}

void GamepadInput::Tap(int key) const
{
	if (m_window == nullptr)
	{
		return;
	}
	QCoreApplication::postEvent(m_window, new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier));
	QCoreApplication::postEvent(m_window, new QKeyEvent(QEvent::KeyRelease, key, Qt::NoModifier));
}

void GamepadInput::OpenController(quint32 joystick_id)
{
	m_pad = SDL_OpenGamepad(joystick_id);
	if (m_pad == nullptr)
	{
		return;
	}
	const char* name = SDL_GetGamepadName(m_pad);
	m_name           = name != nullptr ? QString::fromUtf8(name) : QStringLiteral("Controller");
	UpdateInterval();
	emit ControllerChanged(m_name);
}

void GamepadInput::OpenPreferred()
{
	int   count = 0;
	auto* ids   = SDL_GetGamepads(&count);
	int   pick  = count > 0 ? 0 : -1;
	for (int i = 0; i < count && !m_preferred.isEmpty(); i++)
	{
		if (SameDevice(GuidOf(ids[i]), m_preferred))
		{
			pick = i;
			break;
		}
	}
	if (pick >= 0)
	{
		OpenController(ids[pick]);
	}
	SDL_free(ids);
}

void GamepadInput::CloseController()
{
	if (m_pad == nullptr)
	{
		return;
	}
	SDL_CloseGamepad(m_pad);
	m_pad = nullptr;
	m_name.clear();
	for (bool& down: m_button_down)
	{
		down = false;
	}
	for (bool& down: m_direction_down)
	{
		down = false;
	}
	UpdateInterval();
	emit ControllerChanged(m_name);
}
