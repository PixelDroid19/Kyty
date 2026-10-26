#include "FieldRowItem.h"

#include "Accessibility.h"
#include "EditorFields.h"
#include "FieldControls.h"
#include "FocusButtonItem.h"
#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "TextFieldItem.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal ROW_INSET_X     = 12;
constexpr qreal ROW_INSET_Y     = 4;
constexpr qreal LABEL_WIDTH     = 230;
constexpr qreal LABEL_GAP       = 16;
constexpr qreal CONTROL_HEIGHT  = 40;
constexpr qreal HEADING_GAP     = 6;
constexpr qreal BUTTON_GAP      = 8;
constexpr qreal TOGGLE_WIDTH    = 200;
constexpr qreal MODULE_HEIGHT   = 36;
constexpr qreal MODULE_SPACING  = 6;
constexpr qreal PARAM_SPACING   = 4;
constexpr qreal PARAM_KEY_WIDTH = 200;
constexpr qreal PARAM_GAP       = 12;

constexpr int MAX_PARAMS = 200;

namespace {

class RowNode: public GroupNode
{
public:
	RowNode()
	{
		backing = new RoundedBoxNode;
		appendChildNode(backing);
		texts = new GroupNode;
		appendChildNode(texts);
	}

	RoundedBoxNode* backing    = nullptr;
	GroupNode*      texts      = nullptr;
	int             generation = -1;
};

QString Translated(const char* text)
{
	return QCoreApplication::translate("EditorPageItem", text);
}

// What a choice shows for each stored value; values not listed show as they are.
struct ChoiceText
{
	const char* value;
	const char* text;
	// Language names are written in their own language.
	bool translated = true;
};

const ChoiceText CHOICE_TEXTS[] = {
    {"System", QT_TRANSLATE_NOOP("FieldRowItem", "Same as the system")},
    {"Dark", QT_TRANSLATE_NOOP("FieldRowItem", "Dark")},
    {"Light", QT_TRANSLATE_NOOP("FieldRowItem", "Light")},
    {"en", "English", false},
    {"es", "Español", false},
    {"ru", "Русский", false},
    {"Direct", QT_TRANSLATE_NOOP("FieldRowItem", "Direct, output to a log file")},
    {"Custom", QT_TRANSLATE_NOOP("FieldRowItem", "Custom")},
    {"Native", QT_TRANSLATE_NOOP("FieldRowItem", "Native")},
    {"Quality", QT_TRANSLATE_NOOP("FieldRowItem", "Quality")},
    {"Balanced", QT_TRANSLATE_NOOP("FieldRowItem", "Balanced")},
    {"Performance", QT_TRANSLATE_NOOP("FieldRowItem", "Performance")},
    {"Fixed", QT_TRANSLATE_NOOP("FieldRowItem", "Fixed size")},
    {"Display", QT_TRANSLATE_NOOP("FieldRowItem", "Display only")},
    {"RenderTargets", QT_TRANSLATE_NOOP("FieldRowItem", "Render targets")},
    {"Linear", QT_TRANSLATE_NOOP("FieldRowItem", "Linear")},
    {"Nearest", QT_TRANSLATE_NOOP("FieldRowItem", "Nearest")},
    {"Fsr", "FSR", false},
    {"Guest", QT_TRANSLATE_NOOP("FieldRowItem", "As the game asks")},
    {"X2", "2×", false},
    {"X4", "4×", false},
    {"X8", "8×", false},
    {"X16", "16×", false},
    {"None", QT_TRANSLATE_NOOP("FieldRowItem", "None")},
    {"Size", QT_TRANSLATE_NOOP("FieldRowItem", "Smaller code")},
    {"Silent", QT_TRANSLATE_NOOP("FieldRowItem", "Off")},
    {"Console", QT_TRANSLATE_NOOP("FieldRowItem", "Console")},
    {"File", QT_TRANSLATE_NOOP("FieldRowItem", "File")},
    {"Directory", QT_TRANSLATE_NOOP("FieldRowItem", "Folder, one file per run")},
    {"Network", QT_TRANSLATE_NOOP("FieldRowItem", "Network")},
    {"FileAndNetwork", QT_TRANSLATE_NOOP("FieldRowItem", "File and network")},
    {"Debug", QT_TRANSLATE_NOOP("FieldRowItem", "Debug")},
    {"Info", QT_TRANSLATE_NOOP("FieldRowItem", "Info")},
    {"Warn", QT_TRANSLATE_NOOP("FieldRowItem", "Warnings")},
    {"Error", QT_TRANSLATE_NOOP("FieldRowItem", "Errors")},
    {"Blue", QT_TRANSLATE_NOOP("FieldRowItem", "Blue")},
    {"Violet", QT_TRANSLATE_NOOP("FieldRowItem", "Violet")},
    {"Green", QT_TRANSLATE_NOOP("FieldRowItem", "Green")},
    {"Orange", QT_TRANSLATE_NOOP("FieldRowItem", "Orange")},
    {"Pink", QT_TRANSLATE_NOOP("FieldRowItem", "Pink")},
    {"Compact", QT_TRANSLATE_NOOP("FieldRowItem", "Compact")},
    {"Normal", QT_TRANSLATE_NOOP("FieldRowItem", "Normal")},
    {"Large", QT_TRANSLATE_NOOP("FieldRowItem", "Large")},
    {"ExtraLarge", QT_TRANSLATE_NOOP("FieldRowItem", "Extra large")},
    {"Full", QT_TRANSLATE_NOOP("FieldRowItem", "Full")},
    {"Reduced", QT_TRANSLATE_NOOP("FieldRowItem", "Reduced, nothing loops")},
    {"Off", QT_TRANSLATE_NOOP("FieldRowItem", "No animation")},
    {"Artwork", QT_TRANSLATE_NOOP("FieldRowItem", "Game artwork")},
    {"Dimmed", QT_TRANSLATE_NOOP("FieldRowItem", "Game artwork, dimmed")},
    {"Plain", QT_TRANSLATE_NOOP("FieldRowItem", "Plain")},
    {"Small", QT_TRANSLATE_NOOP("FieldRowItem", "Small")},
    {"Medium", QT_TRANSLATE_NOOP("FieldRowItem", "Medium")},
    {"Minimize", QT_TRANSLATE_NOOP("FieldRowItem", "Minimize the launcher")},
    {"KeepOpen", QT_TRANSLATE_NOOP("FieldRowItem", "Keep the launcher open")},
    {"Manual", QT_TRANSLATE_NOOP("FieldRowItem", "My own order")},
    {"ByName", QT_TRANSLATE_NOOP("FieldRowItem", "Name")},
    {"LastPlayed", QT_TRANSLATE_NOOP("FieldRowItem", "Last played")},
    {"PlayTime", QT_TRANSLATE_NOOP("FieldRowItem", "Time played")},
    {"Windowed", QT_TRANSLATE_NOOP("FieldRowItem", "A window")},
    {"Maximized", QT_TRANSLATE_NOOP("FieldRowItem", "Maximized")},
    {"FullScreen", QT_TRANSLATE_NOOP("FieldRowItem", "Full screen")},
    {"LastGame", QT_TRANSLATE_NOOP("FieldRowItem", "The last game played")},
    {"Sidebar", QT_TRANSLATE_NOOP("FieldRowItem", "Sidebar")},
    {"Console", QT_TRANSLATE_NOOP("FieldRowItem", "Console")},
    {"Quiet", QT_TRANSLATE_NOOP("FieldRowItem", "Quiet")},
    {"Loud", QT_TRANSLATE_NOOP("FieldRowItem", "Loud")},
    {"Library", QT_TRANSLATE_NOOP("FieldRowItem", "The library")},
};

const ChoiceText MOUSE_TEXTS[] = {
    {"Mouse Left", QT_TRANSLATE_NOOP("FieldRowItem", "Left click")},
    {"Mouse Middle", QT_TRANSLATE_NOOP("FieldRowItem", "Middle click")},
    {"Mouse Right", QT_TRANSLATE_NOOP("FieldRowItem", "Right click")},
    {"Mouse X1", QT_TRANSLATE_NOOP("FieldRowItem", "Mouse back button")},
    {"Mouse X2", QT_TRANSLATE_NOOP("FieldRowItem", "Mouse forward button")},
};

QFont SmallFont()
{
	return SceneFont(Theme::Instance()->FontSmall());
}

PageLayout::Text Line(const QRectF& rect, const QString& text, const QFont& font, const QColor& color)
{
	PageLayout::Text line;
	line.rect  = rect;
	line.text  = text;
	line.font  = font;
	line.color = color;
	line.wrap  = QTextOption::NoWrap;
	return line;
}

} // namespace

