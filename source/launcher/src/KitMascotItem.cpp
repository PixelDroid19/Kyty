#include "KitMascotItem.h"

#include "ArtworkSet.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QEasingCurve>
#include <QFontMetricsF>
#include <QImageReader>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSGImageNode>
#include <QSGOpacityNode>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>

// Every layer shares the 120 x 160 viewBox, so stacking them at the same size rebuilds the drawing. The website
// draws Kit 120 px wide, so its offsets below apply in viewBox units.
constexpr qreal VIEW_WIDTH  = 120;
constexpr qreal VIEW_HEIGHT = 160;

constexpr qreal  GLITCH_OPACITY = 0.85;
constexpr qint64 GLITCH_PERIOD  = 4200;
constexpr qint64 FLOAT_PERIOD   = 6000;
constexpr qreal  FLOAT_HEIGHT   = 7;
constexpr qint64 BLINK_PERIOD   = 5400;
constexpr qint64 BLINK_START    = 2430;
constexpr qint64 BLINK_HALF     = 108;
constexpr qreal  BLINK_CLOSED   = 0.08;
constexpr qreal  EYE_LINE       = 70.5;
constexpr qint64 PURR_STEP      = 30;
constexpr qint64 PURR_FADE      = 150;
constexpr qint64 HOP_UP         = 170;
constexpr qint64 HOP_DOWN       = 260;
constexpr qreal  HOP_HEIGHT     = 18;
constexpr qint64 HEART_LIFE     = 1350;
constexpr qint64 HEART_FADE_IN  = 190;
constexpr qreal  HEART_RISE     = 86;
constexpr qreal  HEART_MAX      = 17 * 1.1;
constexpr int    HEART_INTERVAL = 150;
constexpr int    UNPET_DELAY    = 220;
// The edge round the body on the light palette, as a share of Kit's width.
constexpr qreal OUTLINE_SHARE = 0.018;

