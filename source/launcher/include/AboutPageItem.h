#ifndef LAUNCHER_INCLUDE_ABOUT_PAGE_ITEM_H_
#define LAUNCHER_INCLUDE_ABOUT_PAGE_ITEM_H_

#include "PageItem.h"

#include <QPointer>

// What the launcher is and its controls for keyboard, controller and mouse.
class AboutPageItem: public PageItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(AboutPageItem);

public:
	explicit AboutPageItem(QQuickItem* parent = nullptr);
	~AboutPageItem() override = default;

	[[nodiscard]] QVariantList Hints() const override;
	[[nodiscard]] QQuickItem*  Mascot() const { return m_mascot; }
	// The mascot shown in the header; the page sizes it and scrolls it with the content.
	void                       SetMascot(QQuickItem* mascot);

protected:
	void Compose(PageLayout* layout) override;
	void Press(int id) override;
	void Placed() override;

private:
	void AddHeader(PageLayout* layout);
	void AddControls(PageLayout* layout) const;

	QPointer<QQuickItem> m_mascot;
	// Clips the mascot to the scrolled area, as the content's nodes are.
	QQuickItem* m_mascot_clip = nullptr;
	// Where the mascot goes, in content coordinates.
	QRectF m_mascot_slot;
};

#endif /* LAUNCHER_INCLUDE_ABOUT_PAGE_ITEM_H_ */
