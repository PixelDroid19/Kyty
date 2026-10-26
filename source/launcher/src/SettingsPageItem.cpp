#include "SettingsPageItem.h"

#include "BindingCaptureItem.h"
#include "EditorFields.h"
#include "FieldFormItem.h"
#include "FocusButtonItem.h"
#include "GamepadInput.h"
#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "SettingsRailItem.h"
#include "TextPadItem.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal TOP            = 26;
constexpr qreal TITLE_HEIGHT   = 40;
constexpr qreal RAIL_TOP_GAP   = 18;
constexpr qreal RAIL_WIDTH     = 236;
constexpr qreal COLUMN_GAP     = 40;
constexpr qreal MAX_CONTENT    = 780;
constexpr qreal TEXT_SPACING   = 6;
constexpr qreal INFO_SPACING   = 8;
constexpr qreal INFO_LABEL     = 132;
constexpr qreal SCREENSHOT     = 90;
constexpr qreal DOT            = 8;
constexpr qreal DOT_SPACING    = 8;
constexpr qreal BUTTONS_GAP    = 16;
constexpr qreal BUTTON_SPACING = 10;
constexpr qreal FORM_GAP       = 22;
constexpr qreal FORM_BOTTOM    = 8;

// The emulator settings wait this long after the last change before they are written, so typing a number saves
// it once.
constexpr int SAVE_DELAY = 600;

namespace {

enum Category
{
	Graphics,
	Shaders,
	Logs,
	Emulator,
	Controls,
	Interface,
	CATEGORY_COUNT
};

class SettingsNode: public GroupNode
{
public:
	int generation = -1;
};

PageLayout::Text TextAt(const QRectF& rect, const QString& text, const QFont& font, const QColor& color, QTextOption::WrapMode wrap)
{
	PageLayout::Text line;
	line.rect  = rect;
	line.text  = text;
	line.font  = font;
	line.color = color;
	line.wrap  = wrap;
	return line;
}

// The category's fields without their editor headings, which the category's title replaces.
QVector<EditorField> Fields(FieldGroup group)
{
	auto fields = EditorFields(group);
	for (auto& field: fields)
	{
		field.heading = nullptr;
	}
	return fields;
}

} // namespace