// The backend names the values of its own lists first, so an input profile called Normal is not shown as a size.
QString ChoiceDisplay(const char* list, const QString& value)
{
	if (const auto label = LauncherBackend::Instance()->ChoiceLabel(QString::fromLatin1(list), value); !label.isEmpty())
	{
		return label;
	}
	for (const auto& choice: CHOICE_TEXTS)
	{
		if (value == QLatin1String(choice.value))
		{
			return choice.translated ? QCoreApplication::translate("FieldRowItem", choice.text) : QString::fromUtf8(choice.text);
		}
	}
	return value;
}

QString KeyDisplay(const QString& name)
{
	for (const auto& mouse: MOUSE_TEXTS)
	{
		if (name == QLatin1String(mouse.value))
		{
			return QCoreApplication::translate("FieldRowItem", mouse.text);
		}
	}
	return name;
}

FieldRowItem::FieldRowItem(const EditorField& field, QQuickItem* parent): QQuickItem(parent), m_field(field)
{
	setFlag(ItemHasContents);
	setAcceptedMouseButtons(Qt::LeftButton);
	setAcceptHoverEvents(true);
	const QString key = field.key;
	switch (field.type)
	{
		case FieldType::Text:
		case FieldType::Path:
		case FieldType::Number:
			m_input = new TextFieldItem(this);
			connect(m_input, &TextFieldItem::PadRequested, this, [this, key]() { emit PadRequested(key, Translated(m_field.label)); });
			break;
		case FieldType::Toggle:
			m_toggle = new ToggleItem(this);
			connect(m_toggle, &FieldControlItem::Activated, this, [this, key]() { emit Edited(key, !m_value.toBool()); });
			break;
		case FieldType::Choice:
			m_choice = new ChoiceItem(this);
			connect(m_choice, &FieldControlItem::Activated, this, &FieldRowItem::Step);
			connect(m_choice, &FieldControlItem::ListRequested, this, [this, key]() { emit ListRequested(key); });
			break;
		case FieldType::Modules:
			m_button = new FocusButtonItem(this);
			m_button->SetText(tr("Rescan files"));
			connect(m_button, &FocusButtonItem::Activated, this,
			        [this, key]() { emit Edited(key, LauncherBackend::Instance()->ScanElfs(m_basedir, m_value.toList())); });
			break;
		case FieldType::Params: break;
	}

	if (field.type == FieldType::Text || field.type == FieldType::Path)
	{
		connect(m_input, &TextFieldItem::Edited, this, [this, key](const QString& text) { emit Edited(key, text); });
	}
	if (field.type == FieldType::Path)
	{
		m_button = new FocusButtonItem(this);
		m_button->SetText(tr("Browse…"));
		connect(m_button, &FocusButtonItem::Activated, this, [this, key]() { emit BrowseRequested(key, m_field.folder); });
	}
	if (field.type == FieldType::Number)
	{
		m_input->SetDigitsOnly(NUMBER_DIGITS);
		// An emptied field keeps the last number until a new one is typed, and shows it again once left.
		connect(m_input, &TextFieldItem::Edited, this,
		        [this, key](const QString& text)
		        {
			        if (!text.isEmpty())
			        {
				        emit Edited(key, text.toInt());
			        }
		        });
		connect(m_input, &QQuickItem::activeFocusChanged, this, &FieldRowItem::ShowValue);
		m_minus = new FocusButtonItem(this);
		m_minus->SetText(QStringLiteral("−10"));
		connect(m_minus, &FocusButtonItem::Activated, this, [this]() { Step(-10); });
		m_plus = new FocusButtonItem(this);
		m_plus->SetText(QStringLiteral("+10"));
		connect(m_plus, &FocusButtonItem::Activated, this, [this]() { Step(10); });
	}
	for (auto* item: FocusItems())
	{
		connect(item, &QQuickItem::activeFocusChanged, this, &QQuickItem::update);
	}
}

