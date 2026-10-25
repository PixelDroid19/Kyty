#ifndef LAUNCHER_INCLUDE_GAME_BACKDROP_ITEM_H_
#define LAUNCHER_INCLUDE_GAME_BACKDROP_ITEM_H_

#include "Common.h"

#include <QImage>
#include <QQuickItem>
#include <QString>
#include <QTimer>
#include <QVariantAnimation>

// The focused game's key art behind the whole window, darkened so the pages stay readable.
// A new picture loads only after the focus has rested a moment, so scrolling decodes nothing.
// It loads behind the current one and dissolves over it once decoded, so the window never dips to black;
// the picture underneath is released as soon as it is covered, so at most two are held for a moment.
class GameBackdropItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(GameBackdropItem);

public:
	explicit GameBackdropItem(QQuickItem* parent = nullptr);
	~GameBackdropItem() override = default;

	// image:// address of the picture to show; empty shows none.
	[[nodiscard]] const QString& Picture() const { return m_picture; }
	void                         SetPicture(const QString& picture);
	// Darkens the art more, for a calmer window; set before the item is first drawn.
	void SetDimmed(bool dimmed) { m_dimmed = dimmed; }

	// A decoded picture and how it is drawn. The key names the picture for the render thread, which keeps one
	// texture per key; 0 is no picture.
	struct Layer
	{
		QString url;
		QImage  image;
		quint64 key     = 0;
		qreal   opacity = 0;
		qreal   scale   = 1;
	};

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

private:
	void Show();
	void Reveal(const QString& url, const QImage& image);
	void StepDissolve(qreal progress);
	void FinishDissolve();
	void ReleaseFront();

	QString           m_picture;
	bool              m_dimmed = false;
	QTimer            m_settle;
	QVariantAnimation m_dissolve;
	QVariantAnimation m_vanish;
	// The picture on top, and the one it is dissolving over.
	Layer   m_front;
	Layer   m_covered;
	quint64 m_next_key = 1;
	// Front opacity when the vanish started, so a picture caught mid-dissolve fades from where it was.
	qreal m_vanish_from = 1;
};

#endif /* LAUNCHER_INCLUDE_GAME_BACKDROP_ITEM_H_ */
