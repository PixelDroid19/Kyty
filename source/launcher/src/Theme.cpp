#include "Theme.h"

namespace {

Theme* g_theme = nullptr;

} // namespace

Theme::Theme(QObject* parent): QObject(parent)
{
	Q_ASSERT(g_theme == nullptr);
	g_theme = this;
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
