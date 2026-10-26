#ifndef LAUNCHER_INCLUDE_TOP_BAR_ITEM_H_
#define LAUNCHER_INCLUDE_TOP_BAR_ITEM_H_

#include "Common.h"

#include <QImage>
#include <QQuickItem>
#include <QStringList>
#include <QTimer>
#include <QVector>

// The strip along the window's top: the pages as tabs on the left when the layout has them, and on the right the
// emulator's state, the controller's battery, search, the clock and the power menu. Left and Right move between
// the tabs and buttons, Enter activates one, Down and Escape hand the focus back to the page. Clicks outside the
// tabs and buttons go through to the page below.
class TopBarItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(TopBarItem);

signals:
	void TabRequested(int index);
	void SearchRequested();
	void PowerRequested();
	void DownRequested();

public:
	explicit TopBarItem(QQuickItem* parent = nullptr);
	~TopBarItem() override = default;

	// The tabs' names, none for no tabs, and the one marked as the page shown.
	void SetTabs(const QStringList& tabs);
	void SetCurrentTab(int index);
	// What the status shows changed: the emulator or the controller.
	void Refresh();

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     focusInEvent(QFocusEvent* event) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     hoverLeaveEvent(QHoverEvent* event) override;

private:
	enum class Kind
	{
		Tab,
		Status,
		Battery,
		Search,
		Clock,
		Power
	};

	struct Entry
	{
		Kind    kind;
		QString text;
		QRectF  rect;
		// The tab's index, or for the status whether the emulator is ready.
		int value = 0;
	};

	void              Layout();
	void              Activate(int index);
	void              SetFocused(int index);
	[[nodiscard]] int EntryAt(QPointF point) const;
	[[nodiscard]] static bool Focusable(const Entry& entry) { return entry.kind == Kind::Tab || entry.kind == Kind::Search || entry.kind == Kind::Power; }

	QStringList    m_tabs;
	int            m_current_tab = 0;
	QVector<Entry> m_entries;
	int            m_focused = -1;
	int            m_hovered = -1;
	// Wakes the bar so the clock and the battery stay current.
	QTimer m_tick;
	// The search and power icons, painted for the current size.
	QImage m_icons[2];
	qreal  m_icon_scale = 0;
};

#endif /* LAUNCHER_INCLUDE_TOP_BAR_ITEM_H_ */
