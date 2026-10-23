import QtQuick
import Kyty.Launcher

// Text button that works with mouse, keyboard and controller. Enter and Space activate it.
FocusScope {
    id: button

    property alias text: label.text
    property bool primary: false
    property bool danger: false
    property bool allowed: true
    signal activated()

    implicitWidth: label.implicitWidth + 36 * Theme.u
    implicitHeight: 40 * Theme.u
    opacity: allowed ? 1 : 0.4

    Keys.onReturnPressed: if (allowed) activated()
    Keys.onEnterPressed: if (allowed) activated()
    Keys.onSpacePressed: if (allowed) activated()

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: button.primary ? Theme.glassStrong : Theme.glass
        border.width: button.activeFocus ? 3 * Theme.u : 1
        border.color: button.activeFocus ? Theme.focusRing : Theme.glassBorder

        Behavior on border.width { NumberAnimation { duration: Theme.animation } }
    }

    Text {
        id: label
        anchors.centerIn: parent
        color: button.danger ? Theme.danger : Theme.textPrimary
        font.pixelSize: Theme.fontBody
        font.weight: button.primary ? Font.DemiBold : Font.Normal
    }

    MouseArea {
        anchors.fill: parent
        enabled: button.allowed
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            button.forceActiveFocus();
            button.activated();
        }
    }
}
