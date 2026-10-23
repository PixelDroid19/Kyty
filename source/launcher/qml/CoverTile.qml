import QtQuick
import Kyty.Launcher
import "Util.js" as Util

// Game artwork. Without an icon it shows a coloured card with the game's initials.
Item {
    id: tile

    property var  game: ({})
    property real tileSize: 120 * Theme.u
    property bool focused: false

    signal clicked()
    signal doubleClicked()
    signal rightClicked()

    readonly property var coverColors: ["#6f8fd6", "#c96fa0", "#6fb89a", "#d9a35a", "#8f78d6", "#5fa6c9"]
    readonly property real cornerRadius: Theme.artCorner * tileSize

    width: tileSize
    height: tileSize

    Rectangle {
        id: card
        anchors.fill: parent
        radius: tile.cornerRadius
        // A faint placeholder while the artwork loads; the coloured card only for games without one.
        color: tile.game.icon ? Theme.glass : tile.coverColors[(tile.game.row || 0) % tile.coverColors.length]
        scale: tile.focused ? 1.06 : 1

        Behavior on scale { NumberAnimation { duration: Theme.animation } }

        Image {
            anchors.fill: parent
            source: Util.rounded(tile.game.icon)
            visible: source != ""
            asynchronous: true
            fillMode: Image.PreserveAspectCrop
            // Decode at display size, not at the artwork's full resolution.
            sourceSize.width: Math.ceil(tile.width * Screen.devicePixelRatio)
            sourceSize.height: Math.ceil(tile.height * Screen.devicePixelRatio)
        }

        Text {
            anchors.centerIn: parent
            visible: !tile.game.icon
            text: Util.initials(tile.game.title || tile.game.name)
            color: Theme.textPrimary
            font.pixelSize: tile.tileSize * 0.3
            font.weight: Font.DemiBold
        }

        // A hairline edge, so dark artwork does not melt into a dark backdrop.
        Rectangle {
            anchors.fill: parent
            radius: tile.cornerRadius
            color: "transparent"
            border.width: 1
            border.color: Theme.artEdge
        }

        Rectangle {
            visible: tile.game.running === true
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8 * Theme.u
            width: runningText.implicitWidth + 20 * Theme.u
            height: 24 * Theme.u
            radius: height / 2
            color: Qt.rgba(0, 0, 0, 0.7)

            Text {
                id: runningText
                anchors.centerIn: parent
                text: qsTr("Running")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSmall
            }
        }
    }

    // Outside the art and following its corners, so the picture cannot cover it.
    Rectangle {
        anchors.centerIn: card
        width: card.width * card.scale + 2 * Theme.focusGap
        height: card.height * card.scale + 2 * Theme.focusGap
        visible: tile.focused
        radius: tile.cornerRadius * card.scale + Theme.focusGap
        color: "transparent"
        border.width: 3 * Theme.u
        border.color: Theme.focusRing
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton)
                tile.rightClicked();
            else
                tile.clicked();
        }
        onDoubleClicked: (mouse) => {
            if (mouse.button === Qt.LeftButton)
                tile.doubleClicked();
        }
    }
}