SettingsPageItem::SettingsPageItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope);
	// A new theme builds the page again, so the screenshot is read once. The theme was checked when it was read; a
	// picture that still fails to load only leaves the screenshot out.
	if (const auto* pack = LauncherBackend::Instance()->CurrentTheme(); pack != nullptr && !pack->screenshot.isEmpty())
	{
		m_screenshot = QImage(pack->screenshot);
	}

	m_rail = new SettingsRailItem(this);
	m_rail->SetEntries({{tr("Graphics"), SettingsIcon::Graphics},
	                    {tr("Shaders"), SettingsIcon::Shaders},
	                    {tr("Logs and dumps"), SettingsIcon::Logs},
	                    {tr("Emulator"), SettingsIcon::Emulator},
	                    {tr("Controls"), SettingsIcon::Controls},
	                    {tr("Interface"), SettingsIcon::Interface}});
	connect(m_rail, &SettingsRailItem::CurrentChanged, this, &SettingsPageItem::ShowCategory);
	connect(m_rail, &SettingsRailItem::Entered, this, &SettingsPageItem::Enter);
	connect(m_rail, &QQuickItem::activeFocusChanged, this, &SettingsPageItem::UpdateStickNavigation);

	auto add_button = [this](const QString& text)
	{
		auto* button = new FocusButtonItem(this);
		button->SetText(text);
		return button;
	};
	auto* backend = LauncherBackend::Instance();
	m_choose      = add_button(tr("Choose program…"));
	m_search      = add_button(tr("Search automatically"));
	m_log         = add_button(tr("Open last log"));
	m_defaults    = add_button(tr("Restore defaults"));
	m_duplicate   = add_button(tr("Duplicate profile"));
	m_delete      = add_button(tr("Delete profile"));
	m_delete->SetDanger(true);
	connect(m_choose, &FocusButtonItem::Activated, this, &SettingsPageItem::ProgramRequested);
	connect(m_search, &FocusButtonItem::Activated, backend, [backend]() { backend->SetEmulatorPath(QString()); });
	connect(m_log, &FocusButtonItem::Activated, backend, &LauncherBackend::OpenLog);
	connect(m_defaults, &FocusButtonItem::Activated, this, &SettingsPageItem::RestoreDefaults);
	connect(m_duplicate, &FocusButtonItem::Activated, this,
	        [this]() { ProfileAction(&LauncherBackend::DuplicateProfile, QStringLiteral("profile_name")); });
	connect(m_delete, &FocusButtonItem::Activated, this, &SettingsPageItem::DeleteActivated);
	// Leaving Delete profile takes its question back.
	connect(m_delete, &QQuickItem::activeFocusChanged, this,
	        [this](bool focused)
	        {
		        if (!focused && m_confirm_delete)
		        {
			        m_confirm_delete = false;
			        m_delete->SetText(tr("Delete profile"));
		        }
	        });

	// The keyboard covers the whole page, so it is created after the form to draw over it.
	m_pad  = new TextPadItem;
	m_form = new FieldFormItem(m_pad, this);
	m_pad->setParentItem(this);
	m_pad->setParent(this);
	m_capture = new BindingCaptureItem(backend->Gamepad(), this);
	connect(m_form, &FieldFormItem::Edited, this, &SettingsPageItem::Edited);
	connect(m_form, &FieldFormItem::CaptureRequested, this,
	        [this](const QString& key, bool gamepad)
	        {
		        const auto& fields = EditorFields(FieldGroup::Controls);
		        const auto  field  = std::find_if(fields.begin(), fields.end(), [&](const EditorField& f) { return key == QLatin1String(f.key); });
		        m_capture->OpenFor(key, field == fields.end() ? key : QCoreApplication::translate("EditorPageItem", field->label), gamepad);
	        });
	connect(m_capture, &BindingCaptureItem::Captured, this, &SettingsPageItem::Captured);
	connect(m_capture, &BindingCaptureItem::Closed, this,
	        [this]() { m_form->FocusKey(m_capture->Key(), m_capture->ForGamepad() ? 1 : 0); });
	connect(backend, &LauncherBackend::DevicesChanged, this,
	        [this]()
	        {
		        if (m_rail->Current() == Controls)
		        {
			        m_form->SetValues(LauncherBackend::Instance()->ControlsFields());
		        }
		        polish();
	        });
	connect(m_form, &FieldFormItem::TopReached, this,
	        [this]()
	        {
		        const auto buttons = Buttons();
		        if (!buttons.isEmpty())
		        {
			        buttons.first()->forceActiveFocus(Qt::OtherFocusReason);
		        }
	        });

	m_save_timer.setSingleShot(true);
	m_save_timer.setInterval(SAVE_DELAY);
	connect(&m_save_timer, &QTimer::timeout, this, &SettingsPageItem::SaveGlobal);
	connect(backend, &LauncherBackend::StateChanged, this, &QQuickItem::polish);
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::polish);

	m_global = backend->GlobalFields();
	ShowCategory();
}

SettingsPageItem::~SettingsPageItem()
{
	// The window builds its pages again when the language or the theme changes; a pending change is kept.
	if (m_save_timer.isActive())
	{
		SaveGlobal();
	}
}

QVariantList SettingsPageItem::Hints()
{
	return {QVariantMap {{"pad", QStringLiteral("✕")}, {"key", QStringLiteral("Enter")}, {"text", tr("Select")}},
	        QVariantMap {{"pad", QStringLiteral("○")}, {"key", QStringLiteral("Esc")}, {"text", tr("Back")}}};
}

SettingsPageItem::Place SettingsPageItem::CurrentPlace() const
{
	return {m_rail->Current(), m_last_key};
}

void SettingsPageItem::FocusDefault()
{
	m_rail->forceActiveFocus(Qt::OtherFocusReason);
}

void SettingsPageItem::GoTo(const Place& place)
{
	m_rail->SetCurrent(place.category);
	if (place.key.isEmpty())
	{
		FocusDefault();
		return;
	}
	m_form->FocusKey(place.key);
}

bool SettingsPageItem::EmulatorCategory() const
{
	const int category = m_rail->Current();
	return category == Graphics || category == Shaders || category == Logs;
}

