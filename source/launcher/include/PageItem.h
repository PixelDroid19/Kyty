#ifndef LAUNCHER_INCLUDE_PAGE_ITEM_H_
#define LAUNCHER_INCLUDE_PAGE_ITEM_H_

#include "Common.h"
#include "PageLayout.h"
#include "ViewScroll.h"

#include <QQuickItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// A scrolling page of text and buttons, described by a subclass with a PageLayout. The arrow keys move the
// focus between the buttons and scroll it into view, Enter and Space press the focused button, Escape closes
// the page; the mouse presses buttons and the wheel scrolls.
class PageItem: public QQuickItem
{
	Q_OBJECT
	QML_ANONYMOUS
	KYTY_QT_CLASS_NO_COPY(PageItem);

	// What the page's keys and buttons do, for the hint bar; see HintBarItem.
	Q_PROPERTY(QVariantList hints READ Hints CONSTANT)

signals:
	// QML maps only signals that start in lower case to on... handlers.
	void closed(); // NOLINT(readability-identifier-naming)

public:
	~PageItem() override = default;

	[[nodiscard]] virtual QVariantList Hints() const = 0;

	// Puts the focus on the page's first button.
	Q_INVOKABLE void focusDefault(); // NOLINT(readability-identifier-naming)

protected:
	// The content is at most max_width wide and starts top below the item's top; sizes in units.
	PageItem(qreal max_width, qreal top, qreal spacing, QQuickItem* parent);

	// Describes the content in a layout of the content's width.
	virtual void Compose(PageLayout* layout) = 0;
	// The button with this id was pressed.
	virtual void Press(int id) = 0;
	// Called after the content is laid out or scrolled, to place child items over it.
	virtual void Placed() {}

	// The content changed; it is laid out again before the next frame.
	void Recompose();
	// Where content coordinates start in the item, at the current scroll.
	[[nodiscard]] QPointF ContentOrigin() const;

	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     mouseReleaseEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     hoverLeaveEvent(QHoverEvent* event) override;
	void     wheelEvent(QWheelEvent* event) override;

private:
	[[nodiscard]] qreal Viewport() const;
	[[nodiscard]] qreal ScrollLimit() const;
	[[nodiscard]] int   ButtonAt(QPointF point) const;
	// The button in the next row up (-1) or down (1) closest to the focused one, or -1 at the end.
	[[nodiscard]] int ButtonInRow(int direction) const;
	void              FocusButton(int index);
	void              PressButton(int index);

	qreal      m_max_width = 0;
	qreal      m_top       = 0;
	qreal      m_spacing   = 0;
	bool       m_dirty     = true;
	PageLayout m_layout {0, 1, 0};
	// Bumped whenever m_layout is replaced, so the nodes are built again.
	int        m_generation = 0;
	int        m_focused    = 0;
	int        m_pressed    = -1;
	int        m_hovered    = -1;
	ViewScroll m_scroll;
};

#endif /* LAUNCHER_INCLUDE_PAGE_ITEM_H_ */
