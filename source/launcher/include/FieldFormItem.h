#ifndef LAUNCHER_INCLUDE_FIELD_FORM_ITEM_H_
#define LAUNCHER_INCLUDE_FIELD_FORM_ITEM_H_

#include "Common.h"
#include "FileChooser.h"
#include "ViewScroll.h"

#include <QQuickItem>
#include <QVariantMap>
#include <QVector>

class FieldRowItem;
class TextPadItem;
struct EditorField;

// A scrolling column of fields over one set of values; fields that do not apply to the current values are
// hidden. Up and Down move through the fields' controls and keep the focused one in view; Up from the first
// control is left to the owner. Edits change the values at once and are reported, the owner decides when to keep
// them. Text fields open the owner's on-screen keyboard and path fields the platform's file dialog.
class FieldFormItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(FieldFormItem);

signals:
	void Edited(const QString& key);
	// Up was pressed on the first control.
	void TopReached();

public:
	// Text fields edited with a controller type on pad, which covers the owner's page.
	FieldFormItem(TextPadItem* pad, QQuickItem* parent);
	~FieldFormItem() override = default;

	[[nodiscard]] const QVariantMap& Values() const { return m_values; }
	[[nodiscard]] bool               HasControls() const;

	void Clear();
	// Adds rows for fields; custom_only rows show only while the values have custom_settings on.
	void AddFields(const QVector<EditorField>& fields, bool custom_only = false);
	void SetValues(const QVariantMap& values);
	// Sets one value as if the user had edited it.
	void SetField(const QString& key, const QVariant& value);

	// Scrolls to the top and focuses the first control.
	void FocusFirst();
	void FocusKey(const QString& key);
	void ScrollToTop();

protected:
	void updatePolish() override;
	void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void keyPressEvent(QKeyEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;

private:
	void OpenPad(const QString& key, const QString& title);
	void PadEdited(const QString& text);
	void Browse(const QString& key, bool folder);

	[[nodiscard]] QVector<QQuickItem*> FocusItems() const;
	[[nodiscard]] FieldRowItem*        RowFor(const QString& key) const;
	[[nodiscard]] qreal                ScrollLimit() const;
	void                               MoveFocus(int delta);
	void                               FocusItem(QQuickItem* item);

	QVariantMap            m_values;
	QString                m_browse_key;
	QQuickItem*            m_content = nullptr;
	QVector<FieldRowItem*> m_rows;
	// Per row: a game's emulator field, shown only while the game has custom settings.
	QVector<bool> m_custom_only;
	TextPadItem*  m_pad = nullptr;
	FileChooser   m_chooser;
	ViewScroll    m_scroll;
};

#endif /* LAUNCHER_INCLUDE_FIELD_FORM_ITEM_H_ */
