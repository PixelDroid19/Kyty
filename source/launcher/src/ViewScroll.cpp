#include "ViewScroll.h"

#include <QEasingCurve>

#include <algorithm>

constexpr int SCROLL_MS = 180;

ViewScroll::ViewScroll(QObject* parent): QObject(parent)
{
	m_animation.setDuration(SCROLL_MS);
	m_animation.setEasingCurve(QEasingCurve::OutCubic);
	connect(&m_animation, &QVariantAnimation::valueChanged, this,
	        [this](const QVariant& value)
	        {
		        m_position = value.toReal();
		        emit Moved();
	        });
}

void ViewScroll::ScrollTo(qreal position, qreal limit, bool animated)
{
	position = std::clamp<qreal>(position, 0, std::max<qreal>(0, limit));
	m_animation.stop();
	if (!animated || position == m_position)
	{
		if (position != m_position)
		{
			m_position = position;
			emit Moved();
		}
		return;
	}
	m_animation.setStartValue(m_position);
	m_animation.setEndValue(position);
	m_animation.start();
}

void ViewScroll::Show(qreal start, qreal end, qreal viewport, qreal limit, bool animated)
{
	// Measured from where a running scroll is heading, so held arrow keys do not lag behind.
	const qreal heading = m_animation.state() == QAbstractAnimation::Running ? m_animation.endValue().toReal() : m_position;
	qreal       target  = heading;
	if (start < heading)
	{
		target = start;
	} else if (end > heading + viewport)
	{
		target = end - viewport;
	}
	target = std::clamp<qreal>(target, 0, std::max<qreal>(0, limit));
	if (target != heading)
	{
		ScrollTo(target, limit, animated);
	}
}
