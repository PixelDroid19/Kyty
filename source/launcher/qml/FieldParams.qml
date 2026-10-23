import QtQuick
import Kyty.Launcher

// Read-only view of the entries in the parameters file.
Column {
    id: field

    property var    value
    property var    values
    property var    choices
    property string kind

    signal edited(var value)
    signal padRequested()
    signal browseRequested()

    readonly property var entries: value ? backend.ParamEntries(value).slice(0, 200) : []

    width: parent ? parent.width : 0
    spacing: 4 * Theme.u

    function focusItems() {
        return [];
    }

    Text {
        visible: field.entries.length === 0
        text: qsTr("No parameters file selected.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
    }

    Repeater {
        model: field.entries

        Row {
            required property var modelData

            width: field.width
            spacing: 12 * Theme.u

            Text {
                width: 200 * Theme.u
                text: modelData.key
                elide: Text.ElideRight
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }

            Text {
                width: field.width - 212 * Theme.u
                text: modelData.value
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}
