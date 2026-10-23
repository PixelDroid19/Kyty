#ifndef LAUNCHER_INCLUDE_VIEW_SCROLL_H_
#define LAUNCHER_INCLUDE_VIEW_SCROLL_H_

#include "Common.h"

#include <QObject>
#include <QVariantAnimation>

// How far a view is scrolled along one axis. Following the selection eases to the new position; the wheel and
// layout changes jump.
class ViewScroll: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ViewScroll);

signals:
	void Moved();

public:
	explicit ViewScroll(QObject* parent = nullptr);
	~ViewScroll() override = default;

	[[nodiscard]] qreal Position() const { return m_position; }
	// Scrolls to position, kept between 0 and limit.
	void ScrollTo(qreal position, qreal limit, bool animated);
	// Scrolls by delta from where a running scroll is heading, so quick wheel notches add up into one glide.
	void ScrollBy(qreal delta, qreal limit, bool animated);
	// Scrolls just enough to show the span from start to end in a viewport of the given length.
	void Show(qreal start, qreal end, qreal viewport, qreal limit, bool animated);

private:
	[[nodiscard]] qreal Heading() const;

	QVariantAnimation m_animation;
	qreal             m_position = 0;
};

#endif /* LAUNCHER_INCLUDE_VIEW_SCROLL_H_ */
