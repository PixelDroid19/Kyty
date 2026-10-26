#include "ControllerDrawing.h"

#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPathStroker>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Positions in the 1-wide box.
const QPointF DPAD(0.205, 0.235);
const QPointF FACE(0.795, 0.235);
const QPointF LEFT_STICK(0.355, 0.365);
const QPointF RIGHT_STICK(0.645, 0.365);
constexpr qreal FACE_SPREAD = 0.056;
constexpr qreal FACE_RADIUS = 0.028;
constexpr qreal ARM_LENGTH  = 0.048;
constexpr qreal ARM_WIDTH   = 0.042;
constexpr qreal ARM_GAP     = 0.01;
constexpr qreal WELL_RADIUS = 0.056;
constexpr qreal CAP_RADIUS  = 0.04;
// How far a cap moves at full tilt.
constexpr qreal CAP_TRAVEL = 0.014;

const QTransform MIRROR(-1, 0, 0, 1, 1, 0);

QPainterPath Mirrored(const QPainterPath& path)
{
	return MIRROR.map(path);
}

QPainterPath RoundedRect(const QRectF& rect, qreal radius)
{
	QPainterPath path;
	path.addRoundedRect(rect, radius, radius);
	return path;
}

QPainterPath Circle(QPointF centre, qreal radius)
{
	QPainterPath path;
	path.addEllipse(centre, radius, radius);
	return path;
}

QPainterPath Capsule(QPointF centre, qreal width, qreal height)
{
	return RoundedRect(QRectF(centre.x() - width / 2, centre.y() - height / 2, width, height), std::min(width, height) / 2);
}

// A band of the given width around a centre line, with round ends.
QPainterPath AlongCurve(const QPainterPath& centre, qreal width)
{
	// The stroker flattens curves to about a unit, so it works at a thousand times the size.
	constexpr qreal     SCALE = 1000;
	QPainterPathStroker stroker;
	stroker.setWidth(width * SCALE);
	stroker.setCapStyle(Qt::RoundCap);
	stroker.setJoinStyle(Qt::RoundJoin);
	const auto stroke = stroker.createStroke(QTransform::fromScale(SCALE, SCALE).map(centre)).simplified();
	return QTransform::fromScale(1 / SCALE, 1 / SCALE).map(stroke);
}

// The shoulder buttons seen from above: L1 a short bar lying on the shoulder's edge, L2 a wider, flatter one
// standing behind it. Their lower parts go under the body.
QPainterPath TriggerCentre()
{
	QPainterPath centre;
	centre.moveTo(0.272, 0.058);
	centre.cubicTo(0.255, 0.04, 0.173, 0.04, 0.156, 0.058);
	return centre;
}

QPainterPath BumperCentre()
{
	QPainterPath centre;
	centre.moveTo(0.292, 0.092);
	centre.cubicTo(0.27, 0.066, 0.24, 0.06, 0.205, 0.06);
	centre.cubicTo(0.165, 0.06, 0.135, 0.07, 0.118, 0.094);
	return centre;
}

QPainterPath Trigger()
{
	return AlongCurve(TriggerCentre(), 0.05);
}

QPainterPath Bumper()
{
	return AlongCurve(BumperCentre(), 0.032);
}

QPainterPath Touchpad()
{
	return RoundedRect(QRectF(0.345, 0.125, 0.31, 0.165), 0.022);
}

QPainterPath Arm(QPointF direction)
{
	// Each arm of the cross is a separate key pointing away from the centre, its inner end a blunt point. The
	// outline is drawn inset by the corner radius and then grown back, which rounds every corner.
	constexpr qreal CORNER = 0.006;
	const QPointF   across(-direction.y(), direction.x());
	const QPointF   inner = DPAD + direction * (ARM_GAP + CORNER);
	const QPointF   outer = DPAD + direction * (ARM_GAP + ARM_LENGTH - CORNER);
	const qreal     half  = ARM_WIDTH / 2 - CORNER;
	QPainterPath    core;
	core.moveTo(inner);
	core.lineTo(inner + direction * 0.014 + across * half);
	core.lineTo(outer + across * half);
	core.lineTo(outer - across * half);
	core.lineTo(inner + direction * 0.014 - across * half);
	core.closeSubpath();
	return core.united(AlongCurve(core, CORNER * 2));
}