// The current category's buttons, decided from the category and not from visibility, which is false while the
// page itself is hidden.
QVector<FocusButtonItem*> SettingsPageItem::Buttons() const
{
	QVector<FocusButtonItem*> buttons;
	if (m_rail->Current() == Emulator)
	{
		buttons << m_choose << m_search;
		if (!LauncherBackend::Instance()->LogPath().isEmpty())
		{
			buttons << m_log;
		}
	}
	if (EmulatorCategory())
	{
		buttons << m_defaults;
	}
	if (m_rail->Current() == Controls)
	{
		const auto values   = LauncherBackend::Instance()->ControlsFields();
		const bool is_default = values.value("input_profile").toString() == QLatin1String(InputProfiles::DEFAULT_ID);
		const bool usable     = values.value("profile_error").toString().isEmpty();
		if (usable)
		{
			buttons << m_duplicate;
		}
		if (!is_default || !usable)
		{
			buttons << m_defaults;
		}
		if (!is_default)
		{
			buttons << m_delete;
		}
	}
	return buttons;
}

void SettingsPageItem::ShowCategory()
{
	const int category = m_rail->Current();
	m_status.clear();
	m_error.clear();
	m_pad->Close();
	m_capture->Close();
	m_form->Clear();
	switch (category)
	{
		case Graphics: m_form->AddFields(Fields(FieldGroup::Display)); break;
		case Shaders: m_form->AddFields(Fields(FieldGroup::Shaders)); break;
		case Logs: m_form->AddFields(Fields(FieldGroup::Logs)); break;
		case Emulator: m_form->AddFields(Fields(FieldGroup::Program)); break;
		// Not editor sections, so their headings divide the category itself.
		case Controls: m_form->AddFields(EditorFields(FieldGroup::Controls)); break;
		case Interface: m_form->AddFields(EditorFields(FieldGroup::Interface)); break;
		default: break;
	}
	auto* backend = LauncherBackend::Instance();
	UpdateStickNavigation();
	m_form->SetValues(EmulatorCategory() ? m_global : category == Controls ? backend->ControlsFields() : backend->LauncherFields());
	m_form->ScrollToTop();
	polish();
}

// Inside the controls the left stick is there to be tried out, so only the D-pad moves between them.
void SettingsPageItem::UpdateStickNavigation()
{
	const bool trying = isVisible() && m_rail->Current() == Controls && !m_rail->hasActiveFocus();
	LauncherBackend::Instance()->Gamepad()->SetStickNavigation(!trying);
}

void SettingsPageItem::Enter()
{
	if (m_form->HasControls())
	{
		m_form->FocusFirst();
		return;
	}
	const auto buttons = Buttons();
	if (!buttons.isEmpty())
	{
		buttons.first()->forceActiveFocus(Qt::OtherFocusReason);
	}
}

void SettingsPageItem::Edited(const QString& key)
{
	m_last_key = key;
	if (m_rail->Current() == Controls)
	{
		ControlsEdited(key);
		return;
	}
	if (!EmulatorCategory())
	{
		// A new theme or language makes the window build its pages again, this one included.
		LauncherBackend::Instance()->SetLauncherField(key, m_form->Values().value(key));
		return;
	}
	m_global = m_form->Values();
	m_status.clear();
	m_save_timer.start();
	polish();
}

void SettingsPageItem::ControlsEdited(const QString& key)
{
	auto* backend = LauncherBackend::Instance();
	m_status.clear();
	m_error = backend->SetControlsField(key, m_form->Values().value(key));
	m_form->SetValues(backend->ControlsFields());
	polish();
}

void SettingsPageItem::Captured(const QString& key, const QString& name)
{
	auto binding = m_form->Values().value(key).toMap();
	// The input replaces what the cell had; a profile file may still list several.
	binding[m_capture->ForGamepad() ? QStringLiteral("buttons") : QStringLiteral("keys")] = QStringList {name};
	m_form->SetField(key, binding);
}

void SettingsPageItem::ProfileAction(QString (LauncherBackend::*action)(), const QString& key)
{
	auto* backend = LauncherBackend::Instance();
	m_status.clear();
	m_error    = (backend->*action)();
	m_last_key = key;
	m_form->SetValues(backend->ControlsFields());
	// The focus stays among the buttons, on the first one the new profile has.
	if (const auto buttons = Buttons(); !buttons.isEmpty())
	{
		buttons.first()->forceActiveFocus(Qt::OtherFocusReason);
	}
	polish();
}

void SettingsPageItem::DeleteActivated()
{
	if (!m_confirm_delete)
	{
		m_confirm_delete = true;
		m_delete->SetText(tr("Delete “%1”? Select again").arg(m_form->Values().value("profile_name").toString()));
		polish();
		return;
	}
	m_confirm_delete = false;
	m_delete->SetText(tr("Delete profile"));
	ProfileAction(&LauncherBackend::DeleteProfile, QStringLiteral("input_profile"));
}

