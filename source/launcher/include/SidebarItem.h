#ifndef LAUNCHER_INCLUDE_SIDEBAR_ITEM_H_
#define LAUNCHER_INCLUDE_SIDEBAR_ITEM_H_

#include "ArtworkSet.h"
#include "Common.h"

#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QVector>
#include <QtQml/qqmlregistration.h>

// Left rail with the launcher's main actions as icon buttons, drawn straight on the backdrop: library, add game
// and settings below entriesTop, about and quit at the bottom. Up and Down move between them, Right goes to
// rightTarget, Enter and Space activate. A button's label shows while it is hovered or focused.
class SidebarItem: public QQuickItem
{
	Q_OBJECT
	QML_NAMED_ELEMENT(Sidebar)
	KYTY_QT_CLASS_NO_COPY(SidebarItem);

	// The page shown: "library", "settings" or "about"; its button is marked and takes the focus first.
	Q_PROPERTY(QString current READ Current WRITE SetCurrent NOTIFY CurrentChanged)
	Q_PROPERTY(QQuickItem* rightTarget READ RightTarget WRITE SetRightTarget NOTIFY RightTargetChanged)
	// Where the first button starts, below whatever the rail shows above it.
	Q_PROPERTY(qreal entriesTop READ EntriesTop WRITE SetEntriesTop NOTIFY EntriesTopChanged)

signals:
	void CurrentChanged();
	void RightTargetChanged();
	void EntriesTopChanged();
	// QML maps only signals that start in lower case to on... handlers.
	void libraryRequested();  // NOLINT(readability-identifier-naming)
	void addGameRequested();  // NOLINT(readability-identifier-naming)
	void settingsRequested(); // NOLINT(readability-identifier-naming)
	void aboutRequested();    // NOLINT(readability-identifier-naming)
	void quitRequested();     // NOLINT(readability-identifier-naming)

public:
	explicit SidebarItem(QQuickItem* parent = nullptr);
	~SidebarItem() override = default;

	[[nodiscard]] QString     Current() const { return m_current; }
	void                      SetCurrent(const QString& current);
	[[nodiscard]] QQuickItem* RightTarget() const { return m_right_target; }
	void                      SetRightTarget(QQuickItem* target);
	[[nodiscard]] qreal       EntriesTop() const { return m_entries_top; }
	void                      SetEntriesTop(qreal top);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     focusInEvent(QFocusEvent* event) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     mouseReleaseEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     hoverLeaveEvent(QHoverEvent* event) override;

private:
	[[nodiscard]] QRectF EntryRect(int index) const;
	[[nodiscard]] int    EntryAt(QPointF point) const;
	[[nodiscard]] int    CurrentEntry() const;
	void                 Activate(int index);
	void                 SetHovered(int index);

	QString              m_current;
	QPointer<QQuickItem> m_right_target;
	qreal                m_entries_top = 0;
	int                  m_focused     = 0;
	int                  m_hovered     = -1;
	int                  m_pressed     = -1;
	// The icons drawn at their size on screen, painted again when it changes.
	QVector<QImage> m_icons;
	int             m_icon_px = 0;
};

#endif /* LAUNCHER_INCLUDE_SIDEBAR_ITEM_H_ */