QPointF FaceCentre(const QString& name)
{
	if (name == QLatin1String("triangle"))
	{
		return FACE + QPointF(0, -FACE_SPREAD);
	}
	if (name == QLatin1String("circle"))
	{
		return FACE + QPointF(FACE_SPREAD, 0);
	}
	if (name == QLatin1String("cross"))
	{
		return FACE + QPointF(0, FACE_SPREAD);
	}
	return FACE + QPointF(-FACE_SPREAD, 0);
}

QPointF ArmDirection(const QString& name)
{
	if (name == QLatin1String("up"))
	{
		return {0, -1};
	}
	if (name == QLatin1String("down"))
	{
		return {0, 1};
	}
	return name == QLatin1String("left") ? QPointF(-1, 0) : QPointF(1, 0);
}

bool IsFace(const QString& name)
{
	return name == QLatin1String("triangle") || name == QLatin1String("circle") || name == QLatin1String("cross") ||
	       name == QLatin1String("square");
}

bool IsArm(const QString& name)
{
	return name == QLatin1String("up") || name == QLatin1String("down") || name == QLatin1String("left") || name == QLatin1String("right");
}

QPointF CapCentre(bool right, const ControllerLook& look)
{
	return (right ? RIGHT_STICK : LEFT_STICK) + (right ? look.right_stick : look.left_stick) * CAP_TRAVEL;
}

// The shape a part lights up; empty for a name that is not a part.
QPainterPath PartShape(const QString& name, const ControllerLook& look)
{
	if (name == QLatin1String("l2"))
	{
		return Trigger();
	}
	if (name == QLatin1String("r2"))
	{
		return Mirrored(Trigger());
	}
	if (name == QLatin1String("l1"))
	{
		return Bumper();
	}
	if (name == QLatin1String("r1"))
	{
		return Mirrored(Bumper());
	}
	if (name == QLatin1String("touchpad"))
	{
		return Touchpad();
	}
	if (name == QLatin1String("options"))
	{
		return Capsule(QPointF(0.695, 0.145), 0.014, 0.036);
	}
	if (IsFace(name))
	{
		return Circle(FaceCentre(name), FACE_RADIUS);
	}
	if (IsArm(name))
	{
		return Arm(ArmDirection(name));
	}
	// A stick's press and its four directions all light the stick's cap.
	if (name == QLatin1String("l3") || name.startsWith(QLatin1String("left-stick")))
	{
		return Circle(CapCentre(false, look), CAP_RADIUS);
	}
	if (name == QLatin1String("r3") || name.startsWith(QLatin1String("right-stick")))
	{
		return Circle(CapCentre(true, look), CAP_RADIUS);
	}
	return {};
}

void PaintSymbol(QPainter* p, const QString& name, const QPen& pen)
{
	const QPointF c = FaceCentre(name);
	const qreal   s = 0.012;
	p->setPen(pen);
	p->setBrush(Qt::NoBrush);
	if (name == QLatin1String("triangle"))
	{
		const QPointF points[] = {c + QPointF(0, -s), c + QPointF(s * 1.05, s * 0.75), c + QPointF(-s * 1.05, s * 0.75)};
		p->drawPolygon(points, 3);
	} else if (name == QLatin1String("circle"))
	{
		p->drawEllipse(c, s, s);
	} else if (name == QLatin1String("cross"))
	{
		p->drawLine(c + QPointF(-s, -s), c + QPointF(s, s));
		p->drawLine(c + QPointF(-s, s), c + QPointF(s, -s));
	} else
	{
		p->drawRect(QRectF(c.x() - s * 0.9, c.y() - s * 0.9, s * 1.8, s * 1.8));
	}
}

