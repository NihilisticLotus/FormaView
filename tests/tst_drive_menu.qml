import QtQuick
import QtQuick.Controls.Basic
import QtTest
TestCase {
    id: test
    name: "DriveMenu"
    when: windowShown
    visible: true
    width: 1000; height: 500
    QtObject {
        id: service
        signal changed()
        property string directory: "E:/synthetic"
        property string language: "zh-CN"
        property string error: ""
        property bool canBack: false
        property bool canForward: false
        property var available: []
        property var folderMenu: ({})
        property string requestedFolder: ""
        property string unavailable: ""
        property int queries: 0
        function drives() { ++queries; return available }
        function breadcrumbs(path) {
            if (path.startsWith("//")) return [{name:"share",path:"//server/share"},{name:"folder",path:"//server/share/folder"}]
            const root = path.slice(0,3)
            return path.length > 3 ? [{name:root.slice(0,2),path:root},{name:"synthetic",path:path}] : [{name:root.slice(0,2),path:root}]
        }
        function navigate(path) { if(path === unavailable) error = "Synthetic drive unavailable"; else {directory=path;error=""} changed() }
        function listFolders(path) { requestedFolder=path;folderMenu={entries:[],busy:false} }
        function closeFolderMenu() { folderMenu={} }
        function history(offset) {}
        function parentDirectory() {}
        function copy(path) {}
    }
    FolderNavigation { id: navigation; controller: service; width: parent.width; y: 12 }
    function popup() { return findChild(navigation,"folderDrivesPopup") }
    function option(path) {
        for(let i=0;i<popup().count;++i) if(popup().itemAt(i).folderPath === path) return popup().itemAt(i)
        return null
    }
    function paths() {
        let result=[]
        for(let i=0;i<popup().count;++i) {const row=popup().itemAt(i);if(row.visible && row.folderPath) result.push(row.folderPath)}
        return result
    }
    function openName() {
        mouseClick(findChild(navigation,"breadcrumbName"))
        tryCompare(popup(),"opened",true)
        wait(50) // Let newly repeated menu rows finish their layout.
    }
    function init() {
        popup().close()
        navigation.visible=true
        service.directory="E:/synthetic";service.error="";service.unavailable="";service.requestedFolder=""
        service.available=[{name:"C:/",path:"C:/"},{name:"E:/",path:"E:/"}]
        service.changed();wait(30)
    }
    function cleanup() { popup().close();findChild(navigation,"folderChildrenPopup").close();wait(20) }
    function test_chooseRoot() {
        openName()
        compare(service.directory,"E:/synthetic")
        compare(paths(),["C:/","E:/"])
        verify(option("E:/").checked)
        mouseClick(option("C:/"))
        compare(service.directory,"C:/")
        tryCompare(popup(),"visible",false)
    }
    function test_repeatedCloseDuringLayout() {
        service.available="ACDEFGHRTYZ".split("").map(letter => ({name:letter+":/",path:letter+":/"}))
        for(let i=0;i<30;++i) {
            mouseClick(findChild(navigation,"breadcrumbName"))
            tryCompare(popup(),"opened",true)
            wait(i%5)
            const original=option("C:/")
            keyClick(Qt.Key_Escape)
            tryCompare(popup(),"visible",false)
            verify(option("C:/") !== null,"closing must not destroy delegates during pending menu layout")
            compare(option("C:/"),original)
        }
        openName()
        compare(paths().length,service.available.length)
    }
    function test_liveEnumeration() {
        openName()
        const original=option("C:/")
        wait(1150)
        compare(option("C:/"),original,"unchanged enumeration must preserve row/focus identity")
        service.available=[{name:"E:/",path:"E:/"},{name:"R:/",path:"R:/"}]
        tryVerify(function(){return paths().join(',') === "E:/,R:/"},2500)
        compare(option("C:/"),null)
        service.available=[]
        tryVerify(function(){return paths().length===0},2500)
        let empty=null
        for(let i=0;i<popup().count;++i) if(popup().itemAt(i).visible) empty=popup().itemAt(i)
        verify(empty && !empty.enabled)
        compare(empty.text,"没有可用磁盘")
    }
    function test_arrowAndClose() {
        const arrow=findChild(navigation,"breadcrumbChildren")
        mouseClick(arrow);tryCompare(popup(),"opened",true)
        compare(service.requestedFolder,"")
        keyClick(Qt.Key_Escape);tryCompare(popup(),"visible",false)
        tryCompare(arrow,"activeFocus",true)
        openName();navigation.addressFocus();tryCompare(popup(),"visible",false)
        navigation.editing=false
        mouseClick(findChild(navigation,"folderDriveButton"));tryCompare(popup(),"opened",true)
        navigation.visible=false;tryCompare(popup(),"visible",false)
    }
    function test_unavailablePreservesDirectory() {
        service.available=[{name:"E:/",path:"E:/"},{name:"Z:/",path:"Z:/"}]
        service.unavailable="Z:/"
        openName();mouseClick(option("Z:/"))
        compare(service.directory,"E:/synthetic")
        verify(service.error.length>0)
    }
    function test_uncKeepsFolderMeaning() {
        service.directory="//server/share/folder";service.changed();wait(30)
        mouseClick(findChild(navigation,"breadcrumbChildren"))
        tryCompare(findChild(navigation,"folderChildrenPopup"),"opened",true)
        compare(service.requestedFolder,"//server/share")
        verify(!popup().visible)
    }
}
