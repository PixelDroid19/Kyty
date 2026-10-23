import QtQuick
import Kyty.Launcher

// Main page: recently played, the folders as tabs and the games of the selected folder as a grid or a table,
// narrowed by the search box. Everything sits straight on the focused game's artwork, without panels.
FocusScope {
    id: library

    // Left of the leftmost item goes here (the sidebar's library entry).
    property Item sidebarTarget: null

    readonly property var hints: [
        { pad: "✕", key: "Enter", text: qsTr("Run") },
        { pad: "△", key: "O", text: qsTr("Options") },
        { pad: "□", key: "E", text: qsTr("Edit") },
        { pad: "L1 R1", key: "PgUp/PgDn", text: qsTr("Switch folder") },
        { key: "Ctrl+F", text: qsTr("Search") },
        { key: "V", text: qsTr("Grid / table") }
    ]

    signal runRequested(int row)
    signal editRequested(int row)
    signal addRequested()
    signal actionRequested(string action, int row)

    readonly property var folders: backend.folders
    readonly property var folderEntry: backend.folders[folderList.currentIndex] || ({ all: true, title: "", folder: "" })
    readonly property string searchText: searchInput.text.trim().toLowerCase()
    readonly property var visibleGames: filterGames(backend.games, folderEntry, searchText)
    // The view that shows the games; both keep their own current index, copied over when the view changes.
    readonly property Item gameView: backend.tableView ? table : grid
    readonly property var selectedGame: visibleGames[gameView.currentIndex] || ({})
    // The game the user is looking at, in the recently played row or in the list; the window backdrop follows it.
    readonly property var focusedGame: heroList.activeFocus ? backend.recent[heroList.currentIndex] || ({}) : selectedGame

    function filterGames(games, entry, query) {
        const out = [];
        for (let i = 0; i < games.length; i++) {
            const game = games[i];
            if (!entry.all && game.folder !== entry.folder)
                continue;
            const name = (game.title || game.name).toLowerCase();
            if (query === "" || name.includes(query) || game.serial.toLowerCase().includes(query))
                out.push(game);
        }
        return out;
    }

    function toggleView() {
        const index = gameView.currentIndex;
        backend.SetTableView(!backend.tableView);
        gameView.currentIndex = index;
        gameView.forceActiveFocus();
    }

    function focusSearch() {
        searchInput.forceActiveFocus();
        searchInput.selectAll();
    }

    // Keys shared by the grid and the table.
    function handleGameKey(event) {
        if (handleFolderKey(event))
            return;
        if (event.key === Qt.Key_E) {
            editRequested(selectedGame.row);
        } else if (event.key === Qt.Key_O || event.key === Qt.Key_Menu) {
            openMenu(selectedGame.row);
        } else if (event.key === Qt.Key_V) {
            toggleView();
        } else if (event.key === Qt.Key_Slash || (event.key === Qt.Key_F && (event.modifiers & Qt.ControlModifier))) {
            focusSearch();
        } else {
            event.accepted = false;
            return;
        }
        event.accepted = true;
    }

    function focusDefault() {
        if (backend.games.length === 0)
            addButton.forceActiveFocus();
        else
            gameView.forceActiveFocus();
    }

    function changeFolder(delta) {
        const count = backend.folders.length;
        if (count === 0)
            return;
        folderList.currentIndex = (folderList.currentIndex + delta + count) % count;
    }

    // Shows the game with this row, switching to "All games" when it is not in the current folder.
    function selectRow(row) {
        let index = indexOfRow(row);
        if (index < 0) {
            folderList.currentIndex = 0;
            index = indexOfRow(row);
        }
        gameView.currentIndex = Math.max(0, index);
    }

    function indexOfRow(row) {
        for (let i = 0; i < visibleGames.length; i++) {
            if (visibleGames[i].row === row)
                return i;
        }
        return -1;
    }

    // L1 and R1 (PageUp and PageDown) switch folders from any list on the page.
    function handleFolderKey(event) {
        if (event.key === Qt.Key_PageUp)
            changeFolder(-1);
        else if (event.key === Qt.Key_PageDown)
            changeFolder(1);
        else
            return false;
        event.accepted = true;
        return true;
    }

    function runSelected(index) {
        const game = visibleGames[index];
        if (game)
            runRequested(game.row);
    }

    function openMenu(row) {
        const game = backend.games[row];
        if (!game)
            return;
        const last = backend.games.length - 1;
        const items = [
            { label: qsTr("Run"), action: "run", allowed: backend.emulatorReady && !backend.running },
            { label: qsTr("Stop game"), action: "stop", allowed: backend.running, danger: true },
            { label: qsTr("Edit"), action: "edit" },
            { label: qsTr("Move up"), action: "up", allowed: row > 0 },
            { label: qsTr("Move down"), action: "down", allowed: row < last },
            { label: qsTr("Open game folder"), action: "folder" },
            { label: qsTr("Delete"), action: "delete", danger: true },
            { label: qsTr("Close"), action: "close" }
        ];
        menu.openFor(game, row, items);
    }

    function menuChosen(action, row) {
        if (action === "run")
            runRequested(row);
        else if (action === "edit")
            editRequested(row);
        else
            actionRequested(action, row);
    }

    // The focused game's name over its own artwork, as on the console's home screen.
    Column {
        id: header
        anchors.left: parent.left
        anchors.right: status.left
        anchors.top: parent.top
        anchors.leftMargin: Theme.margin
        anchors.rightMargin: Theme.gap
        anchors.topMargin: 26 * Theme.u
        spacing: 4 * Theme.u

        Text {
            width: parent.width
            text: library.focusedGame.title || library.focusedGame.name || library.folderEntry.title || ""
            elide: Text.ElideRight
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }

        // Fixed height, so the page does not move when a game without these details gets the focus.
        Text {
            width: parent.width
            height: Math.ceil(Theme.fontSmall * 1.5)
            text: [library.focusedGame.serial, library.focusedGame.region, library.focusedGame.version].filter(Boolean).join("  ·  ")
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }
    }

    Row {
        id: status
        anchors.right: parent.right
        anchors.rightMargin: Theme.margin
        anchors.top: parent.top
        anchors.topMargin: 34 * Theme.u
        spacing: 22 * Theme.u

        Repeater {
            model: [
                { text: backend.emulatorReady ? qsTr("Emulator ready") : qsTr("Emulator not found"),
                  color: backend.emulatorReady ? Theme.ok : Theme.danger },
                { text: backend.controllerName !== "" ? backend.controllerName : qsTr("No controller"),
                  color: backend.controllerName !== "" ? Theme.ok : Theme.textMuted }
            ]

            Row {
                required property var modelData
                spacing: 8 * Theme.u

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8 * Theme.u
                    height: width
                    radius: width / 2
                    color: parent.modelData.color
                }

                Text {
                    text: parent.modelData.text
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
            }
        }
    }

    Item {
        id: heroBox
        visible: backend.recent.length > 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.topMargin: 20 * Theme.u
        anchors.leftMargin: Theme.margin
        height: visible ? 176 * Theme.u : 0

        Text {
            id: heroLabel
            text: qsTr("Recently played")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }

        HeroRow {
            id: heroList
            anchors.top: heroLabel.bottom
            anchors.topMargin: 12 * Theme.u
            anchors.left: parent.left
            anchors.right: parent.right
            height: 136 * Theme.u
            games: backend.recent
            KeyNavigation.down: folderList
            KeyNavigation.left: library.sidebarTarget

            onActivated: (index) => library.runRequested(backend.recent[index].row)
            onMenuRequested: (index) => library.openMenu(backend.recent[index].row)
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_O || event.key === Qt.Key_Menu) {
                    library.openMenu(backend.recent[heroList.currentIndex].row);
                    event.accepted = true;
                }
            }
        }
    }

    // Folder tabs on the left, search and the grid / table switch on the right.
    Item {
        id: toolbar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: heroBox.visible ? heroBox.bottom : header.bottom
        anchors.topMargin: 18 * Theme.u
        anchors.leftMargin: Theme.margin
        anchors.rightMargin: Theme.margin
        height: 40 * Theme.u

        FolderTabs {
            id: folderList
            // The list starts one tab padding to the left, so the first name lines up with the title above.
            anchors.left: parent.left
            anchors.leftMargin: -tabPadding
            anchors.right: searchBox.left
            anchors.rightMargin: Theme.gap
            height: parent.height
            folders: backend.folders
            focus: true
            KeyNavigation.left: library.sidebarTarget
            KeyNavigation.right: searchBox
            KeyNavigation.up: heroBox.visible ? heroList : null
            KeyNavigation.down: library.gameView

            Keys.onPressed: (event) => {
                if (!library.handleFolderKey(event))
                    event.accepted = false;
            }
        }

        FocusScope {
            id: searchBox
            anchors.right: viewSwitch.left
            anchors.rightMargin: Theme.gap
            width: Math.min(280 * Theme.u, parent.width * 0.4)
            height: parent.height
            KeyNavigation.left: folderList
            KeyNavigation.down: library.gameView
            KeyNavigation.up: heroBox.visible ? heroList : null

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: searchInput.activeFocus ? 2 * Theme.u : 1
                color: searchInput.activeFocus ? Theme.focusRing : Theme.glassBorder
            }

            Image {
                id: searchIcon
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: 18 * Theme.u
                height: width
                source: "icons/search.svg"
                sourceSize.width: Math.ceil(width * Screen.devicePixelRatio)
                opacity: searchInput.activeFocus ? 1 : 0.6
            }

            TextInput {
                id: searchInput
                anchors.left: searchIcon.right
                anchors.right: parent.right
                anchors.leftMargin: 10 * Theme.u
                anchors.verticalCenter: parent.verticalCenter
                focus: true
                clip: true
                selectByMouse: true
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: searchInput.text === ""
                    text: qsTr("Search by name or serial")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontBody
                }

                // A controller cannot type, so Enter opens the on-screen pad while one is connected.
                Keys.onReturnPressed: {
                    if (backend.controllerName !== "")
                        textPad.openFor("search", qsTr("Search games"));
                    else
                        library.gameView.forceActiveFocus();
                }
                Keys.onEnterPressed: library.gameView.forceActiveFocus()
                Keys.onEscapePressed: (event) => {
                    event.accepted = searchInput.text !== "";
                    searchInput.clear();
                }
            }
        }

        Row {
            id: viewSwitch
            anchors.right: parent.right
            height: parent.height
            spacing: 4 * Theme.u

            Repeater {
                model: [
                    { table: false, icon: "icons/library.svg", label: qsTr("Grid") },
                    { table: true, icon: "icons/list.svg", label: qsTr("Table") }
                ]

                Item {
                    required property var modelData
                    readonly property bool active: backend.tableView === modelData.table

                    width: 36 * Theme.u
                    height: parent.height

                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.label

                    Image {
                        anchors.centerIn: parent
                        width: 20 * Theme.u
                        height: width
                        source: parent.modelData.icon
                        sourceSize.width: Math.ceil(width * Screen.devicePixelRatio)
                        opacity: parent.active ? 1 : (switchMouse.containsMouse ? 0.8 : 0.45)
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 16 * Theme.u
                        height: 2 * Theme.u
                        radius: height / 2
                        color: Theme.accent
                        visible: parent.active
                    }

                    MouseArea {
                        id: switchMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (!parent.active)
                                library.toggleView();
                        }
                    }
                }
            }
        }
    }

    GameGrid {
        id: grid
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: toolbar.bottom
        anchors.bottom: parent.bottom
        // Cells pad each cover by 12 units, so the covers line up with the title above.
        anchors.leftMargin: Theme.margin - 12 * Theme.u
        anchors.rightMargin: Theme.margin - 12 * Theme.u
        anchors.topMargin: 14 * Theme.u
        visible: !backend.tableView && library.visibleGames.length > 0
        games: library.visibleGames
        KeyNavigation.up: folderList
        KeyNavigation.left: library.sidebarTarget

        onActivated: (index) => library.runSelected(index)
        onMenuRequested: (index) => library.openMenu(library.visibleGames[index].row)
        Keys.onPressed: (event) => library.handleGameKey(event)
    }

    GameTable {
        id: table
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: toolbar.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.margin - 6 * Theme.u
        anchors.rightMargin: Theme.margin
        anchors.topMargin: 16 * Theme.u
        visible: backend.tableView && library.visibleGames.length > 0
        games: library.visibleGames
        KeyNavigation.up: folderList
        KeyNavigation.left: library.sidebarTarget

        onActivated: (index) => library.runSelected(index)
        onMenuRequested: (index) => library.openMenu(library.visibleGames[index].row)
        Keys.onPressed: (event) => library.handleGameKey(event)
    }

    Column {
        anchors.centerIn: grid
        visible: library.visibleGames.length === 0
        spacing: 16 * Theme.u
        width: grid.width - 80 * Theme.u

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: backend.games.length === 0
                  ? qsTr("No games yet. Add a game folder to start your library.")
                  : library.searchText !== ""
                    ? qsTr("No games match “%1”.").arg(searchInput.text.trim())
                    : qsTr("No games in this folder.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }

        FocusButton {
            id: addButton
            anchors.horizontalCenter: parent.horizontalCenter
            visible: backend.games.length === 0
            text: qsTr("Add game")
            primary: true
            KeyNavigation.left: library.sidebarTarget
            onActivated: library.addRequested()
        }
    }

    ActionMenu {
        id: menu
        anchors.fill: parent
        onChosen: (action, row) => library.menuChosen(action, row)
        onShownChanged: {
            if (!shown)
                library.gameView.forceActiveFocus();
        }
    }

    TextPad {
        id: textPad
        anchors.fill: parent
        text: searchInput.text
        onEdited: (value) => searchInput.text = value
        onShownChanged: {
            if (!shown)
                searchInput.forceActiveFocus();
        }
    }
}
