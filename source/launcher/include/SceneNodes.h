#ifndef LAUNCHER_INCLUDE_SCENE_NODES_H_
#define LAUNCHER_INCLUDE_SCENE_NODES_H_

#include "Common.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QRectF>
#include <QSGGeometry>
#include <QSGNode>
#include <QSGTransformNode>

class QQuickWindow;
class QSGImageNode;
class QSGTextNode;
class QSGTexture;

// Scene graph building blocks for the launcher's items. They use only the node types every Qt Quick backend
// draws (rectangle and image nodes), so the same items render through the GPU and through the CPU renderer.

// A gradient along one axis as a 256-texel strip, to be stretched over a rectangle by an image node.
QSGTexture* CreateGradientTexture(QQuickWindow* window, const QGradientStops& stops, Qt::Orientation orientation);

// Groups nodes whose children are added or moved after the first frame. When such a change happens under a plain
// QSGNode, the CPU renderer starts again from the identity transform and draws the subtree at the window's
// origin; under a transform node it keeps the item's position, so groups are identity transform nodes.
using GroupNode = QSGTransformNode;

// An image node showing a whole texture over rect; the node owns the texture.
QSGImageNode* CreateTextureNode(QQuickWindow* window, QSGTexture* texture, const QRectF& rect);

// A rounded rectangle with an optional border, drawn as nine slices of one small texture: the corners at their
// real size and the edges and middle stretched. The texture is painted again only when the style or the
// device pixel ratio changes.
class RoundedBoxNode: public GroupNode
{
public:
	RoundedBoxNode() = default;
	~RoundedBoxNode() override;

	KYTY_QT_CLASS_NO_COPY(RoundedBoxNode);

	struct Style
	{
		QColor fill;
		QColor border;
		qreal  border_width = 0;
		qreal  radius       = 0;
		qreal  dpr          = 1;

		bool operator==(const Style& other) const
		{
			return fill == other.fill && border == other.border && border_width == other.border_width && radius == other.radius &&
			       dpr == other.dpr;
		}
	};

	void Update(QQuickWindow* window, const QRectF& bounds, const Style& style);

private:
	void CreateSlices(QQuickWindow* window, const Style& style);

	static constexpr int SLICE_COUNT = 9;

	QSGTexture*   m_texture = nullptr;
	QSGImageNode* m_slices[SLICE_COUNT] {};
	Style         m_style;
	// Size of a corner in texture pixels; the texture is two corners and one stretched pixel wide and high.
	int m_corner_px = 0;
};

// A rectangular clip. The GPU renderers apply it as a scissor and the CPU renderer as a clip rectangle.
class RectClipNode: public QSGClipNode
{
public:
	RectClipNode();
	~RectClipNode() override = default;

	KYTY_QT_CLASS_NO_COPY(RectClipNode);

	void SetRect(const QRectF& rect);

private:
	QSGGeometry m_geometry;
};

// The application font at a pixel size, as a QML Text item without a font family uses it.
QFont SceneFont(qreal pixel_size, QFont::Weight weight = QFont::Normal);

// The same, in the platform's fixed-width family.
QFont SceneMonospaceFont(qreal pixel_size);

// Width of text set on one line in font.
qreal TextWidth(const QString& text, const QFont& font);

// Replaces what node shows with one line of text, centred vertically in rect, aligned horizontally by alignment
// and elided at its end when it is wider than rect.
void SetTextLine(QSGTextNode* node, const QString& text, const QFont& font, const QColor& color, const QRectF& rect,
                 Qt::Alignment alignment = Qt::AlignLeft);

// The same with the line's baseline starting at origin, elided past max_width; lines up text of different sizes.
void SetTextAtBaseline(QSGTextNode* node, const QString& text, const QFont& font, const QColor& color, QPointF origin, qreal max_width);

// The baseline SetTextLine gives a line of font centred in rect.
qreal CentredBaseline(const QFont& font, const QRectF& rect);

#endif /* LAUNCHER_INCLUDE_SCENE_NODES_H_ */