QStringList FieldRowItem::ChoiceTexts() const
{
	QStringList texts;
	for (const auto& choice: m_choices)
	{
		texts << ChoiceDisplay(m_field.choices, choice);
	}
	return texts;
}

QQuickItem* FieldRowItem::ChoiceBox() const
{
	return m_choice;
}

bool FieldRowItem::HasFocusWithin() const
{
	const auto items = FocusItems();
	return std::any_of(items.begin(), items.end(), [](const QQuickItem* item) { return item->hasActiveFocus(); });
}

QVector<QQuickItem*> FieldRowItem::FocusItems() const
{
	QVector<QQuickItem*> items;
	for (QQuickItem* item: {static_cast<QQuickItem*>(m_input), static_cast<QQuickItem*>(m_toggle), static_cast<QQuickItem*>(m_choice),
	                        static_cast<QQuickItem*>(m_minus), static_cast<QQuickItem*>(m_plus), static_cast<QQuickItem*>(m_button)})
	{
		if (item != nullptr)
		{
			items << item;
		}
	}
	for (auto* module: m_modules)
	{
		items << module;
	}
	return items;
}

void FieldRowItem::SetValues(const QVariantMap& values, bool shown)
{
	m_shown   = shown;
	m_value   = values.value(m_field.key);
	m_basedir = values.value("basedir").toString();
	if (m_field.type == FieldType::Choice)
	{
		// Some lists change while the form is open, such as the connected controllers.
		m_choices = LauncherBackend::Instance()->Choices().value(m_field.choices).toStringList();
	}
	setVisible(shown);
	ShowValue();
}

