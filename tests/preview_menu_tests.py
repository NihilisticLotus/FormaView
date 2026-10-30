"""Run geometry regression against the actual preview menu and styled controls."""
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
preview = (source / "PreviewPane.qml").read_text(encoding="utf-8-sig")
start = preview.index("AppPopupMenu { id: actions;")
depth = 0
for end in range(preview.index("{", start), len(preview)):
    depth += (preview[end] == "{") - (preview[end] == "}")
    if depth == 0:
        break
menu = preview[start:end + 1]
with tempfile.TemporaryDirectory(prefix="formaview-menu-") as folder:
    work = Path(folder)
    for name in ("AppMenuItem.qml", "AppPopupMenu.qml", "AppIcon.qml", "Theme.qml"):
        shutil.copy2(source / name, work / name)
    (work / "qmldir").write_text("singleton Theme 1.0 Theme.qml\n", encoding="utf-8")
    template = (repo / "tests/preview_menu.qml.in").read_text(encoding="utf-8-sig")
    test = work / "tst_menu.qml"
    test.write_text(template.replace("@@MENU@@", menu), encoding="utf-8")
    log = work / "result.txt"
    result = subprocess.run([str(args.qmltestrunner), "-input", str(test), "-o", str(log) + ",txt"],
                            env={**os.environ, "QT_QPA_PLATFORM": "offscreen"})
    if log.exists():
        print(log.read_text(encoding="utf-8"))
    raise SystemExit(result.returncode)
