import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

UiDialog {
    id: root
    title: qsTr("Read-only storage")
    width: Math.min(mainWindow.width - 40 * appearance.factor, 760 * appearance.factor)
    height: Math.min(mainWindow.height - 40 * appearance.factor, 720 * appearance.factor)
    onOpened: readOnlyResources.refresh()
    onClosed: readOnlyResources.discardDraft()
    contentItem: ColumnLayout {
        spacing: 8 * appearance.factor
        Label {
            Layout.fillWidth: true
            text: qsTr("Registration never shares files. Identity assignments, mounts and contact permissions are configured separately. Registration and removal take effect immediately.")
            wrapMode: Text.WordWrap
        }
        ListView {
            id: sources
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 100 * appearance.factor
            clip: true
            model: readOnlyResources
            delegate: Frame {
                required property string resourceId
                required property string sourceName
                required property string sourcePath
                required property string sourceStatus
                required property string sourceError
                required property int symlinkPolicy
                required property int verificationPolicy
                required property string exclusions
                width: ListView.view.width
                ColumnLayout {
                    anchors.fill: parent
                    Label { text: sourceName + " — " + sourceStatus; textFormat: Text.PlainText }
                    Label { text: sourcePath; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                    Label { text: qsTr("Hash verification: %1").arg([qsTr("Cached"), qsTr("Rehash"), qsTr("Snapshot")][verificationPolicy]) }
                    Label {
                        text: qsTr("Symlinks: %1").arg([qsTr("Never"), qsTr("Within VFS"), qsTr("Always")][symlinkPolicy])
                        color: symlinkPolicy === 2 ? "red" : appearance.text
                    }
                    Label {
                        visible: symlinkPolicy === 2
                        text: qsTr("Always can expose individual regular files outside the configured storage boundary when the assignment permits it. External directory traversal is forbidden.")
                        color: "red"; wrapMode: Text.WordWrap; Layout.fillWidth: true
                    }
                    Label { text: qsTr("Excluded directory roots:\n%1").arg(exclusions); textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                    Label { text: sourceError; textFormat: Text.PlainText; color: appearance.error; visible: text.length > 0; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Button {
                        text: qsTr("Unregister…")
                        enabled: !readOnlyResources.busy
                        onClicked: { removal.resourceId = resourceId; removal.open(); }
                    }
                }
            }
        }
        Label { text: qsTr("Add existing directory"); font.bold: true }
        TextField {
            Layout.fillWidth: true; placeholderText: qsTr("Name"); maximumLength: 128
            text: readOnlyResources.draftName
            onTextEdited: readOnlyResources.draftName = text
        }
        RowLayout {
            TextField {
                id: sourcePath; Layout.fillWidth: true; placeholderText: qsTr("Absolute directory path"); maximumLength: 4096
                text: readOnlyResources.draftPath
                onTextEdited: readOnlyResources.draftPath = text
            }
            Button {
                text: qsTr("Browse…")
                onClicked: {
                    const selected = fileDialogs.openFolder(qsTr("Read-only source"), sourcePath.text);
                    if (selected) readOnlyResources.draftPath = selected;
                }
            }
        }
        RowLayout {
            Label { text: qsTr("Symlinks") }
            ComboBox {
                model: [qsTr("Never"), qsTr("Within VFS"), qsTr("Always")]
                currentIndex: readOnlyResources.draftSymlinks
                onActivated: readOnlyResources.draftSymlinks = currentIndex
            }
            Label { text: qsTr("Hash verification") }
            ComboBox {
                model: [qsTr("Cached"), qsTr("Rehash"), qsTr("Snapshot")]
                currentIndex: readOnlyResources.draftVerification
                onActivated: readOnlyResources.draftVerification = currentIndex
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.WordWrap
            visible: readOnlyResources.draftSymlinks === 2
            color: "red"
            text: qsTr("Always can expose individual regular files outside the configured storage boundary when the assignment permits it. External directory traversal is forbidden.")
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.WordWrap
            text: qsTr("Within VFS requires independent access to the resolved target. Private application and credential paths are always excluded under every symlink policy.")
        }
        TextArea {
            Layout.fillWidth: true; Layout.preferredHeight: 65 * appearance.factor
            placeholderText: qsTr("Additional excluded directory roots, one absolute path per line")
            text: readOnlyResources.draftExclusions
            onTextChanged: if (activeFocus) readOnlyResources.draftExclusions = text
        }
        RowLayout {
            Button { text: qsTr("Register"); enabled: !readOnlyResources.busy; onClicked: readOnlyResources.add() }
            Button { text: qsTr("Refresh / retry"); enabled: !readOnlyResources.busy; onClicked: readOnlyResources.refresh() }
            BusyIndicator { running: readOnlyResources.busy; visible: running }
        }
        Label { text: readOnlyResources.error; textFormat: Text.PlainText; color: appearance.error; Layout.fillWidth: true; wrapMode: Text.WordWrap }
    }
    footer: DialogButtonBox { standardButtons: DialogButtonBox.Close; onRejected: root.close() }
    Dialog {
        id: removal
        property string resourceId
        title: qsTr("Unregister storage?")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: qsTr("Remove registration and its private index. Source files remain in place.\nAssignments and mounts must be removed first; removal never cascades.") }
        onAccepted: readOnlyResources.remove(resourceId)
    }
}
