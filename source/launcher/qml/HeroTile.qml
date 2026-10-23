import QtQuick
import Kyty.Launcher
import "Util.js" as Util

// Wide card for the recently played row. Shows the game's picture, or its icon when there is no picture.
Item {
    id: hero

    property var  game: ({})
    property bool focused: false

    signal clicked()
    signal rightClicked()

    readonly property real cornerRadius: Theme.artCorner * height

    width: 240 * Theme.u
    height: 136 * Theme.u

    Rectangle {
        id: card
        anchors.fill: parent
        radius: hero.cornerRadius
        color: Theme.surface
        scale: hero.focused ? 1.04 : 1

        Behavior on scale { NumberAnimation { duration: Theme.animation } }

        Image {
            anchors.fill: parent
            source: Util.rounded(hero.game.picture || hero.game.icon)
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.width: Math.ceil(hero.width * Screen.devicePixelRatio)
            sourceSize.height: Math.ceil(hero.height * Screen.devicePixelRatio)
        }

        // Fades the art into the name instead of laying a bar over it.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * 0.55
            radius: hero.cornerRadius
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.8) }
            }
        }

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12 * Theme.u
            text: hero.game.title || hero.game.name || ""
            elide: Text.ElideRight
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
        }

        // A hairline edge, so dark artwork does not melt into a dark backdrop.
        Rectangle {
            anchors.fill: parent
            radius: hero.cornerRadius
            color: "transparent"
            border.width: 1
            border.color: Theme.artEdge
        }
    }

    // Outside the art and following its corners, so the picture cannot cover it.
    Rectangle {
        anchors.centerIn: card
        width: card.width * card.scale + 2 * Theme.focusGap
        height: card.height * card.scale + 2 * Theme.focusGap
        visible: hero.focused
        radius: hero.cornerRadius * card.scale + Theme.focusGap
        color: "transparent"
        border.width: 3 * Theme.u
        border.color: Theme.focusRing
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton)
                hero.rightClicked();
            else
                hero.clicked();
        }
    }
}
