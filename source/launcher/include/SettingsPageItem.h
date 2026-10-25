#ifndef LAUNCHER_INCLUDE_SETTINGS_PAGE_ITEM_H_
#define LAUNCHER_INCLUDE_SETTINGS_PAGE_ITEM_H_

#include "Common.h"
#include "PageLayout.h"

#include <QQuickItem>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class FieldFormItem;
class FocusButtonItem;
class SettingsRailItem;
class TextPadItem;

// The settings: categories down the left, the selected category's settings on the right. The emulator's settings
// are the ones every game without custom settings runs with and are saved as they change; the launcher's own
// apply at once. Up and Down pick a category, Right or Enter goes into it, Escape goes back out and closes the
// page from the categories.
class SettingsPageItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(SettingsPageItem);

signals:
	void Closed();
	// The user wants to pick the emulator program in a file dialog.
	void ProgramRequested();

public:
	// What the window keeps when it builds its pages again, so the user stays where they were.
	struct Place
	{
		int     category = 0;
		QString key;
	};

	explicit SettingsPageItem(QQuickItem* parent = nullptr);
	~SettingsPageItem() override;

	[[nodiscard]] static QVariantList Hints();
	[[nodiscard]] Place               CurrentPlace() const;

	// Puts the focus on the categories.
	void FocusDefault();
	// Shows the category of place with the focus on its field.
	void GoTo(const Place& place);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;

private:
	struct Dot
	{
		QRectF rect;
		QColor color;
	};

	void ShowCategory();
	void Enter();
	void Edited(const QString& key);
	void SaveGlobal();
	void RestoreDefaults();
	void AddInfo(const QString& label, const QString& value, const QColor& color, const QColor& dot, qreal x, qreal width, qreal* y);

	[[nodiscard]] bool                      EmulatorCategory() const;
	[[nodiscard]] QVector<FocusButtonItem*> Buttons() const;
	// Moves between the header's buttons; false when the focus is not on them.
	bool MoveAcross(int delta);

	SettingsRailItem* m_rail     = nullptr;
	FieldFormItem*    m_form     = nullptr;
	TextPadItem*      m_pad      = nullptr;
	FocusButtonItem*  m_choose   = nullptr;
	FocusButtonItem*  m_search   = nullptr;
	FocusButtonItem*  m_log      = nullptr;
	FocusButtonItem*  m_defaults = nullptr;
	// The global emulator settings, saved a moment after the last change.
	QVariantMap               m_global;
	QTimer                    m_save_timer;
	QString                   m_status;
	QString                   m_error;
	QString                   m_last_key;
	QRectF                    m_divider;
	QVector<PageLayout::Text> m_texts;
	QVector<Dot>              m_dots;
	// Bumped whenever m_texts is replaced, so the nodes are built again.
	int m_generation = 0;
};

#endif /* LAUNCHER_INCLUDE_SETTINGS_PAGE_ITEM_H_ */
