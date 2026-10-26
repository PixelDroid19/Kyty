#ifndef LAUNCHER_INCLUDE_CONTROLS_PANEL_ITEM_H_
#define LAUNCHER_INCLUDE_CONTROLS_PANEL_ITEM_H_

#include "Common.h"
#include "PageLayout.h"

#include <QHash>
#include <QPolygonF>
#include <QQuickItem>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

class BindingItem;
class ChoiceItem;
class ChoiceListItem;
class FolderTabsItem;
class StickPreviewItem;
class TextFieldItem;
class TextPadItem;
class ToggleItem;

// The active input profile on one panel: the profile, its name and its controller on top, then tabs with the
// guest controller drawn in the middle. Around it, each part's binding on a controller or on the keyboard and
// mouse, with a line to the part; the sticks, triggers and mouse look have tabs of their own. The parts the
// connected controller holds down light up. Edits are reported and the owner sets the saved values back. The
// arrow keys move to the nearest control in their direction, L1 and R1 (Page Up and Page Down) change the tab, and
// Up from the top row is left to the owner, like Left and Right with nothing in that direction.
class ControlsPanelItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ControlsPanelItem);

signals:
	void Edited(const QString& key, const QVariant& value);
	// A binding asks the owner to wait for a key or mouse button, or with gamepad for a controller button.
	void CaptureRequested(const QString& key, bool gamepad);
	void TopReached();

public:
	// The profile name is edited with a controller on pad; the choice lists open over page.
	ControlsPanelItem(TextPadItem* pad, QQuickItem* page);
	~ControlsPanelItem() override = default;

	[[nodiscard]] const QVariantMap& Values() const { return m_values; }
	void                             SetValues(const QVariantMap& values);

	void FocusFirst();
	// Focuses the binding of key on a controller or on the keyboard, showing its tab.
	void FocusKey(const QString& key, bool gamepad);

	// What a binding key ("bind.l2") is called, for the capture's title.
	[[nodiscard]] static QString ControlTitle(const QString& key);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;

private:
	enum Tab
	{
		PadTab,
		KeysTab,
		SticksTab,
		MouseTab,
		TAB_COUNT
	};

	struct Binding
	{
		QString      control;
		BindingItem* chip = nullptr;
	};

	struct StickCard
	{
		StickPreviewItem* preview  = nullptr;
		ChoiceItem*       deadzone = nullptr;
		ToggleItem*       invert_x = nullptr;
		ToggleItem*       invert_y = nullptr;
	};

	// A line from a control to the part of the controller it is about.
	struct Lead
	{
		QPolygonF   points;
		QString     part;
		QQuickItem* owner = nullptr;
	};

	BindingItem* AddBinding(const QString& control, bool gamepad);
	ChoiceItem*  AddChoice(const QString& key, const char* list);
	ToggleItem*  AddToggle(const QString& key);
	void         OpenList(const QString& key, const char* list, ChoiceItem* box);
	void         Step(const QString& key, const char* list, int delta);
	void         ShowBindings();
	void         ShowTab();
	void         Poll();

	void  LayoutBindings(const QRectF& area, bool gamepad);
	void  LayoutSticks(const QRectF& area);
	void  LayoutMouse(const QRectF& area);
	void  AddLead(QQuickItem* owner, const QPointF& from, bool from_left, const QString& part);
	qreal AddCardTitle(const QRectF& card, const QString& title);
	qreal PlaceLabelled(QQuickItem* item, const QString& label, qreal x, qreal y, qreal width, qreal height);
	void  AddNote(const QRectF& rect, const QString& text);

	[[nodiscard]] bool                 Editable() const;
	[[nodiscard]] QVector<QQuickItem*> TabItems(int tab) const;
	[[nodiscard]] QVector<QQuickItem*> FocusItems() const;
	[[nodiscard]] QQuickItem*          Focused() const;
	[[nodiscard]] QSet<QString>        MarkedParts() const;
	bool                               MoveFocus(int dx, int dy);
	void                               ChangeTab(int delta);

	QVariantMap     m_values;
	TextPadItem*    m_pad  = nullptr;
	ChoiceListItem* m_list = nullptr;
	QString         m_list_key;
	QStringList     m_list_values;

	ChoiceItem*      m_profile = nullptr;
	TextFieldItem*   m_name    = nullptr;
	ChoiceItem*      m_device  = nullptr;
	FolderTabsItem*  m_tabs    = nullptr;
	QVector<Binding> m_pad_bindings;
	QVector<Binding> m_key_bindings;
	StickCard        m_sticks[2];
	ChoiceItem*      m_trigger     = nullptr;
	ToggleItem*      m_mouse_look  = nullptr;
	ChoiceItem*      m_sensitivity = nullptr;
	ToggleItem*      m_mouse_invert = nullptr;
	// The choices and switches by the key they edit.
	QHash<QString, QQuickItem*> m_by_key;

	QRectF                    m_drawing;
	QRectF                    m_art;
	QVector<Lead>             m_leads;
	QVector<PageLayout::Text> m_texts;
	QVector<PageLayout::Box>  m_boxes;
	// Bumped whenever the texts and boxes are replaced, and whenever the drawing must be painted again.
	int m_generation     = 0;
	int m_art_generation = 0;

	// What the connected controller holds, read while the panel is on screen.
	QTimer        m_poll;
	QSet<QString> m_held;
	QPointF       m_left_stick;
	QPointF       m_right_stick;
};

#endif /* LAUNCHER_INCLUDE_CONTROLS_PANEL_ITEM_H_ */
