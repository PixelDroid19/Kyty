#include "ControlsPanelItem.h"

#include "Accessibility.h"
#include "ChoiceListItem.h"
#include "ControllerDrawing.h"
#include "FieldControls.h"
#include "FieldRowItem.h"
#include "FolderTabsItem.h"
#include "GamepadInput.h"
#include "InputProfiles.h"
#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "TextFieldItem.h"
#include "TextPadItem.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>
#include <limits>

// Sizes in units.
constexpr qreal LABEL_HEIGHT  = 18;
constexpr qreal CONTROL       = 40;
constexpr qreal CARD_CONTROL  = 36;
constexpr qreal BAR_GAP       = 12;
constexpr qreal TABS_HEIGHT   = 40;
constexpr qreal TABS_GAP      = 14;
constexpr qreal ROW           = 34;
constexpr qreal ROW_SPACING   = 8;
constexpr qreal COLUMN        = 210;
constexpr qreal LABEL_GAP     = 8;
constexpr qreal LEAD_GAP      = 10;
constexpr qreal MIN_DRAWING   = 220;
constexpr qreal MAX_DRAWING   = 400;
constexpr qreal CARD_PADDING  = 12;
constexpr qreal CARD_SPACING  = 8;
constexpr qreal TITLE_HEIGHT  = 24;
constexpr qreal PREVIEW       = 96;
constexpr qreal CROSS_CHIP    = 64;
constexpr qreal CROSS_HEIGHT  = 28;
constexpr qreal CROSS_GAP     = 4;
constexpr qreal NOTE_GAP      = 10;
constexpr qreal STROKE        = 1.4;
constexpr qreal ANCHOR_RADIUS = 2.5;

// The controller is read this often while the panel is on screen.
constexpr int POLL_INTERVAL = 33;
// Stick positions are rounded to this step, so a resting stick does not paint the drawing again.
constexpr qreal STICK_STEP = 0.05;

namespace {

struct ControlText
{
	const char* control;
	// Beside the binding; translated when it is a word.
	const char* label;
	// The full name, for the capture's title and screen readers.
	const char* title;
};

const ControlText CONTROL_TEXTS[] = {
    {"cross", "✕", QT_TRANSLATE_NOOP("ControlsPanelItem", "✕ Cross")},
    {"circle", "○", QT_TRANSLATE_NOOP("ControlsPanelItem", "○ Circle")},
    {"square", "□", QT_TRANSLATE_NOOP("ControlsPanelItem", "□ Square")},
    {"triangle", "△", QT_TRANSLATE_NOOP("ControlsPanelItem", "△ Triangle")},
    {"up", QT_TRANSLATE_NOOP("ControlsPanelItem", "D-pad ↑"), QT_TRANSLATE_NOOP("ControlsPanelItem", "D-pad up")},
    {"down", QT_TRANSLATE_NOOP("ControlsPanelItem", "D-pad ↓"), QT_TRANSLATE_NOOP("ControlsPanelItem", "D-pad down")},
    {"left", QT_TRANSLATE_NOOP("ControlsPanelItem", "D-pad ←"), QT_TRANSLATE_NOOP("ControlsPanelItem", "D-pad left")},
    {"right", QT_TRANSLATE_NOOP("ControlsPanelItem", "D-pad →"), QT_TRANSLATE_NOOP("ControlsPanelItem", "D-pad right")},
    {"l1", "L1", "L1"},
    {"r1", "R1", "R1"},
    {"l2", "L2", "L2"},
    {"r2", "R2", "R2"},
    {"l3", "L3", "L3"},
    {"r3", "R3", "R3"},
    {"options", QT_TRANSLATE_NOOP("ControlsPanelItem", "Options"), QT_TRANSLATE_NOOP("ControlsPanelItem", "Options")},
    {"touchpad", QT_TRANSLATE_NOOP("ControlsPanelItem", "Touchpad"), QT_TRANSLATE_NOOP("ControlsPanelItem", "Touchpad")},
    {"left-stick-left", "←", QT_TRANSLATE_NOOP("ControlsPanelItem", "Left stick left")},
    {"left-stick-right", "→", QT_TRANSLATE_NOOP("ControlsPanelItem", "Left stick right")},
    {"left-stick-up", "↑", QT_TRANSLATE_NOOP("ControlsPanelItem", "Left stick up")},
    {"left-stick-down", "↓", QT_TRANSLATE_NOOP("ControlsPanelItem", "Left stick down")},
    {"right-stick-left", "←", QT_TRANSLATE_NOOP("ControlsPanelItem", "Right stick left")},
    {"right-stick-right", "→", QT_TRANSLATE_NOOP("ControlsPanelItem", "Right stick right")},
    {"right-stick-up", "↑", QT_TRANSLATE_NOOP("ControlsPanelItem", "Right stick up")},
    {"right-stick-down", "↓", QT_TRANSLATE_NOOP("ControlsPanelItem", "Right stick down")},
};

// The bindings down each side of the drawing, ordered from the top of the controller so their lines do not cross.
const char* const LEFT_COLUMN[]  = {"l2", "l1", "touchpad", "up", "left", "right", "down", "l3"};
const char* const RIGHT_COLUMN[] = {"r2", "r1", "options", "triangle", "square", "circle", "cross", "r3"};
// Each stick's four directions as a cross, in the order up, left, right, down.
const char* const STICK_CROSSES[2][4] = {{"left-stick-up", "left-stick-left", "left-stick-right", "left-stick-down"},
                                         {"right-stick-up", "right-stick-left", "right-stick-right", "right-stick-down"}};

const ControlText* FindControl(const QString& control)
{
	for (const auto& text: CONTROL_TEXTS)
	{
		if (control == QLatin1String(text.control))
		{
			return &text;
		}
	}
	return nullptr;
}

QString Translated(const char* text)
{
	return QCoreApplication::translate("ControlsPanelItem", text);
}

// A label is a word to translate unless it is a symbol or a button's printed name.
QString ControlLabel(const QString& control)
{
	const auto* text = FindControl(control);
	if (text == nullptr)
	{
		return control;
	}
	return QString::fromUtf8(text->label).size() <= 2 ? QString::fromUtf8(text->label) : Translated(text->label);
}

PageLayout::Text TextAt(const QRectF& rect, const QString& text, const QFont& font, const QColor& color,
                        Qt::Alignment alignment = Qt::AlignLeft, QTextOption::WrapMode wrap = QTextOption::NoWrap)
{
	PageLayout::Text line;
	line.rect      = rect;
	line.text      = text;
	line.font      = font;
	line.color     = color;
	line.alignment = alignment;
	line.wrap      = wrap;
	return line;
}

QPointF Rounded(QPointF point)
{
	return {std::round(point.x() / STICK_STEP) * STICK_STEP, std::round(point.y() / STICK_STEP) * STICK_STEP};
}

QColor WithAlpha(QColor color, qreal alpha)
{
	color.setAlphaF(static_cast<float>(color.alphaF() * alpha));
	return color;
}

class PanelNode: public GroupNode
{
public:
	PanelNode()
	{
		boxes = new GroupNode;
		art   = new GroupNode;
		texts = new GroupNode;
		appendChildNode(boxes);
		appendChildNode(art);
		appendChildNode(texts);
	}