QColor Faded(QColor color, qreal alpha)
{
	color.setAlphaF(static_cast<float>(color.alphaF() * alpha));
	return color;
}

QColor Shade(qreal alpha)
{
	return QColor::fromRgbF(0, 0, 0, static_cast<float>(alpha));
}

QColor Shine(qreal alpha)
{
	return QColor::fromRgbF(1, 1, 1, static_cast<float>(alpha));
}

// Boolean operations leave hairline slivers along shared edges; this keeps only the pieces of real size.
QPainterPath WithoutSlivers(const QPainterPath& path)
{
	constexpr qreal SLIVER = 0.004;
	QPainterPath    kept;
	for (const auto& polygon: path.toFillPolygons())
	{
		const auto bounds = polygon.boundingRect();
		if (std::min(bounds.width(), bounds.height()) > SLIVER)
		{
			kept.addPolygon(polygon);
		}
	}
	return kept;
}

// The part of a shape left uncovered by the parts in front of it: the shoulder buttons lie under the body, L2 also
// under L1.
QPainterPath Visible(const QString& name, const QPainterPath& shape)
{
	if (name == QLatin1String("l2") || name == QLatin1String("r2"))
	{
		const auto bumper = name.startsWith(QLatin1Char('l')) ? Bumper() : Mirrored(Bumper());
		return WithoutSlivers(shape.subtracted(bumper).subtracted(ControllerBody()));
	}
	if (name == QLatin1String("l1") || name == QLatin1String("r1"))
	{
		return WithoutSlivers(shape.subtracted(ControllerBody()));
	}
	return shape;
}

// A round top lit from the upper left.
QRadialGradient Dome(QPointF centre, qreal radius, const QColor& colour)
{
	QRadialGradient dome(centre, radius * 1.3, centre - QPointF(radius, radius) * 0.45);
	dome.setColorAt(0, colour.lighter(135));
	dome.setColorAt(0.6, colour);
	dome.setColorAt(1, colour.darker(125));
	return dome;
}

// Three box blurs in a row approach a gaussian with a sigma of about the radius.
void BoxBlur(QImage* mask, int radius)
{
	const int        width  = mask->width();
	const int        height = mask->height();
	std::vector<int> line(static_cast<size_t>(std::max(width, height)));
	auto             blur_run = [&](uchar* first, int count, int step)
	{
		for (int i = 0; i < count; i++)
		{
			line[static_cast<size_t>(i)] = first[i * step];
		}
		int sum = 0;
		for (int i = 0; i < std::min(radius, count); i++)
		{
			sum += line[static_cast<size_t>(i)];
		}
		for (int i = 0; i < count; i++)
		{
			if (i + radius < count)
			{
				sum += line[static_cast<size_t>(i + radius)];
			}
			if (i - radius - 1 >= 0)
			{
				sum -= line[static_cast<size_t>(i - radius - 1)];
			}
			first[i * step] = static_cast<uchar>(sum / (2 * radius + 1));
		}
	};
	for (int pass = 0; pass < 3; pass++)
	{
		for (int y = 0; y < height; y++)
		{
			blur_run(mask->scanLine(y), width, 1);
		}
		for (int x = 0; x < width; x++)
		{
			blur_run(mask->scanLine(0) + x, height, static_cast<int>(mask->bytesPerLine()));
		}
	}
}

