import QtQuick
import Kyty.Launcher

// Options for one game, opened with O, the Start button or a right click. Delete asks for confirmation first.
FocusScope {
    id: menu

    property bool shown: false
    property int  row: -1
    property string heading: ""
    property var  actions: []
    property bool confirming: false

    signal chosen(string action, int row)

    visible: shown
    z: 10

    function openFor(game, row, items) {
        menu.row = row;
        menu.heading = game.title || game.name || "";
        menu.actions = items;
        menu.confirming = false;
        menu.shown = true;
        list.currentIndex = 0;
        menu.forceActiveFocus();
    }

    function close() {
        shown = false;
        confirming = false;
    }

    function activate(index) {
        const item = actions[index];
        if (!item || item.allowed === false)
            return;
        if (item.action === "close") {
            close();
            return;
        }
        if (item.action === "delete") {
            confirming = true;
            actions = [{ label: qsTr("Remove from library"), action: "confirm-delete", danger: true },
                       { label: qsTr("Cancel"), action: "close" }];
            list.currentIndex = 1;
            return;
        }
        const action = item.action === "confirm-delete" ? "delete" : item.action;
        close();
        chosen(action, row);
    }

    Keys.onUpPressed: list.decrementCurrentIndex()
    Keys.onDownPressed: list.incrementCurrentIndex()
    Keys.onReturnPressed: activate(list.currentIndex)
    Keys.onEnterPressed: activate(list.currentIndex)
    Keys.onEscapePressed: close()
    Keys.onMenuPressed: close()

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim

        MouseArea {
            anchors.fill: parent
            onClicked: menu.close()
        }
    }

    GlassPanel {
        anchors.centerIn: parent
        width: 440 * Theme.u
        height: content.height + 40 * Theme.u
        color: Theme.scrim

        Column {
            id: content
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 20 * Theme.u
            spacing: 10 * Theme.u

            Text {
                width: parent.width
                text: menu.heading
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle * 0.7
                font.weight: Font.DemiBold
            }

            Text {
                width: parent.width
                visible: menu.confirming
                wrapMode: Text.WordWrap
                text: qsTr("Game files on disk are not deleted.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }

            ListView {
                id: list
                width: parent.width
                height: count * 48 * Theme.u
                interactive: false
                model: menu.actions

                delegate: Rectangle {
                    required property var modelData
                    required property int index

                    width: list.width
                    height: 44 * Theme.u
                    radius: height / 2
                    color: list.currentIndex === index ? Theme.glassStrong : "transparent"
                    opacity: modelData.allowed === false ? 0.4 : 1

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 18 * Theme.u
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        color: modelData.danger ? Theme.danger : Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            list.currentIndex = index;
                            menu.activate(index);
                        }
                    }
                }
            }
        }
    }
}
