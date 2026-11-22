import QtQuick
import QtQuick.Dialogs

FolderDialog {
    id: root
    property PickerBackdrop backdrop: PickerBackdrop { picker: root }
    function present(): void { backdrop.present() }
}