// Fills a shape with a colour whose edge is blurred by about radius, for soft shadows, light and glow. It respects
// the painter's clip.
void PaintSoft(QPainter* p, const QPainterPath& shape, const QColor& colour, qreal radius)
{
	const QTransform to_device = p->deviceTransform();
	const int        blur      = std::max(1, static_cast<int>(std::lround(radius * std::sqrt(std::abs(to_device.determinant())))));
	const qreal      ratio     = p->device()->devicePixelRatio();
	const QRect      device(0, 0, static_cast<int>(std::ceil(p->device()->width() * ratio)), static_cast<int>(std::ceil(p->device()->height() * ratio)));
	const QRect area = to_device.mapRect(shape.boundingRect()).toAlignedRect().adjusted(-3 * blur, -3 * blur, 3 * blur, 3 * blur) & device;
	if (area.isEmpty())
	{
		return;
	}
	QImage mask(area.size(), QImage::Format_Alpha8);
	mask.fill(0);
	{
		QPainter m(&mask);
		m.setRenderHint(QPainter::Antialiasing);
		m.setTransform(to_device * QTransform::fromTranslate(-area.x(), -area.y()));
		m.setPen(Qt::NoPen);
		m.setBrush(Qt::black);
		m.drawPath(shape);
	}
	BoxBlur(&mask, blur);
	QImage tinted(area.size(), QImage::Format_ARGB32_Premultiplied);
	tinted.fill(colour);
	{
		QPainter t(&tinted);
		t.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		t.drawImage(0, 0, mask);
	}
	tinted.setDevicePixelRatio(ratio);
	p->save();
	p->resetTransform();
	p->drawImage(QPointF(area.topLeft()) / ratio, tinted);
	p->restore();
}

// Everything around a shape, out to a margin.
QPainterPath Around(const QPainterPath& shape, qreal margin)
{
	QPainterPath frame;
	frame.addRect(shape.boundingRect().adjusted(-margin, -margin, margin, margin));
	return frame.subtracted(shape);
}

// A part standing out of the surface under a light from above: its upper rim catches the light and its lower rim
// turns away into shade, both fading softly towards the middle.
void PaintForm(QPainter* p, const QPainterPath& shape, qreal size, qreal light, qreal shade)
{
	const auto around = Around(shape, size * 4);
	p->save();
	p->setClipPath(shape, Qt::IntersectClip);
	PaintSoft(p, around.translated(0, size), Shine(light), size);
	PaintSoft(p, around.translated(0, -size), Shade(shade), size);
	p->restore();
}

// A hollow in the surface: the rim shades its upper wall and the light falls on its lower one.
void PaintHollow(QPainter* p, const QPainterPath& shape, qreal size, qreal shade, qreal light)
{
	const auto around = Around(shape, size * 4);
	p->save();
	p->setClipPath(shape, Qt::IntersectClip);
	PaintSoft(p, around.translated(0, size * 1.4), Shade(shade), size);
	PaintSoft(p, around.translated(0, -size * 0.6), Shine(light), size * 0.6);
	p->restore();
}

// The dark where one part goes under another: the part in front casts a soft shade on the one behind.
void PaintTuck(QPainter* p, const QPainterPath& under, const QPainterPath& over, qreal size, qreal shade)
{
	p->save();
	p->setClipPath(under, Qt::IntersectClip);
	PaintSoft(p, over.translated(0, size * 0.3), Shade(shade), size);
	p->restore();
}

// A thin ring following a shape's outline, inset by the given distance.
QPainterPath Inset(const QPainterPath& shape, qreal inset, qreal width)
{
	return AlongCurve(shape, 2 * (inset + width)).subtracted(AlongCurve(shape, 2 * inset)).intersected(shape);
}

} // namespace

QPainterPath ControllerBody()
{
	// The right half, from the top centre round the grip to the bottom centre; the left half is its mirror.
	QPainterPath half;
	half.moveTo(0.5, 0.11);
	half.lineTo(0.655, 0.11);
	half.cubicTo(0.7, 0.11, 0.73, 0.075, 0.79, 0.075);
	half.cubicTo(0.88, 0.075, 0.935, 0.12, 0.955, 0.21);
	half.cubicTo(0.975, 0.3, 0.995, 0.43, 0.995, 0.52);
	half.cubicTo(0.995, 0.6, 0.955, 0.645, 0.9, 0.645);
	half.cubicTo(0.84, 0.645, 0.805, 0.6, 0.775, 0.53);
	half.cubicTo(0.75, 0.475, 0.715, 0.445, 0.655, 0.445);
	half.lineTo(0.5, 0.445);
	QPainterPath body = half;
	body.connectPath(Mirrored(half).toReversed());
	body.closeSubpath();
	return body;
}