namespace {

enum Layer
{
	GLITCH_RED,
	GLITCH_CYAN,
	BODY,
	WHISKERS,
	GEAR,
	EYES_OPEN,
	EYES_SHINE,
	EYES_HAPPY,
	LAYER_COUNT
};

const char* const LAYER_FILES[LAYER_COUNT] = {":/assets/kit/glitch-r.svg",   ":/assets/kit/glitch-c.svg",  ":/assets/kit/body.svg",
                                              ":/assets/kit/whiskers.svg",   ":/assets/kit/gear.svg",      ":/assets/kit/eyes-open.svg",
                                              ":/assets/kit/eyes-shine.svg", ":/assets/kit/eyes-happy.svg"};
const char* const HEART_FILE              = ":/assets/kit/heart.svg";

// The website's glitch keyframes: when a colour slice appears, its offset and its clip insets as shares of the
// height, or when it goes away.
struct GlitchEvent
{
	qint64 t;
	Layer  layer;
	bool   shown;
	qreal  dx;
	qreal  dy;
	qreal  top;
	qreal  bottom;
};

const GlitchEvent GLITCH_EVENTS[] = {
    {420, GLITCH_RED, true, -3, 1, 0.08, 0.58},   {441, GLITCH_CYAN, true, 3, -1, 0.20, 0.44},  {462, GLITCH_RED, true, 2, -2, 0.52, 0.18},
    {483, GLITCH_CYAN, true, -2, 2, 0.46, 0.26},  {504, GLITCH_RED, false, 0, 0, 0, 0},         {525, GLITCH_CYAN, false, 0, 0, 0, 0},
    {1764, GLITCH_RED, true, -2, -1, 0.30, 0.40}, {1785, GLITCH_CYAN, true, 2, 1, 0.12, 0.66},  {1827, GLITCH_RED, false, 0, 0, 0, 0},
    {1848, GLITCH_CYAN, false, 0, 0, 0, 0},       {3066, GLITCH_RED, true, 3, 2, 0.64, 0.06},   {3108, GLITCH_CYAN, true, -3, -2, 0.56, 0.14},
    {3129, GLITCH_RED, false, 0, 0, 0, 0},        {3171, GLITCH_CYAN, false, 0, 0, 0, 0},
};

// The purr shake's steps, each reached PURR_STEP ms after the one before, from the last back to the first.
const QPointF PURR_STEPS[] = {{0.7, -0.7}, {-0.7, 0.6}, {0.6, 0.5}, {0, 0}};

// The event that sets the slice of layer at phase ms into the loop; a phase before the layer's first event is
// still under the previous loop's last one.
const GlitchEvent& GlitchAt(Layer layer, qint64 phase)
{
	const GlitchEvent* found = nullptr;
	const GlitchEvent* last  = nullptr;
	for (const auto& event: GLITCH_EVENTS)
	{
		if (event.layer != layer)
		{
			continue;
		}
		last = &event;
		if (event.t <= phase)
		{
			found = &event;
		}
	}
	return found != nullptr ? *found : *last;
}

qreal Ease(QEasingCurve::Type type, qreal progress)
{
	return QEasingCurve(type).valueForProgress(std::clamp<qreal>(progress, 0, 1));
}

qreal FloatShift(qint64 now)
{
	const qreal phase = static_cast<qreal>(now % FLOAT_PERIOD) / FLOAT_PERIOD;
	// Two InOutSine halves, up and back down.
	return -FLOAT_HEIGHT * (1 - std::cos(2 * M_PI * phase)) / 2;
}

qreal BlinkScale(qint64 now)
{
	const qint64 phase = now % BLINK_PERIOD - BLINK_START;
	if (phase < 0 || phase >= 2 * BLINK_HALF)
	{
		return 1;
	}
	if (phase < BLINK_HALF)
	{
		return 1 + (BLINK_CLOSED - 1) * Ease(QEasingCurve::InOutQuad, static_cast<qreal>(phase) / BLINK_HALF);
	}
	return BLINK_CLOSED + (1 - BLINK_CLOSED) * Ease(QEasingCurve::InOutQuad, static_cast<qreal>(phase - BLINK_HALF) / BLINK_HALF);
}

QPointF PurrShift(qint64 since)
{
	const int     count = static_cast<int>(std::size(PURR_STEPS));
	const qint64  step  = (since / PURR_STEP) % count;
	const QPointF from  = PURR_STEPS[(step + count - 1) % count];
	const qreal   t     = static_cast<qreal>(since % PURR_STEP) / PURR_STEP;
	return from + (PURR_STEPS[step] - from) * t;
}

qreal HopShift(qint64 since)
{
	if (since < HOP_UP)
	{
		return -HOP_HEIGHT * Ease(QEasingCurve::OutQuad, static_cast<qreal>(since) / HOP_UP);
	}
	return -HOP_HEIGHT + HOP_HEIGHT * Ease(QEasingCurve::OutBounce, static_cast<qreal>(since - HOP_UP) / HOP_DOWN);
}

QImage PaintLayer(const char* path, QSize px, qreal opacity)
{
	QImageReader reader(QString::fromLatin1(path));
	reader.setScaledSize(px);
	auto image = reader.read();
	if (image.isNull())
	{
		qWarning("Kit layer %s: %s", path, qPrintable(reader.errorString()));
		return image;
	}
	if (opacity >= 1)
	{
		return image;
	}
	QImage faded(image.size(), QImage::Format_ARGB32_Premultiplied);
	faded.fill(Qt::transparent);
	QPainter painter(&faded);
	painter.setOpacity(opacity);
	painter.drawImage(0, 0, image);
	return faded;
}

// The body with a ring of color round its silhouette, width pixels wide, drawn by stamping the silhouette around
// a circle under the body.
QImage Outlined(const QImage& body, const QColor& color, qreal width)
{
	QImage silhouette = body;
	Recolour(&silhouette, color);
	QImage outlined(body.size(), QImage::Format_ARGB32_Premultiplied);
	outlined.fill(Qt::transparent);
	QPainter   painter(&outlined);
	const auto steps = std::max(8, static_cast<int>(std::ceil(width * 8)));
	for (int i = 0; i < steps; i++)
	{
		const qreal angle = 2 * M_PI * i / steps;
		painter.drawImage(QPointF(width * std::cos(angle), width * std::sin(angle)), silhouette);
	}
	painter.drawImage(0, 0, body);
	return outlined;
}

QSGImageNode* CreateLayerNode(QQuickWindow* window, QSGTexture* texture)
{
	auto* node = window->createImageNode();
	node->setOwnsTexture(false);
	node->setFiltering(QSGTexture::Linear);
	// The software renderer reads the texture as soon as the node joins a shown tree.
	node->setTexture(texture);
	return node;
}

struct HeartNode
{
	QSGOpacityNode* opacity = nullptr;
	QSGImageNode*   image   = nullptr;
};

class KitNode: public GroupNode
{
public:
	KitNode()
	{
		figure = new GroupNode;
		appendChildNode(figure);
		hearts = new GroupNode;
		appendChildNode(hearts);
		purr = new GroupNode;
		appendChildNode(purr);
	}

