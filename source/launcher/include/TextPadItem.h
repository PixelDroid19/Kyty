#ifndef LAUNCHER_INCLUDE_TEXT_PAD_ITEM_H_
#define LAUNCHER_INCLUDE_TEXT_PAD_ITEM_H_

#include "Common.h"

#include <QQuickItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

// An on-screen keyboard over a scrim, so a controller can enter names and paths. It does not keep the text: each
// key reports the text it would make, and the owner sets the text it accepts back. While it is open no key
// reaches the page below; typed characters and Backspace edit the text too.
class TextPadItem: public QQuickItem
{
	Q_OBJECT
	QML_NAMED_ELEMENT(TextPad)
	KYTY_QT_CLASS_NO_COPY(TextPadItem);

	Q_PROPERTY(bool shown READ Shown NOTIFY shownChanged)
	Q_PROPERTY(QString key READ Key NOTIFY shownChanged)
	Q_PROPERTY(QString text READ Text WRITE SetText NOTIFY TextChanged)

signals:
	void TextChanged();
	// QML maps only signals that start in lower case to on... handlers.
	void shownChanged();              // NOLINT(readability-identifier-naming)
	void edited(const QString& text); // NOLINT(readability-identifier-naming)

public:
	explicit TextPadItem(QQuickItem* parent = nullptr);
	~TextPadItem() override = default;

	[[nodiscard]] bool    Shown() const { return m_shown; }
	[[nodiscard]] QString Key() const { return m_key; }
	[[nodiscard]] QString Text() const { return m_text; }
	void                  SetText(const QString& text);

	// Opens the pad for the field named key, with title above the text.
	Q_INVOKABLE void openFor(const QString& key, const QString& title); // NOLINT(readability-identifier-naming)
	Q_INVOKABLE void close();                                           // NOLINT(readability-identifier-naming)

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
