import QtQuick
import Kyty.Launcher

// On-screen keyboard for text fields, so a controller can enter names and paths.
// Keyboard and mouse can use it too; typing in a field still works directly.
FocusScope {
    id: pad

    property bool   shown: false
    property string key: ""
    property string title: ""
    property string text: ""
    property bool   upper: false

    readonly property var tokens: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
        "q", "w", "e", "r", "t", "y", "u", "i", "o", "p",
        "a", "s", "d", "f", "g", "h", "j", "k", "l", "-",
        "z", "x", "c", "v", "b", "n", "m", ".", "_", "/",
        ":", "+", "(", ")", "@", "&", "?", "'", "~", "#",
        "Shift", "Space", "Del", "Done"]

    signal edited(string text)

    visible: shown
    z: 20

    function openFor(fieldKey, fieldTitle) {
        key = fieldKey;
        title = fieldTitle;
        upper = false;
        shown = true;
        grid.currentIndex = 0;
        grid.forceActiveFocus();
    }

    function close() {
        shown = false;
    }

    function label(token) {
        return upper && token.length === 1 && token >= "a" && token <= "z" ? token.toUpperCase() : token;
    }

    function press(token) {
        if (token === "Shift")
            upper = !upper;
        else if (token === "Del")
            edited(text.slice(0, -1));
        else if (token === "Space")
            edited(text + " ");
        else if (token === "Done")
            close();
        else
            edited(text + label(token));
    }

    Keys.onEscapePressed: close()

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim

        MouseArea {
            anchors.fill: parent
            onClicked: pad.close()
        }
    }

    GlassPanel {
        anchors.centerIn: parent
        width: grid.width + 40 * Theme.u
        height: content.height + 40 * Theme.u
        color: Theme.scrim

        Column {
            id: content
            anchors.centerIn: parent
            spacing: 14 * Theme.u

            Text {
                width: grid.width
                text: pad.title
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }

            Rectangle {
                width: grid.width
                height: 40 * Theme.u
                radius: 10 * Theme.u
                color: Theme.glass
                border.color: Theme.glassBorder

                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 14 * Theme.u
                    anchors.rightMargin: 14 * Theme.u
                    anchors.verticalCenter: parent.verticalCenter
                    text: pad.text + "|"
                    elide: Text.ElideLeft
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                }
            }

            GridView {
                id: grid
                width: 10 * cellWidth
                height: Math.ceil(pad.tokens.length / 10) * cellHeight
                cellWidth: 60 * Theme.u
                cellHeight: 46 * Theme.u
                interactive: false
                model: pad.tokens

                delegate: Rectangle {
                    required property string modelData
                    required property int index

                    width: grid.cellWidth - 6 * Theme.u
                    height: grid.cellHeight - 6 * Theme.u
                    radius: 10 * Theme.u
                    color: grid.currentIndex === index ? Theme.glassStrong : Theme.glass
                    border.width: grid.currentIndex === index ? 3 * Theme.u : 1
                    border.color: grid.currentIndex === index ? Theme.focusRing : Theme.glassBorder

                    Text {
                        anchors.centerIn: parent
                        text: pad.label(modelData)
                        color: Theme.textPrimary
                        font.pixelSize: modelData.length > 1 ? Theme.fontSmall : Theme.fontBody
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            grid.currentIndex = index;
                            pad.press(modelData);
                        }
                    }
                }

                Keys.onReturnPressed: pad.press(pad.tokens[currentIndex])
                Keys.onEnterPressed: pad.press(pad.tokens[currentIndex])
                Keys.onEscapePressed: pad.close()
            }
        }
    }
}