	GroupNode* boxes          = nullptr;
	GroupNode* art            = nullptr;
	GroupNode* texts          = nullptr;
	int        generation     = -1;
	int        art_generation = -1;
};

void ClearChildren(QSGNode* node)
{
	while (auto* child = node->firstChild())
	{
		node->removeChildNode(child);
		delete child;
	}
}

} // namespace

ControlsPanelItem::ControlsPanelItem(TextPadItem* pad, QQuickItem* page): QQuickItem(page), m_pad(pad)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope);
	// On the page, so the list is not cut off by the panel's edge and covers what is under it.
	m_list = new ChoiceListItem(page);
	connect(m_list, &ChoiceListItem::Chosen, this,
	        [this](int index)
	        {
		        if (index >= 0 && index < m_list_values.size())
		        {
			        emit Edited(m_list_key, m_list_values.at(index));
		        }
		        FocusKey(m_list_key, false);
	        });
	connect(m_list, &ChoiceListItem::Cancelled, this, [this]() { FocusKey(m_list_key, false); });

	m_profile = AddChoice(QStringLiteral("input_profile"), "input_profile");
	m_name    = new TextFieldItem(this);
	connect(m_name, &TextFieldItem::Edited, this, [this](const QString& text) { emit Edited(QStringLiteral("profile_name"), text); });
	connect(m_name, &TextFieldItem::PadRequested, this,
	        [this]()
	        {
		        m_pad->SetText(m_values.value("profile_name").toString());
		        m_pad->OpenFor(QStringLiteral("profile_name"), tr("Profile name"));
	        });
	connect(m_pad, &TextPadItem::Edited, this,
	        [this](const QString& text)
	        {
		        if (m_pad->Key() == QLatin1String("profile_name") && isVisible())
		        {
			        m_pad->SetText(text);
			        emit Edited(QStringLiteral("profile_name"), text);
		        }
	        });
	connect(m_pad, &TextPadItem::ShownChanged, this,
	        [this]()
	        {
		        if (!m_pad->Shown() && m_pad->Key() == QLatin1String("profile_name") && isVisible())
		        {
			        m_name->forceActiveFocus(Qt::OtherFocusReason);
		        }
	        });
	m_device = AddChoice(QStringLiteral("input_device"), "input_device");

	m_tabs = new FolderTabsItem(this);
	QVariantList folders;
	const QString titles[TAB_COUNT] = {tr("Controller"), tr("Keyboard and mouse"), tr("Sticks and triggers"), tr("Mouse")};
	for (int i = 0; i < TAB_COUNT; i++)
	{
		folders << QVariantMap {{"id", i}, {"title", titles[i]}};
	}
	m_tabs->SetFolders(folders);
	connect(m_tabs, &FolderTabsItem::CurrentIndexChanged, this, &ControlsPanelItem::ShowTab);
	connect(m_tabs, &FolderTabsItem::TabPaddingChanged, this, &QQuickItem::polish);

	const auto& controls = InputProfiles::ControlNames();
	for (int i = 0; i < controls.size(); i++)
	{
		if (i < InputProfiles::BUTTON_CONTROL_COUNT)
		{
			m_pad_bindings << Binding {controls.at(i), AddBinding(controls.at(i), true)};
		}
		m_key_bindings << Binding {controls.at(i), AddBinding(controls.at(i), false)};
	}

	for (int side = 0; side < 2; side++)
	{
		const QString prefix   = side == 0 ? QStringLiteral("left") : QStringLiteral("right");
		auto&         card     = m_sticks[side];
		card.preview           = new StickPreviewItem(this);
		card.deadzone          = AddChoice(prefix + QStringLiteral("_deadzone"), "deadzone");
		card.invert_x          = AddToggle(prefix + QStringLiteral("_invert_x"));
		card.invert_y          = AddToggle(prefix + QStringLiteral("_invert_y"));
	}
	m_trigger      = AddChoice(QStringLiteral("trigger_deadzone"), "deadzone");
	m_mouse_look   = AddToggle(QStringLiteral("mouse_look"));
	m_sensitivity  = AddChoice(QStringLiteral("mouse_sensitivity"), "sensitivity");
	m_mouse_invert = AddToggle(QStringLiteral("mouse_invert_y"));

	for (auto* item: FocusItems() + TabItems(PadTab) + TabItems(KeysTab) + TabItems(SticksTab) + TabItems(MouseTab))
	{
		// The focused control's line and part light up.
		connect(item, &QQuickItem::activeFocusChanged, this,
		        [this]()
		        {
			        m_art_generation++;
			        update();
		        });
	}

	m_poll.setInterval(POLL_INTERVAL);
	connect(&m_poll, &QTimer::timeout, this, &ControlsPanelItem::Poll);
	m_poll.start();
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::polish);
	ShowTab();
}

