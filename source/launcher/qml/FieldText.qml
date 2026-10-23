import QtQuick
import Kyty.Launcher

// Single-line text. Enter opens the on-screen pad, so a controller can type too.
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
        return [input];
    }

    Rectangle {
        anchors.fill: parent
        radius: 12 * Theme.u
        color: Theme.glass
        border.width: input.activeFocus ? 3 * Theme.u : 1
        border.color: input.activeFocus ? Theme.focusRing : Theme.glassBorder

        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: 14 * Theme.u
            anchors.rightMargin: 14 * Theme.u
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            selectByMouse: true
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            text: field.value === undefined || field.value === null ? "" : String(field.value)
            onTextEdited: field.edited(text)

            Keys.onReturnPressed: field.padRequested()
            Keys.onEnterPressed: field.padRequested()
        }
    }
}
