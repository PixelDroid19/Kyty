#ifndef LAUNCHER_INCLUDE_NOTICE_ITEM_H_
#define LAUNCHER_INCLUDE_NOTICE_ITEM_H_

#include "Common.h"

#include <QQuickItem>
#include <QTimer>

// A short message in a pill that hides itself after a few seconds. It sizes itself to its text, at most as wide
// as its parent less a margin, and wraps the text past that.
class NoticeItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(NoticeItem);

public:
	explicit NoticeItem(QQuickItem* parent = nullptr);
	~NoticeItem() override = default;

	// Shows text, replacing any message still shown, and starts the time to hide it again.
	void Show(const QString& text);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;

private:
	void Resize();

	QString                 m_text;
	QTimer                  m_hide_timer;
	QMetaObject::Connection m_parent_width;
};

#endif /* LAUNCHER_INCLUDE_NOTICE_ITEM_H_ */