	ArtworkTextures    textures;
	GroupNode*         figure = nullptr;
	QSGImageNode*      layers[LAYER_COUNT] {};
	GroupNode*         hearts = nullptr;
	QVector<HeartNode> heart_nodes;
	GroupNode*         purr      = nullptr;
	QSGTextNode*       purr_text = nullptr;
};

} // namespace

KitMascotItem::KitMascotItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setAcceptedMouseButtons(Qt::LeftButton);
	setCursor(Qt::PointingHandCursor);
	setImplicitSize(VIEW_WIDTH, VIEW_HEIGHT);
	m_layers.resize(LAYER_COUNT);
	m_time.start();

	m_clock.setStartValue(0.0);
	m_clock.setEndValue(1.0);
	m_clock.setDuration(1000);
	m_clock.setLoopCount(-1);
	connect(&m_clock, &QVariantAnimation::valueChanged, this, &KitMascotItem::Tick);

	m_heart_timer.setInterval(HEART_INTERVAL);
	connect(&m_heart_timer, &QTimer::timeout, this, &KitMascotItem::SpawnHeart);
	m_unpet_timer.setInterval(UNPET_DELAY);
	m_unpet_timer.setSingleShot(true);
	connect(&m_unpet_timer, &QTimer::timeout, this, [this]() { SetPetted(false); });

	connect(Theme::Instance(), &Theme::LiveChanged, this, &KitMascotItem::UpdateClock);
	UpdateClock();
}

void KitMascotItem::SetFloating(bool floating)
{
	m_floating = floating;
	update();
}

void KitMascotItem::Hop()
{
	m_hop_start = Now();
	UpdateClock();
}

bool KitMascotItem::Hopping() const
{
	return m_hop_start >= 0 && m_now - m_hop_start < HOP_UP + HOP_DOWN;
}

qreal KitMascotItem::PurrLevel() const
{
	const qreal target = m_petted ? 1 : 0;
	if (m_purr_start < 0)
	{
		return target;
	}
	const qreal progress = std::min<qreal>(1, static_cast<qreal>(m_now - m_purr_start) / PURR_FADE);
	return m_purr_from + (target - m_purr_from) * progress;
}

void KitMascotItem::Tick()
{
	m_now = Now();
	m_hearts.erase(std::remove_if(m_hearts.begin(), m_hearts.end(), [this](const Heart& heart) { return m_now - heart.start >= HEART_LIFE; }),
	               m_hearts.end());
	UpdateClock();
	update();
}

void KitMascotItem::UpdateClock()
{
	m_now             = Now();
	const bool fading = m_purr_start >= 0 && m_now - m_purr_start < PURR_FADE;
	const bool moving = (Theme::Instance()->Live() && isVisible()) || Hopping() || !m_hearts.isEmpty() || fading;
	if (moving == (m_clock.state() == QAbstractAnimation::Running))
	{
		return;
	}
	if (moving)
	{
		m_clock.start();
	} else
	{
		m_clock.stop();
	}
	// The first frame of a new movement, or the rest pose once everything stopped.
	update();
}

void KitMascotItem::StartPet()
{
	m_unpet_timer.stop();
	SetPetted(true);
	SpawnHeart();
	m_heart_timer.start();
}

void KitMascotItem::StopPet()
{
	if (!m_heart_timer.isActive())
	{
		return;
	}
	m_heart_timer.stop();
	m_unpet_timer.start();
}

void KitMascotItem::SetPetted(bool petted)
{
	if (petted == m_petted)
	{
		return;
	}
	m_now        = Now();
	m_purr_from  = PurrLevel();
	m_purr_start = m_now;
	m_petted     = petted;
	if (petted)
	{
		m_pet_start = m_now;
	}
	UpdateClock();
	update();
}

