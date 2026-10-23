#ifndef LAUNCHER_INCLUDE_COVER_NODE_H_
#define LAUNCHER_INCLUDE_COVER_NODE_H_

#include "Common.h"
#include "SceneNodes.h"

#include <QColor>
#include <QObject>
#include <QRectF>
#include <QSGNode>
#include <QVariantAnimation>

class QQuickItem;
class QQuickWindow;
class QSGImageNode;
class QSGOpacityNode;
class QSGTexture;
class QSGTransformNode;

// Game artwork drawn as a card: a fill under the art, the art, the caller's overlay, a hairline edge and, outside
// all of it, the focus ring. The card grows around its centre by its zoom; the ring follows its corners.
class CoverNode: public GroupNode
{
public:
	CoverNode();
	~CoverNode() override = default;

	KYTY_QT_CLASS_NO_COPY(CoverNode);

	struct Look
	{
		QRectF rect;
		// Shown under the art: a placeholder while it loads, or the whole card for a game without artwork.
		QColor fill;
		qreal  zoom = 1;
		bool   ring = false;
		qreal  dpr  = 1;
	};

	// art is borrowed from the view's ArtworkTextures; null shows the fill alone.
	void Update(QQuickWindow* window, const Look& look, QSGTexture* art);
	// Children drawn over the art and under the edge, in the same coordinates as Look::rect and zoomed with it.
	[[nodiscard]] QSGNode* Overlay() const { return m_overlay; }

private:
	QSGTransformNode* m_zoom         = nullptr;
	RoundedBoxNode*   m_fill         = nullptr;
	QSGImageNode*     m_art          = nullptr;
	QSGNode*          m_overlay      = nullptr;
	RoundedBoxNode*   m_edge         = nullptr;
	QSGOpacityNode*   m_ring_opacity = nullptr;
	RoundedBoxNode*   m_ring         = nullptr;
};

// Grows the focused card of a view and shrinks the one that lost the focus, over the theme's animation time.
class FocusZoom: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(FocusZoom);

public:
	// grown is the zoom of the focused card; item is updated on every step.
	FocusZoom(qreal grown, QQuickItem* item);
	~FocusZoom() override = default;

	// The index of the focused card, or -1 when the view does not have the focus.
	void                SetFocused(int index);
	[[nodiscard]] qreal Zoom(int index) const;

private:
	QVariantAnimation m_animation;
	qreal             m_grown;
	qreal             m_progress = 1;
	int               m_focused  = -1;
	int               m_previous = -1;
};

#endif /* LAUNCHER_INCLUDE_COVER_NODE_H_ */