void FieldRowItem::ShowValue()
{
	const char*   help_text = FieldHelp(m_field.key);
	const QString help      = help_text != nullptr ? Translated(help_text) : QString();
	switch (m_field.type)
	{
		case FieldType::Text:
		case FieldType::Path:
			SetAccessible(m_input, QAccessible::EditableText, Translated(m_field.label), help);
			m_input->SetText(m_value.toString());
			break;
		case FieldType::Number:
			SetAccessible(m_input, QAccessible::SpinBox, Translated(m_field.label), help);
			if (!m_input->hasActiveFocus())
			{
				m_input->SetText(m_value.toString());
			}
			break;
		case FieldType::Toggle:
			SetAccessible(m_toggle, QAccessible::CheckBox, Translated(m_field.label), help);
			SetAccessibleChecked(m_toggle, m_value.toBool());
			m_toggle->SetOn(m_value.toBool());
			break;
		case FieldType::Choice:
		{
			const auto shown = ChoiceDisplay(m_field.choices, m_value.toString());
			SetAccessible(m_choice, QAccessible::ComboBox, Translated(m_field.label), help.isEmpty() ? shown : shown + ". " + help);
			m_choice->SetValue(shown);
			break;
		}
		case FieldType::Modules: ShowModules(m_value.toList()); break;
		case FieldType::Params:
			// Read again only when the file changes, since every edit in the form passes through here.
			if (m_value.toString() != m_params_file)
			{
				m_params_file = m_value.toString();
				m_params =
				    m_params_file.isEmpty() ? QVariantList() : LauncherBackend::Instance()->ParamEntries(m_params_file).mid(0, MAX_PARAMS);
			}
			break;
	}
}

void FieldRowItem::ShowModules(const QVariantList& modules)
{
	while (m_modules.size() > modules.size())
	{
		// The row may be the one handling the key that changed the list, so it goes once the event is over.
		auto* module = m_modules.takeLast();
		module->setParentItem(nullptr);
		module->deleteLater();
	}
	while (m_modules.size() < modules.size())
	{
		const int index  = static_cast<int>(m_modules.size());
		auto*     module = new ModuleRowItem(this);
		connect(module, &FieldControlItem::Activated, this, [this, index]() { ToggleModule(index); });
		connect(module, &QQuickItem::activeFocusChanged, this, &QQuickItem::update);
		m_modules << module;
	}
	for (int i = 0; i < modules.size(); i++)
	{
		const auto module = modules.at(i).toMap();
		m_modules.at(i)->SetModule(module.value("path").toString(), module.value("present").toBool(), module.value("selected").toBool());
	}
}

