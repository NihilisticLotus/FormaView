import QtQuick
import QtQuick.Controls.Basic
import "."
Window {
  id: root; width: 600; height: 400; visible: true
  property string page: "assets"
  property string marker: "literal-updated"
  property color themeColor: Theme.action
  property var themeObject: Theme
  Rail { id: rail; page: root.page; width: 200; height: parent.height; onNavigate: value => root.page = value }
  Loader { anchors.fill: parent; anchors.leftMargin: 200; active: root.page === "conversion"; sourceComponent: Component { Rectangle { color: Theme.action } } }
}