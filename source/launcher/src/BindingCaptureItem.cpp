#include "BindingCaptureItem.h"

#include "GamepadInput.h"
#include "InputProfiles.h"
#include "KeyNames.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGRectangleNode>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal PANEL_WIDTH  = 520;
constexpr qreal PADDING      = 24;
constexpr qreal LINE_SPACING = 10;

constexpr int TICK_MS = 200;

namespace {

class CaptureNode: public GroupNode
{
public:
	explicit CaptureNode(QQuickWindow* window)
	{
		scrim = window->createRectangleNode();
		appendChildNode(scrim);
		panel = new RoundedBoxNode;
		appendChildNode(panel);
		for (auto*& line: lines)
		{
			line = window->createTextNode();
			appendChildNode(line);
		}
	}

	QSGRectangleNode* scrim = nullptr;
	RoundedBoxNode*   panel = nullptr;
	// The prompt, the note, and how to cancel.
	QSGTextNode* lines[3] {};
};

QString MouseButtonName(Qt::MouseButton button)
{
	switch (button)
	{
		case Qt::LeftButton: return QStringLiteral("Mouse Left");
		case Qt::MiddleButton: return QStringLiteral("Mouse Middle");
		case Qt::RightButton: return QStringLiteral("Mouse Right");
		case Qt::BackButton: return QStringLiteral("Mouse X1");
		case Qt::ForwardButton: return QStringLiteral("Mouse X2");
		default: return {};
	}
}

} // namespace

BindingCaptureItem::BindingCaptureItem(GamepadInput* gamepad, QQuickItem* parent): QQuickItem(parent), m_gamepad(gamepad)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope);
	setAcceptedMouseButtons(Qt::AllButtons);
	setAcceptHoverEvents(true);
	setVisible(false);
	setZ(20);
	m_tick.setInterval(TICK_MS);
	connect(&m_tick, &QTimer::timeout, this, &BindingCaptureItem::Tick);
	connect(m_gamepad, &GamepadInput::ButtonCaptured, this,
	        [this](const QString& name)
	        {
		        if (m_shown && m_for_gamepad)
		        {
			        Capture(name);
		        }
	        });
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::update);
}

void BindingCaptureItem::OpenFor(const QString& key, const QString& title, bool gamepad)
{
	m_key          = key;
	m_title        = title;
	m_for_gamepad  = gamepad;
	m_note         = gamepad && m_gamepad->ControllerName().isEmpty() ? tr("No controller is connected.") : QString();
	m_seconds_left = CAPTURE_SECONDS;
	m_shown        = true;
	if (gamepad)
	{
		m_gamepad->BeginCapture();
	}
	m_clock.start();
	m_tick.start();
	setVisible(true);
	forceActiveFocus(Qt::OtherFocusReason);
	update();
}

void BindingCaptureItem::Close()
{
	if (!m_shown)
	{
		return;
	}
	m_shown = false;
	m_tick.stop();
	if (m_for_gamepad)
	{
		m_gamepad->EndCapture();
	}
	setVisible(false);
	emit Closed();
}

void BindingCaptureItem::Capture(const QString& name)
{
	const auto key = m_key;
	Close();
	emit Captured(key, name);
}

void BindingCaptureItem::Tick()
{
	const int left = CAPTURE_SECONDS - static_cast<int>(m_clock.elapsed() / 1000);
	if (left <= 0)
	{
		Close();
		return;
	}
	if (left != m_seconds_left)
	{
		m_seconds_left = left;
		update();
	}
}

void BindingCaptureItem::keyPressEvent(QKeyEvent* event)
{
	// Escape cancels whether it comes from the keyboard or from a controller's back button.
	if (event->key() == Qt::Key_Escape)
	{
		Close();
		return;
	}
	if (event->isAutoRepeat())
	{
		return;
	}
	const auto name = KeyNameFromEvent(event);
	if (m_for_gamepad || name.isEmpty())
	{
		// Controller presses reach the window as keys without a physical key; they navigate, they are not bound.
		m_note = m_for_gamepad ? tr("Press a button on the controller.") : tr("Press a key on the keyboard, or click a mouse button.");
	} else if (InputProfiles::IsReservedKey(name))
	{
		m_note = tr("%1 is kept for the emulator's own commands.").arg(name);
	} else
	{
		Capture(name);
		return;
	}
	update();
}

void BindingCaptureItem::mousePressEvent(QMouseEvent* /*event*/) {}

// The button is taken when it is released, so the click that opened the wait is never bound.
void BindingCaptureItem::mouseReleaseEvent(QMouseEvent* event)
{
	const auto name = MouseButtonName(event->button());
	if (m_for_gamepad || name.isEmpty())
	{
		return;
	}
	Capture(name);
}

void BindingCaptureItem::hoverMoveEvent(QHoverEvent* /*event*/) {}

void BindingCaptureItem::wheelEvent(QWheelEvent* /*event*/) {}

void BindingCaptureItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		update();
	}
}

QRectF BindingCaptureItem::Panel() const
{
	const auto* theme  = Theme::Instance();
	const qreal u      = theme->U();
	const qreal width  = std::min<qreal>(PANEL_WIDTH * u, this->width());
	const qreal lines  = LineHeight(SceneFont(theme->FontTitle())) + 2 * LineHeight(SceneFont(theme->FontSmall()));
	const qreal height = lines + 2 * LINE_SPACING * u + 2 * PADDING * u;
	return {std::round((this->width() - width) / 2), std::round((this->height() - height) / 2), width, height};
}

QSGNode* BindingCaptureItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto*       node  = old_node != nullptr ? static_cast<CaptureNode*>(old_node) : new CaptureNode(window());
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();

	node->scrim->setRect(boundingRect());
	node->scrim->setColor(theme->Scrim());

	const auto            panel = Panel();
	RoundedBoxNode::Style style;
	style.fill         = theme->Panel();
	style.border       = theme->GlassBorder();
	style.border_width = 1;
	style.radius       = theme->Radius();
	style.dpr          = window()->effectiveDevicePixelRatio();
	node->panel->Update(window(), panel, style);

	const auto  title_font = SceneFont(theme->FontTitle(), QFont::DemiBold);
	const auto  small      = SceneFont(theme->FontSmall());
	const qreal x          = panel.left() + PADDING * u;
	const qreal width      = panel.width() - 2 * PADDING * u;
	qreal       y          = panel.top() + PADDING * u;
	const auto  prompt     = m_for_gamepad ? tr("Press a controller button for %1").arg(m_title) : tr("Press a key or click for %1").arg(m_title);
	SetTextLine(node->lines[0], prompt, title_font, theme->TextPrimary(), QRectF(x, y, width, LineHeight(title_font)), Qt::AlignHCenter);
	y += LineHeight(title_font) + LINE_SPACING * u;
	SetTextLine(node->lines[1], m_note, small, theme->Danger(), QRectF(x, y, width, LineHeight(small)), Qt::AlignHCenter);
	y += LineHeight(small) + LINE_SPACING * u;
	const auto cancel = m_for_gamepad ? tr("Esc on the keyboard cancels · %n s left", nullptr, m_seconds_left)
	                                  : tr("Esc cancels · %n s left", nullptr, m_seconds_left);
	SetTextLine(node->lines[2], cancel, small, theme->TextMuted(), QRectF(x, y, width, LineHeight(small)), Qt::AlignHCenter);
	return node;
}
