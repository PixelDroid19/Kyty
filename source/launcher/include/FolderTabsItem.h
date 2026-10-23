#ifndef LAUNCHER_INCLUDE_FOLDER_TABS_ITEM_H_
#define LAUNCHER_INCLUDE_FOLDER_TABS_ITEM_H_

#include "Common.h"
#include "ViewScroll.h"

#include <QQuickItem>
#include <QVariantList>
#include <QVector>
#include <QtQml/qqmlregistration.h>

// The library's folders as tabs, each with its game count, the selected one underlined. Left and Right change
// the folder; Left on the first tab and Right on the last are left to KeyNavigation. Each tab is padded so its
// focus ring fits; place the item that much to the left to line the first name up with the text above.
class FolderTabsItem: public QQuickItem
{
	Q_OBJECT
	QML_NAMED_ELEMENT(FolderTabs)
	KYTY_QT_CLASS_NO_COPY(FolderTabsItem);

	Q_PROPERTY(QVariantList folders READ Folders WRITE SetFolders NOTIFY FoldersChanged)
	Q_PROPERTY(int currentIndex READ CurrentIndex WRITE SetCurrentIndex NOTIFY CurrentIndexChanged)
	Q_PROPERTY(qreal tabPadding READ TabPadding NOTIFY TabPaddingChanged)

signals:
	void FoldersChanged();
	void CurrentIndexChanged();
	void TabPaddingChanged();

public:
	explicit FolderTabsItem(QQuickItem* parent = nullptr);
	~FolderTabsItem() override = default;

	[[nodiscard]] QVariantList Folders() const { return m_folders; }
	void                       SetFolders(const QVariantList& folders);
	[[nodiscard]] int          CurrentIndex() const { return m_current; }
	void                       SetCurrentIndex(int index);
	[[nodiscard]] qreal        TabPadding() const;

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;

private:
	struct Tab
	{
		QString title;
		QString count;
		qreal   x          = 0;
		qreal   width      = 0;
		qreal   name_width = 0;
	};

	void              Layout();
	void              ShowCurrent(bool animated);
	[[nodiscard]] int IndexAt(QPointF point) const;

	QVariantList m_folders;
	QVector<Tab> m_tabs;
	int          m_current = 0;
	ViewScroll   m_scroll;
	// Bumped on every layout, so the scene rebuilds its texts.
	int m_layout = 0;
};

#endif /* LAUNCHER_INCLUDE_FOLDER_TABS_ITEM_H_ */
