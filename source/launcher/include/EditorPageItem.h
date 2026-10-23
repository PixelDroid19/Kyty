#ifndef LAUNCHER_INCLUDE_EDITOR_PAGE_ITEM_H_
#define LAUNCHER_INCLUDE_EDITOR_PAGE_ITEM_H_

#include "Common.h"
#include "FileChooser.h"
#include "PageLayout.h"
#include "ViewScroll.h"

#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class FieldRowItem;
class FocusButtonItem;
class TextPadItem;
struct EditorField;

// The form for one game, or for the global emulator settings that games without custom settings run with. Fields
// that do not apply to the current choices are hidden. Up and Down move through the fields and the header's
// buttons, O saves, E starts a test run of a game and Escape cancels.
class EditorPageItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(EditorPageItem);

signals:
	void ModeChanged();
	void Saved(int row);
	void Cancelled();

public:
	explicit EditorPageItem(QQuickItem* parent = nullptr);
	~EditorPageItem() override = default;

	// Edits the global emulator settings instead of a game.
	[[nodiscard]] bool         Global() const { return m_global; }
	// What the page's keys do, for the hint bar; see HintBarItem.
	[[nodiscard]] QVariantList Hints() const;

	// Edits the game in row, or a new game when row is negative, starting from fields.
	void OpenGame(int row, const QVariantMap& fields);
	void OpenGlobal();

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     wheelEvent(QWheelEvent* event) override;

private:
	void Open(bool global, int row, const QVariantMap& values);
	void BuildRows();
	void AddRow(const EditorField& field, bool custom_only);
	void SetValues(const QVariantMap& values);
	void SetField(const QString& key, const QVariant& value);
	void OpenPad(const QString& key, const QString& title);
	void PadEdited(const QString& text);
	void Browse(const QString& key, bool folder);
	void Save();
	void TestRun();
	void RestoreDefaults();

	[[nodiscard]] QVector<FocusButtonItem*> HeaderButtons() const;
	[[nodiscard]] QVector<QQuickItem*>      FocusItems() const;
	[[nodiscard]] FieldRowItem*             RowFor(const QString& key) const;
	[[nodiscard]] qreal                     ScrollLimit() const;
	void                                    MoveFocus(int delta);
	// Moves between the header's buttons; false when the focus is not on them.
	bool MoveAcross(int delta);
	void FocusItem(QQuickItem* item);
	void FocusFirst();
	void FocusKey(const QString& key);

	bool             m_global     = false;
	bool             m_rows_built = false;
	int              m_row        = -1;
	QVariantMap      m_values;
	QString          m_error;
	QString          m_status;
	QString          m_browse_key;
	FocusButtonItem* m_save     = nullptr;
	FocusButtonItem* m_test     = nullptr;
	FocusButtonItem* m_defaults = nullptr;
	FocusButtonItem* m_cancel   = nullptr;
	// Clips the form below the header; the content inside it moves with the scroll.
	QQuickItem*            m_viewport = nullptr;
	QQuickItem*            m_content  = nullptr;
	QVector<FieldRowItem*> m_rows;
	// Per row: a game's emulator field, shown only while the game has custom settings.
	QVector<bool>             m_custom_only;
	TextPadItem*              m_pad = nullptr;
	FileChooser               m_chooser;
	ViewScroll                m_scroll;
	QVector<PageLayout::Text> m_texts;
	// Bumped whenever m_texts is replaced, so the nodes are built again.
	int m_generation = 0;
};

#endif /* LAUNCHER_INCLUDE_EDITOR_PAGE_ITEM_H_ */
