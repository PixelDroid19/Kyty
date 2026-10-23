import QtQuick
import Kyty.Launcher

// One line of the game editor: optional section heading, label and the control for the field's type.
Item {
    id: row

    // { key, label, type, heading?, choices?, kind?, when? }
    property var desc: ({})
    property var values: ({})

    readonly property bool shown: !desc || !desc.when || !!desc.when(values)
    readonly property var choiceList: (desc.choices && backend.Choices()[desc.choices]) || []
    readonly property var fieldFiles: ({
        text: "FieldText.qml",
        number: "FieldNumber.qml",
        toggle: "FieldToggle.qml",
        choice: "FieldChoice.qml",
        path: "FieldPath.qml",
        elfs: "FieldElfs.qml",
        params: "FieldParams.qml"
    })

    signal edited(string key, var value)
    signal padRequested(string key, string title)
    signal browseRequested(string key, string kind)

    visible: shown
    height: shown ? column.height : 0

    function focusItems() {
        return loader.item && loader.item.focusItems ? loader.item.focusItems() : [];
    }

    Column {
        id: column
        width: row.width
        spacing: 6 * Theme.u

        Text {
            visible: !!row.desc.heading
            text: row.desc.heading || ""
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
            font.capitalization: Font.AllUppercase
        }

        Row {
            width: parent.width
            spacing: 16 * Theme.u

            Text {
                id: label
                width: 230 * Theme.u
                height: 40 * Theme.u
                verticalAlignment: Text.AlignVCenter
                text: row.desc.label || ""
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }

            Loader {
                id: loader
                width: row.width - label.width - 16 * Theme.u
                source: row.fieldFiles[row.desc.type] || ""

                onLoaded: {
                    item.value = Qt.binding(() => row.values[row.desc.key]);
                    item.values = Qt.binding(() => row.values);
                    item.choices = Qt.binding(() => row.choiceList);
                    item.kind = row.desc.kind || "";
                }
            }
        }
    }

    Connections {
        target: loader.item
        ignoreUnknownSignals: true

        function onEdited(value) {
            row.edited(row.desc.key, value);
        }

        function onPadRequested() {
            row.padRequested(row.desc.key, row.desc.label);
        }

        function onBrowseRequested() {
            row.browseRequested(row.desc.key, row.desc.kind || "");
        }
    }
}
