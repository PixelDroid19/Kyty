import QtQuick
import Kyty.Launcher

// On/off switch. Space, Enter or a click flips it.
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
    width: 200 * Theme.u

    function focusItems() {
        return [field];
    }

    function toggle() {
        field.edited(!field.value);
    }

    Keys.onSpacePressed: toggle()
    Keys.onReturnPressed: toggle()
    Keys.onEnterPressed: toggle()

    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        width: 56 * Theme.u
        height: 30 * Theme.u
        radius: height / 2
        color: field.value ? Theme.glassStrong : Theme.glass
        border.width: field.activeFocus ? 3 * Theme.u : 1
        border.color: field.activeFocus ? Theme.focusRing : Theme.glassBorder

        Rectangle {
            width: track.height - 8 * Theme.u
            height: width
            radius: height / 2
            anchors.verticalCenter: parent.verticalCenter
            x: field.value ? track.width - width - 4 * Theme.u : 4 * Theme.u
            color: Theme.textPrimary

            Behavior on x { NumberAnimation { duration: Theme.animation } }
        }
    }

    Text {
        anchors.left: track.right
        anchors.leftMargin: 12 * Theme.u
        anchors.verticalCenter: parent.verticalCenter
        text: field.value ? qsTr("On") : qsTr("Off")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontBody
    }

    MouseArea {
        anchors.fill: parent
        onClicked: {
            field.forceActiveFocus();
            field.toggle();
        }
    }
}