void FieldRowItem::Step(int delta)
{
	if (m_field.type == FieldType::Number)
	{
		emit Edited(m_field.key, std::max(0, m_value.toInt() + delta));
		return;
	}
	if (m_choices.isEmpty())
	{
		return;
	}
	const auto count = m_choices.size();
	const auto index = std::max<qsizetype>(0, m_choices.indexOf(m_value.toString()));
	emit       Edited(m_field.key, m_choices.at((index + delta + count) % count));
}

void FieldRowItem::ToggleModule(int index)
{
	auto modules = m_value.toList();
	if (index >= modules.size() || !modules.at(index).toMap().value("present").toBool())
	{
		return;
	}
	auto module        = modules.at(index).toMap();
	module["selected"] = !module.value("selected").toBool();
	modules[index]     = module;
	emit Edited(m_field.key, modules);
}

qreal FieldRowItem::Layout(qreal width)
{
	if (!m_shown)
	{
		setSize(QSizeF(width, 0));
		return 0;
	}
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	m_texts.clear();

	const qreal inset_x = ROW_INSET_X * u;
	const qreal inset_y = ROW_INSET_Y * u;
	qreal       y       = 0;
	if (m_field.heading != nullptr)
	{
		auto font = SceneFont(theme->FontSmall(), QFont::DemiBold);
		font.setCapitalization(QFont::AllUppercase);
		const qreal height = std::ceil(QFontMetricsF(font).height());
		m_texts << Line(QRectF(inset_x, 0, width - inset_x, height), Translated(m_field.heading), font, theme->TextMuted());
		y = height + HEADING_GAP * u;
	}
	const qreal top        = y + inset_y;
	const auto  label_font = SceneFont(theme->FontBody());
	m_texts << Line(QRectF(inset_x, top, LABEL_WIDTH * u, CONTROL_HEIGHT * u), Translated(m_field.label), label_font,
	                theme->TextSecondary());
	// The help starts just under the label's line, which sits centred on the control.
	qreal label_bottom = top + CONTROL_HEIGHT * u;
	if (const char* help = FieldHelp(m_field.key); help != nullptr)
	{
		auto line            = Line(QRectF(), Translated(help), SmallFont(), theme->TextMuted());
		line.wrap            = QTextOption::WordWrap;
		const qreal help_top = top + std::round((CONTROL_HEIGHT * u + LineHeight(label_font)) / 2);
		line.rect            = QRectF(inset_x, help_top, LABEL_WIDTH * u, TextBlockHeight(line.text, line.font, LABEL_WIDTH * u));
		label_bottom         = line.rect.bottom();
		m_texts << line;
	}

	const qreal control_x = inset_x + (LABEL_WIDTH + LABEL_GAP) * u;
	const qreal control   = PlaceControl(QRectF(control_x, top, std::max<qreal>(0, width - inset_x - control_x), CONTROL_HEIGHT * u));
	const qreal height    = std::max(top + std::max(CONTROL_HEIGHT * u, control), label_bottom) + inset_y;
	m_body                = QRectF(0, y, width, height - y);
	m_generation++;
	setSize(QSizeF(width, height));
	update();
	return height;
}

