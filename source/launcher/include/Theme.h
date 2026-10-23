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
	// The palette switched between dark and light; items keep colours in their nodes, so the window rebuilds them.
	void PaletteChanged();

public:
	// Stored by name in the launcher settings.
	enum class Scheme
	{
		System,
		Dark,
		Light
	};

	explicit Theme(QObject* parent = nullptr);
	~Theme() override;

	// The one theme of the process, created before the UI.
	static Theme* Instance();

	[[nodiscard]] qreal U() const { return m_u; }
	void                SetU(qreal u);
	// False while a game runs or the window is not in front; every looping animation stops on it.
	[[nodiscard]] bool  Live() const { return m_live; }
	void                SetLive(bool live);
	[[nodiscard]] Scheme ColorScheme() const { return m_scheme; }
	// System follows the desktop's light or dark setting while the launcher runs.
	void               SetColorScheme(Scheme scheme);
	[[nodiscard]] bool IsDark() const { return m_dark; }

	// The website's palette, so Kit and the launcher look like the rest of the project; the light one mirrors it.
	[[nodiscard]] QColor Background() const { return m_dark ? QColor(0x09, 0x09, 0x0b) : QColor(0xf4, 0xf4, 0xf6); }
	[[nodiscard]] QColor Surface() const { return m_dark ? QColor(0x10, 0x10, 0x13) : QColor(0xff, 0xff, 0xff); }
	// Modal panels, opaque enough that the page under them does not show through their text.
	[[nodiscard]] QColor Panel() const { return m_dark ? QColor(0x15, 0x15, 0x19, 0xf5) : QColor(0xff, 0xff, 0xff, 0xf8); }
	[[nodiscard]] QColor Accent() const { return m_dark ? QColor(0x6b, 0x9d, 0xff) : QColor(0x2f, 0x62, 0xd8); }
	[[nodiscard]] QColor GlitchRed() const { return {0xff, 0x00, 0x3c}; }
	[[nodiscard]] QColor GlitchCyan() const { return {0x00, 0xe5, 0xff}; }
	[[nodiscard]] QColor TextPrimary() const { return m_dark ? QColor(0xf4, 0xf4, 0xf5) : QColor(0x18, 0x18, 0x1b); }
	[[nodiscard]] QColor TextSecondary() const { return m_dark ? QColor(0xa1, 0xa1, 0xaa) : QColor(0x4b, 0x4b, 0x55); }
	[[nodiscard]] QColor TextMuted() const { return m_dark ? QColor(0x8a, 0x8a, 0x94) : QColor(0x63, 0x63, 0x6e); }
	// Text over game artwork, which sits under a dark shade in both palettes.
	[[nodiscard]] QColor TextOnArt() const { return {0xf4, 0xf4, 0xf5}; }
	[[nodiscard]] QColor Glass() const { return Ink(m_dark ? 0.05F : 0.04F); }
	[[nodiscard]] QColor GlassStrong() const { return Ink(m_dark ? 0.12F : 0.08F); }
	[[nodiscard]] QColor GlassBorder() const { return Ink(m_dark ? 0.12F : 0.16F); }
	[[nodiscard]] QColor Scrim() const { return QColor::fromRgbF(0, 0, 0, m_dark ? 0.6F : 0.32F); }
	// Hairline around game artwork.
	[[nodiscard]] QColor ArtEdge() const { return Ink(0.14F); }
	// Faint light at the bottom of the window background.
	[[nodiscard]] QColor Glow() const { return m_dark ? QColor(0x10, 0x1e, 0x3c, 0x14) : QColor(0x2f, 0x62, 0xd8, 0x10); }
	[[nodiscard]] QColor Danger() const { return m_dark ? QColor(0xff, 0x8a, 0x95) : QColor(0xc4, 0x28, 0x3c); }
	[[nodiscard]] QColor Ok() const { return m_dark ? QColor(0x3e, 0xcf, 0x8e) : QColor(0x15, 0x80, 0x3d); }

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
	// The text colour at this opacity: white on the dark palette, black on the light one.
	[[nodiscard]] QColor Ink(float alpha) const { return m_dark ? QColor::fromRgbF(1, 1, 1, alpha) : QColor::fromRgbF(0, 0, 0, alpha); }
	void                 UpdateDark();

	qreal  m_u      = 1;
	bool   m_live   = true;
	Scheme m_scheme = Scheme::System;
	bool   m_dark   = true;
};

#endif /* LAUNCHER_INCLUDE_THEME_H_ */
