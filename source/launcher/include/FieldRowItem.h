#ifndef LAUNCHER_INCLUDE_FIELD_ROW_ITEM_H_
#define LAUNCHER_INCLUDE_FIELD_ROW_ITEM_H_

#include "Common.h"
#include "PageLayout.h"

#include <QQuickItem>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

class ChoiceItem;
class FocusButtonItem;
class ModuleRowItem;
class TextFieldItem;
class ToggleItem;

// Number fields take whole numbers up to 99999.
constexpr int NUMBER_DIGITS = 5;

enum class FieldType
{
	Text,
	Number,
	Toggle,
	Choice,
	Path,
	Modules,
	Params,
};

// One field of the editor's form. Texts are untranslated; the editor translates them in its own context.
struct EditorField
{
	const char* key   = nullptr;
	const char* label = nullptr;
	FieldType   type  = FieldType::Text;
	// The list in LauncherBackend::Choices() a choice field picks from.
	const char* choices = nullptr;
	// A path field picks a folder instead of a file.
	bool folder = false;
	// Whether the field applies to the current values; always when not set.
	bool (*when)(const QVariantMap& values) = nullptr;
	// Starts a new section of the form.
	const char* heading = nullptr;
};

// One line of the editor: an optional section heading, the label and the control for the field's type. A hidden
// row takes no space.
class FieldRowItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(FieldRowItem);

signals:
	void Edited(const QString& key, const QVariant& value);
	void PadRequested(const QString& key, const QString& title);
	void BrowseRequested(const QString& key, bool folder);

public:
	FieldRowItem(const EditorField& field, QQuickItem* parent);
	~FieldRowItem() override = default;

	[[nodiscard]] const EditorField& Field() const { return m_field; }
	[[nodiscard]] bool               Shown() const { return m_shown; }
	// The row's controls the arrow keys move through, from the top.
	[[nodiscard]] QVector<QQuickItem*> FocusItems() const;

	// Shows the field's value out of values, or hides the row.
	void SetValues(const QVariantMap& values, bool shown);
	// Places the row's parts for a row of this width; returns the height the row takes.
	qreal Layout(qreal width);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;

private:
	void ShowValue();
	// Places the control in the space beside the label; returns the height it takes.
	qreal PlaceControl(const QRectF& rect);
	void  ShowModules(const QVariantList& modules);
	void  Step(int delta);
	void  ToggleModule(int index);

	EditorField      m_field;
	bool             m_shown = true;
	QVariant         m_value;
	QString          m_basedir;
	QStringList      m_choices;
	QString          m_params_file;
	QVariantList     m_params;
	TextFieldItem*   m_input  = nullptr;
	ToggleItem*      m_toggle = nullptr;
	ChoiceItem*      m_choice = nullptr;
	FocusButtonItem* m_minus  = nullptr;
	FocusButtonItem* m_plus   = nullptr;
	// Browse on a path field, Rescan on the modules.
	FocusButtonItem*          m_button = nullptr;
	QVector<ModuleRowItem*>   m_modules;
	QVector<PageLayout::Text> m_texts;
	// Bumped whenever m_texts is replaced, so the nodes are built again.
	int m_generation = 0;
};

#endif /* LAUNCHER_INCLUDE_FIELD_ROW_ITEM_H_ */