BindingItem* ControlsPanelItem::AddBinding(const QString& control, bool gamepad)
{
	auto*      chip = new BindingItem(this);
	const auto key  = QStringLiteral("bind.") + control;
	connect(chip, &FieldControlItem::Activated, this, [this, key, gamepad]() { emit CaptureRequested(key, gamepad); });
	connect(chip, &BindingItem::Cleared, this,
	        [this, key, gamepad]()
	        {
		        auto binding                           = m_values.value(key).toMap();
		        binding[gamepad ? "buttons" : "keys"] = QStringList();
		        emit Edited(key, binding);
	        });
	return chip;
}

ChoiceItem* ControlsPanelItem::AddChoice(const QString& key, const char* list)
{
	auto* choice = new ChoiceItem(this);
	m_by_key.insert(key, choice);
	connect(choice, &FieldControlItem::Activated, this, [this, key, list](int delta) { Step(key, list, delta); });
	connect(choice, &FieldControlItem::ListRequested, this, [this, key, list, choice]() { OpenList(key, list, choice); });
	return choice;
}

ToggleItem* ControlsPanelItem::AddToggle(const QString& key)
{
	auto* toggle = new ToggleItem(this);
	m_by_key.insert(key, toggle);
	connect(toggle, &FieldControlItem::Activated, this, [this, key]() { emit Edited(key, !m_values.value(key).toBool()); });
	return toggle;
}

void ControlsPanelItem::OpenList(const QString& key, const char* list, ChoiceItem* box)
{
	if (m_list->parentItem() == nullptr)
	{
		return;
	}
	m_list_key    = key;
	m_list_values = LauncherBackend::Instance()->Choices().value(QString::fromLatin1(list)).toStringList();
	QStringList texts;
	for (const auto& value: m_list_values)
	{
		texts << ChoiceDisplay(list, value);
	}
	m_list->OpenFor(box->mapRectToItem(m_list->parentItem(), box->boundingRect()), texts,
	                static_cast<int>(m_list_values.indexOf(m_values.value(key).toString())));
}

void ControlsPanelItem::Step(const QString& key, const char* list, int delta)
{
	const auto values = LauncherBackend::Instance()->Choices().value(QString::fromLatin1(list)).toStringList();
	if (values.isEmpty())
	{
		return;
	}
	const auto count = values.size();
	const auto index = std::max<qsizetype>(0, values.indexOf(m_values.value(key).toString()));
	emit       Edited(key, values.at((index + delta + count) % count));
}

QString ControlsPanelItem::ControlTitle(const QString& key)
{
	const auto  control = key.mid(QStringLiteral("bind.").size());
	const auto* text    = FindControl(control);
	return text == nullptr ? control : Translated(text->title);
}

bool ControlsPanelItem::Editable() const
{
	return m_values.value("input_profile").toString() != QLatin1String(InputProfiles::DEFAULT_ID) &&
	       m_values.value("profile_error").toString().isEmpty();
}

void ControlsPanelItem::SetValues(const QVariantMap& values)
{
	m_values = values;
	auto show_choice = [this](ChoiceItem* choice, const QString& key, const char* list, const QString& name)
	{
		const auto shown = ChoiceDisplay(list, m_values.value(key).toString());
		choice->SetValue(shown);
		SetAccessible(choice, QAccessible::ComboBox, name, shown);
	};
	auto show_toggle = [this](ToggleItem* toggle, const QString& key, const QString& name)
	{
		toggle->SetOn(m_values.value(key).toBool());
		SetAccessible(toggle, QAccessible::CheckBox, name);
		SetAccessibleChecked(toggle, m_values.value(key).toBool());
	};
	show_choice(m_profile, QStringLiteral("input_profile"), "input_profile", tr("Profile"));
	show_choice(m_device, QStringLiteral("input_device"), "input_device", tr("Controller"));
	// Typing keeps its own text; the saved name may be trimmed.
	if (!m_name->hasActiveFocus())
	{
		m_name->SetText(m_values.value("profile_name").toString());
	}
	SetAccessible(m_name, QAccessible::EditableText, tr("Profile name"));
	for (int side = 0; side < 2; side++)
	{
		const QString prefix = side == 0 ? QStringLiteral("left") : QStringLiteral("right");
		const auto&   card   = m_sticks[side];
		const auto    stick  = side == 0 ? tr("Left stick") : tr("Right stick");
		show_choice(card.deadzone, prefix + QStringLiteral("_deadzone"), "deadzone", tr("%1 dead zone").arg(stick));
		show_toggle(card.invert_x, prefix + QStringLiteral("_invert_x"), tr("Invert %1 across").arg(stick));
		show_toggle(card.invert_y, prefix + QStringLiteral("_invert_y"), tr("Invert %1 up and down").arg(stick));
		card.preview->SetTuning(side == 1, m_values.value(prefix + QStringLiteral("_deadzone")).toDouble(),
		                        m_values.value(prefix + QStringLiteral("_invert_x")).toBool(),
		                        m_values.value(prefix + QStringLiteral("_invert_y")).toBool());
	}
	show_choice(m_trigger, QStringLiteral("trigger_deadzone"), "deadzone", tr("Trigger press point"));
	show_toggle(m_mouse_look, QStringLiteral("mouse_look"), tr("Look with the mouse"));
	show_choice(m_sensitivity, QStringLiteral("mouse_sensitivity"), "sensitivity", tr("Mouse sensitivity"));
	show_toggle(m_mouse_invert, QStringLiteral("mouse_invert_y"), tr("Invert the mouse up and down"));
	ShowBindings();
	ShowTab();
}

