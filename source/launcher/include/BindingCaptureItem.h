#ifndef LAUNCHER_INCLUDE_BINDING_CAPTURE_ITEM_H_
#define LAUNCHER_INCLUDE_BINDING_CAPTURE_ITEM_H_

#include "Common.h"

#include <QElapsedTimer>
#include <QQuickItem>
#include <QString>
#include <QTimer>

class GamepadInput;

// Waits, over a scrim, for the input to bind to one control: a key or a mouse button, or a controller button.
// Escape on the keyboard cancels, and so does waiting CAPTURE_SECONDS. Keys the emulator keeps for itself are
// refused with a note and the wait goes on. While it is open no input reaches the page below.
class BindingCaptureItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(BindingCaptureItem);

signals:
	// name is the input in the profile format, for the field named key.
	void Captured(const QString& key, const QString& name);
	// The wait is over, captured or not.
	void Closed();

public:
	static constexpr int CAPTURE_SECONDS = 5;

	explicit BindingCaptureItem(GamepadInput* gamepad, QQuickItem* parent = nullptr);
	~BindingCaptureItem() override = default;

	[[nodiscard]] bool    Shown() const { return m_shown; }
	[[nodiscard]] QString Key() const { return m_key; }
	[[nodiscard]] bool    ForGamepad() const { return m_for_gamepad; }

	// Waits for the field named key; title names the control.
	void OpenFor(const QString& key, const QString& title, bool gamepad);
	void Close();

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     mouseReleaseEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     wheelEvent(QWheelEvent* event) override;

private:
	void Capture(const QString& name);
	void Tick();
	[[nodiscard]] QRectF Panel() const;

	GamepadInput* m_gamepad = nullptr;
	QTimer        m_tick;
	QElapsedTimer m_clock;
	bool          m_shown       = false;
	bool          m_for_gamepad = false;
	QString       m_key;
	QString       m_title;
	// Why the last input was not taken.
	QString m_note;
	int     m_seconds_left = 0;
};

#endif /* LAUNCHER_INCLUDE_BINDING_CAPTURE_ITEM_H_ */
