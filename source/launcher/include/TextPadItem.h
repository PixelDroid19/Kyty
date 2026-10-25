#ifndef LAUNCHER_INCLUDE_TEXT_PAD_ITEM_H_
#define LAUNCHER_INCLUDE_TEXT_PAD_ITEM_H_

#include "Common.h"

#include <QQuickItem>
#include <QString>

// An on-screen keyboard over a scrim, so a controller can enter names and paths. It does not keep the text: each
// key reports the text it would make, and the owner sets the text it accepts back. While it is open no key
// reaches the page below; typed characters and Backspace edit the text too.
class TextPadItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(TextPadItem);

signals:
	void ShownChanged();
	void Edited(const QString& text);
	// Done was pressed; Escape and a click outside close the pad without it.
	void Accepted();

public:
	explicit TextPadItem(QQuickItem* parent = nullptr);
	~TextPadItem() override = default;

	[[nodiscard]] bool    Shown() const { return m_shown; }
	[[nodiscard]] QString Key() const { return m_key; }
	[[nodiscard]] QString Text() const { return m_text; }
	void                  SetText(const QString& text);

	// Opens the pad for the field named key, with title above the text.
	void OpenFor(const QString& key, const QString& title);
	void Close();

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     mouseReleaseEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     wheelEvent(QWheelEvent* event) override;

private:
	// Where the panel's parts go in the item.
	struct Geometry
	{
		QRectF  panel;
		QRectF  title;
		QRectF  text;
		QPointF keys; // Top left of the first key's cell.
	};

	[[nodiscard]] Geometry Layout() const;
	[[nodiscard]] QRectF   KeyRect(const Geometry& geometry, int index) const;
	[[nodiscard]] int      KeyAt(QPointF point) const;
	[[nodiscard]] QString  Label(int index) const;
	void                   Press(int index);
	void                   MoveCurrent(int index);

	bool    m_shown   = false;
	bool    m_upper   = false;
	int     m_current = 0;
	QString m_key;
	QString m_title;
	QString m_text;
};

#endif /* LAUNCHER_INCLUDE_TEXT_PAD_ITEM_H_ */
