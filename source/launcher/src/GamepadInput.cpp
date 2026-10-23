#include "GamepadInput.h"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QWindow>

#include <SDL.h>

// Poll rate while a controller is attached and the launcher is in use.
constexpr int ACTIVE_INTERVAL_MS = 16;
// Poll rate otherwise, enough to notice a controller being plugged in.
constexpr int IDLE_INTERVAL_MS = 250;

constexpr qint64 REPEAT_DELAY_MS    = 400;
constexpr qint64 REPEAT_INTERVAL_MS = 110;
constexpr double STICK_DEADZONE     = 0.5;

namespace {

struct ButtonBinding
{
	SDL_GameControllerButton button;
	int                      key;
};

// Positions follow SDL's standard layout, which is Cross/Circle/Square/Triangle on a PlayStation controller.
constexpr ButtonBinding BUTTONS[] = {
    {SDL_CONTROLLER_BUTTON_A, Qt::Key_Return},
    {SDL_CONTROLLER_BUTTON_B, Qt::Key_Escape},
    {SDL_CONTROLLER_BUTTON_X, Qt::Key_E},
    {SDL_CONTROLLER_BUTTON_Y, Qt::Key_O},
    {SDL_CONTROLLER_BUTTON_START, Qt::Key_Menu},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, Qt::Key_PageUp},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, Qt::Key_PageDown},
};

// Direction order matches the arrays in GamepadInput.
constexpr int DIRECTION_KEYS[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};

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
	m_sdl_ready = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
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
		SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
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
		if (event.type == SDL_CONTROLLERDEVICEADDED && m_pad == nullptr)
		{
			OpenController(event.cdevice.which);
		} else if (event.type == SDL_CONTROLLERDEVICEREMOVED && m_pad != nullptr && SDL_GameControllerGetAttached(m_pad) == SDL_FALSE)
		{
			CloseController();
		}
	}

	if (m_pad == nullptr)
	{
		return;
	}

	// Button and stick state is tracked even when input is not delivered, so a press held across a focus change does not fire late.
	const bool deliver = m_window != nullptr && m_window->isActive();
	const auto now     = m_clock.elapsed();

	for (int i = 0; i < BUTTON_COUNT; i++)
	{
		const bool down = SDL_GameControllerGetButton(m_pad, BUTTONS[i].button) != 0;
		if (down && !m_button_down[i] && deliver)
		{
			Tap(BUTTONS[i].key);
		}
		m_button_down[i] = down;
	}

	const double x = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0;
	const double y = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0;

	const bool directions[DIRECTION_COUNT] = {
	    SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0 || y < -STICK_DEADZONE,
	    SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0 || y > STICK_DEADZONE,
	    SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0 || x < -STICK_DEADZONE,
	    SDL_GameControllerGetButton(m_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0 || x > STICK_DEADZONE,
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
			m_next_repeat[d] = now + REPEAT_DELAY_MS;
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

void GamepadInput::Tap(int key) const
{
	if (m_window == nullptr)
	{
		return;
	}
	QCoreApplication::postEvent(m_window, new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier));
	QCoreApplication::postEvent(m_window, new QKeyEvent(QEvent::KeyRelease, key, Qt::NoModifier));
}

void GamepadInput::OpenController(int device_index)
{
	m_pad = SDL_GameControllerOpen(device_index);
	if (m_pad == nullptr)
	{
		return;
	}
	const char* name = SDL_GameControllerName(m_pad);
	m_name           = name != nullptr ? QString::fromUtf8(name) : QStringLiteral("Controller");
	UpdateInterval();
	emit ControllerChanged(m_name);
}

void GamepadInput::CloseController()
{
	if (m_pad == nullptr)
	{
		return;
	}
	SDL_GameControllerClose(m_pad);
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