void ControlsPanelItem::ShowBindings()
{
	const auto* backend   = LauncherBackend::Instance();
	const auto  separator = QStringLiteral(" · ");
	for (const auto& binding: m_pad_bindings)
	{
		const auto  value = m_values.value(QStringLiteral("bind.") + binding.control).toMap();
		QStringList buttons;
		// L2 and R2 always follow the controller's triggers; buttons bound to them come on top.
		if (binding.control == QLatin1String("l2") || binding.control == QLatin1String("r2"))
		{
			buttons << tr("Trigger");
		}
		for (const auto& name: value.value("buttons").toStringList())
		{
			buttons << backend->PadButtonText(name);
		}
		binding.chip->SetText(buttons.isEmpty() ? tr("Not bound") : buttons.join(separator), !buttons.isEmpty());
		SetAccessible(binding.chip, QAccessible::Button, tr("%1, controller").arg(ControlTitle(QStringLiteral("bind.") + binding.control)));
	}
	for (const auto& binding: m_key_bindings)
	{
		const auto  value = m_values.value(QStringLiteral("bind.") + binding.control).toMap();
		QStringList keys;
		for (const auto& name: value.value("keys").toStringList())
		{
			keys << KeyDisplay(name);
		}
		binding.chip->SetText(keys.isEmpty() ? tr("Not bound") : keys.join(separator), !keys.isEmpty());
		SetAccessible(binding.chip, QAccessible::Button,
		              tr("%1, keyboard and mouse").arg(ControlTitle(QStringLiteral("bind.") + binding.control)));
	}
}

QVector<QQuickItem*> ControlsPanelItem::TabItems(int tab) const
{
	QVector<QQuickItem*> items;
	switch (tab)
	{
		case PadTab:
			for (const auto& binding: m_pad_bindings)
			{
				items << binding.chip;
			}
			break;
		case KeysTab:
			for (const auto& binding: m_key_bindings)
			{
				items << binding.chip;
			}
			break;
		case SticksTab:
			for (const auto& card: m_sticks)
			{
				items << card.deadzone << card.invert_x << card.invert_y;
			}
			items << m_trigger;
			break;
		case MouseTab: items << m_mouse_look << m_sensitivity << m_mouse_invert; break;
		default: break;
	}
	return items;
}

// Default and a damaged profile show their controls without letting them change.
void ControlsPanelItem::ShowTab()
{
	const int  current  = m_tabs->CurrentIndex();
	const bool editable = Editable();
	m_name->setVisible(editable);
	m_device->setVisible(editable);
	for (int tab = 0; tab < TAB_COUNT; tab++)
	{
		for (auto* item: TabItems(tab))
		{
			item->setVisible(tab == current);
			item->setEnabled(editable);
			item->setOpacity(editable ? 1 : 0.55);
		}
	}
	for (const auto& card: m_sticks)
	{
		card.preview->setVisible(current == SticksTab);
	}
	const bool look = m_values.value("mouse_look").toBool();
	m_sensitivity->setVisible(current == MouseTab && look);
	m_mouse_invert->setVisible(current == MouseTab && look);
	polish();
}

void ControlsPanelItem::ChangeTab(int delta)
{
	const auto* focus      = Focused();
	const bool  in_content = focus != nullptr && TabItems(m_tabs->CurrentIndex()).contains(const_cast<QQuickItem*>(focus));
	m_tabs->SetCurrentIndex((m_tabs->CurrentIndex() + delta + TAB_COUNT) % TAB_COUNT);
	if (!in_content)
	{
		return;
	}
	for (auto* item: TabItems(m_tabs->CurrentIndex()))
	{
		if (item->isVisible() && item->isEnabled())
		{
			item->forceActiveFocus(Qt::TabFocusReason);
			return;
		}
	}
	m_tabs->forceActiveFocus(Qt::TabFocusReason);
}

QVector<QQuickItem*> ControlsPanelItem::FocusItems() const
{
	QVector<QQuickItem*> items {m_profile, m_name, m_device, m_tabs};
	items << TabItems(m_tabs->CurrentIndex());
	items.erase(std::remove_if(items.begin(), items.end(), [](const QQuickItem* item) { return !item->isVisible() || !item->isEnabled(); }),
	            items.end());
	return items;
}

QQuickItem* ControlsPanelItem::Focused() const
{
	const auto items = FocusItems();
	const auto found = std::find_if(items.begin(), items.end(), [](const QQuickItem* item) { return item->hasActiveFocus(); });
	return found == items.end() ? nullptr : *found;
}

void ControlsPanelItem::FocusFirst()
{
	m_profile->forceActiveFocus(Qt::OtherFocusReason);
}

