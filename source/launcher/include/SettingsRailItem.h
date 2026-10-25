#ifndef LAUNCHER_INCLUDE_SETTINGS_RAIL_ITEM_H_
#define LAUNCHER_INCLUDE_SETTINGS_RAIL_ITEM_H_

#include "Common.h"
#include "SettingsIcons.h"

#include <QElapsedTimer>
#include <QImage>
#include <QQuickItem>
#include <QVariantAnimation>
#include <QVector>

// The settings categories down the left of the settings page, each with an icon of its own. The selected one
// plays its icon's animation while the launcher is live. Up and Down select, Right, Enter and Space go into the
// selected category; the mouse selects and goes in with one click.
class SettingsRailItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(SettingsRailItem);

signals:
	void CurrentChanged();
	void Entered();

public:
	struct Entry
	{
		QString      title;
		SettingsIcon icon = SettingsIcon::Graphics;
	};

	explicit SettingsRailItem(QQuickItem* parent = nullptr);
	~SettingsRailItem() override = default;

	void              SetEntries(const QVector<Entry>& entries);
	[[nodiscard]] int Current() const { return m_current; }
	void              SetCurrent(int index);
	// Height the entries take at the current size.
	[[nodiscard]] qreal ContentHeight() const;

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     mouseReleaseEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     hoverLeaveEvent(QHoverEvent* event) override;

private:
	[[nodiscard]] QRectF EntryRect(int index) const;
	[[nodiscard]] int    EntryAt(QPointF point) const;
	void                 UpdateClock();

	QVector<Entry> m_entries;
	int            m_current = 0;
	int            m_hovered = -1;
	int            m_pressed = -1;
	// Runs while the selected icon animates; each step draws a frame.
	QVariantAnimation m_clock;
	QElapsedTimer     m_since;
	// Bumped whenever the entries' texts or look change, so their nodes are built again.
	int m_generation = 0;
};

#endif /* LAUNCHER_INCLUDE_SETTINGS_RAIL_ITEM_H_ */
