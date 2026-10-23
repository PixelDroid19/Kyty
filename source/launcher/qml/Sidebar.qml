import QtQuick
import Kyty.Launcher

// Left rail with the launcher's main actions, drawn straight on the backdrop.
Item {
    id: rail

    property string current: "library"
    property Item   rightTarget: null
    property Item   libraryButton: libraryEntry
    property alias  mascot: kit

    signal libraryRequested()
    signal addGameRequested()
    signal settingsRequested()
    signal aboutRequested()
    signal quitRequested()

    KitMascot {
        id: kit
        anchors.top: parent.top
        anchors.topMargin: 30 * Theme.u
        anchors.horizontalCenter: parent.horizontalCenter
        width: 44 * Theme.u
        floating: false
    }

    Column {
        anchors.top: kit.bottom
        anchors.topMargin: 18 * Theme.u
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 12 * Theme.u

        SidebarButton {
            id: libraryEntry
            icon: "icons/library.svg"
            label: qsTr("Library")
            selected: rail.current === "library"
            KeyNavigation.down: addEntry
            KeyNavigation.right: rail.rightTarget
            onActivated: rail.libraryRequested()
        }

        SidebarButton {
            id: addEntry
            icon: "icons/add.svg"
            label: qsTr("Add game")
            KeyNavigation.up: libraryEntry
            KeyNavigation.down: settingsEntry
            KeyNavigation.right: rail.rightTarget
            onActivated: rail.addGameRequested()
        }

        SidebarButton {
            id: settingsEntry
            icon: "icons/settings.svg"
            label: qsTr("Settings")
            selected: rail.current === "settings"
            KeyNavigation.up: addEntry
            KeyNavigation.right: rail.rightTarget
            onActivated: rail.settingsRequested()
        }
    }

    Column {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 40 * Theme.u
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 12 * Theme.u

        SidebarButton {
            id: aboutEntry
            icon: "icons/info.svg"
            label: qsTr("About and controls")
            selected: rail.current === "about"
            KeyNavigation.down: quitEntry
            KeyNavigation.right: rail.rightTarget
            onActivated: rail.aboutRequested()
        }

        SidebarButton {
            id: quitEntry
            icon: "icons/power.svg"
            label: qsTr("Quit")
            KeyNavigation.up: aboutEntry
            KeyNavigation.right: rail.rightTarget
            onActivated: rail.quitRequested()
        }
    }

    KeyNavigation.up: quitEntry
}
