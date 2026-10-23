import QtQuick
import Kyty.Launcher

// Static backdrop made of linear gradients only: no blur, no animation, drawn once.
Item {
    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    // Faint accent light from the upper left, the way the console's home screen glows behind its tiles.
    Rectangle {
        // Tall enough that the rotated top and bottom edges stay off screen; only the faded sides show.
        x: -parent.width * 0.15
        y: -parent.height
        width: parent.width * 0.8
        height: parent.height * 3
        rotation: 18

        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#006b9dff" }
            GradientStop { position: 0.5; color: "#266b9dff" }
            GradientStop { position: 1.0; color: "#006b9dff" }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: parent.height * 0.45

        gradient: Gradient {
            GradientStop { position: 0.0; color: "#0009090b" }
            GradientStop { position: 1.0; color: "#14101e3c" }
        }
    }
}
