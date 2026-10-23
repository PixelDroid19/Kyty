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
