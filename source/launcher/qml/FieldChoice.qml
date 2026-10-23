import QtQuick
import Kyty.Launcher

// One value out of a fixed list. Left and Right, Enter or a click move to the next value.
FocusScope {
    id: field

    property var    value
    property var    values
    property var    choices
    property string kind

    signal edited(var value)
    signal padRequested()
    signal browseRequested()

    implicitHeight: 40 * Theme.u
    height: implicitHeight

    function focusItems() {
        return [field];
    }

    function cycle(delta) {
        const list = field.choices || [];
        if (list.length === 0)
            return;
        const index = Math.max(0, list.indexOf(field.value));
        field.edited(list[(index + delta + list.length) % list.length]);
    }

    Keys.onLeftPressed: cycle(-1)
    Keys.onRightPressed: cycle(1)
    Keys.onReturnPressed: cycle(1)
    Keys.onEnterPressed: cycle(1)

    Rectangle {
        anchors.fill: parent
        radius: 12 * Theme.u
        color: Theme.glass
        border.width: field.activeFocus ? 3 * Theme.u : 1
        border.color: field.activeFocus ? Theme.focusRing : Theme.glassBorder

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 14 * Theme.u
            anchors.verticalCenter: parent.verticalCenter
            text: field.value === undefined ? "" : String(field.value)
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: 14 * Theme.u
            anchors.verticalCenter: parent.verticalCenter
            text: "‹  ›"
            color: Theme.textMuted
            font.pixelSize: Theme.fontBody
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: {
            field.forceActiveFocus();
            field.cycle(1);
        }
    }
}