qreal FieldRowItem::PlaceControl(const QRectF& rect)
{
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const qreal gap   = BUTTON_GAP * u;

	// Buttons go to the right end of the control's space, at their own width; the text field takes the rest.
	qreal right = rect.right();
	for (auto* button: {m_plus, m_minus, m_button})
	{
		if (button == nullptr)
		{
			continue;
		}
		right -= button->implicitWidth();
		button->setPosition(QPointF(right, rect.top()));
		button->setSize(QSizeF(button->implicitWidth(), button->implicitHeight()));
		right -= gap;
	}
	if (m_input != nullptr)
	{
		m_input->setPosition(rect.topLeft());
		m_input->setSize(
		    QSizeF(std::max<qreal>(0, (m_button != nullptr || m_plus != nullptr ? right : rect.right()) - rect.left()), rect.height()));
	}
	if (m_toggle != nullptr)
	{
		m_toggle->setPosition(rect.topLeft());
		m_toggle->setSize(QSizeF(TOGGLE_WIDTH * u, rect.height()));
	}
	if (m_choice != nullptr)
	{
		m_choice->setPosition(rect.topLeft());
		m_choice->setSize(rect.size());
	}

	if (m_field.type == FieldType::Modules)
	{
		qreal y = rect.top() + m_button->implicitHeight() + MODULE_SPACING * u;
		if (m_modules.isEmpty())
		{
			auto note = Line(QRectF(), tr("No modules found yet. Set the game folder, then rescan."), SmallFont(), theme->TextMuted());
			note.wrap = QTextOption::WordWrap;
			note.rect = QRectF(rect.left(), y, rect.width(), TextBlockHeight(note.text, note.font, rect.width()));
			m_texts << note;
			return note.rect.bottom() - rect.top();
		}
		for (auto* module: m_modules)
		{
			module->setPosition(QPointF(rect.left(), y));
			module->setSize(QSizeF(rect.width(), MODULE_HEIGHT * u));
			y += (MODULE_HEIGHT + MODULE_SPACING) * u;
		}
		return y - MODULE_SPACING * u - rect.top();
	}

	if (m_field.type == FieldType::Params)
	{
		const auto  font        = SmallFont();
		const qreal line_height = std::ceil(QFontMetricsF(font).height());
		if (m_params.isEmpty())
		{
			m_texts << Line(QRectF(rect.left(), rect.top(), rect.width(), line_height), tr("No parameters file selected."), font,
			                theme->TextMuted());
			return line_height;
		}
		qreal       y         = rect.top();
		const qreal key_width = PARAM_KEY_WIDTH * u;
		const qreal value_x   = rect.left() + key_width + PARAM_GAP * u;
		for (const auto& param: m_params)
		{
			const auto entry = param.toMap();
			m_texts << Line(QRectF(rect.left(), y, key_width, line_height), entry.value("key").toString(), font, theme->TextMuted());
			m_texts << Line(QRectF(value_x, y, std::max<qreal>(0, rect.right() - value_x), line_height), entry.value("value").toString(),
			                font, theme->TextSecondary());
			y += line_height + PARAM_SPACING * u;
		}
		return y - PARAM_SPACING * u - rect.top();
	}
	return rect.height();
}

QSGNode* FieldRowItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<RowNode*>(old_node);
	if (node == nullptr)
	{
		node = new RowNode;
	}
	const auto*           theme = Theme::Instance();
	RoundedBoxNode::Style backing;
	if (HasFocusWithin())
	{
		backing.fill = theme->GlassStrong();
	} else if (m_hovered)
	{
		backing.fill = theme->Glass();
	} else
	{
		backing.fill = Qt::transparent;
	}
	backing.radius = theme->RadiusSmall();
	backing.dpr    = window()->effectiveDevicePixelRatio();
	node->backing->Update(window(), m_body, backing);

	if (node->generation == m_generation)
	{
		return node;
	}
	node->generation = m_generation;
	while (auto* child = node->texts->firstChild())
	{
		node->texts->removeChildNode(child);
		delete child;
	}
	for (const auto& text: m_texts)
	{
		auto* text_node = window()->createTextNode();
		if (text.wrap == QTextOption::NoWrap)
		{
			SetTextLine(text_node, text.text, text.font, text.color, text.rect);
		} else
		{
			SetTextBlock(text_node, text.text, text.font, text.color, text.rect, text.alignment, text.wrap);
		}
		node->texts->appendChildNode(text_node);
	}
	return node;
}

void FieldRowItem::mousePressEvent(QMouseEvent* event)
{
	if (!m_body.contains(event->position()))
	{
		event->ignore();
	}
}

void FieldRowItem::mouseReleaseEvent(QMouseEvent* event)
{
	if (!m_body.contains(event->position()))
	{
		return;
	}
	if (m_toggle != nullptr)
	{
		emit Edited(m_field.key, !m_value.toBool());
	}
	emit FocusRequested(m_field.key);
}

void FieldRowItem::hoverEnterEvent(QHoverEvent* /*event*/)
{
	m_hovered = true;
	update();
}

void FieldRowItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	m_hovered = false;
	update();
}