QPointF ControllerAnchor(const QString& part)
{
	if (part == QLatin1String("l2") || part == QLatin1String("r2"))
	{
		return {part.startsWith(QLatin1Char('l')) ? 0.214 : 0.786, 0.03};
	}
	if (part == QLatin1String("l1") || part == QLatin1String("r1"))
	{
		return {part.startsWith(QLatin1Char('l')) ? 0.205 : 0.795, 0.058};
	}
	// The touchpad's upper left, so a line from the left passes over the D-pad.
	if (part == QLatin1String("touchpad"))
	{
		return {0.4, 0.155};
	}
	if (part == QLatin1String("options"))
	{
		return {0.695, 0.145};
	}
	if (IsFace(part))
	{
		return FaceCentre(part);
	}
	if (IsArm(part))
	{
		return DPAD + ArmDirection(part) * (ARM_GAP + ARM_LENGTH * 0.6);
	}
	const bool right = part.startsWith(QLatin1String("right-stick")) || part == QLatin1String("r3");
	if (part == QLatin1String("l3") || part == QLatin1String("r3") || part.contains(QLatin1String("-stick")))
	{
		QPointF offset;
		if (part.endsWith(QLatin1String("-up")))
		{
			offset = {0, -CAP_RADIUS};
		} else if (part.endsWith(QLatin1String("-down")))
		{
			offset = {0, CAP_RADIUS};
		} else if (part.endsWith(QLatin1String("-left")))
		{
			offset = {-CAP_RADIUS, 0};
		} else if (part.endsWith(QLatin1String("-right")))
		{
			offset = {CAP_RADIUS, 0};
		}
		return (right ? RIGHT_STICK : LEFT_STICK) + offset;
	}
	return {0.5, 0.3};
}

