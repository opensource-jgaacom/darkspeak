import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    spacing: 10 * appearance.factor
    Button {
        text: qsTr("Read-only storage…")
        onClicked: storageDialog.open()
    }
    ReadOnlyStorageDialog {
        id: storageDialog
    }
}
