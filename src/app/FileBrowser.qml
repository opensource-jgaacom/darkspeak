import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.darkspeak 1.0

ColumnLayout {
    id: root
    required property FileBrowserModel browser
    spacing: appearance.spacing
    function showContext(row, x, y) {
        root.browser.prepareContext(row)
        actions.popup(x, y)
    }
    Menu {
        id: actions
        objectName: "browserContextMenu"
        onClosed: entries.forceActiveFocus()
        MenuItem { objectName: "browserContextOpen"; text: qsTr("Open folder"); enabled: root.browser.contextCanOpen; onTriggered: root.browser.openContext() }
        MenuItem { objectName: "browserContextDownload"; text: qsTr("Request download (%1)").arg(root.browser.selectedCount); enabled: root.browser.contextCanDownload; onTriggered: root.browser.requestContextDownloads() }
        MenuItem { text: qsTr("Refresh"); onTriggered: root.browser.refresh() }
        MenuItem { text: qsTr("Select all loaded items (up to 128)"); enabled: root.browser.ready; onTriggered: root.browser.selectAllLoaded() }
        MenuItem { text: qsTr("Clear selection"); enabled: root.browser.selectedCount > 0; onTriggered: root.browser.clearSelection() }
        Label { padding: appearance.spacing; width: 320 * appearance.factor; text: root.browser.contextDownloadReason; visible: text.length > 0; wrapMode: Text.WordWrap; textFormat: Text.PlainText }
    }
    Connections {
        target: root.browser
        function onRevealRow(row) { entries.currentIndex = row; entries.positionViewAtIndex(row, GridView.Contain) }
    }
    Label {
        text: root.browser.location === FileBrowserModel.Local ? qsTr("Local identity files") : qsTr("Remote contact files")
        textFormat: Text.PlainText
        font.bold: true
    }
    RowLayout {
        Button { objectName: "browserBack"; text: qsTr("Back"); enabled: root.browser.canBack; onClicked: root.browser.back() }
        Button { objectName: "browserForward"; text: qsTr("Forward"); enabled: root.browser.canForward; onClicked: root.browser.forward() }
        Button { objectName: "browserUp"; text: qsTr("Up"); enabled: root.browser.canUp; onClicked: root.browser.up() }
        Button { objectName: "browserRefresh"; text: qsTr("Refresh"); onClicked: root.browser.refresh() }
    }
    RowLayout {
        Label { text: qsTr("View:") }
        ComboBox {
            objectName: "browserViewMode"
            model: [qsTr("Thumbnails"), qsTr("Small icons"), qsTr("Details")]
            currentIndex: root.browser.viewMode
            onActivated: root.browser.viewMode = currentIndex
            Accessible.name: qsTr("View mode")
        }
        Button {
            objectName: "browserColumns"
            text: qsTr("Columns")
            visible: root.browser.viewMode === FileBrowserModel.Details
            onClicked: columnsPopup.open()
            Popup {
                id: columnsPopup
                ColumnLayout {
                    Repeater {
                        model: root.browser.detailColumns
                        delegate: RowLayout {
                            required property var modelData
                            required property int index
                            CheckBox {
                                text: modelData.title
                                checked: modelData.visible
                                enabled: modelData.id !== "name"
                                onClicked: root.browser.setColumnVisible(modelData.id, checked)
                            }
                            Button { text: "↑"; Accessible.name: qsTr("Move column left"); enabled: index > 0; onClicked: root.browser.moveColumn(modelData.id, index - 1) }
                            Button { text: "↓"; Accessible.name: qsTr("Move column right"); enabled: index < 4; onClicked: root.browser.moveColumn(modelData.id, index + 1) }
                            SpinBox {
                                from: 80; to: 640; stepSize: 20; value: modelData.width
                                Accessible.name: qsTr("Column width")
                                onValueModified: root.browser.setColumnWidth(modelData.id, value)
                            }
                        }
                    }
                }
            }
        }
    }
    Flickable {
        objectName: "browserBreadcrumbs"
        Layout.fillWidth: true
        Layout.preferredHeight: crumbRow.implicitHeight + 12 * appearance.factor
        contentWidth: crumbRow.implicitWidth
        contentHeight: crumbRow.implicitHeight
        flickableDirection: Flickable.HorizontalFlick
        clip: true
        ScrollBar.horizontal: ScrollBar {}
        Row {
            id: crumbRow
            spacing: appearance.smallSpacing
            Repeater {
                model: root.browser.breadcrumbs
                delegate: Button {
                    required property var modelData
                    objectName: "browserCrumb" + modelData.index
                    width: Math.min(240 * appearance.factor, implicitWidth)
                    Accessible.name: modelData.label
                    contentItem: Label { text: modelData.label; textFormat: Text.PlainText; elide: Text.ElideRight }
                    onClicked: root.browser.openBreadcrumb(modelData.index)
                }
            }
        }
    }
    Label {
        objectName: "browserState"
        Layout.fillWidth: true
        text: root.browser.stateText
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
    BusyIndicator { running: root.browser.busy; visible: running }
    Label {
        Layout.fillWidth: true
        visible: root.browser.error.length > 0
        text: root.browser.error
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
    }
    Label {
        visible: root.browser.ready && entries.count === 0
        text: root.browser.state === FileBrowserModel.OfflineCached ? qsTr("No entries on this cached page.")
              : root.browser.more ? qsTr("No visible entries on this page. More pages are available.")
              : root.browser.state === FileBrowserModel.Empty ? qsTr("This folder is empty.") : qsTr("No entries on this page.")
        textFormat: Text.PlainText
    }
    RowLayout {
        Label { objectName: "browserSelectionCount"; text: qsTr("%1 selected (loaded items only)").arg(root.browser.selectedCount); textFormat: Text.PlainText }
        Button { objectName: "browserDownloadSelection"; text: qsTr("Request download (%1)").arg(root.browser.selectedCount); enabled: root.browser.canDownloadSelection; onClicked: root.browser.requestDownloads(); ToolTip.text: root.browser.downloadReason; ToolTip.visible: hovered && !enabled }
        Button { text: qsTr("Clear"); enabled: root.browser.selectedCount > 0; onClicked: root.browser.clearSelection() }
    }
    ScrollView {
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? Math.min(results.implicitHeight, 90 * appearance.factor) : 0
        visible: root.browser.downloadResults.length > 0
        Column {
            id: results
            Repeater {
                model: root.browser.downloadResults
                Label { required property var modelData; text: modelData.name + ": " + modelData.message; textFormat: Text.PlainText }
            }
        }
    }
    // One virtualized view and one model for all three presentations. No image
    // URLs or original-file reads: providers currently expose no safe previews.
    GridView {
        id: entries
        objectName: "browserEntries"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        focus: true
        activeFocusOnTab: true
        function selectionMode(modifiers) {
            return modifiers & Qt.ShiftModifier ? FileBrowserModel.Range : modifiers & Qt.ControlModifier ? FileBrowserModel.Toggle : FileBrowserModel.Replace
        }
        Keys.onPressed: function(event) {
            const extend = (event.modifiers & Qt.ShiftModifier) !== 0
            const columns = Math.max(1, Math.floor(width / cellWidth))
            if (event.matches(StandardKey.SelectAll)) root.browser.selectAllLoaded()
            else if (event.key === Qt.Key_Escape) root.browser.clearSelection()
            else if (event.key === Qt.Key_Down) root.browser.moveFocus(columns, extend)
            else if (event.key === Qt.Key_Up) root.browser.moveFocus(-columns, extend)
            else if (event.key === Qt.Key_Right) root.browser.moveFocus(1, extend)
            else if (event.key === Qt.Key_Left) root.browser.moveFocus(-1, extend)
            else if (event.key === Qt.Key_Home) root.browser.moveFocus(-2147483647, extend)
            else if (event.key === Qt.Key_End) root.browser.moveFocus(2147483647, extend)
            else if (event.key === Qt.Key_Space) root.browser.selectRow(root.browser.focusedRow, FileBrowserModel.Toggle)
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) root.browser.activateFocused()
            else if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && extend)) root.showContext(root.browser.focusedRow, 0, 0)
            else { event.accepted = false; return }
            event.accepted = true
        }
        cacheBuffer: 2 * cellHeight
        function reportViewport() {
            const columns = Math.max(1, Math.floor(width / cellWidth))
            const top = Math.max(0, contentY - (headerItem ? headerItem.height : 0))
            const first = Math.min(count, Math.floor(top / cellHeight) * columns)
            const last = Math.min(count, first + Math.ceil(height / cellHeight) * columns - 1)
            root.browser.setViewport(first, Math.max(first, last), currentIndex, Math.min(100, 2 * columns),
                                     contentY / Math.max(1, contentHeight - height))
        }
        onContentYChanged: Qt.callLater(reportViewport)
        onCountChanged: Qt.callLater(reportViewport)
        onWidthChanged: Qt.callLater(reportViewport)
        onHeightChanged: Qt.callLater(reportViewport)
        onCellHeightChanged: Qt.callLater(reportViewport)
        onCellWidthChanged: Qt.callLater(reportViewport)
        onCurrentIndexChanged: Qt.callLater(reportViewport)
        Component.onCompleted: Qt.callLater(reportViewport)
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: function(eventPoint) {
                const row = entries.indexAt(eventPoint.position.x + entries.contentX, eventPoint.position.y + entries.contentY)
                if (row < 0) {
                    const point = entries.mapToItem(root, eventPoint.position.x, eventPoint.position.y)
                    root.showContext(-1, point.x, point.y)
                }
            }
        }
        model: root.browser
        readonly property bool details: root.browser.viewMode === FileBrowserModel.Details
        readonly property bool thumbnails: root.browser.viewMode === FileBrowserModel.Thumbnails
        readonly property real columnsWidth: root.browser.detailColumns.reduce(function(total, c) { return total + (c.visible ? c.width * appearance.factor : 0) }, 0)
        contentWidth: details ? Math.max(width, columnsWidth) : width
        cellWidth: details ? contentWidth : Math.max(1, width / Math.max(1, Math.floor(width / ((thumbnails ? 160 : 240) * appearance.factor))))
        cellHeight: (thumbnails ? 142 : 42) * appearance.factor
        header: Row {
            visible: entries.details
            height: visible ? 32 * appearance.factor : 0
            Repeater {
                model: root.browser.detailColumns
                delegate: Label {
                    required property var modelData
                    visible: modelData.visible
                    width: visible ? modelData.width * appearance.factor : 0
                    text: modelData.title
                    font.bold: true
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                }
            }
        }
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}
        delegate: Item {
            id: tile
            required property int index
            required property string entryKey
            required property string entryName
            required property bool directory
            required property bool selected
            width: entries.cellWidth
            height: entries.cellHeight
            activeFocusOnTab: false
            Accessible.name: entryName
            Rectangle { anchors.fill: parent; color: tile.selected ? appearance.accent : "transparent"; opacity: tile.selected ? 0.35 : 1; border.width: entries.activeFocus && entries.currentIndex === tile.index ? 1 : 0; border.color: appearance.accent }
            // Fixed text symbols cannot resolve a peer-controlled URL or path.
            Label {
                id: typeIcon
                visible: !entries.details
                x: entries.thumbnails ? (parent.width - width) / 2 : 4 * appearance.factor
                y: entries.thumbnails ? 8 * appearance.factor : (parent.height - height) / 2
                text: directory ? "▣" : "▤"
                textFormat: Text.PlainText
                font.pixelSize: (entries.thumbnails ? 56 : 22) * appearance.factor
            }
            Label {
                visible: !entries.details
                x: entries.thumbnails ? 4 * appearance.factor : 34 * appearance.factor
                y: entries.thumbnails ? 78 * appearance.factor : 0
                width: parent.width - x - 4 * appearance.factor
                height: entries.thumbnails ? 32 * appearance.factor : parent.height
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: entries.thumbnails ? Text.AlignHCenter : Text.AlignLeft
                text: entryName
                textFormat: Text.PlainText
                elide: Text.ElideRight
            }
            Label {
                visible: entries.thumbnails
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.browser.columnText(tile.index, directory ? "type" : "size")
                textFormat: Text.PlainText
            }
            Row {
                visible: entries.details
                Repeater {
                    model: root.browser.detailColumns
                    delegate: Label {
                        required property var modelData
                        visible: modelData.visible
                        width: visible ? modelData.width * appearance.factor : 0
                        height: tile.height
                        verticalAlignment: Text.AlignVCenter
                        text: {
                            // Model row changes instantiate a new delegate; status
                            // also tracks offline/withdrawal state.
                            const state = root.browser.state
                            return root.browser.columnText(tile.index, modelData.id)
                        }
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }
                }
            }
            ToolTip {
                visible: pointer.containsMouse || (entries.activeFocus && entries.currentIndex === tile.index)
                text: tile.entryName
                contentItem: Label { text: tile.entryName; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere }
            }
            MouseArea {
                id: pointer
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: function(mouse) {
                    entries.forceActiveFocus(); entries.currentIndex = tile.index
                    if (mouse.button === Qt.RightButton) root.showContext(tile.index, mapToItem(root, mouse.x, mouse.y).x, mapToItem(root, mouse.x, mouse.y).y)
                    else root.browser.selectRow(tile.index, entries.selectionMode(mouse.modifiers))
                }
                onPressAndHold: function(mouse) { root.showContext(tile.index, mapToItem(root, mouse.x, mouse.y).x, mapToItem(root, mouse.x, mouse.y).y) }
                onDoubleClicked: function(mouse) { if (mouse.button === Qt.LeftButton && directory && !root.browser.busy) root.browser.openEntry(entryKey) }
                cursorShape: directory ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }
    }
    Button {
        objectName: "nextBrowserPage"
        text: qsTr("Load more")
        visible: root.browser.more
        enabled: !root.browser.busy
        onClicked: root.browser.nextPage()
    }
}
