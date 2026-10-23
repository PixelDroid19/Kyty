import QtQuick
import Kyty.Launcher

// The focused game's key art behind the whole window, darkened so the pages stay readable.
// A new picture loads only after the focus has rested a moment, so scrolling decodes nothing.
// It loads behind the current one and dissolves over it once decoded, so the window never dips to black;
// the picture underneath is released as soon as it is covered, so at most two are held for a moment.
Item {
    id: backdrop

    // image:// address of the picture to show; empty shows none.
    property string picture

    // The layer on top, fully shown once its dissolve ends.
    property Image front: first

    readonly property Image back: front === first ? second : first

    onPictureChanged: settle.restart()

    function show() {
        if (front.source.toString() === picture)
            return;
        vanish.stop();
        // A dissolve still running ends at once, so its picture becomes the one to dissolve from.
        if (dissolve.running) {
            dissolve.complete();
            releaseCovered();
        }
        if (picture === "") {
            vanish.restart();
            return;
        }
        back.source = picture;
    }

    function reveal(layer) {
        if (layer !== back || layer.source.toString() !== picture)
            return;
        front.z = 0;
        layer.z = 1;
        front = layer;
        dissolve.restart();
    }

    function releaseCovered() {
        back.source = "";
        back.opacity = 0;
    }

    component Layer: Image {
        anchors.fill: parent
        opacity: 0
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        // The provider keeps a small copy on disk; holding it here too would only cost memory.
        cache: false
        sourceSize.width: Math.ceil(width * Screen.devicePixelRatio)
        sourceSize.height: Math.ceil(height * Screen.devicePixelRatio)
    }

    Timer {
        id: settle
        interval: 180
        onTriggered: backdrop.show()
    }

    Layer {
        id: first
        onStatusChanged: {
            if (status === Image.Ready)
                backdrop.reveal(first);
        }
    }

    Layer {
        id: second
        onStatusChanged: {
            if (status === Image.Ready)
                backdrop.reveal(second);
        }
    }

    Rectangle {
        anchors.fill: parent
        z: 10
        opacity: Math.max(first.opacity, second.opacity)

        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(0.035, 0.035, 0.043, 0.78) }
            GradientStop { position: 1.0; color: Qt.rgba(0.035, 0.035, 0.043, 0.93) }
        }
    }

    // The new picture fades in while settling from a slight zoom, which reads as a camera move, not a cut.
    SequentialAnimation {
        id: dissolve
        ParallelAnimation {
            NumberAnimation { target: backdrop.front; property: "opacity"; from: 0; to: 1; duration: 420; easing.type: Easing.OutCubic }
            NumberAnimation { target: backdrop.front; property: "scale"; from: 1.04; to: 1; duration: 900; easing.type: Easing.OutCubic }
        }
        ScriptAction { script: backdrop.releaseCovered() }
    }

    SequentialAnimation {
        id: vanish
        NumberAnimation { target: backdrop.front; property: "opacity"; to: 0; duration: 250; easing.type: Easing.InQuad }
        ScriptAction { script: backdrop.front.source = "" }
    }
}
