import QtQuick
import Kyty.Launcher

// Modules inside the game folder. Ticked modules are loaded; missing ones are listed but cannot be ticked.
Column {
    id: field

    property var    value: []
    property var    values
    property var    choices
    property string kind

    signal edited(var value)
    signal padRequested()
    signal browseRequested()

    width: parent ? parent.width : 0
    spacing: 6 * Theme.u

    function focusItems() {
        const items = [rescan];
        for (let i = 0; i < modules.count; i++)
            items.push(modules.itemAt(i));
        return items;
    }

    function toggle(index) {
        const list = (field.value || []).slice();
        if (!list[index].present)
            return;
        list[index] = Object.assign({}, list[index], { selected: !list[index].selected });
        field.edited(list);
    }

    FocusButton {
        id: rescan
        anchors.right: parent.right
        text: qsTr("Rescan files")
        onActivated: field.edited(backend.ScanElfs((field.values || {}).basedir || "", field.value || []))
    }

    Text {
        width: parent.width
        visible: modules.count === 0
        wrapMode: Text.WordWrap
        text: qsTr("No modules found yet. Set the game folder, then rescan.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
    }

    Repeater {
        id: modules
        model: field.value || []

        delegate: FocusScope {
            id: module

            required property var modelData
            required property int index

            width: field.width
            height: 36 * Theme.u

            function toggle() {
                field.toggle(index);
            }

            Keys.onReturnPressed: toggle()
            Keys.onEnterPressed: toggle()
            Keys.onSpacePressed: toggle()

            Rectangle {
                anchors.fill: parent
                radius: 10 * Theme.u
                color: module.activeFocus ? Theme.glassStrong : "transparent"
                border.width: module.activeFocus ? 2 * Theme.u : 0
                border.color: Theme.focusRing

                Rectangle {
                    id: box
                    anchors.left: parent.left
                    anchors.leftMargin: 12 * Theme.u
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20 * Theme.u
                    height: width
                    radius: 5 * Theme.u
                    color: Theme.glass
                    border.color: Theme.glassBorder

                    Text {
                        anchors.centerIn: parent
                        visible: !!module.modelData.selected
                        text: "✓"
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Text {
                    anchors.left: box.right
                    anchors.leftMargin: 12 * Theme.u
                    anchors.right: parent.right
                    anchors.rightMargin: 12 * Theme.u
                    anchors.verticalCenter: parent.verticalCenter
                    text: module.modelData.path + (module.modelData.present ? "" : qsTr("  (missing)"))
                    elide: Text.ElideMiddle
                    color: module.modelData.present ? Theme.textPrimary : Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: {
                    module.forceActiveFocus();
                    module.toggle();
                }
            }
        }
    }
}