void PaintController(QPainter* p, const QRectF& rect, const ControllerLook& look)
{
	p->save();
	p->translate(rect.topLeft());
	p->scale(rect.width(), rect.width());
	const qreal stroke = look.stroke / std::max<qreal>(1, rect.width());
	// Edges come from tone; a fine dark line only keeps neighbouring parts apart.
	QPen edge(Shade(0.5), stroke * 0.7);
	edge.setJoinStyle(Qt::RoundJoin);
	const QColor plastic = look.body;
	const QColor deep    = plastic.darker(170);
	const QColor key     = plastic.darker(118);
	const auto   body    = ControllerBody();
	const bool   both[]  = {false, true};

	auto on       = [&](const QString& name) { return look.lit.contains(name) || look.marked.contains(name); };
	auto lit_fill = [&](const QString& name, const QColor& rest) { return on(name) ? look.accent : rest; };
	auto side     = [](bool right, const QPainterPath& path) { return right ? Mirrored(path) : path; };
	auto fill     = [&](const QPainterPath& shape, const QBrush& brush)
	{
		p->setPen(Qt::NoPen);
		p->setBrush(brush);
		p->drawPath(shape);
	};
	auto outline = [&](const QPainterPath& shape)
	{
		p->setPen(edge);
		p->setBrush(Qt::NoBrush);
		p->drawPath(shape);
	};
	// A key standing out of the shell: its shadow on what lies under it, a face lighter at the top, rounded rims.
	auto paint_key = [&](const QPainterPath& shape, const QBrush& brush, qreal size)
	{
		PaintSoft(p, shape.translated(size * 0.3, size), Shade(0.55), size);
		fill(shape, brush);
		PaintForm(p, shape, size, 0.2, 0.35);
		outline(shape);
	};
	auto top_lit = [](const QPainterPath& shape, const QColor& colour)
	{
		const auto      bounds = shape.boundingRect();
		QLinearGradient face(0, bounds.top(), 0, bounds.bottom());
		face.setColorAt(0, colour.lighter(145));
		face.setColorAt(0.5, colour);
		face.setColorAt(1, colour.darker(125));
		return face;
	};

	// The pad's shadow on the panel under it.
	QPainterPath silhouette = body;
	for (const bool right: both)
	{
		silhouette = silhouette.united(side(right, Trigger()));
	}
	PaintSoft(p, silhouette.translated(0.004, 0.016), Shade(0.5), 0.016);

	// L2 stands behind L1 and both go under the shell; each one in front darkens the one behind where it covers it.
	for (const bool right: both)
	{
		const auto trigger = side(right, Trigger());
		const auto bumper  = side(right, Bumper());
		const auto l2      = right ? QStringLiteral("r2") : QStringLiteral("l2");
		const auto l1      = right ? QStringLiteral("r1") : QStringLiteral("l1");
		paint_key(trigger, top_lit(trigger, lit_fill(l2, plastic.darker(108))), 0.006);
		PaintTuck(p, trigger, bumper, 0.008, 0.6);
		paint_key(bumper, top_lit(bumper, lit_fill(l1, plastic)), 0.005);
		PaintTuck(p, bumper, body, 0.008, 0.6);
	}

	// The shell: lighter on top, rounding over into shade all round its edge, its upper rims catching the light.
	QLinearGradient shell(0, 0.075, 0, 0.645);
	shell.setColorAt(0, plastic.lighter(118));
	shell.setColorAt(0.45, plastic);
	shell.setColorAt(1, plastic.darker(115));
	fill(body, shell);
	p->save();
	p->setClipPath(body, Qt::IntersectClip);
	const auto around = Around(body, 0.1);
	PaintSoft(p, around, Shade(0.55), 0.03);
	PaintSoft(p, around.translated(0, 0.012), Shine(0.12), 0.008);
	// Each grip is a rounded handle, with a long soft highlight down its outer flank.
	for (const bool right: both)
	{
		QPainterPath flank;
		flank.moveTo(0.105, 0.2);
		flank.cubicTo(0.085, 0.28, 0.07, 0.38, 0.072, 0.5);
		PaintSoft(p, side(right, AlongCurve(flank, 0.04)), Shine(0.06), 0.02);
	}
	// The parting line between the upper and lower shells, seen round the grips and fading out towards the top.
	QLinearGradient seam(0, 0.22, 0, 0.42);
	seam.setColorAt(0, Shade(0));
	seam.setColorAt(1, Shade(0.4));
	fill(Inset(body, 0.016, stroke * 0.6), seam);
	QLinearGradient seam_lip(0, 0.22, 0, 0.42);
	seam_lip.setColorAt(0, Shine(0));
	seam_lip.setColorAt(1, Shine(0.07));
	fill(Inset(body, 0.016 + stroke * 0.6, stroke * 0.6), seam_lip);
	p->restore();
	p->setPen(QPen(Shade(0.6), stroke * 0.6));
	p->setBrush(Qt::NoBrush);
	p->drawPath(body);
	// A faint rim light keeps the dark shell apart from a dark panel.
	QLinearGradient rim(0, 0.07, 0, 0.32);
	rim.setColorAt(0, Faded(look.line, 0.5));
	rim.setColorAt(1, Faded(look.line, 0));
	p->setPen(QPen(QBrush(rim), stroke * 0.8));
	p->drawPath(body);

	// The touchpad, a broad matte plate lying in a narrow gap, with a soft sheen across it.
	const auto touchpad = Touchpad();
	PaintSoft(p, touchpad.translated(0, 0.004), Shade(0.5), 0.006);
	QLinearGradient pad(0, 0.125, 0, 0.29);
	pad.setColorAt(0, lit_fill(QStringLiteral("touchpad"), plastic).lighter(116));
	pad.setColorAt(1, lit_fill(QStringLiteral("touchpad"), plastic).darker(104));
	fill(touchpad, pad);
	p->save();
	p->setClipPath(touchpad, Qt::IntersectClip);
	QPainterPath sheen;
	sheen.addEllipse(QPointF(0.45, 0.165), 0.11, 0.035);
	PaintSoft(p, sheen, Shine(0.05), 0.025);
	p->restore();
	PaintForm(p, touchpad, 0.006, 0.14, 0.3);
	outline(touchpad);

	// The light bar shines out of a slot down each side of the touchpad.
	for (const bool right: both)
	{
		const auto slot = side(right, RoundedRect(QRectF(0.326, 0.14, 0.012, 0.135), 0.006));
		fill(slot, deep);
		PaintHollow(p, slot, 0.004, 0.6, 0.1);
		if (look.light_bar > 0)
		{
			const auto strip = side(right, RoundedRect(QRectF(0.329, 0.145, 0.006, 0.125), 0.003));
			PaintSoft(p, strip, Faded(look.accent, 0.55 * look.light_bar), 0.012);
			fill(strip, Faded(look.accent, look.light_bar));
			PaintSoft(p, strip, Shine(0.35 * look.light_bar), 0.002);
		}
	}

	// Create and Options, two small pills at the touchpad's upper corners.
	for (const bool right: both)
	{
		const auto pill = right ? PartShape(QStringLiteral("options"), look) : Capsule(QPointF(0.305, 0.145), 0.014, 0.036);
		fill(AlongCurve(pill, 0.006), deep);
		paint_key(pill, top_lit(pill, right ? lit_fill(QStringLiteral("options"), key) : key), 0.003);
	}

	// The D-pad: four separate keys in a round hollow, each with an arrow pressed into its face.
	const auto dpad_well = Circle(DPAD, ARM_GAP + ARM_LENGTH + 0.01);
	fill(dpad_well, deep.lighter(120));
	PaintHollow(p, dpad_well, 0.01, 0.6, 0.1);
	for (const char* arm: {"up", "down", "left", "right"})
	{
		const QString   name      = QString::fromLatin1(arm);
		const QPointF   direction = ArmDirection(name);
		const auto      shape     = Arm(direction);
		QLinearGradient face(DPAD - QPointF(0.06, 0.06), DPAD + QPointF(0.06, 0.06));
		face.setColorAt(0, lit_fill(name, key).lighter(140));
		face.setColorAt(1, lit_fill(name, key).darker(110));
		paint_key(shape, face, 0.004);
		const QPointF tip    = DPAD + direction * (ARM_GAP + ARM_LENGTH * 0.72);
		const QPointF across = QPointF(-direction.y(), direction.x()) * 0.008;
		QPainterPath  arrow;
		arrow.moveTo(tip);
		arrow.lineTo(tip - direction * 0.011 + across);
		arrow.lineTo(tip - direction * 0.011 - across);
		arrow.closeSubpath();
		fill(arrow.translated(0, stroke * 0.6), Shine(0.12));
		fill(arrow, on(name) ? plastic : Shade(0.45));
	}

	// The face buttons: domes in their own hollows, with the symbols inked in colour.
	for (const char* face: {"triangle", "circle", "cross", "square"})
	{
		const QString name   = QString::fromLatin1(face);
		const QPointF centre = FaceCentre(name);
		const auto    well   = Circle(centre, FACE_RADIUS + 0.007);
		const auto    button = Circle(centre, FACE_RADIUS);
		fill(well, deep);
		PaintHollow(p, well, 0.006, 0.6, 0.1);
		p->save();
		p->setClipPath(well, Qt::IntersectClip);
		PaintSoft(p, button.translated(0.002, 0.006), Shade(0.6), 0.005);
		p->restore();
		fill(button, Dome(centre, FACE_RADIUS, lit_fill(name, key)));
		PaintForm(p, button, 0.005, 0.16, 0.3);
		PaintSoft(p, Circle(centre - QPointF(0.35, 0.42) * FACE_RADIUS, FACE_RADIUS * 0.22), Shine(0.22), 0.004);
		outline(button);
		static const QHash<QString, QColor> inks = {{QStringLiteral("triangle"), QColor(0x4f, 0xd1, 0xb0)},
		                                            {QStringLiteral("circle"), QColor(0xff, 0x6b, 0x7a)},
		                                            {QStringLiteral("cross"), QColor(0x76, 0xa8, 0xff)},
		                                            {QStringLiteral("square"), QColor(0xf0, 0x8c, 0xd2)}};
		QPen symbol(on(name) ? plastic : inks.value(name), stroke * 1.1);
		symbol.setJoinStyle(Qt::RoundJoin);
		symbol.setCapStyle(Qt::RoundCap);
		PaintSymbol(p, name, symbol);
	}

	// The sticks: a rubber dome fills each hollow and the cap sits on it, a lit convex rim round a dished top.
	for (const bool right: both)
	{
		const QPointF well_centre = right ? RIGHT_STICK : LEFT_STICK;
		const QString name        = right ? QStringLiteral("r3") : QStringLiteral("l3");
		const QString stick       = right ? QStringLiteral("right-stick") : QStringLiteral("left-stick");
		bool          held        = on(name);
		for (const auto& part: look.lit + look.marked)
		{
			held = held || part.startsWith(stick);
		}
		const auto well = Circle(well_centre, WELL_RADIUS);
		fill(well, deep);
		PaintHollow(p, well, 0.012, 0.65, 0.12);
		fill(Circle(well_centre, WELL_RADIUS * 0.86), Dome(well_centre, WELL_RADIUS * 0.86, deep.lighter(150)));
		const QPointF cap_centre = CapCentre(right, look);
		const auto    cap        = Circle(cap_centre, CAP_RADIUS);
		p->save();
		p->setClipPath(well, Qt::IntersectClip);
		PaintSoft(p, cap.translated(0.003, 0.009), Shade(0.6), 0.008);
		p->restore();
		const QColor rim_colour = held ? look.accent : plastic.lighter(112);
		fill(cap, Dome(cap_centre, CAP_RADIUS, rim_colour));
		PaintForm(p, cap, 0.006, 0.2, 0.35);
		outline(cap);
		const auto      dish = Circle(cap_centre, CAP_RADIUS * 0.72);
		QLinearGradient hollow(cap_centre - QPointF(1, 1) * CAP_RADIUS * 0.7, cap_centre + QPointF(1, 1) * CAP_RADIUS * 0.7);
		hollow.setColorAt(0, rim_colour.darker(130));
		hollow.setColorAt(1, rim_colour.lighter(115));
		fill(dish, hollow);
		PaintHollow(p, dish, 0.005, 0.35, 0.12);
		p->setPen(QPen(Shade(0.45), stroke * 0.6));
		p->setBrush(Qt::NoBrush);
		p->drawPath(dish);
	}

	// The PS button between the sticks and the mute button under it, each in its own hollow.
	const QPointF home(0.5, 0.372);
	const auto    home_well = Circle(home, 0.021);
	fill(home_well, deep);
	PaintHollow(p, home_well, 0.004, 0.6, 0.1);
	fill(Circle(home, 0.016), Dome(home, 0.016, key.lighter(115)));
	PaintForm(p, Circle(home, 0.016), 0.004, 0.18, 0.3);
	outline(Circle(home, 0.016));
	const auto mute = Capsule(QPointF(0.5, 0.414), 0.032, 0.011);
	fill(AlongCurve(mute, 0.006), deep);
	paint_key(mute, top_lit(mute, key), 0.003);

	// A marked part gets a soft ring of the accent around what can be seen of it.
	for (const auto& name: look.marked)
	{
		const auto shape = Visible(name, PartShape(name, look));
		if (shape.isEmpty())
		{
			continue;
		}
		p->setBrush(Qt::NoBrush);
		p->setPen(QPen(Faded(look.accent, 0.35), stroke * 5));
		p->drawPath(shape);
		p->setPen(QPen(look.accent, stroke * 1.4));
		p->drawPath(shape);
	}
	p->restore();
}