void KitMascotItem::SpawnHeart()
{
	auto* random = QRandomGenerator::global();
	m_hearts.append({Now(), 10 + random->bounded(7.0), random->bounded(70.0) - 35});
	UpdateClock();
}

void KitMascotItem::mousePressEvent(QMouseEvent* /*event*/)
{
	StartPet();
}

void KitMascotItem::mouseMoveEvent(QMouseEvent* event)
{
	if (!contains(event->position()))
	{
		StopPet();
	}
}

void KitMascotItem::mouseReleaseEvent(QMouseEvent* /*event*/)
{
	StopPet();
}

void KitMascotItem::mouseUngrabEvent()
{
	StopPet();
}

void KitMascotItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	const qreal height = new_geometry.width() * VIEW_HEIGHT / VIEW_WIDTH;
	if (!qFuzzyCompare(new_geometry.height() + 1, height + 1))
	{
		setHeight(height);
	}
	if (new_geometry.size() != old_geometry.size())
	{
		polish();
	}
}

void KitMascotItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged)
	{
		polish();
	}
	if (change == ItemVisibleHasChanged)
	{
		UpdateClock();
	}
}

void KitMascotItem::updatePolish()
{
	const qreal dpr = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	const QSize px(static_cast<int>(std::ceil(width() * dpr)), static_cast<int>(std::ceil(height() * dpr)));
	if (px == m_layer_px || px.isEmpty())
	{
		return;
	}
	m_layer_px = px;
	for (int i = 0; i < LAYER_COUNT; i++)
	{
		m_layers[i] = PaintLayer(LAYER_FILES[i], px, i == GLITCH_RED || i == GLITCH_CYAN ? GLITCH_OPACITY : 1);
	}
	const auto* theme = Theme::Instance();
	// Kit stays a white cat; on the light palette a grey edge keeps the white body apart from the page.
	if (!theme->IsDark() && !m_layers[BODY].isNull())
	{
		m_layers[BODY] = Outlined(m_layers[BODY], theme->TextMuted(), std::max<qreal>(1, px.width() * OUTLINE_SHARE));
	}
	const int heart_px = static_cast<int>(std::ceil(HEART_MAX * px.width() / VIEW_WIDTH));
	m_heart            = PaintLayer(HEART_FILE, QSize(heart_px, static_cast<int>(std::ceil(heart_px * 0.9))), 1);
	// The eyes, the collar and the hearts wear the accent.
	for (auto* image: {&m_layers[GEAR], &m_layers[EYES_OPEN], &m_layers[EYES_HAPPY], &m_heart})
	{
		Recolour(image, theme->Accent());
	}
	update();
}

