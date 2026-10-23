import QtQuick
import Kyty.Launcher

// Kit, the project's mascot, built from the website's SVG layers and animated the same way: float, blink,
// red/cyan glitch slices, and petting with happy eyes, a purr shake and rising hearts.
// Every loop runs only while Theme.live, so a hidden launcher or a running game costs no frames.
Item {
    id: kit

    property bool floating: true
    property bool pettable: true
    property bool petted: false
    readonly property bool happy: petted || hopAnimation.running
    // Pixels per unit of the 120 x 160 SVG viewBox; the website draws Kit at 120 px, so its offsets apply 1:1.
    readonly property real k: width / 120

    implicitWidth: 120
    height: width * 160 / 120

    // Short jump with happy eyes, used when a game starts.
    function hop() {
        hopAnimation.restart();
    }

    // The website's glitch keyframes (4.2 s loop): when each colour slice appears, its offset and its clip insets.
    readonly property int glitchPeriod: 4200
    readonly property var glitchEvents: [
        { t: 420,  layer: "r", dx: -3, dy: 1,  top: 0.08, bottom: 0.58 },
        { t: 441,  layer: "c", dx: 3,  dy: -1, top: 0.20, bottom: 0.44 },
        { t: 462,  layer: "r", dx: 2,  dy: -2, top: 0.52, bottom: 0.18 },
        { t: 483,  layer: "c", dx: -2, dy: 2,  top: 0.46, bottom: 0.26 },
        { t: 504,  layer: "r" },
        { t: 525,  layer: "c" },
        { t: 1764, layer: "r", dx: -2, dy: -1, top: 0.30, bottom: 0.40 },
        { t: 1785, layer: "c", dx: 2,  dy: 1,  top: 0.12, bottom: 0.66 },
        { t: 1827, layer: "r" },
        { t: 1848, layer: "c" },
        { t: 3066, layer: "r", dx: 3,  dy: 2,  top: 0.64, bottom: 0.06 },
        { t: 3108, layer: "c", dx: -3, dy: -2, top: 0.56, bottom: 0.14 },
        { t: 3129, layer: "r" },
        { t: 3171, layer: "c" }
    ]
    property int glitchIndex: 0

    function applyGlitch(event) {
        const slice = event.layer === "r" ? redSlice : cyanSlice;
        slice.visible = event.top !== undefined;
        if (!slice.visible)
            return;
        slice.dx = event.dx;
        slice.dy = event.dy;
        slice.insetTop = event.top;
        slice.insetBottom = event.bottom;
    }

    // Every layer shares the 120 x 160 viewBox, so stacking them at the same size rebuilds the drawing.
    component Layer: Image {
        sourceSize.width: Math.ceil(width * Screen.devicePixelRatio)
        sourceSize.height: Math.ceil(height * Screen.devicePixelRatio)
    }

    // A colour copy of the whole shape, clipped to a horizontal band and nudged sideways.
    component GlitchSlice: Item {
        id: slice

        property alias source: sliceImage.source
        property real figureWidth
        property real figureHeight
        property real dx: 0
        property real dy: 0
        property real insetTop: 0
        property real insetBottom: 0

        visible: false
        opacity: 0.85
        x: dx * figureWidth / 120
        y: insetTop * figureHeight + dy * figureWidth / 120
        width: figureWidth
        height: (1 - insetTop - insetBottom) * figureHeight
        clip: true

        Layer {
            id: sliceImage
            y: -slice.insetTop * slice.figureHeight
            width: slice.figureWidth
            height: slice.figureHeight
        }
    }

    Timer {
        id: glitchTimer
        running: Theme.live
        repeat: true
        interval: kit.glitchEvents[0].t
        onRunningChanged: {
            if (running)
                return;
            kit.glitchIndex = 0;
            interval = kit.glitchEvents[0].t;
            redSlice.visible = false;
            cyanSlice.visible = false;
        }
        onTriggered: {
            const events = kit.glitchEvents;
            kit.applyGlitch(events[kit.glitchIndex]);
            const next = (kit.glitchIndex + 1) % events.length;
            interval = (events[next].t - events[kit.glitchIndex].t + kit.glitchPeriod) % kit.glitchPeriod;
            kit.glitchIndex = next;
        }
    }

    Item {
        id: figure
        width: kit.width
        height: kit.height

        transform: [
            Translate { id: floatShift },
            Translate { id: purrShift },
            Translate { id: hopShift }
        ]

        GlitchSlice {
            id: redSlice
            figureWidth: kit.width
            figureHeight: kit.height
            source: "kit/glitch-r.svg"
        }

        GlitchSlice {
            id: cyanSlice
            figureWidth: kit.width
            figureHeight: kit.height
            source: "kit/glitch-c.svg"
        }

        Layer {
            width: kit.width
            height: kit.height
            source: "kit/body.svg"
        }

        Layer {
            width: kit.width
            height: kit.height
            source: "kit/gear.svg"
        }

        Layer {
            visible: !kit.happy
            width: kit.width
            height: kit.height
            source: "kit/eyes-open.svg"

            transform: Scale {
                id: blinkScale
                origin.y: 70.5 * kit.k
            }
        }

        Layer {
            visible: kit.happy
            width: kit.width
            height: kit.height
            source: "kit/eyes-happy.svg"
        }
    }

    Text {
        x: kit.width + 34 * kit.k - implicitWidth
        y: -4 * kit.k
        text: "purrr…"
        color: Theme.accent
        font.family: "monospace"
        font.pixelSize: Math.max(9, 12 * kit.k)
        rotation: 8
        scale: kit.petted ? 1 : 0.6
        opacity: kit.petted ? 1 : 0

        Behavior on opacity { NumberAnimation { duration: 150 } }
        Behavior on scale { NumberAnimation { duration: 150 } }
    }

    // Hearts float up from the head while Kit is petted.
    Component {
        id: heartComponent

        Image {
            id: heart

            property real dx: 0
            readonly property real size: width

            x: kit.width / 2 - width / 2
            y: kit.height * 0.38 - height
            height: width * 0.9
            source: "kit/heart.svg"
            sourceSize.width: Math.ceil(width * Screen.devicePixelRatio)
            opacity: 0
            scale: 0.5

            ParallelAnimation {
                running: true
                onFinished: heart.destroy()

                NumberAnimation { target: heart; property: "x"; to: heart.x + heart.dx; duration: 1350; easing.type: Easing.OutCubic }
                NumberAnimation { target: heart; property: "y"; to: heart.y - 86 * kit.k; duration: 1350; easing.type: Easing.OutCubic }
                NumberAnimation { target: heart; property: "scale"; to: 1.1; duration: 1350; easing.type: Easing.OutCubic }
                SequentialAnimation {
                    NumberAnimation { target: heart; property: "opacity"; to: 1; duration: 190 }
                    NumberAnimation { target: heart; property: "opacity"; to: 0; duration: 1160; easing.type: Easing.InQuad }
                }
            }
        }
    }

    function spawnHeart() {
        heartComponent.createObject(kit, {
            width: (10 + Math.random() * 7) * kit.k,
            dx: (Math.random() * 70 - 35) * kit.k
        });
    }

    function startPet() {
        if (!pettable)
            return;
        stopPetTimer.stop();
        petted = true;
        spawnHeart();
        heartTimer.start();
    }

    function stopPet() {
        if (!heartTimer.running)
            return;
        heartTimer.stop();
        stopPetTimer.start();
    }

    Timer {
        id: heartTimer
        interval: 150
        repeat: true
        onTriggered: kit.spawnHeart()
    }

    Timer {
        id: stopPetTimer
        interval: 220
        onTriggered: kit.petted = false
    }

    MouseArea {
        anchors.fill: parent
        enabled: kit.pettable
        hoverEnabled: false
        cursorShape: Qt.PointingHandCursor
        onPressed: kit.startPet()
        onReleased: kit.stopPet()
        onCanceled: kit.stopPet()
        onExited: kit.stopPet()
    }

    SequentialAnimation {
        running: Theme.live && kit.floating
        loops: Animation.Infinite
        onRunningChanged: if (!running) floatShift.y = 0

        NumberAnimation { target: floatShift; property: "y"; from: 0; to: -7 * kit.k; duration: 3000; easing.type: Easing.InOutSine }
        NumberAnimation { target: floatShift; property: "y"; from: -7 * kit.k; to: 0; duration: 3000; easing.type: Easing.InOutSine }
    }

    // Blink: closed at 47 % of a 5.4 s loop, as on the website.
    SequentialAnimation {
        running: Theme.live
        loops: Animation.Infinite
        onRunningChanged: if (!running) blinkScale.yScale = 1

        PauseAnimation { duration: 2430 }
        NumberAnimation { target: blinkScale; property: "yScale"; to: 0.08; duration: 108; easing.type: Easing.InOutQuad }
        NumberAnimation { target: blinkScale; property: "yScale"; to: 1; duration: 108; easing.type: Easing.InOutQuad }
        PauseAnimation { duration: 2754 }
    }

    SequentialAnimation {
        running: Theme.live && kit.petted
        loops: Animation.Infinite
        onRunningChanged: if (!running) { purrShift.x = 0; purrShift.y = 0; }

        ParallelAnimation {
            NumberAnimation { target: purrShift; property: "x"; to: 0.7 * kit.k; duration: 30 }
            NumberAnimation { target: purrShift; property: "y"; to: -0.7 * kit.k; duration: 30 }
        }
        ParallelAnimation {
            NumberAnimation { target: purrShift; property: "x"; to: -0.7 * kit.k; duration: 30 }
            NumberAnimation { target: purrShift; property: "y"; to: 0.6 * kit.k; duration: 30 }
        }
        ParallelAnimation {
            NumberAnimation { target: purrShift; property: "x"; to: 0.6 * kit.k; duration: 30 }
            NumberAnimation { target: purrShift; property: "y"; to: 0.5 * kit.k; duration: 30 }
        }
        ParallelAnimation {
            NumberAnimation { target: purrShift; property: "x"; to: 0; duration: 30 }
            NumberAnimation { target: purrShift; property: "y"; to: 0; duration: 30 }
        }
    }

    SequentialAnimation {
        id: hopAnimation

        NumberAnimation { target: hopShift; property: "y"; to: -18 * kit.k; duration: 170; easing.type: Easing.OutQuad }
        NumberAnimation { target: hopShift; property: "y"; to: 0; duration: 260; easing.type: Easing.OutBounce }
    }
}
