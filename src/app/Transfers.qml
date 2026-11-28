import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root
    objectName: "transfersPage"
    property var queue: transferQueue
    clip: true
    padding: appearance.spacing
    header: ColumnLayout {
        Label {
            Layout.fillWidth: true
            Layout.margins: appearance.spacing
            text: root.queue.scopeLabel
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            font.bold: true
            font.pixelSize: appearance.fontPixels * 1.4
            Accessible.name: text
        }
        Flow {
            Layout.fillWidth: true
            spacing: appearance.smallSpacing
            Button {
                text: qsTr("All transfers")
                onClicked: root.queue.setScope()
            }
            Button {
                text: qsTr("Identity overview")
                visible: root.queue.identityId.toString() !== "{00000000-0000-0000-0000-000000000000}"
                onClicked: root.queue.setScope(root.queue.identityId)
            }
            ComboBox {
                objectName: "transferCategory"
                model: [qsTr("All states"), qsTr("Active"), qsTr("Queued"), qsTr("Paused / Needs attention"), qsTr("Completed / History")]
                currentIndex: root.queue.category
                Accessible.name: qsTr("Transfer status filter")
                onActivated: root.queue.category = currentIndex
            }
            ComboBox {
                objectName: "transferDirection"
                model: [qsTr("Both directions"), qsTr("Sending"), qsTr("Receiving")]
                currentIndex: root.queue.direction + 1
                Accessible.name: qsTr("Transfer direction filter")
                onActivated: root.queue.direction = currentIndex - 1
            }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("%1 total · %2 active · %3 queued · %4 paused / attention · %5 history · %6 sending / %7 receiving")
                .arg(root.queue.counts.total || 0).arg(root.queue.counts.active || 0).arg(root.queue.counts.queued || 0)
                .arg(root.queue.counts.attention || 0).arg(root.queue.counts.completed || 0)
                .arg(root.queue.counts.sending || 0).arg(root.queue.counts.receiving || 0)
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Conversation work comes first. Class, fairness, contact availability and capacity determine admission. Move changes only this contact's items in the same class and priority. Active pause is unavailable until checkpoint-safe support is verified.")
            wrapMode: Text.WordWrap
            color: appearance.secondaryText
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.queue.busy ? qsTr("Applying…") : root.queue.error
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: appearance.error
            Accessible.role: Accessible.AlertMessage
        }
    }
    ListView {
        id: list
        objectName: "transferList"
        anchors.fill: parent
        model: root.queue
        clip: true
        spacing: appearance.smallSpacing
        keyNavigationEnabled: true
        activeFocusOnTab: true
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            id: row
            required property var entry
            required property int index
            width: list.width
            height: content.implicitHeight + appearance.spacing * 2
            highlighted: ListView.isCurrentItem
            onClicked: list.currentIndex = index
            Accessible.name: entry.title + ", " + entry.direction + ", " + entry.state
            contentItem: ColumnLayout {
                id: content
                spacing: appearance.smallSpacing
                Label {
                    Layout.fillWidth: true
                    text: row.entry.visiblePosition + ". " + row.entry.title + " · " + row.entry.direction + " · " + row.entry.purpose
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.bold: true
                }
                Flow {
                    Layout.fillWidth: true
                    ToolButton {
                        text: qsTr("Identity: %1").arg(row.entry.identity)
                        onClicked: root.queue.setScope(row.entry.identityId, "", row.entry.identity, "")
                    }
                    ToolButton {
                        text: qsTr("Contact: %1").arg(row.entry.contact)
                        onClicked: root.queue.setScope(row.entry.identityId, row.entry.contactId, row.entry.identity, row.entry.contact)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: row.entry.state + (row.entry.reason ? " · " + row.entry.reason : "") + " · " + row.entry.priorityClass
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                }
                ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, row.entry.totalBytes)
                    value: row.entry.provisionalBytes
                    Accessible.name: qsTr("Provisional transfer progress")
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 / %2 bytes provisional · %3 durably verified")
                        .arg(row.entry.provisionalBytes).arg(row.entry.totalBytes)
                        .arg(row.entry.journalKnown ? row.entry.verifiedBytes : qsTr("unknown"))
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: appearance.smallSpacing
                    enabled: !root.queue.busy
                    ComboBox {
                        objectName: "transferPriority"
                        model: [qsTr("High"), qsTr("Normal"), qsTr("Low")]
                        currentIndex: row.entry.priority
                        enabled: row.entry.canPrioritize
                        Accessible.name: qsTr("Priority for %1").arg(row.entry.title)
                        onActivated: root.queue.command(row.entry.workId, "priority", currentIndex)
                    }
                    Button { text: qsTr("Move up"); enabled: row.entry.canPause || row.entry.canResume; onClicked: root.queue.command(row.entry.workId, "up") }
                    Button { text: qsTr("Move down"); enabled: row.entry.canPause || row.entry.canResume; onClicked: root.queue.command(row.entry.workId, "down") }
                    Button { text: qsTr("Pause"); visible: row.entry.canPause; onClicked: root.queue.command(row.entry.workId, "pause") }
                    Button { text: qsTr("Resume"); visible: row.entry.canResume; onClicked: root.queue.command(row.entry.workId, "resume") }
                    Button { text: qsTr("Accept"); visible: row.entry.canAccept; onClicked: root.queue.command(row.entry.workId, "accept") }
                    Button { text: qsTr("Reject"); visible: row.entry.canReject; onClicked: root.queue.command(row.entry.workId, "reject") }
                    Button {
                        text: qsTr("Cancel")
                        visible: row.entry.canCancel
                        onClicked: { cancelDialog.workId = row.entry.workId; cancelDialog.open(); }
                    }
                }
            }
        }
        Label {
            anchors.centerIn: parent
            visible: list.count === 0
            text: qsTr("No transfers in this view")
        }
    }
    Dialog {
        id: cancelDialog
        property var workId
        title: qsTr("Cancel this transfer?")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: root.queue.command(workId, "cancel")
    }
}
