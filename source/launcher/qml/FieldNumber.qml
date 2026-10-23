import QtQuick
import Kyty.Launcher

// Whole number with −10 and +10 buttons. Typing or the on-screen pad also changes it.
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
        return [input, minus, plus];
    }

    function step(delta) {
        field.edited(Math.max(0, Number(field.value || 0) + delta));
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: minus.left
        anchors.rightMargin: 8 * Theme.u
        height: parent.height
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
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            validator: IntValidator { bottom: 0; top: 99999 }
            text: field.value === undefined || field.value === null ? "" : String(field.value)
            // An emptied field keeps the last number until a new one is typed.
            onTextEdited: if (text !== "") field.edited(parseInt(text, 10))

            Keys.onReturnPressed: field.padRequested()
            Keys.onEnterPressed: field.padRequested()
        }
    }

    FocusButton {
        id: minus
        anchors.right: plus.left
        anchors.rightMargin: 8 * Theme.u
        anchors.verticalCenter: parent.verticalCenter
        text: "−10"
        onActivated: field.step(-10)
    }

    FocusButton {
        id: plus
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: "+10"
        onActivated: field.step(10)
    }
}
