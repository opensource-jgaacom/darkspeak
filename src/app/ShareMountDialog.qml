import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

UiDialog {
    id: root
    title: qsTr("Identity FileShare")
    width: Math.min(mainWindow.width - 40 * appearance.factor, 850 * appearance.factor)
    height: Math.min(mainWindow.height - 40 * appearance.factor, 800 * appearance.factor)
    contentItem: ScrollView {
        clip: true
        ColumnLayout {
            width: root.availableWidth
            enabled: !shareMounts.busy
            Label { text: qsTr("Assignments make a source available for mounts. Neither assignment nor mounting grants contact access. This editor manages identity mounts only."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            CheckBox {
                id: remoteFileShareToggle
                text: qsTr("Enable remote read-only FileShare for this identity (reconnect contacts after enabling)")
                visible: typeof fileShareService !== "undefined"
                checked: typeof fileShareService !== "undefined" && fileShareService.enabled(sharePermissions.identityId)
                onClicked: fileShareService.setEnabled(sharePermissions.identityId, checked)
                Connections {
                    target: typeof fileShareService !== "undefined" ? fileShareService : null
                    function onConfigurationChanged() { remoteFileShareToggle.checked = fileShareService.enabled(sharePermissions.identityId) }
                }
            }
            Button {
                text: qsTr("Permissions, groups and View as Contact…")
                onClicked: {
                    var component = Qt.createComponent("qrc:/SharePermissionsDialog.qml")
                    var dialog = component.createObject(mainWindow, { "parent": mainWindow })
                    dialog.open()
                }
            }
            Button { text: qsTr("Refresh"); onClicked: shareMounts.refresh() }
            Label { text: qsTr("Registered read-only sources"); font.bold: true }
            Repeater {
                model: shareMounts.sources
                Button { required property var modelData; text: modelData.name; onClicked: shareMounts.selectSource(modelData.id) }
            }
            Label { text: qsTr("Selected source directory: %1").arg(shareMounts.directoryName); textFormat: Text.PlainText }
            RowLayout {
                Button { text: qsTr("Up"); onClicked: shareMounts.up() }
                Button { text: qsTr("Next directory page"); enabled: shareMounts.moreDirectories; onClicked: shareMounts.nextDirectories() }
                Button { text: qsTr("Assign selected directory"); onClicked: shareMounts.assign() }
            }
            Repeater {
                model: shareMounts.directories
                Button { required property var modelData; text: modelData.name; onClicked: shareMounts.enterDirectory(modelData.id) }
            }
            Label { text: qsTr("Identity assignments (external files disabled)"); font.bold: true }
            TextField {
                Layout.fillWidth: true
                text: shareMounts.draftPath
                placeholderText: qsTr("Absolute virtual mount path, e.g. /shared/books")
                maximumLength: 4096
                onTextEdited: shareMounts.draftPath = text
            }
            Repeater {
                model: shareMounts.assignments
                RowLayout {
                    required property var modelData
                    Label { text: modelData.name; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                    Button { text: qsTr("Mount at path"); onClicked: shareMounts.addMount(modelData.id) }
                    Button { text: qsTr("Unassign…"); onClicked: { unassignment.targetId = modelData.id; unassignment.open() } }
                }
            }
            Label { text: qsTr("Identity mounts"); font.bold: true }
            Repeater {
                model: shareMounts.mounts
                RowLayout {
                    required property var modelData
                    Label { text: modelData.path; textFormat: Text.PlainText; Layout.fillWidth: true }
                    Button { text: qsTr("Move to entered path"); onClicked: shareMounts.moveMount(modelData.id) }
                    Button { text: qsTr("Remove mount…"); onClicked: { removal.targetId = modelData.id; removal.open() } }
                }
            }
            Label { text: qsTr("Composed local VFS — administrative preview, not a contact permission view"); font.bold: true; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                Button { text: qsTr("Root /"); onClicked: shareMounts.browse("") }
                Button { text: qsTr("Next preview page"); enabled: shareMounts.morePreview; onClicked: shareMounts.nextPreview() }
            }
            Repeater {
                model: shareMounts.preview
                Button { required property var modelData; text: modelData.name; enabled: modelData.directory; onClicked: shareMounts.browse(modelData.id) }
            }
            Label { text: shareMounts.error; textFormat: Text.PlainText; color: appearance.error; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
    }
    Dialog {
        id: removal
        property string targetId
        title: qsTr("Remove only this virtual mount? Source files and assignments are retained.")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: shareMounts.removeMount(targetId)
    }
    Dialog {
        id: unassignment
        property string targetId
        title: qsTr("Unassign this source? Remove dependent mounts first. Source files are retained.")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: shareMounts.unassign(targetId)
    }
}