void SettingsPageItem::SaveGlobal()
{
	m_save_timer.stop();
	m_error  = LauncherBackend::Instance()->SaveGlobal(m_global);
	m_status = m_error.isEmpty() ? tr("Saved. Games without custom settings use these from their next start.") : QString();
	polish();
}

void SettingsPageItem::RestoreDefaults()
{
	if (m_rail->Current() == Controls)
	{
		ProfileAction(&LauncherBackend::ResetProfile, QStringLiteral("input_profile"));
		m_status = m_error.isEmpty() ? tr("The profile has the default controls again.") : QString();
		return;
	}
	const auto defaults = LauncherBackend::Instance()->FieldsFor(-1);
	auto       values   = m_global;
	for (const auto& field: EditorFields(m_rail->Current() == Graphics  ? FieldGroup::Display
	                                     : m_rail->Current() == Shaders ? FieldGroup::Shaders
	                                                                    : FieldGroup::Logs))
	{
		values[field.key] = defaults.value(field.key);
	}
	m_global = values;
	m_form->SetValues(values);
	m_save_timer.start();
	polish();
}

bool SettingsPageItem::MoveAcross(int delta)
{
	const auto buttons = Buttons();
	const auto found = std::find_if(buttons.begin(), buttons.end(), [](const FocusButtonItem* button) { return button->hasActiveFocus(); });
	if (found == buttons.end())
	{
		return false;
	}
	const auto index = std::distance(buttons.begin(), found) + delta;
	if (index < 0)
	{
		FocusDefault();
	} else if (index < buttons.size())
	{
		buttons.at(index)->forceActiveFocus(Qt::OtherFocusReason);
	}
	return true;
}

void SettingsPageItem::keyPressEvent(QKeyEvent* event)
{
	const auto buttons = Buttons();
	const bool on_buttons =
	    std::any_of(buttons.begin(), buttons.end(), [](const FocusButtonItem* button) { return button->hasActiveFocus(); });
	switch (event->key())
	{
		case Qt::Key_Escape:
			if (m_rail->hasActiveFocus())
			{
				emit Closed();
			} else
			{
				FocusDefault();
			}
			return;
		case Qt::Key_Left:
			if (MoveAcross(-1))
			{
				return;
			}
			// Controls that do not use Left hand it back to the categories.
			if (!m_rail->hasActiveFocus())
			{
				FocusDefault();
				return;
			}
			break;
		case Qt::Key_Right:
			if (MoveAcross(1))
			{
				return;
			}
			break;
		case Qt::Key_Up:
			if (on_buttons)
			{
				return;
			}
			break;
		case Qt::Key_Down:
			if (on_buttons)
			{
				m_form->FocusFirst();
				return;
			}
			break;
		default: break;
	}
	event->ignore();
}

void SettingsPageItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemVisibleHasChanged)
	{
		UpdateStickNavigation();
	}
	// Leaving the page keeps what was changed, and coming back shows what is saved.
	if (change == ItemVisibleHasChanged && !value.boolValue && m_save_timer.isActive())
	{
		SaveGlobal();
	}
}

void SettingsPageItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		m_pad->setSize(new_geometry.size());
		m_capture->setSize(new_geometry.size());
		polish();
	}
}

void SettingsPageItem::AddInfo(const QString& label, const QString& value, const QColor& color, const QColor& dot, qreal x, qreal width,
                               qreal* y)
{
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const auto  small = SceneFont(theme->FontSmall());
	const auto  body  = SceneFont(theme->FontBody());
	const qreal line  = LineHeight(body);
	*y += INFO_SPACING * u;
	m_texts << TextAt(QRectF(x, *y, INFO_LABEL * u, line), label, small, theme->TextMuted(), QTextOption::NoWrap);
	qreal value_x = x + INFO_LABEL * u;
	if (dot.isValid())
	{
		m_dots << Dot {QRectF(value_x, std::round(*y + (line - DOT * u) / 2), DOT * u, DOT * u), dot};
		value_x += (DOT + DOT_SPACING) * u;
	}
	const qreal value_width = std::max<qreal>(0, x + width - value_x);
	// Paths keep their start and their file name; the middle gives way.
	const auto shown = QFontMetricsF(body).elidedText(value, Qt::ElideMiddle, value_width);
	m_texts << TextAt(QRectF(value_x, *y, value_width, line), shown, body, color, QTextOption::NoWrap);
	*y += line;
}

