#ifndef LAUNCHER_INCLUDE_ACTION_MENU_ITEM_H_
#define LAUNCHER_INCLUDE_ACTION_MENU_ITEM_H_

#include "Common.h"

#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>

// Options for one game, opened with O, the Start button or a right click, over a scrim that keeps the page
// below from taking input. Delete asks for confirmation first.
class ActionMenuItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ActionMenuItem);

signals:
	void ShownChanged();
	void Chosen(const QString& action, int row);

public:
	explicit ActionMenuItem(QQuickItem* parent = nullptr);
	~ActionMenuItem() override = default;

	[[nodiscard]] bool Shown() const { return m_shown; }

	// Opens the menu for the game in row with items, each { label, action, allowed, danger }; allowed defaults
	// to true and danger to false.
	void OpenFor(const QVariantMap& game, int row, const QVariantList& items);
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
	// Where the panel's parts go in the item, for the current actions.
	struct Geometry
	{
		QRectF panel;
		QRectF heading;
		QRectF note; // Empty unless confirming.
		QRectF rows; // The first row; the others follow at the row pitch.
	};

	[[nodiscard]] Geometry Layout() const;
	[[nodiscard]] QRectF   RowRect(const Geometry& geometry, int index) const;
	[[nodiscard]] int      RowAt(QPointF point) const;
	void                   SetShown(bool shown);
	void                   SetCurrent(int index);
	void                   Activate(int index);

	bool         m_shown      = false;
	bool         m_confirming = false;
	int          m_row        = -1;
	int          m_current    = 0;
	QString      m_heading;
	QVariantList m_actions;
};

#endif /* LAUNCHER_INCLUDE_ACTION_MENU_ITEM_H_ */