QSGNode* KitMascotItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<KitNode*>(old_node);
	if (node == nullptr)
	{
		node = new KitNode;
	}
	const auto* theme = Theme::Instance();
	const bool  live  = theme->Live();
	const qreal w     = width();
	const qreal h     = height();
	const qreal k     = w / VIEW_WIDTH;
	const bool  happy = m_petted || Hopping();

	for (int i = 0; i < LAYER_COUNT; i++)
	{
		auto* texture = node->textures.Get(window(), QString::number(i), m_layers.at(i));
		if (texture == nullptr)
		{
			continue;
		}
		if (node->layers[i] == nullptr)
		{
			node->layers[i] = CreateLayerNode(window(), texture);
			node->figure->appendChildNode(node->layers[i]);
		}
		node->layers[i]->setTexture(texture);
	}

	QPointF shift(0, live && m_floating ? FloatShift(m_now) * k : 0);
	if (live && m_petted)
	{
		shift += PurrShift(m_now - m_pet_start) * k;
	}
	if (Hopping())
	{
		shift.ry() += HopShift(m_now - m_hop_start) * k;
	}
	QMatrix4x4 figure;
	figure.translate(static_cast<float>(shift.x()), static_cast<float>(shift.y()));
	node->figure->setMatrix(figure);

	const QRectF whole(0, 0, w, h);
	auto         place = [&](int layer, const QRectF& rect)
	{
		if (node->layers[layer] != nullptr)
		{
			node->layers[layer]->setRect(rect);
		}
	};
	for (const auto layer: {GLITCH_RED, GLITCH_CYAN})
	{
		const auto& event = GlitchAt(layer, m_now % GLITCH_PERIOD);
		if (!live || !event.shown || node->layers[layer] == nullptr)
		{
			place(layer, QRectF());
			continue;
		}
		// A colour copy of the whole shape, cut to a horizontal band and nudged sideways.
		const qreal band = 1 - event.top - event.bottom;
		place(layer, QRectF(event.dx * k, event.top * h + event.dy * k, w, band * h));
		const auto size = node->layers[layer]->texture()->textureSize();
		node->layers[layer]->setSourceRect(QRectF(0, event.top * size.height(), size.width(), band * size.height()));
	}
	place(BODY, whole);
	place(WHISKERS, whole);
	place(GEAR, whole);
	const qreal  blink   = live ? BlinkScale(m_now) : 1;
	const qreal  eye_top = EYE_LINE * k * (1 - blink);
	const QRectF eyes    = happy ? QRectF() : QRectF(0, eye_top, w, h * blink);
	place(EYES_OPEN, eyes);
	place(EYES_SHINE, eyes);
	place(EYES_HAPPY, happy ? whole : QRectF());

	auto* heart_texture = node->textures.Get(window(), QStringLiteral("heart"), m_heart);
	const qsizetype hearts = heart_texture != nullptr ? m_hearts.size() : 0;
	while (node->heart_nodes.size() > hearts)
	{
		delete node->heart_nodes.takeLast().opacity;
	}
	while (node->heart_nodes.size() < hearts)
	{
		HeartNode heart;
		heart.opacity = new QSGOpacityNode;
		heart.image   = CreateLayerNode(window(), heart_texture);
		heart.opacity->appendChildNode(heart.image);
		node->hearts->appendChildNode(heart.opacity);
		node->heart_nodes.append(heart);
	}
	for (qsizetype i = 0; i < hearts; i++)
	{
		const auto& heart  = m_hearts.at(i);
		const auto  age    = m_now - heart.start;
		const qreal eased  = Ease(QEasingCurve::OutCubic, static_cast<qreal>(age) / HEART_LIFE);
		const qreal size   = heart.size * k;
		const qreal scale  = 0.5 + 0.6 * eased;
		const QPointF centre(w / 2 + heart.dx * k * eased, h * 0.38 - size * 0.45 - HEART_RISE * k * eased);
		const qreal opacity = age < HEART_FADE_IN
		                          ? static_cast<qreal>(age) / HEART_FADE_IN
		                          : 1 - Ease(QEasingCurve::InQuad, static_cast<qreal>(age - HEART_FADE_IN) / (HEART_LIFE - HEART_FADE_IN));
		node->heart_nodes[i].image->setTexture(heart_texture);
		node->heart_nodes[i].image->setRect(
		    QRectF(centre.x() - size * scale / 2, centre.y() - size * 0.45 * scale, size * scale, size * 0.9 * scale));
		node->heart_nodes[i].opacity->setOpacity(std::clamp<qreal>(opacity, 0, 1));
	}

	if (node->purr_text == nullptr)
	{
		node->purr_text = window()->createTextNode();
		node->purr->appendChildNode(node->purr_text);
	}
	const qreal level = PurrLevel();
	if (level <= 0)
	{
		node->purr_text->clear();
	} else
	{
		const auto    font = SceneMonospaceFont(std::max<qreal>(9, 12 * k));
		const QString text = QStringLiteral("purrr…");
		const qreal   tw   = TextWidth(text, font);
		const QRectF  rect(w + 34 * k - tw, -4 * k, tw, QFontMetricsF(font).height());
		QColor        color = theme->Accent();
		color.setAlphaF(static_cast<float>(level));
		SetTextLine(node->purr_text, text, font, color, rect);
		// Tilted and growing from its centre as it fades in.
		const qreal scale = 0.6 + 0.4 * level;
		QMatrix4x4  matrix;
		matrix.translate(static_cast<float>(rect.center().x()), static_cast<float>(rect.center().y()));
		matrix.rotate(8, 0, 0, 1);
		matrix.scale(static_cast<float>(scale));
		matrix.translate(static_cast<float>(-rect.center().x()), static_cast<float>(-rect.center().y()));
		node->purr->setMatrix(matrix);
	}
	node->textures.Prune();
	return node;
}
