import QtQuick
import Kyty.Launcher
import QtQuick.Dialogs
import "Util.js" as Util

// Form for one game, or for the global emulator settings that games without custom settings run with.
// Fields that do not apply to the current choices are hidden.
FocusScope {
    id: editor

    property int  row: -1
    // Edits the global emulator settings instead of a game.
    property bool global: false
    property var  values: ({})
    property string errorText: ""
    property string statusText: ""
    property string browseKey: ""
    readonly property bool isNew: row < 0

    readonly property var hints: global ? [
        { pad: "△", key: "O", text: qsTr("Save") },
        { pad: "○", key: "Esc", text: qsTr("Cancel") }
    ] : [
        { pad: "△", key: "O", text: qsTr("Save") },
        { pad: "□", key: "E", text: qsTr("Test run") },
        { pad: "○", key: "Esc", text: qsTr("Cancel") }
    ]

    signal saved(int row)
    signal cancelled()

    readonly property var gameDescriptors: [
        { heading: qsTr("General"), key: "name", label: qsTr("Name"), type: "text" },
        { key: "folder", label: qsTr("Folder (group)"), type: "text" },
        { key: "basedir", label: qsTr("Game folder"), type: "path", kind: "dir" },
        { key: "param_file", label: qsTr("Parameters file"), type: "path", kind: "file" },
        { key: "custom_settings", label: qsTr("Custom emulator settings"), type: "toggle" }
    ]

    readonly property var emulatorDescriptors: [
        { heading: qsTr("Display"), key: "screen_width", label: qsTr("Screen width"), type: "number" },
        { key: "screen_height", label: qsTr("Screen height"), type: "number" },
        { key: "graphics_preset", label: qsTr("Graphics preset"), type: "choice", choices: "graphics_preset" },
        { key: "render_resolution_mode", label: qsTr("Render resolution"), type: "choice", choices: "render_resolution_mode" },
        { key: "render_resolution_scope", label: qsTr("Scaled surfaces"), type: "choice", choices: "render_resolution_scope",
          when: (f) => f.render_resolution_mode === "Fixed" },
        { key: "render_resolution_width", label: qsTr("Render width"), type: "number",
          when: (f) => f.render_resolution_mode === "Fixed" },
        { key: "render_resolution_height", label: qsTr("Render height"), type: "number",
          when: (f) => f.render_resolution_mode === "Fixed" },
        { key: "presentation_filter", label: qsTr("Presentation filter"), type: "choice", choices: "presentation_filter" },
        { key: "presentation_fsr_sharpness", label: qsTr("FSR sharpness (0–100)"), type: "number",
          when: (f) => f.presentation_filter === "Fsr" },
        { key: "texture_anisotropy", label: qsTr("Texture anisotropy"), type: "choice", choices: "texture_anisotropy" },
        { key: "neo", label: qsTr("Neo mode"), type: "toggle" },

        { heading: qsTr("Shaders and validation"), key: "shader_optimization_type", label: qsTr("Optimization"), type: "choice",
          choices: "shader_optimization_type" },
        { key: "shader_validation_enabled", label: qsTr("Shader validation"), type: "toggle" },
        { key: "vulkan_validation_enabled", label: qsTr("Vulkan validation"), type: "toggle" },
        { key: "spirv_debug_printf_enabled", label: qsTr("SPIR-V debug printf"), type: "toggle" },

        { heading: qsTr("Logs and dumps"), key: "printf_direction", label: qsTr("Log output"), type: "choice", choices: "printf_direction" },
        { key: "printf_level", label: qsTr("Log level"), type: "choice", choices: "printf_level",
          when: (f) => f.printf_direction !== "Silent" },
        { key: "printf_output_file", label: qsTr("Log file"), type: "path", kind: "file",
          when: (f) => f.printf_direction === "File" },
        { key: "printf_output_folder", label: qsTr("Log folder"), type: "path", kind: "dir",
          when: (f) => f.printf_direction === "Directory" },
        { key: "shader_log_direction", label: qsTr("Shader log"), type: "choice", choices: "shader_log_direction" },
        { key: "shader_log_folder", label: qsTr("Shader log folder"), type: "path", kind: "dir",
          when: (f) => f.shader_log_direction === "File" },
        { key: "command_buffer_dump_enabled", label: qsTr("Command buffer dump"), type: "toggle" },
        { key: "command_buffer_dump_folder", label: qsTr("Dump folder"), type: "path", kind: "dir",
          when: (f) => f.command_buffer_dump_enabled },
        { key: "pipeline_dump_enabled", label: qsTr("Pipeline dump"), type: "toggle" },
        { key: "pipeline_dump_folder", label: qsTr("Pipeline dump folder"), type: "path", kind: "dir",
          when: (f) => f.pipeline_dump_enabled },
        { key: "profiler_direction", label: qsTr("Profiler output"), type: "choice", choices: "profiler_direction" },
        { key: "profiler_output_file", label: qsTr("Profiler file"), type: "path", kind: "file",
          when: (f) => f.profiler_direction === "File" || f.profiler_direction === "FileAndNetwork" }
    ]

    readonly property var moduleDescriptors: [
        { heading: qsTr("Modules"), key: "elfs", label: qsTr("ELF and PRX files"), type: "elfs" },

        { heading: qsTr("Parameters"), key: "param_file", label: qsTr("param.sfo"), type: "params" }
    ]

    // A game shows its emulator settings only while it has custom ones; otherwise the global ones apply.
    readonly property var descriptors: global ? emulatorDescriptors : gameDescriptors.concat(
        emulatorDescriptors.map((d) => Object.assign({}, d, { when: (f) => !!f.custom_settings && (!d.when || d.when(f)) })),
        moduleDescriptors)

    function open(newRow, fields) {
        global = false;
        row = newRow;
        const next = Object.assign({}, fields);
        if (next.basedir)
            next.elfs = backend.ScanElfs(next.basedir, next.elfs || []);
        values = next;
        errorText = "";
        statusText = "";
        Qt.callLater(focusFirst);
    }

    function openGlobal() {
        global = true;
        row = -1;
        values = backend.globalFields;
        errorText = "";
        statusText = "";
        Qt.callLater(focusFirst);
    }

    function restoreDefaults() {
        values = backend.FieldsFor(-1);
        errorText = "";
        statusText = qsTr("Default settings loaded. Save to keep them.");
    }

    // Editing any of these by hand leaves the chosen graphics preset.
    readonly property var presetKeys: ["screen_width", "screen_height", "render_resolution_mode", "render_resolution_scope",
        "render_resolution_width", "render_resolution_height", "presentation_filter", "texture_anisotropy"]

    function setField(key, value) {
        if (key === "graphics_preset") {
            values = Object.assign(backend.GraphicsPreset(values, value), { graphics_preset: value });
            return;
        }
        const next = Object.assign({}, values);
        next[key] = value;
        if (presetKeys.includes(key))
            next.graphics_preset = "Custom";
        values = next;
        if (key === "basedir")
            setField("elfs", backend.ScanElfs(value, values.elfs || []));
    }

    function save() {
        if (global) {
            errorText = backend.SaveGlobal(values);
            if (errorText === "")
                saved(-1);
            return;
        }
        const result = backend.SaveGame(row, values);
        if (result.error) {
            errorText = result.error;
            return;
        }
        saved(result.row);
    }

    function testRun() {
        const error = backend.TestGame(values);
        errorText = error;
        statusText = error ? "" : qsTr("Test run started in a separate window. Nothing is saved.");
    }

    function browse(key, kind) {
        browseKey = key;
        if (kind === "dir")
            folderDialog.open();
        else
            fileDialog.open();
    }

    function focusItems() {
        const items = [];
        for (let i = 0; i < fieldRows.count; i++) {
            const item = fieldRows.itemAt(i);
            if (item && item.shown)
                items.push(...item.focusItems());
        }
        return items;
    }

    function focusKey(key) {
        for (let i = 0; i < fieldRows.count; i++) {
            const item = fieldRows.itemAt(i);
            if (item && item.desc.key === key && item.shown && item.focusItems().length > 0) {
                item.focusItems()[0].forceActiveFocus();
                return;
            }
        }
        focusFirst();
    }

    function focusFirst() {
        // Another form may have been scrolled; a new one starts at its top.
        flick.contentY = 0;
        const items = focusItems();
        if (items.length > 0)
            items[0].forceActiveFocus();
        else
            saveButton.forceActiveFocus();
    }

    function ensureVisible(item) {
        const top = item.mapToItem(column, 0, 0).y;
        if (top < flick.contentY)
            flick.contentY = Math.max(0, top - 16 * Theme.u);
        else if (top + item.height > flick.contentY + flick.height)
            flick.contentY = top + item.height - flick.height + 16 * Theme.u;
    }

    function moveFocus(delta) {
        const onHeader = [saveButton, testButton, defaultsButton, cancelButton].some((b) => b.activeFocus);
        const items = focusItems();
        if (onHeader) {
            if (delta > 0 && items.length > 0)
                items[0].forceActiveFocus();
            return;
        }
        const index = items.findIndex((item) => item.activeFocus);
        if (index < 0) {
            focusFirst();
            return;
        }
        if (index === 0 && delta < 0) {
            saveButton.forceActiveFocus();
            return;
        }
        const next = items[Math.max(0, Math.min(items.length - 1, index + delta))];
        next.forceActiveFocus();
        ensureVisible(next);
    }

    Keys.onUpPressed: moveFocus(-1)
    Keys.onDownPressed: moveFocus(1)
    Keys.onEscapePressed: cancelled()
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_O) {
            save();
        } else if (event.key === Qt.Key_E && !global) {
            testRun();
        } else {
            event.accepted = false;
            return;
        }
        event.accepted = true;
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose the game folder")
        onAccepted: editor.setField(editor.browseKey, Util.localPath(selectedFolder))
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Choose a file")
        onAccepted: editor.setField(editor.browseKey, Util.localPath(selectedFile))
    }

    Column {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: Theme.margin
        anchors.rightMargin: Theme.margin
        anchors.topMargin: 26 * Theme.u
        spacing: 6 * Theme.u

        Row {
            width: parent.width
            spacing: 12 * Theme.u

            Text {
                width: parent.width - buttons.width - 12 * Theme.u
                anchors.verticalCenter: buttons.verticalCenter
                text: editor.global ? qsTr("Emulator settings") : editor.isNew ? qsTr("New game") : qsTr("Edit game")
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }

            Row {
                id: buttons
                spacing: 10 * Theme.u

                FocusButton {
                    id: saveButton
                    text: qsTr("Save")
                    primary: true
                    KeyNavigation.right: editor.global ? defaultsButton : testButton
                    onActivated: editor.save()
                }

                FocusButton {
                    id: testButton
                    visible: !editor.global
                    text: qsTr("Test run")
                    KeyNavigation.left: saveButton
                    KeyNavigation.right: cancelButton
                    onActivated: editor.testRun()
                }

                FocusButton {
                    id: defaultsButton
                    visible: editor.global
                    text: qsTr("Restore defaults")
                    KeyNavigation.left: saveButton
                    KeyNavigation.right: cancelButton
                    onActivated: editor.restoreDefaults()
                }

                FocusButton {
                    id: cancelButton
                    text: qsTr("Cancel")
                    KeyNavigation.left: editor.global ? defaultsButton : testButton
                    onActivated: editor.cancelled()
                }
            }
        }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: editor.global
                  ? qsTr("Every game runs with these settings unless it has custom emulator settings of its own.")
                  : qsTr("This game runs with the emulator settings from Settings. Turn on custom emulator settings to change them for this game only.")
            visible: editor.global || !editor.values.custom_settings
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }

        Text {
            width: parent.width
            visible: editor.errorText !== ""
            wrapMode: Text.WordWrap
            text: editor.errorText
            color: Theme.danger
            font.pixelSize: Theme.fontSmall
        }

        Text {
            width: parent.width
            visible: editor.statusText !== ""
            wrapMode: Text.WordWrap
            text: editor.statusText
            color: Theme.ok
            font.pixelSize: Theme.fontSmall
        }
    }

    Flickable {
        id: flick
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: 12 * Theme.u
        anchors.leftMargin: Theme.margin
        anchors.rightMargin: Theme.margin
        anchors.bottomMargin: 8 * Theme.u
        contentHeight: column.height
        clip: true

        Column {
            id: column
            width: flick.width - 12 * Theme.u

            Repeater {
                id: fieldRows
                model: editor.descriptors

                FieldRow {
                    required property var modelData

                    width: column.width
                    desc: modelData
                    values: editor.values

                    onEdited: (key, value) => editor.setField(key, value)
                    onPadRequested: (key, title) => pad.openFor(key, title)
                    onBrowseRequested: (key, kind) => editor.browse(key, kind)
                }
            }
        }
    }

    TextPad {
        id: pad
        anchors.fill: parent
        text: editor.values[pad.key] || ""
        onEdited: (value) => editor.setField(pad.key, value)
        onShownChanged: {
            if (!shown)
                editor.focusKey(pad.key);
        }
    }
}
