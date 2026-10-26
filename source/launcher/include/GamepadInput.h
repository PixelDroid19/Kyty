#ifndef LAUNCHER_INCLUDE_GAMEPAD_INPUT_H_
#define LAUNCHER_INCLUDE_GAMEPAD_INPUT_H_

#include "Common.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

class QWindow;
struct SDL_Gamepad;

// Turns a game controller into key presses sent to the window, so the pages share one navigation path with the keyboard.
// Holding the east button (○ on a PlayStation controller) sends Qt::Key_PowerOff instead of Escape.
// Uses SDL 3 for input only; no SDL video is initialised.
class GamepadInput: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(GamepadInput);

signals:
	// Emitted when a controller is attached or detached; name is empty when none is connected.
	void ControllerChanged(const QString& name);

public:
	explicit GamepadInput(QObject* parent = nullptr);
	~GamepadInput() override;

	void SetWindow(QWindow* window);

	// Paused while a game runs: the controller is released and nothing is polled.
	void SetPaused(bool paused);

	[[nodiscard]] const QString& ControllerName() const { return m_name; }
	// What the connected controller prints on its south, east, west and north buttons, then on its left and
	// right shoulder buttons; a PlayStation controller's when none is connected.
	[[nodiscard]] QStringList ButtonLabels() const;
	// The controller's charge in percent, or -1 when it has no battery or does not report it.
	[[nodiscard]] int  BatteryPercent() const;
	[[nodiscard]] bool Charging() const;

private:
	void Poll();
	void OpenSdl();
	void CloseSdl();
	void OpenController(quint32 joystick_id);
	void CloseController();
	void UpdateInterval();
	void Tap(int key) const;
	void PollBack(bool down, bool was_down, qint64 now, bool deliver);

	QTimer              m_timer;
	QElapsedTimer       m_clock;
	QWindow*            m_window = nullptr;
	SDL_Gamepad*        m_pad    = nullptr;
	QString             m_name;
	bool                m_paused = false;
	bool                m_sdl_ready = false;

	static constexpr int BUTTON_COUNT    = 7;
	static constexpr int DIRECTION_COUNT = 4;

	bool  m_button_down[BUTTON_COUNT]      = {};
	bool  m_direction_down[DIRECTION_COUNT] = {};
	qint64 m_next_repeat[DIRECTION_COUNT]   = {};
	// When the east button went down, and whether its hold already sent Key_PowerOff.
	qint64 m_back_down_at = 0;
	bool   m_back_held    = false;
};

#endif /* LAUNCHER_INCLUDE_GAMEPAD_INPUT_H_ */