void ControlsPanelItem::FocusKey(const QString& key, bool gamepad)
{
	QQuickItem* target  = key == QLatin1String("profile_name") ? m_name : m_by_key.value(key);
	const auto  control = key.mid(QStringLiteral("bind.").size());
	for (const auto& binding: gamepad ? m_pad_bindings : m_key_bindings)
	{
		if (key.startsWith(QLatin1String("bind.")) && binding.control == control)
		{
			target = binding.chip;
		}
	}
	for (int tab = 0; tab < TAB_COUNT; tab++)
	{
		if (TabItems(tab).contains(target))
		{
			m_tabs->SetCurrentIndex(tab);
		}
	}
	if (target == nullptr || !target->isVisible() || !target->isEnabled())
	{
		FocusFirst();
		return;
	}
	target->forceActiveFocus(Qt::OtherFocusReason);
}

// The nearest control whose box lies past the focused one's edge in the direction, preferring ones in line with it.
bool ControlsPanelItem::MoveFocus(int dx, int dy)
{
	auto* from = Focused();
	if (from == nullptr)
	{
		FocusFirst();
		return true;
	}
	const QRectF a = from->mapRectToItem(this, from->boundingRect());
	QQuickItem*  best       = nullptr;
	qreal        best_score = std::numeric_limits<qreal>::max();
	const qreal  slack      = 1;
	for (auto* item: FocusItems())
	{
		const QRectF b     = item->mapRectToItem(this, item->boundingRect());
		const bool   ahead = (dx > 0 && b.left() >= a.right() - slack) || (dx < 0 && b.right() <= a.left() + slack) ||
		                   (dy > 0 && b.top() >= a.bottom() - slack) || (dy < 0 && b.bottom() <= a.top() + slack);
		if (item == from || !ahead)
		{
			continue;
		}
		const QPointF d      = b.center() - a.center();
		const qreal   along  = d.x() * dx + d.y() * dy;
		const qreal   across = std::abs(d.x() * dy) + std::abs(d.y() * dx);
		if (const qreal score = along + 2 * across; score < best_score)
		{
			best_score = score;
			best       = item;
		}
	}
	if (best == nullptr)
	{
		return false;
	}
	best->forceActiveFocus(dy != 0 ? Qt::TabFocusReason : Qt::OtherFocusReason);
	return true;
}

void ControlsPanelItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Up:
			if (!MoveFocus(0, -1))
			{
				emit TopReached();
			}
			return;
		case Qt::Key_Down: MoveFocus(0, 1); return;
		case Qt::Key_Left:
			if (MoveFocus(-1, 0))
			{
				return;
			}
			break;
		case Qt::Key_Right:
			if (MoveFocus(1, 0))
			{
				return;
			}
			break;
		case Qt::Key_PageUp: ChangeTab(-1); return;
		case Qt::Key_PageDown: ChangeTab(1); return;
		default: break;
	}
	event->ignore();
}

QSet<QString> ControlsPanelItem::MarkedParts() const
{
	const auto* focus = Focused();
	if (focus == nullptr)
	{
		return {};
	}
	for (const auto* bindings: {&m_pad_bindings, &m_key_bindings})
	{
		for (const auto& binding: *bindings)
		{
			if (binding.chip == focus)
			{
				return {binding.control};
			}
		}
	}
	for (int side = 0; side < 2; side++)
	{
		const auto& card = m_sticks[side];
		if (focus == card.deadzone || focus == card.invert_x || focus == card.invert_y)
		{
			return {side == 0 ? QStringLiteral("l3") : QStringLiteral("r3")};
		}
	}
	if (focus == m_trigger)
	{
		return {QStringLiteral("l2"), QStringLiteral("r2")};
	}
	if (focus == m_mouse_look || focus == m_sensitivity || focus == m_mouse_invert)
	{
		return {QStringLiteral("r3")};
	}
	return {};
}

// The parts held on the controller, through the profile's bindings, and where its sticks point.
void ControlsPanelItem::Poll()
{
	const auto*   gamepad = LauncherBackend::Instance()->Gamepad();
	const auto    held    = gamepad->HeldButtons();
	QSet<QString> lit;
	for (const auto& binding: m_pad_bindings)
	{
		const auto buttons = m_values.value(QStringLiteral("bind.") + binding.control).toMap().value("buttons").toStringList();
		if (std::any_of(buttons.begin(), buttons.end(), [&](const QString& button) { return held.contains(button); }))
		{
			lit << binding.control;
		}
	}
	const qreal press = m_values.value("trigger_deadzone").toDouble();
	for (const bool right: {false, true})
	{
		if (gamepad->TriggerPosition(right) > std::max<qreal>(press, 0.05))
		{
			lit << (right ? QStringLiteral("r2") : QStringLiteral("l2"));
		}
	}
	const auto left_stick  = Rounded(gamepad->StickPosition(false));
	const auto right_stick = Rounded(gamepad->StickPosition(true));
	if (lit != m_held || left_stick != m_left_stick || right_stick != m_right_stick)
	{
		m_held        = lit;
		m_left_stick  = left_stick;
		m_right_stick = right_stick;
		m_art_generation++;
		update();
	}
}

void ControlsPanelItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change != ItemVisibleHasChanged)
	{
		return;
	}
	if (value.boolValue)
	{
		m_poll.start();
	} else
	{
		m_poll.stop();
	}
}

void ControlsPanelItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		polish();
	}
}

void ControlsPanelItem::AddLead(QQuickItem* owner, const QPointF& from, bool from_left, const QString& part)
{
	const qreal   u      = Theme::Instance()->U();
	const QPointF anchor = m_drawing.topLeft() + ControllerAnchor(part) * m_drawing.width();
	const QPointF elbow(from.x() + (from_left ? LEAD_GAP : -LEAD_GAP) * u, from.y());
	m_leads << Lead {QPolygonF {from, elbow, anchor}, part, owner};
}

