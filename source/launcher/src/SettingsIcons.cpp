#include "SettingsIcons.h"

#include "ControllerDrawing.h"

#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>

// Every icon is drawn in a 1 × 1 box; lines are as thick as the launcher's other icons.
constexpr qreal STROKE = 0.075;

namespace {

qreal Phase(qreal seconds, qreal period)
{
	return std::fmod(seconds, period) / period;
}

qreal Smooth(qreal x)
{
	x = std::clamp<qreal>(x, 0, 1);
	return x * x * (3 - 2 * x);
}

QColor WithAlpha(QColor color, qreal alpha)
{
	color.setAlphaF(static_cast<float>(color.alphaF() * std::clamp<qreal>(alpha, 0, 1)));
	return color;
}

QPen Line(const QColor& color, qreal width = STROKE)
{
	QPen pen(color, width);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	return pen;
}

// A four-pointed glint, size from its centre to a point.
void PaintSparkle(QPainter* p, QPointF centre, qreal size, const QColor& color)
{
	if (size <= 0)
	{
		return;
	}
	const qreal  waist = size * 0.28;
	QPainterPath star;
	star.moveTo(centre.x(), centre.y() - size);
	star.quadTo(centre.x() + waist, centre.y() - waist, centre.x() + size, centre.y());
	star.quadTo(centre.x() + waist, centre.y() + waist, centre.x(), centre.y() + size);
	star.quadTo(centre.x() - waist, centre.y() + waist, centre.x() - size, centre.y());
	star.quadTo(centre.x() - waist, centre.y() - waist, centre.x(), centre.y() - size);
	p->fillPath(star, color);
}

// A monitor showing a small landscape: a scan line sweeps the screen, the sun breathes and a glint twinkles.
void PaintGraphics(QPainter* p, qreal t, const QColor& ink, const QColor& accent)
{
	const QRectF screen(0.1, 0.16, 0.8, 0.54);
	p->setPen(Line(ink));
	p->setBrush(Qt::NoBrush);
	p->drawRoundedRect(screen, 0.08, 0.08);
	p->drawLine(QPointF(0.5, 0.7), QPointF(0.5, 0.83));
	p->drawLine(QPointF(0.33, 0.85), QPointF(0.67, 0.85));

	p->save();
	p->setClipRect(screen.adjusted(STROKE, STROKE, -STROKE, -STROKE));
	const qreal sun = 0.055 + 0.012 * std::sin(t * 2.6);
	p->setPen(Qt::NoPen);
	p->setBrush(accent);
	p->drawEllipse(QPointF(0.69, 0.33), sun, sun);
	p->setPen(Line(WithAlpha(ink, 0.8), STROKE * 0.8));
	p->setBrush(Qt::NoBrush);
	const QPointF hills[] = {{0.16, 0.63}, {0.36, 0.43}, {0.49, 0.55}, {0.61, 0.46}, {0.84, 0.66}};
	p->drawPolyline(hills, static_cast<int>(std::size(hills)));

	// The scan line fades in at the top and out at the bottom, so the loop has no visible seam.
	const qreal sweep = Phase(t, 2.4);
	const qreal y     = screen.top() + sweep * screen.height();
	const qreal fade  = std::sin(sweep * M_PI);
	if (t > 0 && fade > 0.01)
	{
		QLinearGradient band(0, y - 0.12, 0, y);
		band.setColorAt(0, WithAlpha(accent, 0));
		band.setColorAt(1, WithAlpha(accent, 0.45 * fade));
		p->fillRect(QRectF(screen.left(), y - 0.12, screen.width(), 0.12), band);
		p->setPen(Line(WithAlpha(accent, 0.9 * fade), STROKE * 0.5));
		p->drawLine(QPointF(screen.left(), y), QPointF(screen.right(), y));
	}
	p->restore();

	const qreal twinkle = std::pow(std::max<qreal>(0, std::sin(t * 1.9 + 0.4)), 6);
	PaintSparkle(p, QPointF(0.27, 0.31), 0.03 + 0.07 * twinkle, WithAlpha(accent, 0.35 + 0.65 * twinkle));
}

struct Vec3
{
	qreal x;
	qreal y;
	qreal z;
};

Vec3 Rotated(Vec3 v, qreal yaw, qreal pitch)
{
	const Vec3 turned {v.x * std::cos(yaw) + v.z * std::sin(yaw), v.y, -v.x * std::sin(yaw) + v.z * std::cos(yaw)};
	return {turned.x, turned.y * std::cos(pitch) - turned.z * std::sin(pitch), turned.y * std::sin(pitch) + turned.z * std::cos(pitch)};
}

Vec3 Cross(Vec3 a, Vec3 b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 Minus(Vec3 a, Vec3 b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Normalized(Vec3 v)
{
	const qreal length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	return length > 0 ? Vec3 {v.x / length, v.y / length, v.z / length} : v;
}

// A turning tetrahedron lit from the upper left: each face is shaded by how much it faces the light, which is
// what a shader does.
void PaintShaders(QPainter* p, qreal t, const QColor& ink, const QColor& accent)
{
	const std::array<Vec3, 4> corners = {Vec3 {1, 1, 1}, Vec3 {1, -1, -1}, Vec3 {-1, 1, -1}, Vec3 {-1, -1, 1}};
	// Each face lists its corners counter-clockwise seen from outside.
	const int   faces[4][3] = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
	const Vec3  light       = Normalized({-0.5, 0.7, 0.6});
	const qreal yaw         = 0.6 + t * 2 * M_PI / 7;
	const qreal pitch       = 0.35 + 0.12 * std::sin(t * 0.9);

	std::array<Vec3, 4> turned {};
	for (size_t i = 0; i < corners.size(); i++)
	{
		turned[i] = Rotated(corners.at(i), yaw, pitch);
	}
	auto to_screen = [](Vec3 v) { return QPointF(0.5 + v.x * 0.24, 0.53 - v.y * 0.24); };

	struct Face
	{
		int   index;
		qreal depth;
		Vec3  normal;
	};
	std::array<Face, 4> order {};
	for (int i = 0; i < 4; i++)
	{
		const Vec3& a = turned.at(faces[i][0]);
		const Vec3& b = turned.at(faces[i][1]);
		const Vec3& c = turned.at(faces[i][2]);
		order.at(i)   = {i, (a.z + b.z + c.z) / 3, Normalized(Cross(Minus(b, a), Minus(c, a)))};
	}
	// Painter's order: the faces furthest from the viewer first.
	std::sort(order.begin(), order.end(), [](const Face& a, const Face& b) { return a.depth < b.depth; });

	for (const auto& face: order)
	{
		QPolygonF polygon;
		for (int corner: faces[face.index])
		{
			polygon << to_screen(turned.at(corner));
		}
		const bool front = face.normal.z > 0;
		if (!front)
		{
			p->setPen(Line(WithAlpha(ink, 0.3), STROKE * 0.6));
			p->setBrush(Qt::NoBrush);
			p->drawPolygon(polygon);
			continue;
		}
		const qreal lit = std::max<qreal>(0, face.normal.x * light.x + face.normal.y * light.y + face.normal.z * light.z);
		p->setPen(Line(ink));
		p->setBrush(WithAlpha(accent, 0.18 + 0.7 * lit));
		p->drawPolygon(polygon);
	}
}

// A page with a folded corner whose lines type themselves in behind a blinking cursor, then clear.
void PaintLogs(QPainter* p, qreal t, const QColor& ink, const QColor& accent)
{
	QPainterPath page;
	page.moveTo(0.22, 0.1);
	page.lineTo(0.6, 0.1);
	page.lineTo(0.8, 0.3);
	page.lineTo(0.8, 0.9);
	page.lineTo(0.22, 0.9);
	page.closeSubpath();
	p->setPen(Line(ink));
	p->setBrush(Qt::NoBrush);
	p->drawPath(page);
	const QPointF fold[] = {{0.6, 0.1}, {0.6, 0.3}, {0.8, 0.3}};
	p->drawPolyline(fold, 3);

	// The loop starts with the page full; at rest it stays that way.
	constexpr qreal PERIOD   = 4.0;
	constexpr qreal TYPING   = 2.4;
	constexpr qreal CLEARING = 0.4;
	const qreal     time     = std::fmod(t + TYPING + 0.6, PERIOD);
	const qreal     widths[] = {0.22, 0.38, 0.3, 0.34};
	const qreal     top      = 0.24;
	qreal           total    = 0;
	for (qreal width: widths)
	{
		total += width;
	}
	const qreal typed   = time < TYPING ? total * time / TYPING : total;
	const qreal opacity = time > PERIOD - CLEARING ? (PERIOD - time) / CLEARING : 1;

	qreal   left_to_type = typed;
	QPointF cursor(0.32, top);
	for (int i = 0; i < 4; i++)
	{
		// The first line is short so it clears the fold.
		const qreal y     = top + 0.15 * i + (i > 0 ? 0.06 : 0);
		const qreal x     = i == 0 ? 0.3 : 0.31;
		const qreal shown = std::clamp<qreal>(left_to_type, 0, widths[i]);
		left_to_type -= widths[i];
		if (shown <= 0)
		{
			break;
		}
		p->setPen(Line(WithAlpha(i == 1 ? accent : ink, (i == 1 ? 1.0 : 0.75) * opacity), STROKE * 0.85));
		p->drawLine(QPointF(x, y), QPointF(x + shown, y));
		cursor = QPointF(x + shown + 0.05, y);
	}
	const bool blink_on = std::fmod(t, 0.8) < 0.45;
	if (t > 0 && blink_on && opacity >= 1)
	{
		p->fillRect(QRectF(cursor.x() - 0.02, cursor.y() - 0.05, 0.04, 0.1), accent);
	}
}

// A chip with pins on every side; pulses run in along the pins and the die glows as they arrive.
void PaintEmulator(QPainter* p, qreal t, const QColor& ink, const QColor& accent)
{
	const QRectF body(0.27, 0.27, 0.46, 0.46);
	const qreal  pins[] = {0.38, 0.5, 0.62};
	p->setPen(Line(ink));
	for (qreal at: pins)
	{
		p->drawLine(QPointF(at, 0.1), QPointF(at, body.top()));
		p->drawLine(QPointF(at, body.bottom()), QPointF(at, 0.9));
		p->drawLine(QPointF(0.1, at), QPointF(body.left(), at));
		p->drawLine(QPointF(body.right(), at), QPointF(0.9, at));
	}
	p->setBrush(Qt::NoBrush);
	p->drawRoundedRect(body, 0.07, 0.07);

	const qreal  glow = 0.5 + 0.5 * std::sin(t * 2 * M_PI / 1.6);
	const QRectF die(0.4, 0.4, 0.2, 0.2);
	p->setPen(Line(accent, STROKE * 0.8));
	p->setBrush(WithAlpha(accent, 0.25 + (t > 0 ? 0.45 * glow : 0)));
	p->drawRoundedRect(die, 0.03, 0.03);

	if (t <= 0)
	{
		return;
	}
	p->setPen(Qt::NoPen);
	p->setBrush(accent);
	const qreal length = body.left() - 0.1;
	for (int side = 0; side < 4; side++)
	{
		for (int i = 0; i < 3; i++)
		{
			const qreal   travel   = Phase(t + (side * 3 + i) * 0.37, 1.6);
			const qreal   alpha    = std::sin(travel * M_PI);
			const qreal   along    = 0.1 + travel * length;
			const qreal   at       = pins[i];
			const QPointF points[] = {{at, along}, {at, 1 - along}, {along, at}, {1 - along, at}};
			p->setBrush(WithAlpha(accent, alpha));
			p->drawEllipse(points[side], 0.035, 0.035);
		}
	}
}

// The guest pad: it rocks gently and now and then buzzes, the left stick circles, the right one flicks, the face
// buttons light one after another and the light bar breathes.
void PaintControls(QPainter* p, qreal t, const QColor& ink, const QColor& accent)
{
	constexpr qreal WIDTH = 0.9;
	const bool      live  = t > 0;
	const qreal     rock  = live ? 3.5 * std::sin(t * 2 * M_PI / 3.2) : 0;
	const qreal     buzz  = live && Phase(t, 4.8) > 0.9 ? 0.012 * std::sin(t * 2 * M_PI * 14) : 0;
	p->save();
	p->translate(0.5 + buzz, 0.5);
	p->rotate(rock);
	p->scale(WIDTH, WIDTH);
	// The body's outline spans 0.075 to 0.645 of its height; centre that on the icon.
	p->translate(-0.5, -0.36);
	const qreal stroke = STROKE / WIDTH;

	p->setPen(Line(ink, stroke));
	p->setBrush(Qt::NoBrush);
	p->drawPath(ControllerBody());
	p->setPen(Line(WithAlpha(ink, 0.7), stroke * 0.7));
	p->drawRoundedRect(QRectF(0.36, 0.13, 0.28, 0.14), 0.03, 0.03);

	const qreal breath = live ? 0.5 + 0.5 * std::sin(t * 2 * M_PI / 2) : 0.5;
	p->setPen(Line(WithAlpha(accent, 0.35 + 0.65 * breath), stroke * 0.8));
	p->drawLine(QPointF(0.322, 0.15), QPointF(0.322, 0.25));
	p->drawLine(QPointF(0.678, 0.15), QPointF(0.678, 0.25));

	const QPointF cross(0.2, 0.25);
	p->setPen(Line(ink, stroke));
	p->drawLine(cross - QPointF(0.065, 0), cross + QPointF(0.065, 0));
	p->drawLine(cross - QPointF(0, 0.065), cross + QPointF(0, 0.065));

	const QPointF sticks[] = {{0.36, 0.385}, {0.64, 0.385}};
	const qreal   orbit    = t * 2 * M_PI / 2.4;
	const qreal   reach    = live ? 0.032 * Smooth(t / 0.6) : 0;
	const qreal   flick    = live ? 0.032 * std::pow(std::max<qreal>(0, std::sin(t * 2 * M_PI / 1.7)), 3) : 0;
	const QPointF caps[]   = {sticks[0] + QPointF(std::cos(orbit), std::sin(orbit)) * reach, sticks[1] + QPointF(flick, 0)};
	for (int i = 0; i < 2; i++)
	{
		p->setPen(Line(ink, stroke * 0.85));
		p->setBrush(Qt::NoBrush);
		p->drawEllipse(sticks[i], 0.065, 0.065);
		p->setPen(Qt::NoPen);
		p->setBrush(accent);
		p->drawEllipse(caps[i], 0.034, 0.034);
	}

	// Each face button flashes in turn and fades before the next one lights.
	const QPointF face(0.8, 0.25);
	const QPointF buttons[] = {face + QPointF(0, -0.062), face + QPointF(0.062, 0), face + QPointF(0, 0.062), face + QPointF(-0.062, 0)};
	const qreal   step      = t / 0.45;
	const int     lit       = live ? static_cast<int>(std::fmod(step, 4)) : -1;
	const qreal   fade      = 1 - (step - std::floor(step));
	p->setPen(Qt::NoPen);
	for (int i = 0; i < 4; i++)
	{
		const QColor rest = WithAlpha(ink, 0.6);
		p->setBrush(i == lit ? WithAlpha(accent, 0.45 + 0.55 * fade) : rest);
		p->drawEllipse(buttons[i], i == lit ? 0.034 : 0.028, i == lit ? 0.034 : 0.028);
	}
	p->restore();
}

// A sun that turns into a crescent moon and back, its rays drawing in as the stars fade.
void PaintInterface(QPainter* p, qreal t, const QColor& ink, const QColor& accent)
{
	constexpr qreal HOLD  = 1.3;
	constexpr qreal MORPH = 1.1;
	const qreal     time  = std::fmod(t, 2 * (HOLD + MORPH));
	qreal           moon  = 0;
	if (time > HOLD && time < HOLD + MORPH)
	{
		moon = Smooth((time - HOLD) / MORPH);
	} else if (time >= HOLD + MORPH && time < 2 * HOLD + MORPH)
	{
		moon = 1;
	} else if (time >= 2 * HOLD + MORPH)
	{
		moon = 1 - Smooth((time - 2 * HOLD - MORPH) / MORPH);
	}

	const QPointF centre(0.5, 0.5);
	const qreal   radius = 0.19 + 0.07 * moon;
	QPainterPath  disc;
	disc.addEllipse(centre, radius, radius);
	QPainterPath cutter;
	cutter.addEllipse(QPointF(0.62 + 0.45 * (1 - moon), 0.38 - 0.4 * (1 - moon)), radius * 0.92, radius * 0.92);
	const QColor fill = QColor::fromRgbF(static_cast<float>(accent.redF() + (ink.redF() - accent.redF()) * moon),
	                                     static_cast<float>(accent.greenF() + (ink.greenF() - accent.greenF()) * moon),
	                                     static_cast<float>(accent.blueF() + (ink.blueF() - accent.blueF()) * moon), accent.alphaF());
	p->fillPath(disc.subtracted(cutter), fill);

	const qreal rays = 1 - moon;
	if (rays > 0.01)
	{
		p->setPen(Line(WithAlpha(ink, rays)));
		const qreal turn = t * 0.5;
		for (int i = 0; i < 8; i++)
		{
			const qreal   angle = turn + i * M_PI / 4;
			const QPointF direction(std::cos(angle), std::sin(angle));
			const qreal   start = radius + 0.08;
			p->drawLine(centre + direction * start, centre + direction * (start + 0.1 * rays));
		}
	}
	PaintSparkle(p, QPointF(0.2, 0.26), 0.07 * moon, WithAlpha(accent, moon));
	PaintSparkle(p, QPointF(0.27, 0.76), 0.045 * moon, WithAlpha(ink, moon));
}

} // namespace

QImage PaintSettingsIcon(SettingsIcon icon, int px, qreal seconds, const QColor& ink, const QColor& accent)
{
	QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.scale(px, px);
	switch (icon)
	{
		case SettingsIcon::Graphics: PaintGraphics(&painter, seconds, ink, accent); break;
		case SettingsIcon::Shaders: PaintShaders(&painter, seconds, ink, accent); break;
		case SettingsIcon::Logs: PaintLogs(&painter, seconds, ink, accent); break;
		case SettingsIcon::Emulator: PaintEmulator(&painter, seconds, ink, accent); break;
		case SettingsIcon::Controls: PaintControls(&painter, seconds, ink, accent); break;
		case SettingsIcon::Interface: PaintInterface(&painter, seconds, ink, accent); break;
	}
	return image;
}
