#ifndef LAUNCHER_INCLUDE_EDITOR_PAGE_ITEM_H_
#define LAUNCHER_INCLUDE_EDITOR_PAGE_ITEM_H_

#include "Common.h"
#include "PageLayout.h"

#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class FieldFormItem;
class FocusButtonItem;
class TextPadItem;

// The form for one game. Fields that do not apply to the current choices are hidden. Up and Down move through the
// fields and the header's buttons, O saves, E starts a test run and Escape cancels.
class EditorPageItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(EditorPageItem);

signals:
	void Saved(int row);
	void Cancelled();

public:
	explicit EditorPageItem(QQuickItem* parent = nullptr);
	~EditorPageItem() override = default;

	// What the page's keys do, for the hint bar; see HintBarItem.
	[[nodiscard]] QVariantList Hints() const;

	// Edits the game in row, or a new game when row is negative, starting from fields.
	void OpenGame(int row, const QVariantMap& fields);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     keyPressEvent(QKeyEvent* event) override;

private:
	void Save();
	void TestRun();

	[[nodiscard]] QVector<FocusButtonItem*> HeaderButtons() const;
	// Moves between the header's buttons; false when the focus is not on them.
	bool MoveAcross(int delta);
	[[nodiscard]] bool HeaderFocused() const;

	int                       m_row = -1;
	QString                   m_error;
	QString                   m_status;
	FocusButtonItem*          m_save   = nullptr;
	FocusButtonItem*          m_test   = nullptr;
	FocusButtonItem*          m_cancel = nullptr;
	TextPadItem*              m_pad    = nullptr;
	FieldFormItem*            m_form   = nullptr;
	QVector<PageLayout::Text> m_texts;
	// Bumped whenever m_texts is replaced, so the nodes are built again.
	int m_generation = 0;
};

#endif /* LAUNCHER_INCLUDE_EDITOR_PAGE_ITEM_H_ */