qreal ControlsPanelItem::AddCardTitle(const QRectF& card, const QString& title)
{
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	m_boxes << PageLayout::Box {card, theme->Glass(), theme->GlassBorder(), theme->RadiusSmall()};
	const QRectF rect(card.left() + CARD_PADDING * u, card.top() + CARD_PADDING * u, card.width() - 2 * CARD_PADDING * u, TITLE_HEIGHT * u);
	m_texts << TextAt(rect, title, SceneFont(theme->FontBody(), QFont::DemiBold), theme->TextPrimary());
	return rect.bottom() + CARD_SPACING * u;
}

qreal ControlsPanelItem::PlaceLabelled(QQuickItem* item, const QString& label, qreal x, qreal y, qreal width, qreal height)
{
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	m_texts << TextAt(QRectF(x, y, width, LABEL_HEIGHT * u), label, SceneFont(theme->FontSmall()), theme->TextMuted());
	item->setPosition(QPointF(x, y + LABEL_HEIGHT * u));
	item->setSize(QSizeF(width, height));
	return y + LABEL_HEIGHT * u + height + CARD_SPACING * u;
}

void ControlsPanelItem::AddNote(const QRectF& rect, const QString& text)
{
	const auto* theme = Theme::Instance();
	const auto  font  = SceneFont(theme->FontSmall());
	// Narrower than the drawing so the leads to its lower parts pass beside it; left out when it does not fit whole.
	const qreal width  = rect.width() * 0.8;
	const qreal height = TextBlockHeight(text, font, width);
	if (height <= rect.height())
	{
		m_texts << TextAt(QRectF(rect.center().x() - width / 2, rect.top(), width, height), text, font, theme->TextMuted(), Qt::AlignHCenter,
		                  QTextOption::WordWrap);
	}
}

// The bindings down both sides of the drawing; on the keyboard tab the sticks' directions sit under it as crosses.
void ControlsPanelItem::LayoutBindings(const QRectF& area, bool gamepad)
{
	const auto* theme    = Theme::Instance();
	const qreal u        = theme->U();
	const auto& bindings = gamepad ? m_pad_bindings : m_key_bindings;
	auto        chip_for = [&](const QString& control) -> BindingItem*
	{
		const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& b) { return b.control == control; });
		return found == bindings.end() ? nullptr : found->chip;
	};
	const auto  small  = SceneFont(theme->FontSmall(), QFont::DemiBold);
	const qreal column = m_drawing.left() - 2 * LEAD_GAP * u;
	qreal       label  = 0;
	for (const auto* names: {LEFT_COLUMN, RIGHT_COLUMN})
	{
		for (int i = 0; i < 8; i++)
		{
			label = std::max(label, TextWidth(ControlLabel(QString::fromLatin1(names[i])), small));
		}
	}
	label               = std::min(label + LABEL_GAP * u, column * 0.4);
	const qreal spacing = std::clamp((area.height() - 8 * ROW * u) / 7, 2 * u, ROW_SPACING * u);
	for (int side = 0; side < 2; side++)
	{
		const auto* names = side == 0 ? LEFT_COLUMN : RIGHT_COLUMN;
		for (int i = 0; i < 8; i++)
		{
			const QString control = QString::fromLatin1(names[i]);
			auto*         chip    = chip_for(control);
			const qreal   y       = area.top() + i * (ROW * u + spacing);
			const qreal   chip_w  = std::max<qreal>(0, column - label);
			const qreal   chip_x  = side == 0 ? label : width() - column;
			const qreal   label_x = side == 0 ? 0 : width() - label + LABEL_GAP * u;
			m_texts << TextAt(QRectF(label_x, y, label - LABEL_GAP * u, ROW * u), ControlLabel(control), small, theme->TextSecondary());
			chip->setPosition(QPointF(chip_x, y));
			chip->setSize(QSizeF(chip_w, ROW * u));
			AddLead(chip, QPointF(side == 0 ? chip_x + chip_w : chip_x, y + ROW * u / 2), side == 0, control);
		}
	}

	qreal below = m_drawing.bottom() + NOTE_GAP * u;
	if (!gamepad)
	{
		// Each cross fills at most its half of the drawing, a gap apart; its centre cell holds a small stick.
		const qreal g    = CROSS_GAP * u;
		const qreal cell = std::min(CROSS_CHIP * u, (m_drawing.width() / 2 - 3 * g) / 3);
		const qreal h    = CROSS_HEIGHT * u;
		for (int side = 0; side < 2; side++)
		{
			const QPointF centre(m_drawing.left() + m_drawing.width() * (side == 0 ? 0.25 : 0.75), below + h * 1.5 + g);
			const QPointF offsets[] = {{0, -(h + g)}, {-(cell + g), 0}, {cell + g, 0}, {0, h + g}};
			for (int i = 0; i < 4; i++)
			{
				auto* chip = chip_for(QString::fromLatin1(STICK_CROSSES[side][i]));
				chip->setPosition(centre + offsets[i] - QPointF(cell / 2, h / 2));
				chip->setSize(QSizeF(cell, h));
			}
			const qreal stick = h * 0.8;
			m_boxes << PageLayout::Box {QRectF(centre - QPointF(stick / 2, stick / 2), QSizeF(stick, stick)), theme->Glass(),
			                            theme->TextMuted(), stick / 2};
			const QString part   = side == 0 ? QStringLiteral("l3") : QStringLiteral("r3");
			const QPointF anchor = m_drawing.topLeft() + ControllerAnchor(part) * m_drawing.width();
			// The cross's lead leaves from the top of its upper chip, clear of the chips.
			m_leads << Lead {QPolygonF {centre + offsets[0] - QPointF(0, h / 2), anchor}, part, chip_for(QString::fromLatin1(STICK_CROSSES[side][0]))};
		}
		below += 3 * h + 2 * g + NOTE_GAP * u;
	}
	const auto note = Editable() ? tr("Choose a box, then press the key or button to use. Delete or □ clears it.")
	                             : tr("Default is the emulator's own layout and can't be changed; duplicate it to make a profile of "
	                                  "your own.");
	AddNote(QRectF(m_drawing.left(), below, m_drawing.width(), area.bottom() - below), note);
}

