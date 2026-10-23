#ifndef LAUNCHER_INCLUDE_FIELD_CONTROLS_H_
#define LAUNCHER_INCLUDE_FIELD_CONTROLS_H_

#include "Common.h"

#include <QQuickItem>
#include <QString>
#include <QVariantAnimation>

// The editor's single-value controls. Each reports what the user asked for and shows whatever value the editor
// sets back, so the editor stays the one owner of the values.
class FieldControlItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(FieldControlItem);

signals:
	// Enter, Return, Space or a click step by 1; Left and Right on a choice step by -1 and 1.
	void Activated(int delta);

public:
	~FieldControlItem() override = default;

protected:
	explicit FieldControlItem(QQuickItem* parent);

	void itemChange(ItemChange change, const ItemChangeData& value) override;
	void keyPressEvent(QKeyEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseReleaseEvent(QMouseEvent* event) override;
};

// An on/off switch with its state written beside it.
class ToggleItem: public FieldControlItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ToggleItem);

public:
	explicit ToggleItem(QQuickItem* parent = nullptr);
	~ToggleItem() override = default;

	void SetOn(bool on);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;

private:
	bool m_on = false;
	// Where the knob is, from 0 (off) to 1 (on); it slides when the value changes.
	qreal             m_knob = 0;
	QVariantAnimation m_slide;
};

// One value out of a fixed list.
class ChoiceItem: public FieldControlItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ChoiceItem);

public:
	explicit ChoiceItem(QQuickItem* parent = nullptr);
	~ChoiceItem() override = default;

	void SetValue(const QString& value);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     keyPressEvent(QKeyEvent* event) override;

private:
	QString m_value;
};

// One module of a game with a tick box; a module missing from the game folder is listed but cannot be ticked.
class ModuleRowItem: public FieldControlItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ModuleRowItem);

public:
	explicit ModuleRowItem(QQuickItem* parent = nullptr);
	~ModuleRowItem() override = default;

	void SetModule(const QString& path, bool present, bool selected);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;

private:
	QString m_path;
	bool    m_present  = false;
	bool    m_selected = false;
};

#endif /* LAUNCHER_INCLUDE_FIELD_CONTROLS_H_ */
