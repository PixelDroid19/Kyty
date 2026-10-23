import QtQuick
import Kyty.Launcher

// Bottom strip listing what the current page's buttons do. Shows controller glyphs when a pad is connected.
Item {
    id: bar

    // Each hint: { pad: "✕", key: "Enter", text: "Run" }
    property var hints: []
    readonly property bool usePad: backend.controllerName !== ""

    Row {
        anchors.left: parent.left
        anchors.leftMargin: Theme.margin
        anchors.verticalCenter: parent.verticalCenter
        spacing: 26 * Theme.u

        Repeater {
            model: bar.hints

            // Each hint is one item with its own anchored children, so Row does not position anchored children.
            Item {
                id: hint

                required property var modelData

                readonly property bool glyph: bar.usePad && modelData.pad !== undefined
                readonly property real keyWidth: glyph ? Math.max(26 * Theme.u, glyphText.implicitWidth + 12 * Theme.u)
                                                       : keyText.implicitWidth + 16 * Theme.u

                width: keyWidth + 8 * Theme.u + label.implicitWidth
                height: bar.height

                Rectangle {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: hint.keyWidth
                    height: hint.glyph ? width : 24 * Theme.u
                    radius: hint.glyph ? height / 2 : 6 * Theme.u
                    color: hint.glyph ? Theme.glassStrong : Theme.glass
                    border.color: Theme.glassBorder

                    Text {
                        id: glyphText
                        anchors.centerIn: parent
                        visible: hint.glyph
                        text: hint.modelData.pad || ""
                        color: Theme.textPrimary
                        font.pixelSize: 14 * Theme.u
                    }

                    Text {
                        id: keyText
                        anchors.centerIn: parent
                        visible: !hint.glyph
                        text: hint.modelData.key || ""
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Text {
                    id: label
                    anchors.left: parent.left
                    anchors.leftMargin: hint.keyWidth + 8 * Theme.u
                    anchors.verticalCenter: parent.verticalCenter
                    text: hint.modelData.text
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }
            }
        }
    }
}
