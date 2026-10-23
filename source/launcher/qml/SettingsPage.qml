import QtQuick
import Kyty.Launcher
import QtQuick.Dialogs
import "Util.js" as Util

// The global emulator settings, the emulator program, the run mode, the controller and where the settings are stored.
FocusScope {
    id: settings

    readonly property var hints: [
        { pad: "✕", key: "Enter", text: qsTr("Select") },
        { pad: "○", key: "Esc", text: qsTr("Back") }
    ]

    // What every game without custom settings runs with, one line per area.
    readonly property var summary: {
        const f = backend.globalFields;
        const on = (value) => value ? qsTr("On") : qsTr("Off");
        return [
            { label: qsTr("Screen"), value: "%1 × %2".arg(f.screen_width).arg(f.screen_height) },
            { label: qsTr("Render resolution"), value: f.render_resolution_mode === "Fixed"
                  ? "%1 × %2 · %3".arg(f.render_resolution_width).arg(f.render_resolution_height).arg(f.render_resolution_scope)
                  : qsTr("Native") },
            { label: qsTr("Presentation filter"), value: f.presentation_filter === "Fsr"
                  ? "FSR · %1".arg(f.presentation_fsr_sharpness)
                  : f.presentation_filter },
            { label: qsTr("Texture anisotropy"), value: f.texture_anisotropy },
            { label: qsTr("Neo mode"), value: on(f.neo) },
            { label: qsTr("Shader optimization"), value: f.shader_optimization_type },
            { label: qsTr("Log output"), value: f.printf_direction === "Silent"
                  ? f.printf_direction
                  : "%1 · %2".arg(f.printf_direction).arg(f.printf_level) },
            { label: qsTr("Validation"), value: on(f.vulkan_validation_enabled || f.shader_validation_enabled) }
        ];
    }

    signal closed()
    signal emulatorSettingsRequested()

    function focusDefault() {
        emulatorButton.forceActiveFocus();
    }

    function ensureVisible(item) {
        // Focus can arrive before the page is laid out; there is nothing to scroll to yet.
        if (flick.height <= 0)
            return;
        const top = item.mapToItem(content, 0, 0).y;
        if (top < flick.contentY)
            flick.contentY = Math.max(0, top - 16 * Theme.u);
        else if (top + item.height > flick.contentY + flick.height)
            flick.contentY = top + item.height - flick.height + 16 * Theme.u;
    }

    Keys.onEscapePressed: closed()

    FileDialog {
        id: programDialog
        title: qsTr("Choose the emulator program")
        onAccepted: backend.SetEmulatorPath(Util.localPath(selectedFile))
    }

    component Heading: Text {
        topPadding: 14 * Theme.u
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
    }

    // Inline components cannot see this file's ids, so each use sets its own width.
    component Note: Text {
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: Theme.margin
        anchors.topMargin: 26 * Theme.u
        contentHeight: content.height
        clip: true

        Column {
            id: content
            width: Math.min(parent.width, 720 * Theme.u)
            spacing: 10 * Theme.u

            Text {
                text: qsTr("Settings")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }

            Heading { text: qsTr("Emulator settings") }

            Note { width: content.width; text: qsTr("Every game runs with these unless it has custom emulator settings of its own.") }

            Grid {
                columns: 2
                columnSpacing: 24 * Theme.u
                rowSpacing: 8 * Theme.u

                // Labels in the first column, values in the second.
                Repeater {
                    model: settings.summary.reduce((cells, entry) => cells.concat([{ text: entry.label, label: true }, { text: entry.value, label: false }]), [])

                    Text {
                        required property var modelData
                        text: modelData.text
                        color: modelData.label ? Theme.textSecondary : Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                    }
                }
            }

            FocusButton {
                id: emulatorButton
                primary: true
                text: qsTr("Change emulator settings…")
                KeyNavigation.down: chooseButton
                onActivated: settings.emulatorSettingsRequested()
                onActiveFocusChanged: if (activeFocus) settings.ensureVisible(emulatorButton)
            }

            Heading { text: qsTr("Emulator program") }

            Row {
                spacing: 10 * Theme.u

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8 * Theme.u
                    height: width
                    radius: width / 2
                    color: backend.emulatorReady ? Theme.ok : Theme.danger
                }

                Text {
                    width: content.width - 18 * Theme.u
                    wrapMode: Text.WrapAnywhere
                    text: backend.emulatorReady ? backend.emulatorPath : backend.emulatorMessage
                    color: backend.emulatorReady ? Theme.textPrimary : Theme.danger
                    font.pixelSize: Theme.fontBody
                }
            }

            Note {
                width: content.width
                visible: backend.emulatorVersion !== ""
                text: backend.emulatorVersion
            }

            Row {
                spacing: 10 * Theme.u

                FocusButton {
                    id: chooseButton
                    text: qsTr("Choose program…")
                    KeyNavigation.up: emulatorButton
                    KeyNavigation.right: autoButton
                    KeyNavigation.down: terminalButton
                    onActivated: programDialog.open()
                    onActiveFocusChanged: if (activeFocus) settings.ensureVisible(chooseButton)
                }

                FocusButton {
                    id: autoButton
                    text: qsTr("Search automatically")
                    KeyNavigation.up: emulatorButton
                    KeyNavigation.left: chooseButton
                    KeyNavigation.down: terminalButton
                    onActivated: backend.SetEmulatorPath("")
                }
            }

            Heading { text: qsTr("Run mode") }

            Row {
                spacing: 10 * Theme.u

                FocusButton {
                    id: terminalButton
                    text: qsTr("Run: %1").arg(backend.runModes[backend.runModeIndex])
                    KeyNavigation.up: chooseButton
                    KeyNavigation.down: backButton
                    KeyNavigation.right: logButton
                    onActivated: backend.SetRunModeIndex((backend.runModeIndex + 1) % backend.runModes.length)
                    onActiveFocusChanged: if (activeFocus) settings.ensureVisible(terminalButton)
                }

                FocusButton {
                    id: logButton
                    visible: backend.logPath !== ""
                    text: qsTr("Open last log")
                    KeyNavigation.left: terminalButton
                    KeyNavigation.up: chooseButton
                    KeyNavigation.down: backButton
                    onActivated: backend.OpenLog()
                }
            }

            Note { width: content.width; text: qsTr("Direct runs the emulator without a window of its own and writes its output to a log file. A terminal shows the output live, for debugging.") }

            Heading { text: qsTr("Controller") }

            Text {
                width: content.width
                wrapMode: Text.WordWrap
                text: backend.controllerName !== "" ? backend.controllerName : qsTr("No controller connected. Keyboard and mouse work everywhere.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }

            Heading { text: qsTr("Settings file") }

            TextEdit {
                width: content.width
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                text: backend.settingsFile
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }

            Item { width: 1; height: 6 * Theme.u }

            FocusButton {
                id: backButton
                text: qsTr("Back")
                KeyNavigation.up: terminalButton
                onActivated: settings.closed()
                onActiveFocusChanged: if (activeFocus) settings.ensureVisible(backButton)
            }
        }
    }
}
