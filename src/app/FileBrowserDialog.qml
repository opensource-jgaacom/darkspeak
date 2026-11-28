import QtQuick
import QtQuick.Controls
import org.darkspeak 1.0

UiDialog {
    id: root
    required property var identityId
    property var contactId: null
    property bool remote: false
    title: remote ? qsTr("Remote FileShare") : qsTr("Local FileShare")
    width: Math.min(mainWindow.width - 40 * appearance.factor, 850 * appearance.factor)
    height: Math.min(mainWindow.height - 40 * appearance.factor, 650 * appearance.factor)
    standardButtons: Dialog.Close
    FileBrowserModel { id: browserModel }
    contentItem: FileBrowser { browser: browserModel }
    Component.onCompleted: {
        fileBrowsers.configure(browserModel)
        if (remote) browserModel.openRemote(identityId, contactId)
        else browserModel.openLocal(identityId)
    }
    onClosed: { browserModel.stop(); destroy() }
}
