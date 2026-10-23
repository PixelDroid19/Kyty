import QtQuick
import Kyty.Launcher

// Controls for keyboard, mouse and controller.
FocusScope {
    id: about

    readonly property var hints: [
        { pad: "○", key: "Esc", text: qsTr("Back") }
    ]

    signal closed()

    readonly property var controls: [
        { action: qsTr("Move"), keys: qsTr("Arrow keys"), pad: qsTr("D-pad or left stick"), mouse: qsTr("Click") },
        { action: qsTr("Run a game"), keys: qsTr("Enter"), pad: "✕", mouse: qsTr("Double-click") },
        { action: qsTr("Options"), keys: "O", pad: "△ / Start", mouse: qsTr("Right-click") },
        { action: qsTr("Edit a game"), keys: "E", pad: "□", mouse: qsTr("Options, then Edit") },
        { action: qsTr("Switch folder"), keys: "PgUp / PgDn", pad: "L1 / R1", mouse: qsTr("Click a folder") },
        { action: qsTr("Save in the editor"), keys: "O", pad: "△", mouse: qsTr("Save") },
        { action: qsTr("Test run in the editor"), keys: "E", pad: "□", mouse: qsTr("Test run") },
        { action: qsTr("Type text"), keys: qsTr("Type"), pad: qsTr("Enter opens the on-screen keyboard"), mouse: qsTr("Type") },
        { action: qsTr("Back or cancel"), keys: "Esc", pad: "○", mouse: qsTr("Click outside") }
    ]

    function focusDefault() {
        backButton.forceActiveFocus();
    }

    // Header row followed by one row of four cells per control, flattened for the grid.
    function cells() {
        const out = [qsTr("Action"), qsTr("Keyboard"), qsTr("Controller"), qsTr("Mouse")];
        for (let i = 0; i < controls.length; i++) {
            const c = controls[i];
            out.push(c.action, c.keys, c.pad, c.mouse);
        }
        return out;
    }

    Keys.onEscapePressed: closed()

    Flickable {
        anchors.fill: parent
        anchors.margins: Theme.margin
        contentHeight: content.height
        clip: true

        Column {
            id: content
            width: Math.min(parent.width, 820 * Theme.u)
            topPadding: 12 * Theme.u
            spacing: 14 * Theme.u

            Row {
                spacing: 28 * Theme.u

                KitMascot {
                    width: 96 * Theme.u
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6 * Theme.u

                    Text {
                        text: qsTr("Kyty Launcher")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontTitle
                        font.weight: Font.DemiBold
                    }

                    Text {
                        text: qsTr("Kit is the Kyty mascot. Hold the mouse on him to pet him.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Games are started by the Kyty emulator. The launcher only keeps the list of games and their settings.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }

            GlassPanel {
                width: content.width
                height: grid.height + 32 * Theme.u

                Grid {
                    id: grid
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 16 * Theme.u
                    columns: 4
                    columnSpacing: 18 * Theme.u
                    rowSpacing: 10 * Theme.u

                    Repeater {
                        model: about.cells()

                        Text {
                            required property string modelData
                            required property int index

                            width: index % 4 === 0 ? 210 * Theme.u : 190 * Theme.u
                            wrapMode: Text.WordWrap
                            text: modelData
                            color: index < 4 ? Theme.textMuted : Theme.textPrimary
                            font.pixelSize: index < 4 ? Theme.fontSmall : Theme.fontBody
                            font.weight: index < 4 ? Font.DemiBold : Font.Normal
                        }
                    }
                }
            }

            FocusButton {
                id: backButton
                text: qsTr("Back")
                onActivated: about.closed()
            }
        }
    }
}
