import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

UiDialog {
    id: root
    title: qsTr("Read-only storage")
    width: Math.min(mainWindow.width - 40 * appearance.factor, 900 * appearance.factor)
    height: Math.min(mainWindow.height - 40 * appearance.factor, 650 * appearance.factor)
    property bool submitting: false
    property string selectedId: ""
    property string selectedName: ""
    property string selectedPath: ""
    property int selectedSymlinks: 0
    property string selectedDetails: ""
    onOpened: readOnlyResources.refresh()
    onClosed: readOnlyResources.discardDraft()
    function compactSize(bytes) {
        if (bytes === undefined || bytes === null)
            return "—";
        const units = ["b", "k", "m", "g", "t", "p", "e"];
        let value = Number(bytes);
        let unit = 0;
        while (value >= 1000 && unit < units.length - 1) {
            value /= 1000;
            ++unit;
        }
        return (unit === 0 ? value.toFixed(0) : value.toFixed(1).replace(/\.0$/, "")) + units[unit];
    }
    contentItem: ColumnLayout {
        spacing: 8 * appearance.factor
        Label {
            Layout.fillWidth: true
            text: qsTr("Registration never shares files. Identity assignments, mounts and contact permissions are configured separately. Changes take effect immediately.")
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Name")
                Layout.preferredWidth: 150 * appearance.factor
                font.bold: true
            }
            Label {
                text: qsTr("Path")
                Layout.fillWidth: true
                font.bold: true
            }
            Label {
                text: qsTr("Files")
                Layout.preferredWidth: 70 * appearance.factor
                horizontalAlignment: Text.AlignRight
                font.bold: true
            }
            Label {
                text: qsTr("Size")
                Layout.preferredWidth: 70 * appearance.factor
                horizontalAlignment: Text.AlignRight
                font.bold: true
            }
        }
        ListView {
            id: sources
            objectName: "readOnlyStorageList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 150 * appearance.factor
            clip: true
            model: readOnlyResources
            currentIndex: -1
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property string resourceId
                required property string sourceName
                required property string sourcePath
                required property string sourceStatus
                required property string sourceError
                required property int symlinkPolicy
                required property int verificationPolicy
                required property string exclusions
                required property var fileCount
                required property var sourceSize
                width: ListView.view.width
                highlighted: root.selectedId === resourceId
                onClicked: {
                    root.selectedId = resourceId;
                    root.selectedName = sourceName;
                    root.selectedPath = sourcePath;
                    root.selectedSymlinks = symlinkPolicy;
                    root.selectedDetails = qsTr("Status: %1\nHash verification: %2\nSymlinks: %3\nExcluded directory roots:\n%4\n%5").arg(sourceStatus).arg([qsTr("Cached"), qsTr("Rehash"), qsTr("Snapshot")][verificationPolicy]).arg([qsTr("Never"), qsTr("Within VFS"), qsTr("Always")][symlinkPolicy]).arg(exclusions).arg(sourceError);
                }
                contentItem: ColumnLayout {
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: sourceName
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            Layout.preferredWidth: 150 * appearance.factor
                        }
                        Label {
                            text: sourcePath
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                        Label {
                            text: fileCount === undefined || fileCount === null ? "—" : String(fileCount)
                            Layout.preferredWidth: 70 * appearance.factor
                            horizontalAlignment: Text.AlignRight
                        }
                        Label {
                            text: root.compactSize(sourceSize)
                            Layout.preferredWidth: 70 * appearance.factor
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                    Label {
                        text: sourceStatus
                        textFormat: Text.PlainText
                        color: appearance.secondaryText
                    }
                    Label {
                        text: sourceError
                        textFormat: Text.PlainText
                        visible: text.length > 0
                        color: appearance.error
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        visible: symlinkPolicy === 2
                        text: qsTr("Symlinks: Always — can expose individual regular files outside the configured storage boundary when the assignment permits it. External directory traversal is forbidden.")
                        color: "red"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: sources.count === 0 && !readOnlyResources.busy
                text: qsTr("No read-only storage registered.")
            }
        }
        RowLayout {
            Button {
                text: qsTr("Add…")
                enabled: !readOnlyResources.busy
                onClicked: {
                    readOnlyResources.discardDraft();
                    editor.open();
                }
            }
            Button {
                text: qsTr("Delete…")
                enabled: !readOnlyResources.busy && root.selectedId.length > 0
                onClicked: removal.open()
            }
            Button {
                text: qsTr("Edit…")
                enabled: !readOnlyResources.busy && root.selectedId.length > 0
                onClicked: {
                    editName.text = root.selectedName;
                    editDialog.open();
                }
            }
            Button {
                text: qsTr("Refresh / retry")
                enabled: !readOnlyResources.busy
                onClicked: readOnlyResources.refresh()
            }
            BusyIndicator {
                running: readOnlyResources.busy
                visible: running
            }
        }
        Label {
            text: qsTr("Counts and sizes reflect indexed available files; they update while indexing. Sizes use decimal units (k, m, g, t).")
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: appearance.secondaryText
        }
        Label {
            text: readOnlyResources.error
            textFormat: Text.PlainText
            color: appearance.error
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }
    footer: DialogButtonBox {
        standardButtons: DialogButtonBox.Close
        onRejected: root.close()
    }
    Connections {
        target: readOnlyResources
        function onRowsRemoved() {
            root.selectedId = "";
        }
        function onModelReset() {
            root.selectedId = "";
        }
        function onStateChanged() {
            if (root.submitting && !readOnlyResources.busy) {
                root.submitting = false;
                if (readOnlyResources.errorCode !== -1)
                    return;
                if (editor.opened)
                    editor.close();
                if (editDialog.opened) {
                    root.selectedId = "";
                    editDialog.close();
                }
            }
        }
    }
    UiDialog {
        id: editor
        title: qsTr("Add read-only storage")
        width: Math.min(root.width, 720 * appearance.factor)
        height: Math.min(mainWindow.height - 40 * appearance.factor, 680 * appearance.factor)
        onClosed: readOnlyResources.discardDraft()
        contentItem: ScrollView {
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                TextField {
                    Layout.fillWidth: true
                    placeholderText: qsTr("Name (defaults to directory name)")
                    maximumLength: 128
                    text: readOnlyResources.draftName
                    onTextEdited: readOnlyResources.draftName = text
                }
                RowLayout {
                    TextField {
                        id: sourcePath
                        Layout.fillWidth: true
                        placeholderText: qsTr("Absolute directory path")
                        maximumLength: 4096
                        text: readOnlyResources.draftPath
                        onTextEdited: readOnlyResources.draftPath = text
                    }
                    Button {
                        text: qsTr("Browse…")
                        onClicked: {
                            const selected = fileDialogs.openFolder(qsTr("Read-only source"), sourcePath.text);
                            if (selected)
                                readOnlyResources.draftPath = selected;
                        }
                    }
                }
                RowLayout {
                    Label {
                        text: qsTr("Symlinks")
                    }
                    ComboBox {
                        palette.buttonText: currentIndex === 2 ? "red" : appearance.text
                        delegate: ItemDelegate {
                            required property int index
                            required property string modelData
                            text: modelData
                            contentItem: Label {
                                text: modelData
                                color: index === 2 ? "red" : appearance.text
                            }
                        }
                        model: [qsTr("Never"), qsTr("Within VFS"), qsTr("Always")]
                        currentIndex: readOnlyResources.draftSymlinks
                        onActivated: readOnlyResources.draftSymlinks = currentIndex
                    }
                    Label {
                        text: qsTr("Hash verification")
                    }
                    ComboBox {
                        model: [qsTr("Cached"), qsTr("Rehash"), qsTr("Snapshot")]
                        currentIndex: readOnlyResources.draftVerification
                        onActivated: readOnlyResources.draftVerification = currentIndex
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: readOnlyResources.draftSymlinks === 2
                    color: "red"
                    text: qsTr("Always can expose individual regular files outside the configured storage boundary when the assignment permits it. External directory traversal is forbidden.")
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Within VFS requires independent access to the resolved target. Private application and credential paths are always excluded under every symlink policy.")
                }
                TextArea {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 65 * appearance.factor
                    placeholderText: qsTr("Additional excluded directory roots, one absolute path per line")
                    text: readOnlyResources.draftExclusions
                    onTextChanged: if (activeFocus)
                        readOnlyResources.draftExclusions = text
                }
                Label {
                    text: readOnlyResources.error
                    textFormat: Text.PlainText
                    color: appearance.error
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
        }
        footer: DialogButtonBox {
            Button {
                text: qsTr("Add")
                enabled: !readOnlyResources.busy
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    root.submitting = true;
                    readOnlyResources.add();
                }
            }
            standardButtons: DialogButtonBox.Cancel
            onRejected: editor.close()
        }
    }
    UiDialog {
        id: editDialog
        title: qsTr("Edit read-only storage")
        width: Math.min(root.width, 680 * appearance.factor)
        contentItem: ColumnLayout {
            TextField {
                id: editName
                Layout.fillWidth: true
                maximumLength: 128
                placeholderText: qsTr("Name (defaults to directory name)")
                Accessible.name: qsTr("Storage name")
            }
            Label {
                text: root.selectedPath
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
            }
            Label {
                visible: root.selectedSymlinks === 2
                text: qsTr("Symlinks: Always — can expose individual regular files outside the configured storage boundary when the assignment permits it. External directory traversal is forbidden.")
                color: "red"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                text: root.selectedDetails
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                text: qsTr("To change the source path or indexing policies, remove the registration and add it again. Source files remain in place.")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                text: readOnlyResources.error
                textFormat: Text.PlainText
                color: appearance.error
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }
        footer: DialogButtonBox {
            Button {
                text: qsTr("Save")
                enabled: !readOnlyResources.busy
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    root.submitting = true;
                    readOnlyResources.rename(root.selectedId, editName.text);
                }
            }
            standardButtons: DialogButtonBox.Cancel
            onRejected: editDialog.close()
        }
    }
    UiDialog {
        id: removal
        title: qsTr("Delete storage registration?")
        width: Math.min(root.width, 600 * appearance.factor)
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label {
            text: qsTr("Remove registration and its private index for %1? Source files remain in place.\nAssignments and mounts must be removed first; removal never cascades.").arg(root.selectedName)
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
        }
        onAccepted: readOnlyResources.remove(root.selectedId)
    }
}
