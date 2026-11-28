import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

UiDialog {
    id: root
    title: qsTr("Identity permissions and contact groups")
    width: Math.min(mainWindow.width - 40 * appearance.factor, 900 * appearance.factor)
    height: Math.min(mainWindow.height - 40 * appearance.factor, 850 * appearance.factor)
    property string contactId: ""
    property string groupId: ""
    property string ruleId: ""
    property var draft: [0, 0, 0, 0, 0, 0]
    property var operationNames: [qsTr("Traversal / navigation"), qsTr("Inspect / STAT"), qsTr("LIST"), qsTr("READ / DOWNLOAD"), qsTr("RECURSE"), qsTr("EXPORT")]
    property int contactPageNumber: 0
    function propose(command, values) {
        confirmation.command = command
        confirmation.values = values
        confirmation.expected = sharePermissions.revision
        confirmation.open()
    }
    function editRule(row) {
        ruleId = row.id
        tier.currentIndex = row.tier
        principal.text = row.principal
        path.text = row.path
        draft = row.permissions
        denied.checked = row.denied === 2
    }
    contentItem: ScrollView {
        clip: true
        ColumnLayout {
            width: root.availableWidth
            enabled: !sharePermissions.busy
            Label { text: qsTr("Identity: %1").arg(sharePermissions.identityId); textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            Label { text: qsTr("Local administration for the selected identity. Grants remain subject to assignments, mounts, exclusions and read-only storage limits. Unspecified permissions inherit; identity defaults deny. Changes revoke existing handles immediately."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Label { text: qsTr("Identity contacts — select one for membership and View as Contact"); font.bold: true }
            RowLayout {
                TextField { id: contactFilter; placeholderText: qsTr("Filter contacts"); maximumLength: 128; Layout.fillWidth: true }
                Button { text: qsTr("Search / refresh"); onClicked: { root.contactPageNumber = 0; sharePermissions.contactPage(contactFilter.text, 0) } }
                Button { text: qsTr("Previous"); enabled: root.contactPageNumber > 0; onClicked: sharePermissions.contactPage(contactFilter.text, --root.contactPageNumber) }
                Button { text: qsTr("Next"); enabled: sharePermissions.moreContacts; onClicked: sharePermissions.contactPage(contactFilter.text, ++root.contactPageNumber) }
            }
            ListView {
                Layout.fillWidth: true; Layout.preferredHeight: 100; clip: true
                model: sharePermissions.contacts
                delegate: Button {
                    required property var modelData
                    width: ListView.view.width
                    text: modelData.name + " (" + modelData.id + ")"
                    contentItem: Label { text: parent.text; textFormat: Text.PlainText; elide: Text.ElideRight }
                    highlighted: root.contactId === modelData.id
                    onClicked: root.contactId = modelData.id
                }
            }
            Label { text: qsTr("Selected contact: %1").arg(root.contactId); textFormat: Text.PlainText }
            Label { text: qsTr("General identity contact groups (all memberships)"); font.bold: true }
            RowLayout {
                TextField { id: groupName; placeholderText: qsTr("Group name"); maximumLength: 128; Layout.fillWidth: true }
                Button { text: qsTr("Create"); onClicked: root.propose("group-add", {name: groupName.text}) }
                Button { text: qsTr("Rename selected"); enabled: root.groupId !== ""; onClicked: root.propose("group-rename", {id: root.groupId, name: groupName.text}) }
            }
            ListView {
                Layout.fillWidth: true; Layout.preferredHeight: 140; clip: true
                model: sharePermissions.groups
                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view.width
                    Button {
                        text: modelData.name + " (" + modelData.count + ")"
                        contentItem: Label { text: parent.text; textFormat: Text.PlainText }
                        highlighted: root.groupId === modelData.id
                        onClicked: { root.groupId = modelData.id; groupName.text = modelData.name }
                    }
                    Label { text: modelData.id; textFormat: Text.PlainText; Layout.fillWidth: true }
                    Button {
                        property bool member: modelData.members.indexOf(root.contactId) >= 0
                        text: member ? qsTr("Remove contact") : qsTr("Add contact")
                        enabled: root.contactId !== ""
                        onClicked: root.propose(member ? "member-remove" : "member-add", {id: modelData.id, contact: root.contactId})
                    }
                    Button { text: qsTr("Delete…"); onClicked: root.propose("group-remove", {id: modelData.id, name: modelData.name, members: modelData.count}) }
                }
            }
            Label { text: qsTr("Select a group to inspect its members on each contact page. Deleting a group removes its memberships and is refused while group rules or mounts depend on it."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            ListView {
                Layout.fillWidth: true; Layout.preferredHeight: 100; clip: true
                model: sharePermissions.contacts
                delegate: Label {
                    required property var modelData
                    width: ListView.view.width
                    text: {
                        for (var g of sharePermissions.groups)
                            if (g.id === root.groupId && g.members.indexOf(modelData.id) >= 0)
                                return modelData.name + " (" + modelData.id + ")"
                        return ""
                    }
                    textFormat: Text.PlainText
                }
            }
            Label { text: qsTr("Permission-only path rules — editing does not change mounts"); font.bold: true }
            ListView {
                Layout.fillWidth: true; Layout.preferredHeight: 110; clip: true
                model: sharePermissions.rules
                delegate: Button {
                    required property var modelData
                    width: ListView.view.width
                    text: [qsTr("Identity"), qsTr("Group"), qsTr("Contact")][modelData.tier] + " " + modelData.principal + " " + modelData.path
                    contentItem: Label { text: parent.text; textFormat: Text.PlainText; elide: Text.ElideRight }
                    onClicked: root.editRule(modelData)
                }
            }
            RowLayout {
                ComboBox { id: tier; model: [qsTr("Identity"), qsTr("Group"), qsTr("Contact")] }
                TextField { id: principal; placeholderText: qsTr("Group/contact stable ID (empty for Identity)"); Layout.fillWidth: true; maximumLength: 38 }
                Button { text: qsTr("Use selected"); onClicked: principal.text = tier.currentIndex === 1 ? root.groupId : (tier.currentIndex === 2 ? root.contactId : "") }
            }
            TextField { id: path; text: "/"; maximumLength: 4096; Layout.fillWidth: true; placeholderText: qsTr("Absolute virtual path") }
            Repeater {
                model: 6
                RowLayout {
                    required property int index
                    Label { text: root.operationNames[index]; Layout.fillWidth: true }
                    ComboBox {
                        model: [qsTr("Unspecified / inherit"), qsTr("Allow"), qsTr("Deny")]
                        currentIndex: root.draft[index]
                        onActivated: { var values = root.draft.slice(); values[index] = currentIndex; root.draft = values }
                    }
                }
            }
            CheckBox { id: denied; text: qsTr("DENIED — hide and block this subtree (higher-tier grants may reopen descendants)") }
            RowLayout {
                Button { text: qsTr("New rule"); onClicked: { root.ruleId = ""; root.draft = [0,0,0,0,0,0]; denied.checked = false } }
                Button {
                    text: qsTr("Save rule…")
                    onClicked: root.propose("rule-save", {id: root.ruleId, path: path.text, tier: tier.currentIndex,
                        principal: principal.text, permissions: root.draft, denied: denied.checked ? 2 : 0})
                }
                Button { text: qsTr("Remove rule / inherit…"); enabled: root.ruleId !== ""; onClicked: root.propose("rule-remove", {id: root.ruleId, path: path.text}) }
            }
            Label { text: qsTr("View as Contact — production VFS and permission explanation at the entered path"); font.bold: true; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                Button { text: qsTr("View as selected contact"); enabled: root.contactId !== ""; onClicked: sharePermissions.viewAs(root.contactId, path.text) }
                Button { text: qsTr("Next listing page"); enabled: sharePermissions.morePreview; onClicked: sharePermissions.nextPreview() }
            }
            Label { text: qsTr("Result contact: %1; path: %2").arg(sharePermissions.viewedContact).arg(sharePermissions.viewedPath); textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            Label { text: qsTr("Policy explanations include losing contributions. The listing below also enforces mount, traversal, assignment and backend limits; an unavailable listing does not imply a grant can override these limits."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Repeater {
                model: sharePermissions.decisions
                ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label { text: root.operationNames[[0,1,2,3,11,9].indexOf(modelData.operation)] + ": " + [qsTr("Allowed"),qsTr("Denied"),qsTr("Hidden"),qsTr("Unavailable")][modelData.outcome] + qsTr("; reason %1; generation %2").arg(modelData.reason).arg(modelData.generation) }
                    Repeater {
                        model: modelData.trace
                        Label {
                            required property var modelData
                            Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText
                            color: modelData.tier === 0 ? "#86b7e8" : (modelData.tier === 2 ? "#e2a5d6" : Qt.hsla(modelData.hue / 360, 0.55, 0.65, 1))
                            text: (modelData.tier === 1 ? "● " : "") + [qsTr("Identity"),qsTr("Group"),qsTr("Contact")][modelData.tier] + " · " + modelData.principal + " · " + modelData.path
                                + " · " + [qsTr("Inherit"),qsTr("Allow"),qsTr("Deny")][modelData.permission]
                                + (modelData.barrier === 2 ? qsTr(" · DENIED subtree") : "")
                                + (modelData.decisive ? qsTr(" · decisive") : qsTr(" · contributing / overridden"))
                                + (modelData.mostSpecific ? qsTr(" · most specific") : qsTr(" · inherited ancestor"))
                                + (modelData.identityDefault ? qsTr(" · default") : " · " + modelData.rule)
                        }
                    }
                }
            }
            Repeater { model: sharePermissions.preview; Label { required property var modelData; text: (modelData.directory ? "▸ " : "") + modelData.name; textFormat: Text.PlainText } }
            Label { text: sharePermissions.error; textFormat: Text.PlainText; color: appearance.error; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
    }
    Dialog {
        id: confirmation
        property string command
        property var values: ({})
        property string expected
        title: qsTr("Apply identity access change?")
        width: Math.min(root.width - 30, 650)
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label {
            text: qsTr("This change can broaden or withdraw access for connected contacts. Identity %3. Scope and proposed values: %1 %2. Existing handles will be revoked; dependent group deletion is refused.").arg(confirmation.command).arg(JSON.stringify(confirmation.values)).arg(sharePermissions.identityId)
            textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere
        }
        onAccepted: sharePermissions.edit(command, values, expected)
    }
}
