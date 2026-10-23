#include "FieldControls.h"

#include "Accessibility.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGTextNode>

#include <algorithm>

// Sizes in units.
constexpr qreal RING_WIDTH    = 3;
constexpr qreal INSET         = 14;
constexpr qreal BOX_RADIUS    = 12;
constexpr qreal ARROW_ROOM    = 24;
constexpr qreal TRACK_WIDTH   = 56;
constexpr qreal TRACK_HEIGHT  = 30;
constexpr qreal KNOB_INSET    = 4;
constexpr qreal STATE_GAP     = 12;
constexpr qreal MODULE_RADIUS = 10;
constexpr qreal MODULE_RING   = 2;
constexpr qreal MODULE_INSET  = 12;
constexpr qreal TICK_SIZE     = 20;
constexpr qreal TICK_RADIUS   = 5;

namespace {

// A framed box and the texts on it; every control is one of these with different parts used.
class ControlNode: public GroupNode
{
public:
	explicit ControlNode(QQuickWindow* window)
	{
		box = new RoundedBoxNode;
		appendChildNode(box);
		inner = new RoundedBoxNode;
		appendChildNode(inner);
		first = window->createTextNode();
		appendChildNode(first);
		second = window->createTextNode();
		appendChildNode(second);
	}

	RoundedBoxNode* box    = nullptr;
	RoundedBoxNode* inner  = nullptr;
	QSGTextNode*    first  = nullptr;
	QSGTextNode*    second = nullptr;
};

ControlNode* ControlNodeFor(QSGNode* old_node, QQuickWindow* window)
{
	return old_node != nullptr ? static_cast<ControlNode*>(old_node) : new ControlNode(window);
}

RoundedBoxNode::Style BoxStyle(const QQuickItem* item, bool hovered, QColor fill, qreal radius)
{
	const auto*           theme   = Theme::Instance();
	const bool            focused = item->hasActiveFocus();
	RoundedBoxNode::Style style;
	style.fill         = fill;
	style.border       = focused ? theme->Accent() : (hovered ? theme->TextMuted() : theme->GlassBorder());
	style.border_width = focused ? RING_WIDTH * theme->U() : 1;
	style.radius       = radius;
	style.dpr          = item->window()->effectiveDevicePixelRatio();
	return style;
}

QFont BodyFont()
{
	return SceneFont(Theme::Instance()->FontBody());
}

} // namespace

FieldControlItem::FieldControlItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setAcceptedMouseButtons(Qt::LeftButton);
	setAcceptHoverEvents(true);
	setCursor(Qt::PointingHandCursor);
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::update);
}

void FieldControlItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemActiveFocusHasChanged)
	{
		update();
	}
}

void FieldControlItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space: Press(); return;
		default: event->ignore(); return;
	}
}

void FieldControlItem::mousePressEvent(QMouseEvent* /*event*/) {}

void FieldControlItem::mouseReleaseEvent(QMouseEvent* event)
{
	if (!contains(event->position()))
	{
		return;
	}
	forceActiveFocus(Qt::MouseFocusReason);
	Press();
}

void FieldControlItem::hoverEnterEvent(QHoverEvent* event)
{
	m_hovered = true;
	update();
	// The row under the control shows the pointer too.
	event->ignore();
}

void FieldControlItem::hoverLeaveEvent(QHoverEvent* event)
{
	m_hovered = false;
	update();
	event->ignore();
}

ToggleItem::ToggleItem(QQuickItem* parent): FieldControlItem(parent)
{
	m_slide.setDuration(Theme::Instance()->Animation());
	connect(&m_slide, &QVariantAnimation::valueChanged, this,
	        [this](const QVariant& value)
	        {
		        m_knob = value.toReal();
		        update();
	        });
}

void ToggleItem::SetOn(bool on)
{
	if (on == m_on)
	{
		return;
	}
	m_on = on;
	m_slide.stop();
	m_slide.setStartValue(m_knob);
	m_slide.setEndValue(on ? 1.0 : 0.0);
	// A row that is not on screen yet shows its value at once.
	if (isVisible() && window() != nullptr)
	{
		m_slide.start();
	} else
	{
		m_knob = on ? 1 : 0;
	}
	update();
}