void ControlsPanelItem::LayoutSticks(const QRectF& area)
{
	const auto* theme  = Theme::Instance();
	const qreal u      = theme->U();
	const qreal column = m_drawing.left() - 2 * LEAD_GAP * u;
	const qreal inner  = column - 2 * CARD_PADDING * u;
	for (int side = 0; side < 2; side++)
	{
		const auto&  card = m_sticks[side];
		const qreal  left = side == 0 ? 0 : width() - column;
		const qreal  x    = left + CARD_PADDING * u;
		const QRectF box(left, area.top(), column,
		                 (2 * CARD_PADDING + TITLE_HEIGHT + PREVIEW) * u + 4 * CARD_SPACING * u + 3 * (LABEL_HEIGHT + CARD_CONTROL) * u);
		qreal        y = AddCardTitle(box, side == 0 ? tr("Left stick") : tr("Right stick"));
		card.preview->setPosition(QPointF(x, y));
		card.preview->setSize(QSizeF(inner, PREVIEW * u));
		AddLead(card.preview, QPointF(side == 0 ? box.right() : box.left(), y + PREVIEW * u / 2), side == 0,
		        side == 0 ? QStringLiteral("l3") : QStringLiteral("r3"));
		y += (PREVIEW + CARD_SPACING) * u;
		y = PlaceLabelled(card.deadzone, tr("Dead zone"), x, y, inner, CARD_CONTROL * u);
		y = PlaceLabelled(card.invert_x, tr("Invert across"), x, y, inner, CARD_CONTROL * u);
		PlaceLabelled(card.invert_y, tr("Invert up and down"), x, y, inner, CARD_CONTROL * u);
	}

	const QRectF box(m_drawing.left(), m_drawing.bottom() + NOTE_GAP * u, m_drawing.width(),
	                 (2 * CARD_PADDING + TITLE_HEIGHT + LABEL_HEIGHT + CARD_CONTROL) * u + CARD_SPACING * u);
	const qreal  x = box.left() + CARD_PADDING * u;
	PlaceLabelled(m_trigger, tr("Press point"), x, AddCardTitle(box, tr("L2 and R2")), box.width() - 2 * CARD_PADDING * u,
	              CARD_CONTROL * u);
	const qreal below = box.bottom() + NOTE_GAP * u;
	AddNote(QRectF(box.left(), below, box.width(), area.bottom() - below),
	        tr("How far L2 and R2 must be pulled to count as pressed. Movements of a stick smaller than its dead zone are ignored, so a "
	           "worn stick does not drift."));
}

void ControlsPanelItem::LayoutMouse(const QRectF& area)
{
	const auto*  theme  = Theme::Instance();
	const qreal  u      = theme->U();
	const qreal  column = m_drawing.left() - 2 * LEAD_GAP * u;
	const qreal  inner  = column - 2 * CARD_PADDING * u;
	const bool   look   = m_values.value("mouse_look").toBool();
	const auto   help   = tr("In the game, F3 captures the mouse; while it is captured, moving it moves the right stick.");
	const auto   font   = SceneFont(theme->FontSmall());
	const qreal  note   = TextBlockHeight(help, font, inner);
	const int    rows   = look ? 3 : 1;
	const QRectF box(width() - column, area.top(), column,
	                 (2 * CARD_PADDING + TITLE_HEIGHT) * u + rows * ((LABEL_HEIGHT + CARD_CONTROL + CARD_SPACING) * u) + CARD_SPACING * u +
	                     note);
	const qreal  x = box.left() + CARD_PADDING * u;
	qreal        y = AddCardTitle(box, tr("Mouse look"));
	AddLead(m_mouse_look, QPointF(box.left(), y + (LABEL_HEIGHT + CARD_CONTROL / 2) * u), false, QStringLiteral("r3"));
	y = PlaceLabelled(m_mouse_look, tr("Look with the mouse"), x, y, inner, CARD_CONTROL * u);
	if (look)
	{
		y = PlaceLabelled(m_sensitivity, tr("Sensitivity"), x, y, inner, CARD_CONTROL * u);
		y = PlaceLabelled(m_mouse_invert, tr("Invert up and down"), x, y, inner, CARD_CONTROL * u);
	}
	m_texts << TextAt(QRectF(x, y, inner, note), help, font, theme->TextMuted(), Qt::AlignLeft, QTextOption::WordWrap);
}

