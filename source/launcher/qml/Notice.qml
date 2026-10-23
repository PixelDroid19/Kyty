import QtQuick
import Kyty.Launcher

// Short message that hides itself after a few seconds.
Rectangle {
    id: notice

    property alias text: message.text

    function show(value) {
        message.text = value;
        hideTimer.restart();
    }

    visible: message.text !== ""
    width: Math.min(message.implicitWidth + 40 * Theme.u, parent ? parent.width - 80 * Theme.u : 600)
    height: message.implicitHeight + 20 * Theme.u
    radius: height / 2
    color: Theme.glassStrong
    border.color: Theme.glassBorder

    Text {
        id: message
        anchors.centerIn: parent
        width: parent.width - 40 * Theme.u
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontBody
    }

    Timer {
        id: hideTimer
        interval: 4000
        onTriggered: message.text = ""
    }
}
