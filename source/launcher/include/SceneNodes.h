#ifndef LAUNCHER_INCLUDE_SCENE_NODES_H_
#define LAUNCHER_INCLUDE_SCENE_NODES_H_

#include "Common.h"

#include <QBrush>
#include <QColor>
#include <QRectF>
#include <QSGNode>

class QQuickWindow;
class QSGImageNode;
class QSGTexture;

// Scene graph building blocks for the launcher's items. They use only the node types every Qt Quick backend
// draws (rectangle and image nodes), so the same items render through the GPU and through the CPU renderer.

// A gradient along one axis as a 256-texel strip, to be stretched over a rectangle by an image node.
QSGTexture* CreateGradientTexture(QQuickWindow* window, const QGradientStops& stops, Qt::Orientation orientation);

// An image node showing a whole texture over rect; the node owns the texture.
QSGImageNode* CreateTextureNode(QQuickWindow* window, QSGTexture* texture, const QRectF& rect);

// A rounded rectangle with an optional border, drawn as nine slices of one small texture: the corners at their
// real size and the edges and middle stretched. The texture is painted again only when the style or the
// device pixel ratio changes.
class RoundedBoxNode: public QSGNode
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

#endif /* LAUNCHER_INCLUDE_SCENE_NODES_H_ */