void SettingsPageItem::updatePolish()
{
	const auto* theme   = Theme::Instance();
	const auto* backend = LauncherBackend::Instance();
	const qreal u        = theme->U();
	const qreal margin   = theme->Margin();
	const int   category = m_rail->Current();
	m_texts.clear();
	m_dots.clear();
	m_screenshot_rect = {};

	// The categories under the page's title.
	const auto title_font = SceneFont(theme->FontTitle(), QFont::DemiBold);
	m_texts << TextAt(QRectF(margin, TOP * u, RAIL_WIDTH * u, TITLE_HEIGHT * u), tr("Settings"), title_font, theme->TextPrimary(),
	                  QTextOption::NoWrap);
	const qreal rail_top = (TOP + TITLE_HEIGHT + RAIL_TOP_GAP) * u;
	m_rail->setPosition(QPointF(margin, rail_top));
	m_rail->setSize(QSizeF(RAIL_WIDTH * u, std::max<qreal>(0, height() - rail_top - FORM_BOTTOM * u)));
	m_divider = QRectF(std::round(margin + (RAIL_WIDTH + COLUMN_GAP / 2) * u), rail_top, 1, std::max<qreal>(0, m_rail->ContentHeight()));

	const qreal   x                      = margin + (RAIL_WIDTH + COLUMN_GAP) * u;
	const qreal   width                  = std::clamp<qreal>(this->width() - margin - x, 0, MAX_CONTENT * u);
	qreal         y                      = TOP * u;
	const QString titles[CATEGORY_COUNT] = {tr("Graphics"), tr("Shaders"), tr("Logs and dumps"), tr("Emulator"), tr("Controls"), tr("Interface")};
	m_texts << TextAt(QRectF(x, y, width, TITLE_HEIGHT * u), titles[std::clamp(category, 0, CATEGORY_COUNT - 1)], title_font,
	                  theme->TextPrimary(), QTextOption::NoWrap);
	y += TITLE_HEIGHT * u;

	const auto small     = SceneFont(theme->FontSmall());
	auto       add_block = [&](const QString& text, const QColor& color)
	{
		if (text.isEmpty())
		{
			return;
		}
		y += TEXT_SPACING * u;
		const qreal height = TextBlockHeight(text, small, width);
		m_texts << TextAt(QRectF(x, y, width, height), text, small, color, QTextOption::WordWrap);
		y += height;
	};
	const QString saved_note = tr("Every game without custom emulator settings uses these. Changes are saved as you make them.");
	switch (category)
	{
		case Graphics:
			add_block(tr("Screen size, render resolution and how the image is filtered.") + QLatin1Char(' ') + saved_note,
			          theme->TextMuted());
			break;
		case Shaders:
			add_block(tr("How shaders are optimized, and the validation that helps find rendering bugs; validation slows games down.") +
			              QLatin1Char(' ') + saved_note,
			          theme->TextMuted());
			break;
		case Logs:
			add_block(tr("Where the emulator writes its log, the shader log, dumps and profiler output.") + QLatin1Char(' ') + saved_note,
			          theme->TextMuted());
			break;
		case Emulator:
		{
			add_block(tr("The emulator program the launcher starts. Direct runs it without a window of its own and writes its output to a "
			             "log file; a terminal shows the output live, for debugging."),
			          theme->TextMuted());
			const bool ready = backend->IsEmulatorReady();
			const auto state = ready
			                       ? (backend->EmulatorVersion().isEmpty() ? tr("Ready") : tr("Ready · %1").arg(backend->EmulatorVersion()))
			                       : backend->EmulatorMessage();
			AddInfo(tr("Status"), state, ready ? theme->TextPrimary() : theme->Danger(), ready ? theme->Ok() : theme->Danger(), x, width,
			        &y);
			if (!backend->EmulatorPath().isEmpty())
			{
				AddInfo(tr("Program"), backend->EmulatorPath(), theme->TextSecondary(), QColor(), x, width, &y);
			}
			break;
		}
		case Controls:
		{
			add_block(tr("The keys, mouse and controller buttons every game gets, kept in profiles. Games use the chosen profile from "
			             "their next start; changes are saved as you make them."),
			          theme->TextMuted());
			const auto controller = backend->ControllerName();
			AddInfo(tr("Controller"), controller.isEmpty() ? tr("None connected") : controller, theme->TextSecondary(),
			        controller.isEmpty() ? QColor() : theme->Ok(), x, width, &y);
			AddInfo(tr("Profiles folder"), backend->ProfilesFolder(), theme->TextSecondary(), QColor(), x, width, &y);
			// On Default, duplicating is the way forward.
			m_duplicate->SetPrimary(m_form->Values().value("input_profile").toString() == QLatin1String(InputProfiles::DEFAULT_ID));
			add_block(backend->ControlsFields().value("profile_error").toString(), theme->Danger());
			break;
		}
		case Interface:
		{
			add_block(tr("How the launcher looks and behaves. Every change applies at once."), theme->TextMuted());
			const auto controller = backend->ControllerName();
			AddInfo(tr("Controller"), controller.isEmpty() ? tr("None connected. Keyboard and mouse work everywhere.") : controller,
			        theme->TextSecondary(), controller.isEmpty() ? QColor() : theme->Ok(), x, width, &y);
			AddInfo(tr("Settings file"), backend->SettingsFile(), theme->TextSecondary(), QColor(), x, width, &y);
			AddInfo(tr("Themes folder"), ThemesFolder(), theme->TextSecondary(), QColor(), x, width, &y);
			if (const auto* pack = backend->CurrentTheme(); pack != nullptr)
			{
				QStringList about {pack->name};
				if (!pack->version.isEmpty())
				{
					about << pack->version;
				}
				if (!pack->author.isEmpty())
				{
					about << tr("by %1").arg(pack->author);
				}
				if (!pack->license.isEmpty())
				{
					about << pack->license;
				}
				AddInfo(tr("Style"), about.join(QStringLiteral(" · ")), theme->TextSecondary(), QColor(), x, width, &y);
				if (!m_screenshot.isNull())
				{
					// As tall as SCREENSHOT, as wide as its shape asks, within the value column.
					const qreal height = SCREENSHOT * u;
					const qreal shown  = std::min(width - INFO_LABEL * u, height * m_screenshot.width() / m_screenshot.height());
					y += INFO_SPACING * u;
					m_screenshot_rect = QRectF(x + INFO_LABEL * u, y, std::max<qreal>(0, shown), height);
					y += height;
				}
			}
			break;
		}
		default: break;
	}
	add_block(m_error, theme->Danger());
	add_block(m_status, theme->Ok());

	const auto buttons = Buttons();
	for (auto* button: {m_choose, m_search, m_log, m_defaults, m_duplicate, m_delete})
	{
		button->setVisible(buttons.contains(button));
	}
	if (!buttons.isEmpty())
	{
		y += BUTTONS_GAP * u;
		qreal left = x;
		for (auto* button: buttons)
		{
			button->setPosition(QPointF(left, y));
			button->setSize(QSizeF(button->implicitWidth(), button->implicitHeight()));
			left += button->implicitWidth() + BUTTON_SPACING * u;
		}
		y += buttons.first()->implicitHeight();
	}

	y += FORM_GAP * u;
	m_form->setPosition(QPointF(x, y));
	m_form->setSize(QSizeF(width, std::max<qreal>(0, height() - FORM_BOTTOM * u - y)));

	m_generation++;
	update();
}

QSGNode* SettingsPageItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<SettingsNode*>(old_node);
	if (node == nullptr)
	{
		node = new SettingsNode;
	}
	if (node->generation == m_generation)
	{
		return node;
	}
	node->generation = m_generation;
	while (auto* child = node->firstChild())
	{
		node->removeChildNode(child);
		delete child;
	}
	const auto* theme = Theme::Instance();
	auto*       line  = window()->createRectangleNode();
	line->setRect(m_divider);
	line->setColor(theme->GlassBorder());
	node->appendChildNode(line);
	for (const auto& dot: m_dots)
	{
		auto*                 box = new RoundedBoxNode;
		RoundedBoxNode::Style style;
		style.fill   = dot.color;
		style.radius = dot.rect.width() / 2;
		style.dpr    = window()->effectiveDevicePixelRatio();
		box->Update(window(), dot.rect, style);
		node->appendChildNode(box);
	}
	if (!m_screenshot_rect.isEmpty())
	{
		node->appendChildNode(CreateTextureNode(window(), window()->createTextureFromImage(m_screenshot), m_screenshot_rect));
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
		node->appendChildNode(text_node);
	}
	return node;
}
