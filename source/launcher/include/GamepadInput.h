#ifndef LAUNCHER_INCLUDE_GAMEPAD_INPUT_H_
#define LAUNCHER_INCLUDE_GAMEPAD_INPUT_H_

#include "Common.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

class QWindow;
struct _SDL_GameController;

// Turns a game controller into key presses sent to the window, so the QML pages share one navigation path with the keyboard.
// Uses SDL2 for input only; no SDL video is initialised.
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

private:
	void Poll();
	void OpenSdl();
	void CloseSdl();
	void OpenController(int device_index);
	void CloseController();
	void UpdateInterval();
	void Tap(int key) const;

	QTimer              m_timer;
	QElapsedTimer       m_clock;
	QWindow*            m_window = nullptr;
	_SDL_GameController* m_pad   = nullptr;
	QString             m_name;
	bool                m_paused = false;
	bool                m_sdl_ready = false;

	static constexpr int BUTTON_COUNT    = 7;
	static constexpr int DIRECTION_COUNT = 4;

	bool  m_button_down[BUTTON_COUNT]      = {};
	bool  m_direction_down[DIRECTION_COUNT] = {};
	qint64 m_next_repeat[DIRECTION_COUNT]   = {};
};

#endif /* LAUNCHER_INCLUDE_GAMEPAD_INPUT_H_ */
