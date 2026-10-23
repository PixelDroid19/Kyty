import QtQuick
import Kyty.Launcher
import QtQuick.Dialogs
import "Util.js" as Util

// Window and page switcher. Each page handles its own keys; the sidebar moves between pages.
Window {
    id: root

    width: 1280
    height: 720
    minimumWidth: 960
    minimumHeight: 540
    visible: true
    title: qsTr("Kyty Launcher")
    color: Theme.background

    // Scales every size with the window height, from 0.75 on small windows to 1.5 on large ones.
    readonly property real u: Math.max(0.75, Math.min(width / 1280, height / 720, 1.5))
    property string page: "library"
    // Visibility to return to when the game closes, so a maximized launcher comes back maximized.
    property int restoreVisibility: Window.Windowed
    readonly property bool gameRunning: backend.running
    readonly property var activePage: page === "editor" ? editor : page === "settings" ? settings : page === "about" ? about : library

    Binding {
        target: Theme
        property: "u"
        value: root.u
    }

    // Animations run only while someone can see the launcher; otherwise the scene graph stays idle.
    Binding {
        target: Theme
        property: "live"
        value: !root.gameRunning && root.active && root.visibility !== Window.Minimized && root.visibility !== Window.Hidden
    }

    // The launcher steps aside while a game runs and frees its GPU resources, so the emulator gets the machine.
    onGameRunningChanged: {
        if (gameRunning) {
            restoreVisibility = visibility === Window.Minimized ? Window.Windowed : visibility;
            sidebar.mascot.hop();
            stepAside.start();
        } else {
            stepAside.stop();
            visibility = restoreVisibility;
            raise();
            requestActivate();
        }
    }

    // Lets Kit's hop finish before the window goes away.
    Timer {
        id: stepAside
        interval: 450
        onTriggered: {
            root.showMinimized();
            root.releaseResources();
        }
    }

    // Backend signals start with a capital, which QML cannot map to on... handlers, so Component.onCompleted connects them.
    function reportGameFinished(message) {
        if (message)
            notice.show(backend.logPath ? qsTr("%1 Log: %2").arg(message).arg(backend.logPath) : message);
    }

    function reportFolderImported(added, found, error) {
        let message = found === 0 ? qsTr("No games were found in that folder.")
                    : added === 0 ? qsTr("All %1 games in that folder are already in the library.").arg(found)
                    : qsTr("Added %1 of the %2 games found.").arg(added).arg(found);
        if (error)
            message += " " + error;
        notice.show(message);
    }

    // A game folder opens the editor; any other folder adds every game found inside it.
    function addFolder(folder) {
        if (backend.IsGameFolder(folder)) {
            openEditor(-1, backend.DetectGame(folder));
            return;
        }
        const error = backend.ImportFolder(folder);
        notice.show(error ? error : qsTr("Looking for games…"));
    }

    function focusPage() {
        if (page === "library")
            library.focusDefault();
        else if (page === "settings")
            settings.focusDefault();
        else if (page === "about")
            about.focusDefault();
    }

    function showPage(name) {
        page = name;
        Qt.callLater(focusPage);
    }

    function openLibrary() {
        showPage("library");
    }

    function openEditor(row, fields) {
        editor.open(row, fields);
        page = "editor";
    }

    function runGame(row) {
        const error = backend.RunGame(row);
        if (error)
            notice.show(error);
    }

    function handleAction(action, row) {
        if (action === "delete") {
            backend.DeleteGame(row);
        } else if (action === "up" || action === "down") {
            const delta = action === "up" ? -1 : 1;
            backend.MoveGame(row, delta);
            library.selectRow(row + delta);
        } else if (action === "folder") {
            backend.OpenGameFolder(row);
        } else if (action === "stop") {
            backend.StopGame();
        }
    }

    Background {
        anchors.fill: parent
    }

    // Empty while a game runs, so the launcher holds no picture the emulator could use.
    GameBackdrop {
        anchors.fill: parent
        picture: root.gameRunning ? "" : library.focusedGame.picture || ""
    }

    Sidebar {
        id: sidebar
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 96 * Theme.u
        current: root.page === "editor" ? (editor.global ? "settings" : "library") : root.page
        rightTarget: root.page === "library" ? library.gameView : null

        onLibraryRequested: root.openLibrary()
        onAddGameRequested: addDialog.open()
        onSettingsRequested: root.showPage("settings")
        onAboutRequested: root.showPage("about")
        onQuitRequested: root.close()
    }

    Item {
        id: pageArea
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: hints.top
    }

    LibraryPage {
        id: library
        anchors.fill: pageArea
        visible: root.page === "library"
        sidebarTarget: sidebar.libraryButton

        onRunRequested: (row) => root.runGame(row)
        onEditRequested: (row) => root.openEditor(row, backend.FieldsFor(row))
        onAddRequested: addDialog.open()
        onActionRequested: (action, row) => root.handleAction(action, row)
    }

    EditorPage {
        id: editor
        anchors.fill: pageArea
        visible: root.page === "editor"

        onSaved: (row) => {
            if (editor.global) {
                root.showPage("settings");
                return;
            }
            root.openLibrary();
            library.selectRow(row);
        }
        onCancelled: editor.global ? root.showPage("settings") : root.openLibrary()
    }

    SettingsPage {
        id: settings
        anchors.fill: pageArea
        visible: root.page === "settings"
        onClosed: root.openLibrary()
        onEmulatorSettingsRequested: {
            editor.openGlobal();
            root.page = "editor";
        }
    }

    AboutPage {
        id: about
        anchors.fill: pageArea
        visible: root.page === "about"
        onClosed: root.openLibrary()
    }

    HintBar {
        id: hints
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 56 * Theme.u
        hints: root.activePage.hints
    }

    Notice {
        id: notice
        z: 30
        anchors.horizontalCenter: pageArea.horizontalCenter
        anchors.bottom: hints.top
        anchors.bottomMargin: 12 * Theme.u
    }

    FolderDialog {
        id: addDialog
        title: qsTr("Choose a game folder or a folder of games")
        onAccepted: root.addFolder(Util.localPath(selectedFolder))
    }

    Component.onCompleted: {
        backend.GameFinished.connect(root.reportGameFinished);
        backend.FolderImported.connect(root.reportFolderImported);
        const geometry = backend.WindowGeometry();
        if (geometry.width !== undefined) {
            root.x = geometry.x;
            root.y = geometry.y;
            root.width = Math.max(root.minimumWidth, geometry.width);
            root.height = Math.max(root.minimumHeight, geometry.height);
        }
        root.focusPage();
    }

    onClosing: backend.SaveWindowGeometry(root.x, root.y, root.width, root.height)
}
