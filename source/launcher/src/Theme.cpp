#include "Theme.h"

#include <QGuiApplication>
#include <QStyleHints>

namespace {

Theme* g_theme = nullptr;

} // namespace

Theme::Theme(QObject* parent): QObject(parent)
{
	Q_ASSERT(g_theme == nullptr);
	g_theme = this;
	UpdateDark();
	connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &Theme::UpdateDark);
}

Theme::~Theme()
{
	g_theme = nullptr;
}

Theme* Theme::Instance()
{
	Q_ASSERT(g_theme != nullptr);
	return g_theme;
}

void Theme::SetU(qreal u)
{
	if (qFuzzyCompare(u, m_u))
	{
		return;
	}
	m_u = u;
	emit UChanged();
}

void Theme::SetLive(bool live)
{
	if (live == m_live)
	{
		return;
	}
	m_live = live;
	emit LiveChanged();
}

void Theme::SetMotion(Motion motion)
{
	const bool live = Live();
	m_motion        = motion;
	if (Live() != live)
	{
		emit LiveChanged();
	}
}

// Each accent in a lighter tone for the dark palette and a deeper one for the light palette, so text in it keeps
// its contrast on either.
QColor Theme::Accent() const
{
	switch (m_accent)
	{
		case AccentColor::Blue: break;
		case AccentColor::Violet: return m_dark ? QColor(0xa7, 0x8b, 0xfa) : QColor(0x6d, 0x28, 0xd9);
		case AccentColor::Green: return m_dark ? QColor(0x4a, 0xde, 0x80) : QColor(0x15, 0x80, 0x3d);
		case AccentColor::Orange: return m_dark ? QColor(0xfb, 0x92, 0x3c) : QColor(0xc2, 0x41, 0x0c);
		case AccentColor::Pink: return m_dark ? QColor(0xf4, 0x72, 0xb6) : QColor(0xbe, 0x18, 0x5d);
	}
	return m_dark ? QColor(0x6b, 0x9d, 0xff) : QColor(0x2f, 0x62, 0xd8);
}

void Theme::SetColorScheme(Scheme scheme)
{
	m_scheme = scheme;
	UpdateDark();
}

void Theme::UpdateDark()
{
	// An unknown desktop scheme keeps the dark palette, the launcher's own look.
	const bool dark =
	    m_scheme == Scheme::Dark || (m_scheme == Scheme::System && QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light);
	if (dark == m_dark)
	{
		return;
	}
	m_dark = dark;
	emit PaletteChanged();
}
