"""Drive-menu states against production QML and a controlled enumeration double."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("qmltestrunner", type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
source = repo / "src/host"
with tempfile.TemporaryDirectory(prefix="formaview-drives-") as folder:
    work = Path(folder)
    for name in ("FolderNavigation.qml", "AppMenuItem.qml", "AppPopupMenu.qml", "AppIcon.qml",
                 "Tool.qml", "Field.qml", "Theme.qml", "Explain.qml"):
        shutil.copy2(source / name, work / name)
    icon = work / "AppIcon.qml"
    icon.write_text(icon.read_text(encoding="utf-8").replace(
        "qrc:/formaview/icons/", (source / "icons").as_uri() + "/"), encoding="utf-8")
    (work / "qmldir").write_text("singleton Theme 1.0 Theme.qml\nsingleton Explain 1.0 Explain.qml\n", encoding="utf-8")
    test = work / "tst_drive_menu.qml"
    shutil.copy2(repo / "tests/tst_drive_menu.qml", test)
    log = work / "result.txt"
    result = subprocess.run([str(args.qmltestrunner), "-input", str(test), "-o", str(log) + ",txt"],
                            env={**os.environ, "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_CONTROLS_STYLE": "Basic"})
    if log.exists():
        output = log.read_text(encoding="utf-8")
        print(output)
        if any(error in output for error in ("TypeError:", "ReferenceError:", "Binding loop", "Unable to assign", "DelegateModel::cancel")):
            raise SystemExit(1)
    raise SystemExit(result.returncode)
