import QtQuick
import QtQuick.Dialogs

FileDialog {
    id: root
    property PickerBackdrop backdrop: PickerBackdrop { picker: root }
    function present(): void { backdrop.present() }
}
