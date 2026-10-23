#ifndef LAUNCHER_INCLUDE_CHOICE_LIST_ITEM_H_
#define LAUNCHER_INCLUDE_CHOICE_LIST_ITEM_H_

#include "Common.h"

#include <QQuickItem>
#include <QStringList>
#include <QVariantAnimation>

// The drop-down list of a choice field, opened under the field, or over it when there is no room below. It covers
// its parent while open, so the page below takes no input: Up and Down move, Enter and Space pick, Escape
// closes; the pointer highlights the row under it, a click picks, and a click outside the list closes it.
class ChoiceListItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ChoiceListItem);

signals:
	void Chosen(int index);
	// Closed without a pick.
	void Cancelled();

public:
	explicit ChoiceListItem(QQuickItem* parent);
	~ChoiceListItem() override = default;

	// Opens the list of texts beside anchor, a rectangle in this item's coordinates, with current highlighted.
	void OpenFor(const QRectF& anchor, const QStringList& texts, int current);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     mouseReleaseEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     wheelEvent(QWheelEvent* event) override;

private:
	[[nodiscard]] int    VisibleRows() const;
	[[nodiscard]] QRectF PanelRect() const;
	[[nodiscard]] QRectF RowRect(int index) const;
	[[nodiscard]] int    RowAt(QPointF point) const;
	void                 MoveCurrent(int index);
	void                 ScrollBy(int rows);
	void                 Close(bool picked);

	QStringList m_texts;
	QRectF      m_anchor;
	int         m_chosen  = -1;
	int         m_current = 0;
	// The first row in view when the list is longer than the panel.
	int               m_first = 0;
	int               m_wheel = 0;
	QVariantAnimation m_fade;
};

#endif /* LAUNCHER_INCLUDE_CHOICE_LIST_ITEM_H_ */
