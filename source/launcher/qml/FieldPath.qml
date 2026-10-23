import QtQuick
import Kyty.Launcher

// File or folder path: typed or picked with the Browse button.
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
        return [text, browse];
    }

    FieldText {
        id: text
        anchors.left: parent.left
        anchors.right: browse.left
        anchors.rightMargin: 8 * Theme.u
        value: field.value
        onEdited: (v) => field.edited(v)
        onPadRequested: field.padRequested()
    }

    FocusButton {
        id: browse
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: qsTr("Browse…")
        onActivated: field.browseRequested()
    }
}
