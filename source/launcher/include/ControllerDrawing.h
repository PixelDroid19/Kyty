#ifndef LAUNCHER_INCLUDE_CONTROLLER_DRAWING_H_
#define LAUNCHER_INCLUDE_CONTROLLER_DRAWING_H_

#include "Common.h"

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QSet>
#include <QString>

class QPainter;

// The guest pad, a DualSense seen from above, drawn in a box 1 wide and CONTROLLER_HEIGHT tall. Parts are named
// like the controls of the input profile format ("cross", "l2", "touchpad", ...).
constexpr qreal CONTROLLER_HEIGHT = 0.66;

struct ControllerLook
{
	QColor body;
	QColor line;
	QColor accent;
	// Parts drawn lit, and the ones drawn lit and outlined, such as the part whose binding has the focus.
	QSet<QString> lit;
	QSet<QString> marked;
	// Where each stick cap sits, from -1 to 1 on each axis.
	QPointF left_stick;
	QPointF right_stick;
	// How strongly the light bar beside the touchpad glows, from 0 to 1.
	qreal light_bar = 0.5;
	// Line width in pixels.
	qreal stroke = 1.5;
};

// The outline of the body, grips included, without the shoulder buttons.
[[nodiscard]] QPainterPath ControllerBody();

// Where a part is, for a line pointing at it; the body's centre for a name it does not know.
[[nodiscard]] QPointF ControllerAnchor(const QString& part);

// Paints the controller into rect, which should be CONTROLLER_HEIGHT as tall as it is wide.
void PaintController(QPainter* painter, const QRectF& rect, const ControllerLook& look);

#endif /* LAUNCHER_INCLUDE_CONTROLLER_DRAWING_H_ */
