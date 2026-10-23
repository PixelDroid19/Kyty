import QtQuick
import Kyty.Launcher

// Icon button on the left rail. Keyboard and controller select it; the label shows while it is hovered or focused.
FocusScope {
    id: entry

    property string icon
    property string label
    property bool   selected: false
    signal activated()

    implicitWidth: 60 * Theme.u
    implicitHeight: 60 * Theme.u

    Keys.onReturnPressed: activated()
    Keys.onEnterPressed: activated()
    Keys.onSpacePressed: activated()

    Rectangle {
        anchors.centerIn: parent
        width: 46 * Theme.u
        height: width
        radius: width / 2
        // Only the current page and the pointer get a disc; the focus ring stays for keyboard and controller.
        color: entry.selected ? Theme.glassStrong : (hover.hovered ? Theme.glass : "transparent")
        border.width: entry.activeFocus ? 3 * Theme.u : 0
        border.color: Theme.focusRing
    }

    Image {
        anchors.centerIn: parent
        width: 24 * Theme.u
        height: width
        source: entry.icon
        sourceSize.width: 48
        sourceSize.height: 48
    }

    Rectangle {
        visible: hover.hovered || entry.activeFocus
        anchors.left: parent.right
        anchors.leftMargin: 10 * Theme.u
        anchors.verticalCenter: parent.verticalCenter
        width: labelText.implicitWidth + 20 * Theme.u
        height: 30 * Theme.u
        radius: height / 2
        color: Theme.glassStrong
        border.color: Theme.glassBorder

        Text {
            id: labelText
            anchors.centerIn: parent
            text: entry.label
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
        }
    }

    HoverHandler {
        id: hover
    }

    TapHandler {
        onTapped: {
            entry.forceActiveFocus();
            entry.activated();
        }
    }
}
