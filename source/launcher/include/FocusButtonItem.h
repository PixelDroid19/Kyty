#ifndef LAUNCHER_INCLUDE_FOCUS_BUTTON_ITEM_H_
#define LAUNCHER_INCLUDE_FOCUS_BUTTON_ITEM_H_

#include "Common.h"

#include <QQuickItem>
#include <QString>

// A pill-shaped text button for mouse, keyboard and controller: Enter, Return and Space activate it. It sizes
// itself to its text, like the buttons PageLayout places.
class FocusButtonItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(FocusButtonItem);

signals:
	void Changed();
	void Activated();

public:
	explicit FocusButtonItem(QQuickItem* parent = nullptr);
	~FocusButtonItem() override = default;

	[[nodiscard]] QString Text() const { return m_text; }
	void                  SetText(const QString& text);
	[[nodiscard]] bool    Primary() const { return m_primary; }
	void                  SetPrimary(bool primary);
	[[nodiscard]] bool    Danger() const { return m_danger; }
	void                  SetDanger(bool danger);
	[[nodiscard]] bool    Allowed() const { return m_allowed; }
	void                  SetAllowed(bool allowed);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     mouseReleaseEvent(QMouseEvent* event) override;

private:
	void Resize();
	void Activate();

	QString m_text;
	bool    m_primary = false;
	bool    m_danger  = false;
	bool    m_allowed = true;
};

#endif /* LAUNCHER_INCLUDE_FOCUS_BUTTON_ITEM_H_ */
