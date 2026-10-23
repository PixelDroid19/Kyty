#ifndef LAUNCHER_INCLUDE_THEME_H_
#define LAUNCHER_INCLUDE_THEME_H_

#include "Common.h"

#include <QColor>
#include <QObject>

// Colours and sizes shared by every page and scene item. Sizes scale with u, which is 1 at 720 lines high.
class Theme: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(Theme);

signals:
	void UChanged();
	void LiveChanged();

public:
	explicit Theme(QObject* parent = nullptr);
	~Theme() override;

	// The one theme of the process, created before the UI.
	static Theme* Instance();

	[[nodiscard]] qreal U() const { return m_u; }
	void                SetU(qreal u);
	// False while a game runs or the window is not in front; every looping animation stops on it.
	[[nodiscard]] bool  Live() const { return m_live; }
	void                SetLive(bool live);

	// The website's palette, so Kit and the launcher look like the rest of the project.
	[[nodiscard]] QColor Background() const { return {0x09, 0x09, 0x0b}; }
	[[nodiscard]] QColor Surface() const { return {0x10, 0x10, 0x13}; }
	[[nodiscard]] QColor Accent() const { return {0x6b, 0x9d, 0xff}; }
	[[nodiscard]] QColor GlitchRed() const { return {0xff, 0x00, 0x3c}; }
	[[nodiscard]] QColor GlitchCyan() const { return {0x00, 0xe5, 0xff}; }
	[[nodiscard]] QColor TextPrimary() const { return {0xf4, 0xf4, 0xf5}; }
	[[nodiscard]] QColor TextSecondary() const { return {0xa1, 0xa1, 0xaa}; }
	[[nodiscard]] QColor TextMuted() const { return {0x8a, 0x8a, 0x94}; }
	[[nodiscard]] QColor Glass() const { return QColor::fromRgbF(1, 1, 1, 0.05F); }
	[[nodiscard]] QColor GlassStrong() const { return QColor::fromRgbF(1, 1, 1, 0.12F); }
	[[nodiscard]] QColor GlassBorder() const { return QColor::fromRgbF(1, 1, 1, 0.12F); }
	[[nodiscard]] QColor Scrim() const { return QColor::fromRgbF(0, 0, 0, 0.6F); }
	// Hairline around game artwork.
	[[nodiscard]] QColor ArtEdge() const { return QColor::fromRgbF(1, 1, 1, 0.14F); }
	[[nodiscard]] QColor Danger() const { return {0xff, 0x8a, 0x95}; }
	[[nodiscard]] QColor Ok() const { return {0x3e, 0xcf, 0x8e}; }

	[[nodiscard]] qreal Gap() const { return 16 * m_u; }
	[[nodiscard]] qreal Margin() const { return 28 * m_u; }
	[[nodiscard]] qreal Radius() const { return 22 * m_u; }
	[[nodiscard]] qreal RadiusSmall() const { return 12 * m_u; }
	[[nodiscard]] qreal FontBody() const { return 15 * m_u; }
	[[nodiscard]] qreal FontSmall() const { return 12 * m_u; }
	[[nodiscard]] qreal FontTitle() const { return 26 * m_u; }
	[[nodiscard]] int   Animation() const { return 140; }
	// Corner radius of game artwork as a share of its shorter side; the artwork provider rounds with the same share.
	[[nodiscard]] qreal ArtCorner() const { return 0.08; }
	// Space between game artwork and its focus ring.
	[[nodiscard]] qreal FocusGap() const { return 5 * m_u; }

private:
	qreal m_u    = 1;
	bool  m_live = true;
};

#endif /* LAUNCHER_INCLUDE_THEME_H_ */
