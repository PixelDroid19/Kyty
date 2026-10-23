pragma Singleton
import QtQuick

// Colours and sizes shared by every page. Sizes scale with u, which is 1 at 720 lines high.
QtObject {
    property real u: 1
    // False while a game runs or the window is not in front; every looping animation stops on it.
    property bool live: true

    // The website's palette, so Kit and the launcher look like the rest of the project.
    readonly property color background: "#09090b"
    readonly property color surface: "#101013"
    readonly property color accent: "#6b9dff"
    readonly property color glitchRed: "#ff003c"
    readonly property color glitchCyan: "#00e5ff"
    readonly property color textPrimary: "#f4f4f5"
    readonly property color textSecondary: "#a1a1aa"
    readonly property color textMuted: "#8a8a94"
    readonly property color glass: Qt.rgba(1, 1, 1, 0.05)
    readonly property color glassStrong: Qt.rgba(1, 1, 1, 0.12)
    readonly property color glassBorder: Qt.rgba(1, 1, 1, 0.12)
    readonly property color scrim: Qt.rgba(0, 0, 0, 0.6)
    readonly property color focusRing: accent
    // Hairline around game artwork.
    readonly property color artEdge: Qt.rgba(1, 1, 1, 0.14)
    readonly property color danger: "#ff8a95"
    readonly property color ok: "#3ecf8e"

    readonly property real gap: 16 * u
    readonly property real margin: 28 * u
    readonly property real radius: 22 * u
    readonly property real radiusSmall: 12 * u
    readonly property real fontBody: 15 * u
    readonly property real fontSmall: 12 * u
    readonly property real fontTitle: 26 * u
    readonly property int  animation: 140
    // Corner radius of game artwork as a share of its shorter side; the artwork provider rounds with the same share.
    readonly property real artCorner: 0.08
    // Space between game artwork and its focus ring.
    readonly property real focusGap: 5 * u
}