QSGNode* ToggleItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto*       node  = ControlNodeFor(old_node, window());
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();

	const QRectF track(0, std::round((height() - TRACK_HEIGHT * u) / 2), TRACK_WIDTH * u, TRACK_HEIGHT * u);
	node->box->Update(window(), track, BoxStyle(this, Hovered(), m_on ? theme->GlassStrong() : theme->Glass(), track.height() / 2));

	const qreal           knob_size = track.height() - 2 * KNOB_INSET * u;
	const qreal           travel    = track.width() - knob_size - 2 * KNOB_INSET * u;
	RoundedBoxNode::Style knob;
	knob.fill   = theme->TextPrimary();
	knob.radius = knob_size / 2;
	knob.dpr    = window()->effectiveDevicePixelRatio();
	node->inner->Update(window(),
	                    QRectF(track.left() + KNOB_INSET * u + m_knob * travel, track.top() + KNOB_INSET * u, knob_size, knob_size), knob);

	const QRectF state(track.right() + STATE_GAP * u, 0, std::max<qreal>(0, width() - track.right() - STATE_GAP * u), height());
	SetTextLine(node->first, m_on ? tr("On") : tr("Off"), BodyFont(), theme->TextSecondary(), state);
	return node;
}

ChoiceItem::ChoiceItem(QQuickItem* parent): FieldControlItem(parent) {}

void ChoiceItem::SetValue(const QString& value)
{
	if (value != m_value)
	{
		m_value = value;
		update();
	}
}

void ChoiceItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Left: emit Activated(-1); return;
		case Qt::Key_Right: emit Activated(1); return;
		default: FieldControlItem::keyPressEvent(event); return;
	}
}

QSGNode* ChoiceItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto*       node  = ControlNodeFor(old_node, window());
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();

	node->box->Update(window(), boundingRect(),
	                  BoxStyle(this, Hovered(), Hovered() ? theme->GlassStrong() : theme->Glass(), BOX_RADIUS * u));
	const auto text = boundingRect().adjusted(INSET * u, 0, -INSET * u, 0);
	SetTextLine(node->first, m_value, BodyFont(), theme->TextPrimary(), text.adjusted(0, 0, -ARROW_ROOM * u, 0));
	SetTextLine(node->second, QStringLiteral("▾"), BodyFont(), theme->TextSecondary(), text, Qt::AlignRight);
	return node;
}

ModuleRowItem::ModuleRowItem(QQuickItem* parent): FieldControlItem(parent) {}

void ModuleRowItem::SetModule(const QString& path, bool present, bool selected)
{
	m_path     = path;
	m_present  = present;
	m_selected = selected;
	SetAccessible(this, QAccessible::CheckBox, path);
	SetAccessibleChecked(this, selected);
	update();
}

QSGNode* ModuleRowItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto*       node    = ControlNodeFor(old_node, window());
	const auto* theme   = Theme::Instance();
	const qreal u       = theme->U();
	const qreal dpr     = window()->effectiveDevicePixelRatio();
	const bool  focused = hasActiveFocus();

	RoundedBoxNode::Style highlight;
	highlight.fill         = focused ? theme->GlassStrong() : (Hovered() ? theme->Glass() : QColor(Qt::transparent));
	highlight.border       = theme->Accent();
	highlight.border_width = focused ? MODULE_RING * u : 0;
	highlight.radius       = MODULE_RADIUS * u;
	highlight.dpr          = dpr;
	node->box->Update(window(), boundingRect(), highlight);

	const QRectF          tick(MODULE_INSET * u, std::round((height() - TICK_SIZE * u) / 2), TICK_SIZE * u, TICK_SIZE * u);
	RoundedBoxNode::Style tick_style;
	tick_style.fill         = theme->Glass();
	tick_style.border       = theme->GlassBorder();
	tick_style.border_width = 1;
	tick_style.radius       = TICK_RADIUS * u;
	tick_style.dpr          = dpr;
	node->inner->Update(window(), tick, tick_style);
	SetTextLine(node->first, m_selected ? QStringLiteral("✓") : QString(), SceneFont(theme->FontSmall()), theme->TextPrimary(), tick,
	            Qt::AlignHCenter);

	const QRectF text(tick.right() + MODULE_INSET * u, 0, std::max<qreal>(0, width() - tick.right() - 2 * MODULE_INSET * u), height());
	const auto   font  = SceneFont(theme->FontSmall());
	const auto   label = m_path + (m_present ? QString() : tr("  (missing)"));
	SetTextLine(node->second, QFontMetricsF(font).elidedText(label, Qt::ElideMiddle, text.width()), font,
	            m_present ? theme->TextPrimary() : theme->TextMuted(), text);
	return node;
}