void ControlsPanelItem::updatePolish()
{
	const auto* theme    = Theme::Instance();
	const qreal u        = theme->U();
	const qreal w        = width();
	const qreal gap      = BAR_GAP * u;
	const bool  editable = Editable();
	m_texts.clear();
	m_boxes.clear();
	m_leads.clear();

	// The profile, its name and its controller side by side, each under its label.
	const auto  small = SceneFont(theme->FontSmall());
	const qreal slot  = std::max<qreal>(0, (w - 2 * gap) / 3);
	const QString labels[] = {tr("Profile"), tr("Name"), tr("Controller")};
	QQuickItem*   bar[]    = {m_profile, m_name, m_device};
	for (int i = 0; i < 3; i++)
	{
		if (i > 0 && !editable)
		{
			continue;
		}
		const qreal x = i * (slot + gap);
		m_texts << TextAt(QRectF(x, 0, slot, LABEL_HEIGHT * u), labels[i], small, theme->TextMuted());
		bar[i]->setPosition(QPointF(x, LABEL_HEIGHT * u));
		bar[i]->setSize(QSizeF(slot, CONTROL * u));
	}
	qreal y = (LABEL_HEIGHT + CONTROL + BAR_GAP) * u;
	m_tabs->setPosition(QPointF(-m_tabs->TabPadding(), y));
	m_tabs->setSize(QSizeF(w + m_tabs->TabPadding(), TABS_HEIGHT * u));
	y += (TABS_HEIGHT + TABS_GAP) * u;

	// The drawing keeps its place on every tab; the columns beside it take what is left, up to COLUMN.
	const qreal column  = std::min(COLUMN * u, std::max<qreal>(0, (w - MIN_DRAWING * u) / 2 - 2 * LEAD_GAP * u));
	const qreal drawing = std::clamp(w - 2 * (column + 2 * LEAD_GAP * u), std::min(MIN_DRAWING * u, w), MAX_DRAWING * u);
	m_art               = QRectF(0, y, w, std::max<qreal>(0, height() - y));
	// Centred on the column of bindings, so the leads from its lower rows do not climb steeply.
	const qreal rows    = std::min(m_art.height(), 8 * ROW * u + 7 * ROW_SPACING * u);
	const qreal top     = y + std::max<qreal>(0, (rows - drawing * CONTROLLER_HEIGHT) / 2);
	m_drawing           = QRectF((w - drawing) / 2, top, drawing, drawing * CONTROLLER_HEIGHT);
	switch (m_tabs->CurrentIndex())
	{
		case PadTab: LayoutBindings(m_art, true); break;
		case KeysTab: LayoutBindings(m_art, false); break;
		case SticksTab: LayoutSticks(m_art); break;
		case MouseTab: LayoutMouse(m_art); break;
		default: break;
	}
	m_generation++;
	m_art_generation++;
	update();
}

QSGNode* ControlsPanelItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto*       node  = old_node != nullptr ? static_cast<PanelNode*>(old_node) : new PanelNode;
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const qreal dpr   = window()->effectiveDevicePixelRatio();
	if (node->generation != m_generation)
	{
		node->generation = m_generation;
		ClearChildren(node->boxes);
		ClearChildren(node->texts);
		for (const auto& box: m_boxes)
		{
			auto*                 child = new RoundedBoxNode;
			RoundedBoxNode::Style style;
			style.fill         = box.fill;
			style.border       = box.border;
			style.border_width = 1;
			style.radius       = box.radius;
			style.dpr          = dpr;
			child->Update(window(), box.rect, style);
			node->boxes->appendChildNode(child);
		}
		for (const auto& text: m_texts)
		{
			auto* text_node = window()->createTextNode();
			if (text.wrap == QTextOption::NoWrap)
			{
				SetTextLine(text_node, text.text, text.font, text.color, text.rect, text.alignment);
			} else
			{
				SetTextBlock(text_node, text.text, text.font, text.color, text.rect, text.alignment, text.wrap);
			}
			node->texts->appendChildNode(text_node);
		}
	}
	if (node->art_generation == m_art_generation)
	{
		return node;
	}
	node->art_generation = m_art_generation;
	ClearChildren(node->art);
	const QSize size(static_cast<int>(std::ceil(m_art.width() * dpr)), static_cast<int>(std::ceil(m_art.height() * dpr)));
	if (size.isEmpty())
	{
		return node;
	}
	QImage image(size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.scale(dpr, dpr);
	painter.translate(-m_art.topLeft());

	ControllerLook look;
	// The pad is solid plastic: the strong glass tint laid over the surface colour.
	const QColor glass = theme->GlassStrong();
	const QColor under = theme->Surface();
	const auto   tint  = glass.alphaF();
	look.body          = QColor::fromRgbF(under.redF() + (glass.redF() - under.redF()) * tint, under.greenF() + (glass.greenF() - under.greenF()) * tint,
	                                      under.blueF() + (glass.blueF() - under.blueF()) * tint);
	look.line        = theme->TextMuted();
	look.accent      = theme->Accent();
	look.lit         = m_held;
	look.marked      = MarkedParts();
	look.left_stick  = m_left_stick;
	look.right_stick = m_right_stick;
	look.light_bar   = 0.7;
	look.stroke      = STROKE * u;
	PaintController(&painter, m_drawing, look);

	for (const auto& lead: m_leads)
	{
		const bool focused = lead.owner->hasActiveFocus() || look.marked.contains(lead.part) || m_held.contains(lead.part);
		QPen       pen(focused ? theme->Accent() : WithAlpha(theme->TextMuted(), 0.55), (focused ? 2 : 1.2) * u);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		painter.setPen(pen);
		painter.setBrush(Qt::NoBrush);
		painter.drawPolyline(lead.points);
		painter.setPen(Qt::NoPen);
		painter.setBrush(focused ? theme->Accent() : theme->TextMuted());
		const qreal radius = (focused ? ANCHOR_RADIUS + 1.5 : ANCHOR_RADIUS) * u;
		painter.drawEllipse(lead.points.last(), radius, radius);
	}
	painter.end();
	node->art->appendChildNode(CreateTextureNode(window(), window()->createTextureFromImage(image), m_art));
	return node;
}
